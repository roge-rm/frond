#!/usr/bin/env python3
"""Infrared between two real T|X cores joined by two link hubs on loopback.

  tests/link-irda-cores.py [--output DIR] [--probe FrondIRProbe.prc]

Two hubs (frond-desktop --link-service, each with its own runtime and data folder and an ephemeral
port, discovery off) pair over their sockets as two windows would. Each T|X runs a copy of the smoke
session with --irda unix:<its hub's runtime folder>/frond/ir-N.sock and is attached to its hub. The
IR probe app beams its 4096-byte payload A to B, then B to A, through the guests' own IrLAP, IAS,
TinyTP and OBEX, checked with tools/check-irda-probe.py. B then sleeps and wakes with the power key:
A's hub has to see it asleep and running again, and A beams to B once more.

Sessions: FROND_ARM_SESSIONS/palm-tx, else build/smoke/machines/palm-tx; skipped without one. The
probe is --probe or FROND_IR_PROBE, else built with tools/build-palm-probe.py (needs pilrc and the
Palm OS 5 SDK). Logs (hub and core logs, status messages, probe exports) go in --output, mode 0700;
the machine copies are deleted after a pass.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import threading
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'tools'))
import smoke  # noqa: E402

DESKTOP = Path(os.environ.get('FROND_DESKTOP') or ROOT / 'out/frond-desktop').absolute()
SEND = (40, 132)  # the probe's Send button, in screen pixels


def session():
    folder = os.environ.get('FROND_ARM_SESSIONS')
    for path in ([Path(folder) / 'palm-tx'] if folder else []) + [ROOT / 'build/smoke/machines/palm-tx']:
        if path.exists():
            return path
    return None


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class HubSocket:
    """One client connection to a hub, like a window's LinkClient: JSON lines both ways."""

    def __init__(self, path, log, tag):
        self.sock = socket.socket(socket.AF_UNIX)
        end = time.monotonic() + 15
        while True:
            try:
                self.sock.connect(str(path))
                break
            except OSError:
                if time.monotonic() > end:
                    raise
                time.sleep(0.1)
        self.got, self.log, self.tag = [], log, tag
        self.lock = threading.Lock()
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.sock.makefile('rb'):
            message = json.loads(line)
            with self.lock:
                self.got.append(message)
                self.log.write(f'{time.strftime("%H:%M:%S")} {self.tag} {json.dumps(message)}\n')
                self.log.flush()

    def send(self, **op):
        self.sock.sendall(json.dumps(op).encode() + b'\n')

    def last(self, kind):
        with self.lock:
            return next((m for m in reversed(self.got) if m.get('type') == kind), None)

    def wait(self, check, what, timeout=30, since=0):
        """The first message from index since on that passes check."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with self.lock:
                for message in self.got[since:]:
                    if check(message):
                        return message
            time.sleep(0.1)
        raise smoke.Fail(f'{self.tag}: no {what} in {timeout} s')

    def close(self):
        self.sock.close()


class Hub:
    def __init__(self, root, tag, log):
        self.root = root / f'hub-{tag}'
        run, data = self.root / 'run', self.root / 'data'
        run.mkdir(mode=0o700, parents=True)
        data.mkdir(mode=0o700)
        env = dict(os.environ, XDG_RUNTIME_DIR=str(run), XDG_DATA_HOME=str(data), FROND_LINK_PORT='0',
                   FROND_LINK_DISCOVERY_PORT='0', QT_QPA_PLATFORM='offscreen')
        env.pop('FROND_LINK_SOCKET', None)
        self.folder = run / 'frond'
        self.log = (root / f'hub-{tag}.log').open('w')
        self.process = subprocess.Popen([str(DESKTOP), '--link-service'], env=env, stdout=self.log,
                                        stderr=subprocess.STDOUT, start_new_session=True)
        self.status = log
        self.tag = tag
        self.control = self.client('control')
        self.hello = self.control.wait(lambda m: m.get('type') == 'hello', 'hello')

    def client(self, tag):
        return HubSocket(self.folder / 'link.sock', self.status, f'{self.tag}/{tag}')

    def close(self):
        self.control.close()
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.log.close()


def pair(a, b):
    """A pairs with B by address, as Add by address… does."""
    b.control.send(op='watch')
    a.control.send(op='pair', address=f'127.0.0.1:{b.hello["port"]}')
    a.control.wait(lambda m: m.get('type') == 'pair-peer', 'pair-peer')
    code = b.control.wait(lambda m: m.get('type') == 'pair-request', 'pair-request')['code'].replace(' ', '')
    a.control.send(op='pair-code', code=code)
    result = a.control.wait(lambda m: m.get('type') == 'pair-result', 'pair-result')
    smoke.need(result['result'] == 'paired', f'pairing: {result}')
    b.control.wait(lambda m: m.get('type') == 'pair-request-end', 'pair-request-end')


def probe_prc(args, root):
    if args.probe:
        return args.probe.resolve()
    if os.environ.get('FROND_IR_PROBE'):
        return Path(os.environ['FROND_IR_PROBE']).resolve()
    if not ((smoke.SDK / 'include/PalmOS.h').is_file() and shutil.which('pilrc') and shutil.which('m68k-palmos-gcc')):
        return None
    subprocess.run([sys.executable, str(ROOT / 'tools/build-palm-probe.py'), '--sdk', str(smoke.SDK), '--probe', 'irda',
                    '--output', str(root / 'probe')], cwd=ROOT, check=True, capture_output=True)
    return root / 'probe/FrondIRProbe.prc'


def records(path):
    """FrondIRLog's records in an export, as text."""
    spec = __import__('importlib.util').util.spec_from_file_location('checker', ROOT / 'tools/check-irda-probe.py')
    checker = __import__('importlib.util').util.module_from_spec(spec)
    spec.loader.exec_module(checker)
    return [r.rstrip(b'\0').decode('ascii', 'replace') for r in checker.read_records(path, 'FrondIRLog')]


