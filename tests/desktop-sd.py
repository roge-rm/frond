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
import subprocess
import tempfile
import time

from sd_results import ControlError, check_records, export_if_ready, read_log

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--core', type=Path, default=Path('out/frond'))
p.add_argument('--skip-reset', action='store_true', help='Check file exchange and persistence without the separate TX reset check')
p.add_argument('--exchange-driver', type=Path, default=Path('build/desktop/sd-exchange-driver'))
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
                if ok is not None and fields[1] != (b'ok' if ok else b'error'):
                    raise ControlError(fields[2].decode(), message)
                return (fields[1] == b'ok', message) if ok is None else message
            time.sleep(2)
            try:
                yield request
            except Exception as failure:
                # Keep the failing guest for diagnosis, without changing the test result.
                try:
                    if process.poll() is None and not isinstance(failure, (TimeoutError, OSError)):
                        connection.settimeout(20)
                        request('save', root / (label + '-failed.frond'))
                        replay = root / (label + '-failed.replay')
                        snapshot = str(root / (label + '-failed.png')).replace('\\', '\\\\').replace('"', '\\"')
                        replay.write_text(f'100 snapshot "{snapshot}"\n')
                        request('input-replay', replay)
                        time.sleep(.3)
                except Exception as error:
                    print('Failure capture:', error, flush=True)
                raise
            request('quit'); assert process.wait(timeout=20) == 0
        finally:
            if stream: stream.close()
            if connection: connection.close()
            listener.close()
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired: os.killpg(process.pid, signal.SIGKILL); process.wait()

def seed(path):
    # Exercise the same rootless, transactional copy backend as the Qt dialog.
    host = root / 'HOST.TXT'
    host.write_bytes(b'Hello from host\n')
    before = hashlib.sha256(path.read_bytes()).hexdigest()
    edited = root / 'host-edited.img'
    subprocess.run([str(a.exchange_driver.resolve()), 'put', str(path), str(host), '/HOST.TXT', str(edited)], check=True)
    assert hashlib.sha256(path.read_bytes()).hexdigest() == before
    return edited

def check_image(path, size):
    assert path.stat().st_size == size << 20
    output = root / (path.stem + '-GUEST.TXT')
    subprocess.run([str(a.exchange_driver.resolve()), 'get', str(path), '/GUEST.TXT', str(output)], check=True)
    assert output.read_bytes() == b'Written by Palm TX\n'

def probe_results(request, path, *, previous=(), size=32, prior=False, host=True):
    # Readiness is a point-in-time result. Export prepares again atomically with
    # checkpoint creation; only its explicit not-ready result can be retried.
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        ready, _ = request('guest-ready', ok=None)
        if ready and export_if_ready(request, path):
            lines = read_log(path)
            starts = [i for i, line in enumerate(lines) if line.startswith('begin ')]
            if starts and starts[-1] >= len(previous) and lines[-1].startswith('complete '):
                print('\n'.join(lines[starts[-1]:]), flush=True)
                return check_records(lines, size, previous=previous, prior=prior, host=host)
        time.sleep(1)
    raise AssertionError('No fresh, completed SD probe within 60 seconds; inspect the failure capture and rebuild the current probe if necessary')

with emulator(source, 'initial') as request:
    assert request('sd-state') == 'none'
    request('sd-create', '7', ok=False)
    request('sd-create', '32'); time.sleep(3)
    request('sd-create', '64', ok=False)
    request('sd-eject'); time.sleep(2)
    image = root / 'host.img'
    request('sd-export', image); image = seed(image)
    imported_hash = hashlib.sha256(image.read_bytes()).hexdigest()
    request('sd-insert', image); time.sleep(3)
    request('install', a.probe.resolve()); time.sleep(2)
    request('launch-app', 'Frond SD probe'); time.sleep(3)
    initial = probe_results(request, root / 'initial.zip')
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
    reinserted = probe_results(request, root / 'reinserted.zip', previous=initial, prior=True)
    reset = reinserted
    if not a.skip_reset:
        request('reset'); time.sleep(25)
        for attempt in range(20):
            ready, _ = request('launch-app', 'Frond SD probe', ok=None)
            if ready: break
            time.sleep(5)
        assert ready, 'Palm OS did not become ready after reset'
        time.sleep(3)
        reset = probe_results(request, root / 'reset.zip', previous=reinserted, prior=True)
    request('sd-eject'); time.sleep(3)
    request('sd-create', '128'); time.sleep(3)
    request('launch-app', 'Frond SD probe'); time.sleep(3)
    probe_results(request, root / 'large.zip', previous=reset, size=128, host=False)
    request('save', root / 'large.frond')
with emulator(root / 'large.frond', 'large-resumed') as request:
    assert request('sd-state') == 'inserted'
    request('sd-eject'); time.sleep(2)
    request('sd-export', root / 'large.img'); check_image(root / 'large.img', 128)
assert hashlib.sha256(source.read_bytes()).hexdigest() == original
print('PASS: TX VFS file exchange, embedded card restore, ejected restore, reinsert and 128 MiB persistence; imports unchanged' + (' (reset explicitly skipped)' if a.skip_reset else '; reset verified'))
