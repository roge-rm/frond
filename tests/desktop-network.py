#!/usr/bin/env python3
"""Optional TX simulated NetLib persistence acceptance test using the locally built Palm probe."""
import argparse
from contextlib import contextmanager
import hashlib
import os
from pathlib import Path
import signal
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import zipfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--core', type=Path, default=Path('out/frond'))
p.add_argument('--session', type=Path, required=True)
p.add_argument('--probe', type=Path, default=Path('build/network-probe/FrondNetProbe.prc'))
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
root, core, source = a.output.resolve(), a.core.resolve(), a.session.resolve()
original = hashlib.sha256(source.read_bytes()).hexdigest()

@contextmanager
def emulator(session, label):
    with tempfile.TemporaryDirectory(prefix='frond-net-') as directory, (root / (label + '.log')).open('wb') as log:
        listener = socket.socket(socket.AF_UNIX)
        address = str(Path(directory) / 'control')
        listener.bind(address); listener.listen(1); listener.settimeout(30)
        process = subprocess.Popen((['heavy', 'emulator'] if shutil.which('heavy') else []) + [str(core), str(session), '--no-sound', '--desktop-socket', address],
            stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'))
        connection = stream = None
        try:
            connection, _ = listener.accept(); connection.settimeout(120); stream = connection.makefile('rb')
            assert stream.readline() == b'0\tevent\tready\t\n'
            serial = 0
            def request(command, argument='', ok=True):
                nonlocal serial
                serial += 1
                connection.sendall(f'{serial}\t{command}\t{str(argument).encode().hex()}\n'.encode())
                fields = stream.readline().rstrip(b'\n').split(b'\t')
                assert len(fields) == 4 and fields[0] == str(serial).encode(), fields
                message = bytes.fromhex(fields[3].decode()).decode()
                print(label, command, fields[1].decode(), message, flush=True)
                if ok is not None: assert fields[1] == (b'ok' if ok else b'error'), fields
                return (fields[1] == b'ok', message) if ok is None else message
            time.sleep(2)
            yield request
            request('quit'); assert process.wait(timeout=20) == 0
        finally:
            if stream: stream.close()
            if connection: connection.close()
            listener.close()
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired: os.killpg(process.pid, signal.SIGKILL); process.wait()

def lines(path):
    with zipfile.ZipFile(path) as archive:
        raw = archive.read(next(n for n in archive.namelist() if 'FrondNetLog' in n))
    count = struct.unpack_from('>H', raw, 76)[0]
    offsets = [struct.unpack_from('>I', raw, 78+i*8)[0] for i in range(count)] + [len(raw)]
    records = [raw[x:y].rstrip(b'\0').decode() for x,y in zip(offsets, offsets[1:])]
    print('\n'.join(records), flush=True)
    return records

def retry(request, command, argument):
    for _ in range(12):
        ok, message = request(command, argument, ok=None)
        if ok: return
        time.sleep(3)
    raise AssertionError(message)

with emulator(source, 'connected') as request:
    assert request('network-state') == 'disabled'
    request('network-mode', 'unknown', ok=False)
    request('network-mode', 'connected')
    retry(request, 'install', a.probe.resolve())
    retry(request, 'launch-app', 'Frond net probe'); time.sleep(5)
    retry(request, 'export', root / 'pending.zip')
    records = lines(root / 'pending.zip')
    assert 'find err=0000 value=32766' in records
    assert 'dns err=0000 value=167772162' in records
    assert 'dns-missing err=123E value=0' in records
    assert 'connect err=0000 value=0' in records
    assert 'send err=0000 value=16' in records
    assert not any(x.startswith('receive ') for x in records)
    request('save', root / 'pending.frond')
with emulator(root / 'pending.frond', 'resumed') as request:
    assert request('network-state') == 'connected'
    replay = root / 'receive.replay'
    replay.write_text('500 pen-down 100 140\n650 pen-up\n1000 snapshot ' + str(root / 'received.png') + '\n')
    request('input-replay', replay); time.sleep(5)
    retry(request, 'export', root / 'received.zip')
    records = lines(root / 'received.zip')
    assert 'select err=0000 value=1' in records
    assert 'receive err=0000 value=16' in records
    assert 'echo-match err=0000 value=0' in records
    assert any(x.startswith('empty err=122F ') for x in records)
for mode, expected in [('timeout', 'connect err=1212 '), ('disconnected', 'open err=120C ')]:
    with emulator(source, mode) as request:
        request('network-mode', mode)
        retry(request, 'install', a.probe.resolve())
        retry(request, 'launch-app', 'Frond net probe'); time.sleep(5)
        retry(request, 'export', root / (mode + '.zip'))
        assert any(x.startswith(expected) for x in lines(root / (mode + '.zip')))
assert hashlib.sha256(source.read_bytes()).hexdigest() == original
print('PASS: Palm NetLib DNS/echo/select, pending receive across session restore, timeout/disconnection, import unchanged')
