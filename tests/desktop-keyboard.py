#!/usr/bin/env python3
"""Optional TX infrared keyboard driver and session acceptance test using the locally built Palm probe."""
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
p.add_argument('--probe', type=Path, default=Path('build/keyboard-probe/FrondKeyboardProbe.prc'))
p.add_argument('--driver', type=Path, required=True, help='Locally supplied Palm Universal Wireless Keyboard 1.13 PRC')
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
root, core, source = a.output.resolve(), a.core.resolve(), a.session.resolve()
original = hashlib.sha256(source.read_bytes()).hexdigest()

@contextmanager
def emulator(session, label):
    with tempfile.TemporaryDirectory(prefix='frond-keyboard-') as directory, (root / (label + '.log')).open('wb') as log:
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

def retry(request, command, argument):
    for _ in range(20):
        ok, message = request(command, argument, ok=None)
        if ok: return
        time.sleep(5)
    raise AssertionError(message)

def characters(path):
    with zipfile.ZipFile(path) as archive:
        raw = archive.read(next(n for n in archive.namelist() if 'FrondKeyboardLog' in n))
    count = struct.unpack_from('>H', raw, 76)[0]
    offsets = [struct.unpack_from('>I', raw, 78+i*8)[0] for i in range(count)] + [len(raw)]
    records = [raw[x:y].rstrip(b'\0').decode() for x,y in zip(offsets, offsets[1:])]
    print('\n'.join(records), flush=True)
    import re
    return [int(m[1],16) for line in records if (m:=re.search(r'event=4 chr=([0-9A-Fa-f]+)',line)) and int(m[1],16) in [ord('z'),ord('x'),ord('Z')]]

driver = a.driver.resolve()
driver_hash = hashlib.sha256(driver.read_bytes()).hexdigest()
with emulator(source, 'driver') as request:
    assert request('keyboard-state') == 'detached'
    retry(request,'install',driver)
    time.sleep(3) # Let the freshly installed driver commit before hardware reset.
    request('keyboard-attach','on');request('reset');time.sleep(25)
    retry(request,'launch-app','Keyboard');time.sleep(3)
    assert request('keyboard-state') == 'attached'
    enable=root/'enable.replay'
    enable.write_text('200 pen-down 150 50\n400 pen-up\n1500 snapshot '+str(root/'driver.png')+'\n')
    request('input-replay',enable);time.sleep(3)
    request('save',root/'driver-on.frond')
    retry(request,'install',a.probe.resolve())
    retry(request,'launch-app','Frond keyboard probe');time.sleep(2)
    replay=root/'type.replay'
    replay.write_text('500 keyboard-down Z\n650 keyboard-up Z\n1000 keyboard-down X\n1150 keyboard-up X\n1500 keyboard-down "Left Shift"\n1600 keyboard-down Z\n1750 keyboard-up Z\n1850 keyboard-up "Left Shift"\n2200 keyboard-down Z\n2350 keyboard-up Z\n')
    request('input-replay',replay);time.sleep(4)
    retry(request,'export',root/'typed.zip')
    assert characters(root/'typed.zip') == list(map(ord,'zxZz'))
    held=root/'held.replay';held.write_text('200 keyboard-down "Left Shift"\n')
    request('input-replay',held);time.sleep(1)
    request('save',root/'held.frond')
with emulator(root/'held.frond','resumed') as request:
    assert request('keyboard-state') == 'attached'
    replay=root/'resume.replay';replay.write_text('500 keyboard-down Z\n650 keyboard-up Z\n')
    request('input-replay',replay);time.sleep(2)
    retry(request,'export',root/'resumed.zip')
    assert characters(root/'resumed.zip') == list(map(ord,'zxZzz'))
    request('keyboard-attach','off');request('keyboard-key','1a d',ok=False)
    request('save',root/'detached.frond')
with emulator(root/'detached.frond','detached') as request:
    assert request('keyboard-state') == 'detached'
    request('keyboard-attach','on')
    request('input-replay',root/'resume.replay');time.sleep(2)
    retry(request,'export',root/'reattached.zip')
    assert characters(root/'reattached.zip') == list(map(ord,'zxZzzz'))
assert hashlib.sha256(source.read_bytes()).hexdigest() == original
assert hashlib.sha256(driver.read_bytes()).hexdigest() == driver_hash
print('PASS: Palm keyboard driver receives real UART input; case, modifier release, warm reset, attached/detached persistence, resume without stuck Shift; imports unchanged')
