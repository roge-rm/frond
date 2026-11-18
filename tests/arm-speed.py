#!/usr/bin/env python3
"""The ARM runner's speed setting on a private T|X smoke session: --speed takes original, the Faster
multipliers and uncapped (and refuses a bad one or --mips beside it), and the control socket's speed
command reports and changes it while the guest runs.

Session: --session, else FROND_ARM_SESSION, else build/smoke/machines/palm-tx; skips without one.
Scratch goes in build/arm-speed-*."""
import argparse
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

CORE = Path(os.environ.get('FROND_CORE') or ROOT / 'out/frond')
ENV = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')


class Client:
    def __init__(self, session, args, log):
        self.socket_dir = tempfile.mkdtemp(prefix='frond-speed-')
        address = str(Path(self.socket_dir) / 'control')
        listener = socket.socket(socket.AF_UNIX)
        listener.bind(address)
        listener.listen(1)
        runner = ['heavy', 'emulator'] if shutil.which('heavy') else []
        self.process = subprocess.Popen([*runner, str(CORE), str(session), *args, '--no-sound', '--desktop-socket', address],
                                        stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                        start_new_session=True, env=ENV)
        listener.settimeout(300)
        self.connection, _ = listener.accept()
        listener.close()
        self.connection.settimeout(120)
        self.stream = self.connection.makefile('rb')
        assert self.stream.readline() == b'0\tevent\tready\t\n'
        self.serial = 0

    def request(self, command, argument='', ok=True):
        self.serial += 1
        self.connection.sendall(f'{self.serial}\t{command}\t{argument.encode().hex()}\n'.encode())
        while True:
            fields = self.stream.readline().rstrip(b'\n').split(b'\t')
            if fields[0] != b'0':
                break
        assert len(fields) == 4 and fields[0] == str(self.serial).encode(), fields
        assert fields[1] == (b'ok' if ok else b'error'), fields
        return fields[2].decode(), bytes.fromhex(fields[3].decode()).decode()

    def close(self):
        self.request('quit')
        assert self.process.wait(timeout=30) == 0
        self.connection.close()
        shutil.rmtree(self.socket_dir, ignore_errors=True)


def refused(session, args):
    result = subprocess.run([str(CORE), str(session), *args, '--no-sound'], capture_output=True, text=True,
                            timeout=120, env=ENV, stdin=subprocess.DEVNULL)
    assert result.returncode != 0, (args, result.stdout[-1000:])
    return result.stdout + result.stderr


def rate(client):
    """The clock the guest got over the last second, in MHz (speed-state's first figure)."""
    time.sleep(2.5)
    _, text = client.request('speed-state')
    found = re.match(r'([\d.]+) MHz', text)
    assert found, text
    return float(found.group(1)), text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', type=Path)
    args = parser.parse_args()
    source = args.session or Path(os.environ.get('FROND_ARM_SESSION') or ROOT / 'build/smoke/machines/palm-tx')
    if not source.exists():
        print('SKIP: no ARM session (--session or FROND_ARM_SESSION)')
        return
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='arm-speed-', dir=ROOT / 'build') as directory:
        root = Path(directory)
        copy = root / source.name
        if source.is_dir():
            shutil.copytree(source, copy, ignore=shutil.ignore_patterns('*.tmp-*'))
        else:
            shutil.copy2(source, copy)
        session = palm_boot.session_file(copy)
        assert '--speed must be original|1.5x|2x|3x|4x|8x|uncapped' in refused(session, ['--speed', '5x'])
        assert "can't be used together" in refused(session, ['--speed', '2x', '--mips', '100'])
        with (root / 'core.log').open('wb') as log:
            client = Client(session, ['--speed', '2x'], log)
            try:
                assert client.request('speed') == ('speed', '2x')
                faster, text = rate(client)
                print('2x:', text)
                client.request('speed', 'bogus', ok=False)
                client.request('speed', '1x', ok=False)
                assert client.request('speed') == ('speed', '2x')
                assert client.request('speed', 'original') == ('speed', 'original')
                original, text = rate(client)
                print('original:', text)
                assert client.request('speed', '1.5x') == ('speed', '1.5x')
                assert client.request('speed', 'uncapped') == ('speed', 'uncapped')
                assert client.request('speed') == ('speed', 'uncapped')
                assert client.request('speed-state')[1].endswith(' underruns, uncapped')
                # The rate is what the host managed, so it's only printed; test-pxa-faster checks the clock.
                print(f'measured {faster:.2f} MHz at 2x, {original:.2f} MHz at original')
            finally:
                if client.process.poll() is None:
                    client.close()
        # --mips is a fixed rate in place of a setting, until a speed is set.
        with (root / 'core-mips.log').open('wb') as log:
            client = Client(session, ['--mips', '100'], log)
            try:
                assert client.request('speed') == ('speed', 'fixed 100 MIPS')
                assert client.request('speed-state')[1].endswith(', fixed 100 MIPS')
                assert client.request('speed', '2x') == ('speed', '2x')
            finally:
                if client.process.poll() is None:
                    client.close()
    print('PASS: ARM --speed and the speed command: original, Faster multipliers, uncapped')


if __name__ == '__main__':
    main()
