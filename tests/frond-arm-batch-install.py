#!/usr/bin/env python3
"""Many installs in a row on the T|X over the control socket, timed, as a desktop drop or Android share sends them.

Installs 12 small resource databases and a 2 MB one with 521 resources, in Fast, on a copy of the
session, then lists them all. Each install's time is printed; a small one taking over 4 s on average
fails (about 1.2 s here), and so does any refused install. The sync notifies and the launcher refresh
run once for the batch, before the list. Sessions: --session PATH, else FROND_ARM_SESSIONS/palm-tx,
else build/smoke/machines/palm-tx; with none it prints SKIPPED, and fails with FROND_REQUIRE_SESSIONS=1."""
import argparse
import os
import random
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SMALL = 12
SMALL_MEAN_LIMIT = 4.0  # seconds


def build_prc(name, count, size, seed):
    rng = random.Random(seed)
    records = [rng.randbytes(size) for _ in range(count)]
    header = name.ljust(32, b'\0') + struct.pack('>HHIIIIII4s4sIIH', 1, 1, 0xc0000000, 0xc0000000,
                                                    0, 0, 0, 0, b'appl', b'FrB%c' % (65 + seed % 26), 0, 0, count)
    offset = len(header) + count * 10 + 2
    table = b''
    for index, data in enumerate(records):
        table += struct.pack('>4sHI', b'Xdat', 1000 + index, offset)
        offset += len(data)
    return header + table + b'\0\0' + b''.join(records)


def session(args):
    if args.session:
        return Path(args.session)
    folder = os.environ.get('FROND_ARM_SESSIONS')
    for path in ([Path(folder) / 'palm-tx'] if folder else []) + [ROOT / 'build/smoke/machines/palm-tx']:
        if path.exists():
            return path
    return None


class Control:
    def __init__(self, listener, timeout):
        listener.settimeout(timeout)
        self.sock, _ = listener.accept()
        self.sock.settimeout(timeout)
        self.buffer, self.serial = b'', 0

    def line(self):
        while b'\n' not in self.buffer:
            data = self.sock.recv(65536)
            if not data:
                raise RuntimeError('the core closed the control socket')
            self.buffer += data
        line, self.buffer = self.buffer.split(b'\n', 1)
        return line.decode().split('\t')

    def request(self, command, argument=''):
        self.serial += 1
        self.sock.sendall(f'{self.serial}\t{command}\t{argument.encode().hex()}\n'.encode())
        while True:
            fields = self.line()
            if fields[0] == str(self.serial):
                return fields[1], fields[2], bytes.fromhex(fields[3]).decode(errors='replace')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', help='T|X machine folder or .frond file')
    parser.add_argument('--core', type=Path, default=ROOT / 'out/frond')
    args = parser.parse_args()
    path = session(args)
    if not path:
        print('SKIPPED: no palm-tx session')
        return 1 if os.environ.get('FROND_REQUIRE_SESSIONS') == '1' else 0
    with tempfile.TemporaryDirectory(dir=ROOT / 'build') as work:
        work = Path(work)
        copy = work / path.name
        (shutil.copytree if path.is_dir() else shutil.copy2)(path, copy)
        files = []
        for index in range(SMALL):
            name = b'FrondBatch%02d' % index
            files.append((name.decode(), work / f'{name.decode()}.prc'))
            files[-1][1].write_bytes(build_prc(name, 8, 4000, index))
        files.append(('FrondBatchBig', work / 'FrondBatchBig.prc'))
        files[-1][1].write_bytes(build_prc(b'FrondBatchBig', 521, 3880, 99))
        address = str(work / 'control')
        listener = socket.socket(socket.AF_UNIX)
        listener.bind(address)
        listener.listen(1)
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
        with open(work / 'core.log', 'wb') as log:
            core = subprocess.Popen([str(args.core), str(copy), '--desktop-socket', address, '--no-sound',
                                     '--engine', 'fast'], stdout=log, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, env=env)
        try:
            control = Control(listener, 120)
            control.line()  # ready event
            deadline = time.monotonic() + 120
            while control.request('guest-ready')[0] != 'ok':
                if time.monotonic() > deadline:
                    print('FAIL: the guest never got ready')
                    return 1
                time.sleep(0.2)
            time.sleep(1)
            times = []
            for name, file in files:
                started = time.monotonic()
                while True:
                    status, code, message = control.request('install', str(file))
                    if code != 'not-ready' or time.monotonic() - started > 60:
                        break
                    time.sleep(0.1)
                times.append(time.monotonic() - started)
                print(f'{times[-1]:6.2f} s  {file.stat().st_size:8d} bytes  {name}  {status} {code} {message if status != "ok" else ""}')
                if status != 'ok':
                    print(f'FAIL: {name} was refused')
                    return 1
            status, _, listing = control.request('db-list')
            names = {bytes.fromhex(line.split()[-1]).decode(errors='replace') for line in listing.splitlines() if line.strip()}
            missing = [name for name, _ in files if name not in names]
            control.request('quit')
            core.wait(30)
        finally:
            if core.poll() is None:
                core.kill()
                core.wait()
        if status != 'ok' or missing:
            print('FAIL: the list is missing', missing or status)
            return 1
        mean = sum(times[:SMALL]) / SMALL
        if mean > SMALL_MEAN_LIMIT:
            print(f'FAIL: small installs took {mean:.2f} s on average, over {SMALL_MEAN_LIMIT} s')
            return 1
    print(f'PASS: {len(files)} installs in {sum(times):.1f} s, small ones {mean:.2f} s on average, '
          f'the 2 MB one {times[-1]:.1f} s, all listed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
