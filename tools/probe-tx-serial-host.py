#!/usr/bin/env python3
"""Run the serial host probe on a private TX session copy with a host peer on the PTY or TCP endpoint."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import signal
import socket
import struct
import sys
import threading
import time
import tty
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('frond_run', ROOT / 'tools/frond-run.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
APP, LOG = 'Frond serial host probe', 'FrondSerialHostLog'
BURST = 2048
BAUDS = {1: 9600, 2: 57600, 3: 115200}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def pattern(i, phase):
    return (i * 37 + phase * 11 + (i >> 3)) & 0xff


def burst(i, phase):
    return (i * 7 + (i >> 8) + phase) & 0xff


def config_database(token):
    raw = bytearray(88)
    raw[:15] = b'FrondSerialHCfg'
    raw[60:68] = b'DATAFrSh'
    struct.pack_into('>H', raw, 76, 1)
    struct.pack_into('>I', raw, 78, 88)
    raw[85] = 1
    return raw + token.encode() + b'\0'


def read_log(path):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        require(len(names) == 1 and names[0].rsplit('/', 1)[-1] == LOG + '.pdb', 'Expected one probe log')
        raw = archive.read(names[0])
    require(raw[:32].split(b'\0', 1)[0] == LOG.encode() and raw[60:68] == b'LOGSFrSh', 'Wrong log identity')
    count = struct.unpack_from('>H', raw, 76)[0]
    offsets = [struct.unpack_from('>I', raw, 78 + i * 8)[0] for i in range(count)] + [len(raw)]
    return [raw[a:b].rstrip(b'\0').decode('ascii') for a, b in zip(offsets, offsets[1:])]


class Peer(threading.Thread):
    """Answers each 70-byte request with its 64 data bytes inverted and a burst."""

    def __init__(self, log, kind, stop):
        super().__init__(daemon=True)
        self.log, self.kind, self.stop = log, kind, stop
        self.requests, self.error, self.endpoint = [], None, None

    def run(self):
        try:
            self.serve()
        except Exception as error:  # reported in the result
            self.error = f'{type(error).__name__}: {error}'

    def serve(self):
        deadline = time.monotonic() + 120
        while not self.endpoint:
            require(time.monotonic() < deadline and not self.stop.is_set(), 'No serial endpoint in core.log')
            text = self.log.read_text(errors='replace') if self.log.exists() else ''
            match = re.search(r'^serial: (pty|tcp) (\S+) ', text, re.M)
            if match:
                require(match[1] == self.kind, 'Wrong endpoint kind')
                self.endpoint = match[2]
            time.sleep(.05)
        if self.kind == 'pty':
            fd = os.open(self.endpoint, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            tty.setraw(fd)
            read, write, close = (lambda: os.read(fd, 4096)), (lambda b: os.write(fd, b)), (lambda: os.close(fd))
        else:
            host, port = self.endpoint.rsplit(':', 1)
            require(host == '127.0.0.1', 'TCP endpoint is not loopback')
            sock = socket.create_connection((host, int(port)))
            sock.setblocking(False)
            fd = sock.fileno()
            read, write, close = (lambda: sock.recv(4096)), sock.send, sock.close
        pending, out = b'', b''
        try:
            while not self.stop.is_set():
                r, w, _ = select.select([fd], [fd] if out else [], [], .1)
                if r:
                    try:
                        data = read()
                    except BlockingIOError:
                        data = None
                    if data == b'':
                        break
                    pending += data or b''
                while len(pending) >= 70:
                    start = pending.find(b'FRSH')
                    if start < 0:
                        pending = pending[-3:]
                        break
                    if len(pending) - start < 70:
                        pending = pending[start:]
                        break
                    request, pending = pending[start:start + 70], pending[start + 70:]
                    phase = request[4]
                    self.requests.append({'phase': phase, 'baud_code': request[5], 'skipped': start,
                                          'match': list(request[6:]) == [pattern(i, phase) for i in range(64)]})
                    out += bytes(~b & 0xff for b in request[6:]) + bytes(burst(i, phase) for i in range(BURST))
                if w and out:
                    try:
                        out = out[write(out):]
                    except BlockingIOError:
                        pass
        finally:
            close()


def check(lines, token, peer):
    require(lines[0] == f'begin token={token} version=1', 'Stale probe log')
    require(lines[-1] == f'complete token={token} failures=0', 'Probe reported failures')
    phases = []
    for line in lines[1:-1]:
        fields = dict(item.split('=') for item in line.split())
        phases.append(fields)
        require(fields['got'] == str(64 + BURST) and fields['bad'] == '0' and fields['line'] == '0000',
                'Bad reply in ' + line)
        require(fields['cts'] == '1' and fields['dsr'] == '1', 'Peer modem lines not seen: ' + line)
    require([int(p['baud']) for p in phases] == list(BAUDS.values()), 'Missing baud rates')
    require(len(peer.requests) == 3 and all(r['match'] and not r['skipped'] for r in peer.requests),
            'Host did not receive every request intact')
    return phases


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--session', type=Path, required=True, help='user-supplied ready TX session')
    p.add_argument('--core', type=Path, default=ROOT / 'out/frond')
    p.add_argument('--probe', type=Path, default=ROOT / 'build/serial-host-probe/FrondSerialHostProbe.prc')
    p.add_argument('--endpoint', choices=['pty', 'tcp'], required=True)
    p.add_argument('--output', type=Path, required=True, help='new private result directory')
    p.add_argument('--timeout', type=runner.positive, default=240)
    args = p.parse_args()
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    report = {'version': 1, 'status': 'error', 'endpoint': args.endpoint, 'error': None}
    stop = threading.Event()
    peer = None
    try:
        require(not args.output.exists(), 'Use a new output directory')
        args.output.mkdir(parents=True, mode=0o700)
        root = args.output.resolve()
        report['input_sha256'] = runner.sha256(args.session)
        token = uuid.uuid4().hex
        config = root / 'config.pdb'
        config.write_bytes(config_database(token))
        serial = 'pty' if args.endpoint == 'pty' else f'tcp:{free_port()}'
        report['serial'] = serial
        peer = Peer(root / 'run' / 'core.log', args.endpoint, stop)
        peer.start()
        argv = ['--core', str(args.core.resolve()), '--session', str(args.session.resolve()),
                '--output', str(root / 'run'), '--app', APP, '--install', str(config),
                '--install', str(args.probe.resolve()), '--export-db', LOG, '--headless',
                '--run-for', '15', '--settle', '2', '--serial', serial,
                '--timeout', str(args.timeout), '--startup-timeout', str(args.timeout), '--ready-timeout', '45']
        result = runner.run(runner.parser().parse_args(argv))
        stop.set()
        peer.join(5)
        report['peer'] = {'endpoint': peer.endpoint, 'requests': peer.requests, 'error': peer.error}
        require(result['status'] == 'ok', f'{result.get("stage")}: {result.get("error")}')
        lines = read_log(Path(result['artifacts']['exports'][0]['path']))
        report['log'] = lines
        report['phases'] = check(lines, token, peer)
        require(runner.sha256(args.session) == report['input_sha256'], 'Input session changed')
        report['status'] = 'ok'
    except KeyboardInterrupt:
        report['error'] = 'Interrupted'
    except (OSError, ValueError, KeyError, IndexError, zipfile.BadZipFile) as error:
        report['error'] = str(error)
    finally:
        stop.set()
    if args.output.exists():
        (args.output / 'serial-host.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if report['status'] == 'ok' else 1


if __name__ == '__main__':
    sys.exit(main())
