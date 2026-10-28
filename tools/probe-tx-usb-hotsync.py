#!/usr/bin/env python3
"""HotSync a private TX session copy over the USB bridge with pilot-link.

The core runs with --usb pty, or tcp:PORT with a PTY relayed to it here. A replay taps the HotSync
button in the HotSync app (Local, Cradle/Cable) once per step; each step runs one
pilot-link command on the endpoint: list, fetch MemoDB, install a PRC, list again
and a few pilot-dlpsh commands. Needs pilot-xfer and pilot-dlpsh on PATH.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import os
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import tty

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('frond_run', ROOT / 'tools/frond-run.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
FIRST_TAP_MS, STEP_MS = 12000, 30000
BUTTON = (160, 160)  # HotSync button in the HotSync app at 320x320


def require(condition, message):
    if not condition:
        raise ValueError(message)


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def prc_name(path):
    return path.read_bytes()[:32].split(b'\0', 1)[0].decode('latin-1')


def steps(install):
    name = prc_name(install)
    return [
        {'name': 'list', 'argv': ['pilot-xfer', '-l'], 'expect': r'List complete\. \d+ files found'},
        {'name': 'fetch', 'argv': ['pilot-xfer', '-f', 'MemoDB'], 'expect': r'complete\.', 'file': 'MemoDB.pdb'},
        {'name': 'install', 'argv': ['pilot-xfer', '-i', str(install)], 'expect': r'complete|Installing'},
        {'name': 'list-after-install', 'argv': ['pilot-xfer', '-l'], 'expect': re.escape(name)},
        {'name': 'dlpsh', 'argv': ['pilot-dlpsh', '-i'], 'stdin': 'user\ndf\nls\nquit\n',
         'expect': r'(?s)Username.*Card0: RAM'},
    ]


class Pilot(threading.Thread):
    """Runs each step's command once the endpoint is known, one after another."""

    def __init__(self, root, plan, log, kind, stop):
        super().__init__(daemon=True)
        self.root, self.plan, self.log, self.kind, self.stop = root, plan, log, kind, stop
        self.results, self.error, self.endpoint, self.keep = [], None, None, None

    def run(self):
        try:
            self.serve()
        except Exception as error:  # reported in the result
            self.error = f'{type(error).__name__}: {error}'

    def relay(self):
        master, slave = os.openpty()
        tty.setraw(slave)
        client = socket.create_connection(tuple(self.endpoint.rsplit(':', 1)[0:1]) + (int(self.endpoint.rsplit(':', 1)[1]),))

        def pump():
            while not self.stop.is_set():
                ready = select.select([master, client], [], [], .1)[0]
                if master in ready:
                    try:
                        data = os.read(master, 4096)
                    except OSError:
                        data = b''  # nobody has the PTY open between pilot-link runs
                    if data:
                        client.sendall(data)
                    else:
                        time.sleep(.01)
                if client in ready:
                    data = client.recv(4096)
                    if not data:
                        return
                    os.write(master, data)

        threading.Thread(target=pump, daemon=True).start()
        self.keep = (master, slave, client)
        return os.ttyname(slave)

    def serve(self):
        deadline = time.monotonic() + 120
        while not self.endpoint:
            require(time.monotonic() < deadline and not self.stop.is_set(), 'No usb endpoint in core.log')
            if self.log.exists():
                match = re.search(r'^usb: (pty|tcp) (\S+) ', self.log.read_text(errors='replace'), re.M)
                if match:
                    self.endpoint = match.group(2)
            time.sleep(.05)
        port = self.endpoint
        if self.kind == 'tcp':
            # pilot-link opens serial ports, not TCP clients: relay a PTY to the listener.
            port = self.relay()
        started = time.monotonic()
        for index, step in enumerate(self.plan):
            work = self.root / f'step{index + 1}-{step["name"]}'
            work.mkdir()
            begin = time.monotonic() - started
            # Leave enough time to finish before the next tap.
            limit = (FIRST_TAP_MS + STEP_MS * (index + 1)) / 1000 - begin - 2
            try:
                done = subprocess.run(step['argv'] + ['-p', port], cwd=work, input=step.get('stdin', ''),
                                      capture_output=True, text=True, timeout=max(5, limit))
                output, code = done.stdout + done.stderr, done.returncode
            except subprocess.TimeoutExpired as error:
                output = (error.stdout or b'').decode(errors='replace') if isinstance(error.stdout, bytes) else (error.stdout or '')
                code = 'timeout'
            (work / 'output.txt').write_text(output)
            ok = code == 0 and re.search(step['expect'], output) is not None
            if step.get('file'):
                fetched = work / step['file']
                ok = ok and fetched.exists() and fetched.stat().st_size > 78
            self.results.append({'name': step['name'], 'exit': code, 'ok': bool(ok),
                                 'seconds': round(time.monotonic() - started - begin, 1)})
            if self.stop.is_set():
                break
            # Wait for the next tap window before the next listener.
            if index + 1 < len(self.plan):
                ready = (FIRST_TAP_MS + STEP_MS * (index + 1)) / 1000 - 8
                while time.monotonic() - started < ready and not self.stop.is_set():
                    time.sleep(.1)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--session', type=Path, required=True, help='user-supplied ready TX session')
    p.add_argument('--core', type=Path, default=ROOT / 'out/frond')
    p.add_argument('--install', type=Path, required=True, help='small PRC to install')
    p.add_argument('--endpoint', choices=['pty', 'tcp'], default='pty')
    p.add_argument('--only', help='comma-separated step names to run (default all)')
    p.add_argument('--output', type=Path, required=True, help='new private result directory')
    p.add_argument('--timeout', type=runner.positive, default=400)
    args = p.parse_args()
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    report = {'version': 1, 'status': 'error', 'endpoint': args.endpoint, 'error': None}
    stop = threading.Event()
    pilot = None
    try:
        require(not args.output.exists(), 'Use a new output directory')
        for tool in ['pilot-xfer', 'pilot-dlpsh']:
            require(shutil.which(tool), f'{tool} is not installed')
        args.output.mkdir(parents=True, mode=0o700)
        root = args.output.resolve()
        report['input_sha256'] = runner.sha256(args.session)
        plan = steps(args.install.resolve())
        if args.only:
            names = args.only.split(',')
            require(set(names) <= {step['name'] for step in plan}, 'Unknown step in --only')
            plan = [step for step in plan if step['name'] in names]
        usb = 'pty' if args.endpoint == 'pty' else f'tcp:{free_port()}'
        report['usb'] = usb
        replay = root / 'taps.replay'
        replay.write_text(''.join(f'{FIRST_TAP_MS + STEP_MS * i} pen-down {BUTTON[0]} {BUTTON[1]}\n'
                                  f'{FIRST_TAP_MS + STEP_MS * i + 150} pen-up\n' for i in range(len(plan))))
        pilot = Pilot(root, plan, root / 'run' / 'core.log', args.endpoint, stop)
        pilot.start()
        run_for = (FIRST_TAP_MS + STEP_MS * len(plan)) // 1000
        argv = ['--core', str(args.core.resolve()), '--session', str(args.session.resolve()),
                '--output', str(root / 'run'), '--app', 'HotSync', '--headless', '--replay', str(replay),
                '--run-for', str(run_for), '--screenshot', str(run_for * 1000 - 500), '--usb', usb,
                '--timeout', str(args.timeout), '--startup-timeout', str(args.timeout), '--ready-timeout', '45']
        result = runner.run(runner.parser().parse_args(argv))
        stop.set()
        pilot.join(30)
        report['steps'] = pilot.results
        report['pilot_error'] = pilot.error
        log = (root / 'run' / 'core.log').read_text(errors='replace')
        report['usb_log'] = [line for line in log.splitlines() if line.startswith('usb: ')]
        require(result['status'] == 'ok', f'{result.get("stage")}: {result.get("error")}')
        require(not pilot.error, pilot.error or '')
        require(len(pilot.results) == len(plan) and all(s['ok'] for s in pilot.results),
                'A pilot-link step failed; see step*/output.txt')
        require(runner.sha256(args.session) == report['input_sha256'], 'Input session changed')
        report['status'] = 'ok'
    except KeyboardInterrupt:
        report['error'] = 'Interrupted'
    except (OSError, ValueError, KeyError, IndexError) as error:
        report['error'] = str(error)
    finally:
        stop.set()
    if args.output.exists():
        (args.output / 'usb-hotsync.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if report['status'] == 'ok' else 1


if __name__ == '__main__':
    sys.exit(main())