class Machine:
    def __init__(self, root, tag, source, hub, peer):
        self.tag, self.root = tag, root
        self.folder = root / f'machine-{tag}'
        shutil.copytree(source, self.folder) if source.is_dir() else (
            self.folder.mkdir(), shutil.copy2(source, self.folder / f'{tag}.frond'))
        self.ir = hub.folder / f'ir-{tag}.sock'
        work = root / f'core-{tag}'
        work.mkdir()
        self.run = smoke.Run('palm-tx', work, self.folder, timeout=1200, extra=['--irda', f'unix:{self.ir}'])
        self.link = hub.client(f'ir-{tag}')
        self.link.send(op='ir-attach', machine=f'Palm {tag.upper()}', device='Palm TX', link=peer, socket=str(self.ir))
        self.exports = 0

    def export(self, name, databases=('FrondIRLog',)):
        """One ZIP holding the named databases, each exported on its own."""
        path = self.root / f'{self.tag}-{name}.zip'
        with zipfile.ZipFile(path, 'w') as merged:
            for database in databases:
                part = self.root / f'{self.tag}-{name}-{database}.zip'
                end = time.monotonic() + 60
                while True:
                    ok, message = self.run.request('export-db', f'{part}\n{database}', ok=None)
                    if ok:
                        break
                    smoke.need(time.monotonic() < end, f'export {database} on {self.tag}: {message}')
                    time.sleep(1)
                with zipfile.ZipFile(part) as single:
                    for entry in single.infolist():
                        merged.writestr(entry, single.read(entry))
                part.unlink()
        return path

    def power(self, asleep):
        """The power key until the core's power-state says asleep (or awake)."""
        for _ in range(3):
            self.run.boot.key(self.root, 'power')
            end = time.monotonic() + 10
            while time.monotonic() < end:
                if (self.run.require('power-state')['message'].split()[0] == 'asleep') == asleep:
                    time.sleep(1)
                    return
                time.sleep(0.5)
        raise smoke.Fail(f'{self.tag}: the Palm did not {"sleep" if asleep else "wake"}')

    def ir_state(self):
        return self.link.last('ir') or {}

    def close(self):
        self.link.close()
        self.run.close()


