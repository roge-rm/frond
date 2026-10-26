#!/usr/bin/env python3
"""Optional real-TX desktop protocol/save/export regression; requires private firmware state."""
import argparse
import hashlib
import os
from pathlib import Path
import socket
import signal
import shutil
import subprocess
import tempfile
import time
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--core', type=Path, default=Path('out/frond'))
parser.add_argument('--session', type=Path, required=True)
args = parser.parse_args()
core, source = args.core.resolve(), args.session.resolve()
original = hashlib.sha256(source.read_bytes()).digest()
# Scratch stays in build/ rather than occupying /tmp with session/ZIP copies.
Path('build').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='desktop-control-', dir='build') as directory:
    root = Path(directory).resolve()
    # AF_UNIX has a small pathname limit; keep only the socket in this private directory.
    with tempfile.TemporaryDirectory(prefix='frond-control-') as socket_dir:
        listener = socket.socket(socket.AF_UNIX)
        address = str(Path(socket_dir) / 'control')
        listener.bind(address)
        listener.listen(1)
        listener.settimeout(30)
        def launch(session, log):
            return subprocess.Popen((['heavy', 'emulator'] if shutil.which('heavy') else []) + [str(core), str(session), '--desktop-socket', address],
                                    stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True,
                                    env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'))
        with (root / 'core.log').open('wb') as log:
            process = launch(source, log)
            connection = None
            try:
                connection, _ = listener.accept()
                connection.settimeout(120)
                stream = connection.makefile('rb')
                assert stream.readline() == b'0\tevent\tready\t\n'
                serial = 0
                time.sleep(2)  # Allow the restored guest to re-establish its Palm API boundary.
                def request(command, argument='', ok=True):
                    global serial
                    serial += 1
                    line = f'{serial}\t{command}\t{str(argument).encode().hex()}\n'.encode()
                    # Exercise partial request buffering, including file names with spaces/newlines.
                    connection.sendall(line[:4])
                    time.sleep(.02)
                    connection.sendall(line[4:])
                    fields = stream.readline().rstrip(b'\n').split(b'\t')
                    assert len(fields) == 4 and fields[0] == str(serial).encode(), fields
                    assert fields[1] == (b'ok' if ok else b'error'), fields
                    return fields[2], bytes.fromhex(fields[3].decode()).decode()
                request('not-a-command', ok=False)
                request('save', 'relative.frond', ok=False)
                request('save', root / 'missing' / 'state.frond', ok=False)
                # Export retains the guest checkpoint, and reports failure without a partial file.
                request('export', root / 'missing' / 'database.zip', ok=False)
                time.sleep(2)
                request('install', Path('tests/fixtures/frond-pattern.pdb').resolve())
                time.sleep(3)
                archive = root / 'RAM databases.zip'
                request('export', archive)
                with zipfile.ZipFile(archive) as z:
                    assert any('frond-pattern' in name for name in z.namelist()), z.namelist()
                    assert z.testzip() is None
                saved = root / 'saved 音\nstate.frond'
                request('save', saved)
                assert saved.stat().st_size > 1000000
                saved_hash = hashlib.sha256(saved.read_bytes()).digest()
                request('save', saved / 'impossible', ok=False)
                assert hashlib.sha256(saved.read_bytes()).digest() == saved_hash
                request('rotate')
                request('audio-off'); request('audio-on')
                request('quit'); assert process.wait(timeout=15) == 0
                stream.close(); connection.close(); connection = None
                # Restore the saved image, dispatch reset, and shut down cleanly again.
                process = launch(saved, log)
                connection, _ = listener.accept()
                connection.settimeout(30)
                stream = connection.makefile('rb')
                assert stream.readline() == b'0\tevent\tready\t\n'
                request('reset'); request('quit')
                assert process.wait(timeout=15) == 0
                stream.close()
            finally:
                if connection: connection.close()
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: os.killpg(process.pid, signal.SIGKILL); process.wait()
                listener.close()
        assert hashlib.sha256(source.read_bytes()).digest() == original
print('PASS: desktop commands, errors, Unicode paths, checkpoint export, atomic save and restore; import unchanged')
