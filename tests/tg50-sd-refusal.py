#!/usr/bin/env python3
"""A TG50 has a Memory Stick and no SD slot: every sd-* control command answers unsupported, with or without arguments.
Needs a private TG50 session: --session or FROND_TEST_TG50_SESSION; skipped without one."""
import argparse
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--core', type=Path, default=Path('out/frond'))
parser.add_argument('--session', type=Path, default=os.environ.get('FROND_TEST_TG50_SESSION'))
args = parser.parse_args()
if not args.session or not Path(args.session).is_file():
    print('test-tg50-sd-refusal skipped: no TG50 session (--session or FROND_TEST_TG50_SESSION)')
    sys.exit(0)

CASES = [('sd-create', ''), ('sd-create', '/x.img'), ('sd-insert', ''), ('sd-insert', 'relative.img'),
         ('sd-insert', '/x.img'), ('sd-eject', ''), ('sd-eject', 'x'), ('sd-take-out', 'x'), ('sd-put-in', ''),
         ('sd-reinsert', ''), ('sd-export', ''), ('sd-export', '/y.img'), ('sd-bogus', '')]
with tempfile.TemporaryDirectory(prefix='tg50-sd-refusal-') as directory:
    root = Path(directory)
    session = root / 'session.frond'
    shutil.copy(args.session, session)
    address = str(root / 'control')
    listener = socket.socket(socket.AF_UNIX)
    listener.bind(address)
    listener.listen(1)
    listener.settimeout(60)
    process = subprocess.Popen([str(args.core.resolve()), str(session), '--desktop-socket', address],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
                               env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'))
    failures = []
    try:
        connection, _ = listener.accept()
        connection.settimeout(60)
        stream = connection.makefile('rb')
        assert stream.readline() == b'0\tevent\tready\t\n'
        time.sleep(2)
        def ask(serial, command, argument):
            connection.sendall(f'{serial}\t{command}\t{argument.encode().hex()}\n'.encode())
            fields = stream.readline().rstrip(b'\n').split(b'\t')
            return fields[1], fields[2], bytes.fromhex(fields[3].decode()).decode()
        for serial, (command, argument) in enumerate(CASES, 1):
            got = ask(serial, command, argument)
            if got != (b'error', b'unsupported', 'This device has no SD slot.'):
                failures.append(f'{command} {argument!r}: {got}')
        got = ask(len(CASES) + 1, 'sd-state', '')
        if got != (b'ok', b'card', 'none'):
            failures.append(f'sd-state: {got}')
    finally:
        process.kill()
        process.wait()
    for line in failures:
        print('FAIL', line)
    if failures:
        sys.exit(1)
    print('test-tg50-sd-refusal ok')
