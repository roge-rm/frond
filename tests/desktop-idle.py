#!/usr/bin/env python3
"""Real TX item-1 acceptance; no key, pen, replay, reset, or wake injection.

Build: python3 tools/build-palm-probe.py --probe idle
Run probe: python3 tests/desktop-idle.py --session PRIVATE --output NEW_PRIVATE_DIR
Run Blazer: add --case native --native-app Blazer and --native-wait-range
START:END (hex, end exclusive), using ranges independently verified for that ROM.
Native evidence is a kernel idle PC, not a claim of general UI task attribution.
--baseline repeats readiness on independent restores and always exits nonzero:
not-ready is a reproduced defect, never acceptance. All requests are bounded;
results and failed checkpoints are retained. Saved-session RAM counters prove
that the probe entered EvtGetEvent and did not return across the observation
window. A readiness log alone or elapsed host time is insufficient.
"""
import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
import zlib


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def session_memory(path):
    """Read v6 session RAM without modifying/restoring the guest."""
    data = path.read_bytes()
    magic, version = struct.unpack_from('<II', data)
    require(magic == 0x19800819 and version == 6, 'need a v6 saved session')
    # Magic, version, 32-byte device name, display, RAM size, then six section sizes.
    header = 72
    sizes = struct.unpack_from('<6I', data, 48)
    decoder = zlib.decompressobj()
    payload = decoder.decompress(data[header:], 512 * 1024 * 1024 + 1)
    require(decoder.eof and len(payload) == sum(sizes), 'invalid session compression/length')
    start = sum(sizes[:3])
    return payload[start:start + sizes[3]]


def witness(path):
    memory = session_memory(path)
    magic = b'FrI2WAITRAM!v001'
    offsets = []
    at = memory.find(magic)
    while at >= 0:
        if at + 44 <= len(memory):
            values = struct.unpack_from('>7I', memory, at + 16)
            if values[0] == 1 and values[1] in (1, 2, 3):
                offsets.append((at, values))
        at = memory.find(magic, at + 1)
    require(len(offsets) == 1, f'expected one live RAM witness, found {offsets}')
    at, values = offsets[0]
    result = dict(zip(('version', 'phase', 'entered', 'returned', 'nil', 'other', 'api'), values))
    result['ram_offset'] = at
    require(result['phase'] == 1 and result['entered'] == result['returned'] + 1,
            f'probe has not entered an outstanding EvtGetEvent: {result}')
    require(result['api'] == 1, 'acceptance needs the EvtGetEvent probe, not a diagnostic API variant')
    require(result['nil'] + result['other'] == result['returned'], 'inconsistent counters')
    return result


def database(path, name):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        require(len(names) == 1 and not archive.testzip(), f'invalid named ZIP: {names}')
        data = archive.read(names[0])
    require(len(data) >= 78 and data[:32].split(b'\0')[0] == name.encode(),
            f'wrong database in named ZIP {path}')
    count = int.from_bytes(data[76:78], 'big')
    resource = bool(int.from_bytes(data[32:34], 'big') & 1)
    stride, offset = (10, 6) if resource else (8, 0)
    require(len(data) >= 78 + count*stride, 'short database directory')
    offsets = [int.from_bytes(data[78+i*stride+offset:82+i*stride+offset], 'big') for i in range(count)]
    require(offsets == sorted(offsets) and all(78+count*stride <= n <= len(data) for n in offsets),
            'bad record/resource directory')
    return [data[start:end] for start, end in zip(offsets, offsets[1:] + [len(data)])]


def fixture(path, value, broken=False):
    # Dedicated database; no third-party fixture is replaced. The broken stream
    # has a valid header/directory, changes record 1, then ends before record 2.
    header = bytearray(78)
    header[:len(b'FrondIdleTxn')] = b'FrondIdleTxn'
    header[60:68] = b'DATAFrId'
    header[76:78] = (2).to_bytes(2, 'big')
    second = 4096 if broken else 96 + len(value)
    directory = struct.pack('>IB3sIB3s', 96, 0, b'\0\0\1', second, 0, b'\0\0\2')
    path.write_bytes(header + directory + b'\0\0' + value + (b'' if broken else b'second\0'))


