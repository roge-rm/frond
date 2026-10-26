#!/usr/bin/env python3
"""Optional TX VFS/SD persistence acceptance test using the locally built Palm probe."""
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
p.add_argument('--probe', type=Path, default=Path('build/sd-probe/FrondSDProbe.prc'))
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
root, core, source = a.output.resolve(), a.core.resolve(), a.session.resolve()
original = hashlib.sha256(source.read_bytes()).hexdigest()

@contextmanager
def emulator(session, label):
    with tempfile.TemporaryDirectory(prefix='frond-sd-') as directory, (root / (label + '.log')).open('wb') as log:
        listener = socket.socket(socket.AF_UNIX)
        address = str(Path(directory) / 'control')
        # A shared heavy emulator slot can be busy before the core starts.
        listener.bind(address); listener.listen(1); listener.settimeout(180)
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

def geometry(data):
    start = struct.unpack_from('<I', data, 454)[0]
    boot = start * 512
    fat = struct.unpack_from('<H', data, boot + 22)[0]
    root = (start + 1 + 2 * fat) * 512
    return start, fat, root, root + 32 * 512, data[boot + 13]

def seed(path):
    data = bytearray(path.read_bytes())
    start, fat, directory, payload, _ = geometry(data)
    at = directory + 32
    data[at:at+11] = b'HOST    TXT'; data[at+11] = 32
    struct.pack_into('<H', data, at+26, 2)
    text = b'Hello from host\n'; struct.pack_into('<I', data, at+28, len(text))
    data[payload:payload+len(text)] = text
    for i in range(2): struct.pack_into('<H', data, (start+1+i*fat)*512+4, 0xffff)
    path.write_bytes(data)

def check_image(path, size):
    data = path.read_bytes(); assert len(data) == size << 20
    _, _, directory, payload, cluster_size = geometry(data)
    for at in range(directory, directory + 32*512, 32):
        if data[at:at+11] != b'GUEST   TXT': continue
        cluster = struct.unpack_from('<H', data, at+26)[0]
        length = struct.unpack_from('<I', data, at+28)[0]
        begin = payload + (cluster-2)*cluster_size*512
        assert data[begin:begin+length] == b'Written by Palm TX\n'
        return
    raise AssertionError('Palm-created GUEST.TXT is absent')

def check_log(path, prior=False, host=True):
    with zipfile.ZipFile(path) as archive:
        raw = archive.read(next(n for n in archive.namelist() if 'FrondSDLog' in n))
    count = struct.unpack_from('>H', raw, 76)[0]
    offsets = [struct.unpack_from('>I', raw, 78+i*8)[0] for i in range(count)] + [len(raw)]
    lines = [raw[x:y].rstrip(b'\0').decode() for x,y in zip(offsets, offsets[1:])]
    # Evaluate only the most recent complete invocation.
    start = max(i for i,line in enumerate(lines) if line.startswith('enumerate '))
    lines = lines[start:]
    print('\n'.join(lines), flush=True)
    assert any(x.startswith('enumerate err=0000') for x in lines)
    assert 'guest-match err=0000 value=0' in lines
    if host: assert 'host-match err=0000 value=0' in lines
    if prior: assert 'prior-match err=0000 value=0' in lines

with emulator(source, 'initial') as request:
    assert request('sd-state') == 'none'
    request('sd-create', '7', ok=False)
    request('sd-create', '32'); time.sleep(3)
    request('sd-create', '64', ok=False)
    request('sd-eject'); time.sleep(2)
    image = root / 'host.img'
    request('sd-export', image); seed(image)
    imported_hash = hashlib.sha256(image.read_bytes()).hexdigest()
    request('sd-insert', image); time.sleep(3)
    request('install', a.probe.resolve()); time.sleep(2)
    request('launch-app', 'Frond SD probe'); time.sleep(3)
    request('export', root / 'initial.zip'); check_log(root / 'initial.zip')
    request('save', root / 'inserted.frond')
    assert hashlib.sha256(image.read_bytes()).hexdigest() == imported_hash
with emulator(root / 'inserted.frond', 'resumed') as request:
    assert request('sd-state') == 'inserted'
    request('sd-eject'); time.sleep(2)
    request('sd-export', root / 'resumed.img'); check_image(root / 'resumed.img', 32)
    request('save', root / 'ejected.frond')
with emulator(root / 'ejected.frond', 'ejected') as request:
    assert request('sd-state') == 'ejected'
    request('sd-reinsert'); time.sleep(3)
    request('launch-app', 'Frond SD probe'); time.sleep(3)
    request('export', root / 'reinserted.zip'); check_log(root / 'reinserted.zip', prior=True)
    request('reset'); time.sleep(25)
    for attempt in range(20):
        ready, _ = request('launch-app', 'Frond SD probe', ok=None)
        if ready: break
        time.sleep(5)
    assert ready, 'Palm OS did not become ready after reset'
    time.sleep(3)
    for attempt in range(10):
        exported, _ = request('export', root / 'reset.zip', ok=None)
        if exported: break
        time.sleep(3)
    assert exported, 'Palm export API did not become ready after reset/probe launch'
    check_log(root / 'reset.zip', prior=True)
    request('sd-eject'); time.sleep(3)
    request('sd-create', '128'); time.sleep(3)
    request('launch-app', 'Frond SD probe'); time.sleep(3)
    request('export', root / 'large.zip'); check_log(root / 'large.zip', host=False)
    request('save', root / 'large.frond')
with emulator(root / 'large.frond', 'large-resumed') as request:
    assert request('sd-state') == 'inserted'
    request('sd-eject'); time.sleep(2)
    request('sd-export', root / 'large.img'); check_image(root / 'large.img', 128)
assert hashlib.sha256(source.read_bytes()).hexdigest() == original
print('PASS: TX VFS read/write, embedded card restore, ejected restore, reinsert, reset and 128 MiB persistence; imports unchanged')