def beam(sender, receiver, name):
    """Taps Send on sender and waits for one more send-complete there and receive-complete on receiver."""
    before = [sender.export(f'{name}-before'), receiver.export(f'{name}-before')]
    sent0, got0 = records(before[0]).count('send-complete'), records(before[1]).count('receive-complete')
    sender.run.tap(sender.root, *SEND)
    end = time.monotonic() + 90
    while True:
        time.sleep(3)
        after = [sender.export(f'{name}-after'), receiver.export(f'{name}-after')]
        sent, got = records(after[0]), records(after[1])
        if sent.count('send-complete') > sent0 and got.count('receive-complete') > got0:
            after[1] = receiver.export(f'{name}-after', ('FrondIRLog', 'FrondIRReceived'))
            break
        smoke.need(time.monotonic() < end, f'{name}: no complete beam in 90 s; sender {sent[-4:]}, receiver {got[-4:]}')
    check = subprocess.run([sys.executable, str(ROOT / 'tools/check-irda-probe.py'), *map(str, after),
                            '--sender-before', str(before[0]), '--receiver-before', str(before[1])],
                           capture_output=True, text=True)
    (sender.root / f'{name}-check.txt').write_text(check.stdout + check.stderr)
    smoke.need(check.returncode == 0 and '"status": "pass"' in check.stdout, f'{name}: {check.stdout}{check.stderr}')
    print(f'{name}: pass')


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--output', type=Path, default=ROOT / f'build/link-irda-cores/{time.strftime("%Y%m%d-%H%M%S")}')
    p.add_argument('--probe', type=Path)
    args = p.parse_args()
    source = session()
    if not source:
        print('link IR between cores: SKIP, no T|X smoke session (make smoke-sessions or FROND_ARM_SESSIONS)')
        return 0
    smoke.need(DESKTOP.is_file() and smoke.CORE.is_file(), f'needs {DESKTOP} and {smoke.CORE}')
    root = args.output.resolve()
    root.mkdir(mode=0o700, parents=True)
    os.chmod(root, 0o700)
    prc = probe_prc(args, root)
    if not prc:
        print('link IR between cores: SKIP, no IR probe (--probe, FROND_IR_PROBE, or pilrc and the SDK to build it)')
        return 0
    status = (root / 'hub-messages.log').open('w')
    hubs, machines, passed = [], [], False
    try:
        a, b = Hub(root, 'a', status), Hub(root, 'b', status)
        hubs += [a, b]
        a.control.send(op='rename', name='Frond A')
        b.control.send(op='rename', name='Frond B')
        pair(a, b)
        print('paired')
        ma = Machine(root, 'a', source, a, b.hello['id'])
        machines.append(ma)
        mb = Machine(root, 'b', source, b, a.hello['id'])
        machines.append(mb)
        for m in machines:
            m.link.wait(lambda s: s.get('type') == 'ir' and s.get('state') == 'joined' and s.get('core'), f'{m.tag} joined', 60)
        print('joined')
        for m in machines:
            m.run.install_and_launch(str(prc), 'Frond IR probe')
        time.sleep(3)
        beam(ma, mb, 'a-to-b')
        beam(mb, ma, 'b-to-a')
        # B sleeps: its core closes the IR socket, A's hub shows it asleep; on wake it reopens.
        mark_a, mark_b = len(ma.link.got), len(mb.link.got)
        mb.power(True)
        ma.link.wait(lambda s: s.get('type') == 'ir' and s.get('far') == 'asleep', 'far asleep on A', 20, mark_a)
        mb.link.wait(lambda s: s.get('type') == 'ir' and not s.get('core'), 'core closed on B', 20, mark_b)
        print('b asleep')
        mark_a, mark_b = len(ma.link.got), len(mb.link.got)
        mb.power(False)
        ma.link.wait(lambda s: s.get('type') == 'ir' and s.get('far') == 'running', 'far running on A', 20, mark_a)
        mb.link.wait(lambda s: s.get('type') == 'ir' and s.get('core'), 'core back on B', 20, mark_b)
        print('b awake')
        time.sleep(2)
        beam(ma, mb, 'a-to-b-after-wake')
        passed = True
    finally:
        for m in machines:
            m.close()
        for h in hubs:
            h.close()
        status.close()
        if passed:
            for m in machines:
                shutil.rmtree(m.folder, ignore_errors=True)
    print(json.dumps({'status': 'pass', 'output': str(root)}))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except smoke.Fail as error:
        print(f'link IR between cores: FAIL, {error}')
        sys.exit(1)