class Acceptance:
    def __init__(self, args):
        self.a = args
        self.root = args.output.resolve()
        self.root.mkdir(parents=True, exist_ok=False, mode=0o700)
        self.events = (self.root / 'requests.jsonl').open('w')
        self.files = [args.session.resolve(), args.probe.resolve(), args.core.resolve(),
                      Path(__file__).resolve(), Path('tests/palm/idle-probe.c').resolve()]
        self.hashes = {str(p): digest(p) for p in self.files}
        for source in [Path(__file__), Path('tests/palm/idle-probe.c'),
                       Path('tests/palm/idle-probe.rcp'), Path('tests/palm/idle-probe.def'),
                       Path('tools/build-palm-probe.py')]:
            shutil.copyfile(source, self.root / source.name)
        (self.root / 'manifest.json').write_text(json.dumps(
            {'argv': sys.argv, 'sha256': self.hashes, 'baseline': args.baseline}, indent=2))
        self.port = None

    def record(self, value):
        self.events.write(json.dumps(value) + '\n'); self.events.flush()
        print(json.dumps(value), flush=True)

    @contextmanager
    def emulator(self, session, label):
        with tempfile.TemporaryDirectory(prefix='frond-idle-') as temp, (self.root / (label+'.log')).open('wb') as log:
            listener = socket.socket(socket.AF_UNIX)
            address = str(Path(temp) / 'control')
            listener.bind(address); listener.listen(1); listener.settimeout(self.a.timeout)
            # Select a loopback debug port for read-only native observations.
            with socket.socket() as port:
                port.bind(('127.0.0.1', 0)); self.port = port.getsockname()[1]
            command = (['heavy', 'emulator'] if shutil.which('heavy') else []) + [
                str(self.a.core.resolve()), str(session), '--no-sound', '--desktop-socket', address]
            if self.a.case == 'native': command += ['--gdb', str(self.port)]
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                stderr=subprocess.STDOUT, start_new_session=True,
                env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'))
            connection = stream = None
            try:
                if self.a.case == 'native':
                    rsp = self.debugger()
                    require(rsp.request('?').startswith('S'), 'no initial debugger stop')
                    require(rsp.request('D') == 'OK', 'detach failed'); rsp.sock.close()
                connection, _ = listener.accept()
                connection.settimeout(self.a.timeout); stream = connection.makefile('rb')
                require(stream.readline() == b'0\tevent\tready\t\n', 'bad desktop greeting')
                serial = 0
                def request(command, argument='', *, error=False):
                    nonlocal serial
                    serial += 1
                    begin = time.monotonic()
                    connection.sendall(f'{serial}\t{command}\t{str(argument).encode().hex()}\n'.encode())
                    fields = stream.readline().rstrip(b'\n').split(b'\t')
                    require(len(fields) == 4 and fields[0] == str(serial).encode(), f'bad response: {fields}')
                    result = dict(label=label, command=command, argument=str(argument),
                        status=fields[1].decode(), code=fields[2].decode(),
                        message=bytes.fromhex(fields[3].decode()).decode(),
                        seconds=round(time.monotonic()-begin, 3))
                    self.record(result)
                    if not error: require(result['status'] == 'ok', str(result))
                    return result
                yield request
                request('quit'); require(process.wait(timeout=20) == 0, 'core exited unsuccessfully')
            finally:
                if stream: stream.close()
                if connection: connection.close()
                listener.close()
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL); process.wait(timeout=5)

    def debugger(self):
        spec = importlib.util.spec_from_file_location('idle_rsp', Path(__file__).with_name('gdbstub.py'))
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        deadline = time.monotonic() + self.a.timeout
        while True:
            try:
                rsp = module.Rsp(self.port); rsp.sock.settimeout(self.a.timeout); return rsp
            except OSError:
                if time.monotonic() >= deadline: raise
                time.sleep(.1)

    def prepare(self, request, command, argument):
        deadline = time.monotonic() + 35
        while True:
            result = request(command, argument, error=True)
            if result['status'] == 'ok': return
            require(result['code'] == 'not-ready' and time.monotonic() < deadline, str(result))
            time.sleep(.2)

    def native_wait(self, label):
        require(self.a.native_wait_range, 'native case needs independently verified --native-wait-range')
        for attempt in range(20):
            rsp = self.debugger()
            try:
                require(rsp.request('?').startswith('S'), 'no debugger stop')
                if self.a.native_wait_break is not None:
                    pc = self.a.native_wait_break
                    require(rsp.request(f'Z0,{pc:x},4') == 'OK', 'idle breakpoint refused')
                    rsp.send('c')
                    require(rsp.packet().startswith('S'), 'idle breakpoint not reached')
                regs = bytes.fromhex(rsp.request('g'))
                values = struct.unpack('<17I', regs)
                pace = rsp.monitor('pace')
                pc, cpsr = values[15], values[16]
                match = any(start <= pc < end for start, end in self.a.native_wait_range)
                evidence = dict(label=label, attempt=attempt, pc=hex(pc), cpsr=hex(cpsr),
                    registers=regs.hex(), pace=pace, idle_range_match=match)
                if match: evidence['code'] = rsp.read(pc & ~3, 32).hex()
                self.record(evidence)
                # ARM kernel idle with IRQs enabled and no executing PACE frame.
                if match and not cpsr & 0x80 and '"available":false' in pace[1].replace(' ', ''):
                    return
            finally:
                require(rsp.request('D') == 'OK', 'detach failed'); rsp.sock.close()
            time.sleep(.1)
        raise RuntimeError('no verified IRQ-enabled native idle sample (no acceptance)')

    def save(self, request, label):
        path = self.root / (label + '.frond'); request('save', path); return path

    def observe(self, request, label):
        if self.a.case == 'native':
            self.native_wait(label+'-a'); time.sleep(self.a.observe)
            self.native_wait(label+'-b')
            return self.save(request, label)
        first = witness(self.save(request, label+'-a'))
        time.sleep(self.a.observe)
        path = self.save(request, label+'-b'); second = witness(path)
        self.record(dict(label=label, witness_before=first, witness_after=second))
        require(first == second, 'EvtGetEvent returned during observation; this is not a blocked control')
        return path

    def export(self, request, label, name):
        path = self.root / (label + '.zip')
        request('export-db', f'{path}\n{name}')
        return database(path, name)

    def operations(self, request, label):
        self.observe(request, label+'-wait')
        logs = self.export(request, label+'-ready-log', 'FrondIdleLog' if self.a.case == 'probe' else 'Saved Preferences')
        if self.a.case == 'probe':
            require(any(r.startswith(b'idle-v2 event-loop-ready ') for r in logs), 'missing readiness log')
        request('install', self.root / 'original.pdb')
        expected = [b'original-record\0', b'second\0']
        require(self.export(request, label+'-installed', 'FrondIdleTxn') == expected, 'installed data mismatch')
        self.save(request, label+'-before-failed-replacement')
        failed = request('install', self.root / 'broken-replacement.pdb', error=True)
        self.save(request, label+'-after-failed-replacement')
        require(failed['status'] == 'error' and failed['code'] in ('corrupt', 'install', 'memory'),
                'replacement must fail inside guest import, not not-ready/read/argument')
        require(self.export(request, label+'-rolled-back', 'FrondIdleTxn') == expected,
                'failed replacement lost/changed the original records')
        request('install', self.root / 'replacement.pdb')
        require(self.export(request, label+'-replaced', 'FrondIdleTxn') == [b'replaced-record\0', b'second\0'],
                'replacement data mismatch')
        self.observe(request, label+'-after-operations')
        # Controlled switch flushes the RAM counters after the wait evidence.
        request('launch-app', self.a.retrieval_app)
        if self.a.case == 'probe':
            logs = self.export(request, label+'-stopped-log', 'FrondIdleLog')
            require(any(r.startswith(b'idle-v2 stopped ') for r in logs), 'no controlled counter flush')
        else:
            request('install', self.a.probe.resolve())
            request('launch-app', 'Frond idle probe')
            time.sleep(self.a.observe)
            logs = self.export(request, label+'-probe-log', 'FrondIdleLog')
            require(any(r.startswith(b'idle-v2 event-loop-ready ') for r in logs), 'missing native-workflow probe log')
        request('launch-app', 'Frond idle probe' if self.a.case == 'probe' else self.a.native_app)
        time.sleep(self.a.observe)
        return self.observe(request, label+'-relaunched')

    def run(self):
        fixture(self.root/'original.pdb', b'original-record\0')
        fixture(self.root/'replacement.pdb', b'replaced-record\0')
        fixture(self.root/'broken-replacement.pdb', b'changed-before-failure\0', broken=True)
        if self.a.retrieve_log:
            require(self.a.case == 'probe' and self.a.prepared, '--retrieve-log needs a prepared probe session')
            with self.emulator(self.a.session.resolve(), 'retrieve') as request:
                before = witness(self.save(request, 'retrieve-before'))
                self.prepare(request, 'launch-app', self.a.retrieval_app)
                logs = self.export(request, 'retrieved', 'FrondIdleLog')
                text = [r.rstrip(b'\0').decode() for r in logs]
                self.record(dict(diagnostic='controlled log retrieval only', witness=before, records=text))
                require(any(r.startswith('idle-v2 event-loop-ready ') for r in text), 'missing readiness')
                require(any(r.startswith('idle-v2 stopped ') for r in text), 'missing stopped counters')
                self.save(request, 'retrieved')
            raise RuntimeError('CONTROLLED LOG RETRIEVAL VERIFIED ONLY: blocked-wait acceptance remains required')
        if self.a.baseline:
            failures = []
            for i in range(3):
                with self.emulator(self.a.session.resolve(), f'baseline-{i}') as request:
                    time.sleep(self.a.observe)
                    if self.a.case == 'native': self.observe(request, f'baseline-{i}-wait')
                    elif self.a.prepared: self.observe(request, f'baseline-{i}-wait')
                    result = request('guest-ready', error=True)
                    failures.append(result['status'] == 'error' and result['code'] == 'not-ready')
                    self.save(request, f'baseline-{i}-after')
            require(all(failures), f'not-ready not reproduced on all restores: {failures}')
            raise RuntimeError('BASELINE REPRODUCED: not-ready on 3 independent restores; item1 incomplete')
        with self.emulator(self.a.session.resolve(), 'initial') as request:
            if self.a.case == 'probe' and not self.a.prepared:
                self.prepare(request, 'install', self.a.probe.resolve())
                self.prepare(request, 'launch-app', 'Frond idle probe')
            time.sleep(self.a.observe)
            self.observe(request, 'initial-idle')
            # Always retain a prepared fixture even if the first operation fails.
            after = self.operations(request, 'initial')
        with self.emulator(after, 'restored') as request:
            self.observe(request, 'restored-initial-wait')
            require(self.export(request, 'restored-persisted', 'FrondIdleTxn') ==
                    [b'replaced-record\0', b'second\0'], 'replacement did not survive save/restore')
            self.operations(request, 'restored')
        return 'PASS: actual wait, install, failed replacement preservation, replacement, named export, launch, restore, logs'


def ranges(value):
    start, end = (int(s, 16) for s in value.split(':'))
    if not 0 <= start < end <= 0x100000000: raise argparse.ArgumentTypeError('invalid range')
    return start, end


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--core', type=Path, default=Path('out/frond'))
    p.add_argument('--session', type=Path, required=True)
    p.add_argument('--probe', type=Path, default=Path('build/idle-probe/FrondIdleProbe.prc'))
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--case', choices=('probe', 'native'), default='probe')
    p.add_argument('--prepared', action='store_true', help='session already runs this v2 probe')
    p.add_argument('--retrieve-log', action='store_true', help='controlled diagnostic retrieval only; exits 1, never acceptance')
    p.add_argument('--baseline', action='store_true', help='repeat current-core readiness failure; exits 1')
    p.add_argument('--native-app', default='Blazer')
    p.add_argument('--native-wait-range', type=ranges, action='append', default=[])
    p.add_argument('--native-wait-break', type=lambda s: int(s, 16),
                   help='verified ARM idle instruction; read-only host breakpoint, no guest memory patch')
    p.add_argument('--retrieval-app', default='Preferences')
    p.add_argument('--observe', type=float, default=2)
    p.add_argument('--timeout', type=float, default=90)
    a = p.parse_args()
    require(1 <= a.observe <= 60 and 1 <= a.timeout <= 180, 'invalid time bounds')
    test = Acceptance(a)
    status, message = 1, 'incomplete'
    try:
        message = test.run(); status = 0
    except Exception as error:
        message = f'FAIL: {error}'
    finally:
        unchanged = {str(p): digest(p) == test.hashes[str(p)] for p in test.files}
        if not all(unchanged.values()): status, message = 1, 'FAIL: source/fixture hash changed'
        result = dict(exit_code=status, result=message, original_hashes_unchanged=unchanged)
        (test.root/'result.json').write_text(json.dumps(result, indent=2))
        test.record(result); test.events.close()
    return status


if __name__ == '__main__':
    sys.exit(main())
