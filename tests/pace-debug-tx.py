#!/usr/bin/env python3
"""Optional private TX-session smoke test. No fixture is bundled or required."""
import argparse
from contextlib import suppress
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('gdbstub_tests', Path(__file__).with_name('gdbstub.py'))
gdb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gdb)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path, help='new private directory outside the checkout')
    parser.add_argument('--core', type=Path, default=ROOT / 'out/frond')
    parser.add_argument('--timeout', type=float, default=90)
    args = parser.parse_args()
    if not 1 <= args.timeout <= 3600:
        parser.error('--timeout must be 1..3600 seconds')
    output = args.output.resolve()
    if output == ROOT or ROOT in output.parents:
        parser.error('private fixture results must be outside the checkout')
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    deadline = time.monotonic() + args.timeout
    proc = rsp = None
    result = {'version': 1, 'status': 'error', 'context': None}
    try:
        # Run a private copy, with no save option. Never modify the input session.
        session = output / 'input.session'
        shutil.copyfile(args.session, session)
        session.chmod(0o600)
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        command = [str(args.core.resolve()), str(session), '--gdb', str(port), '--no-sound', '--run-for', str(int(args.timeout) + 1)]
        if shutil.which('heavy'):
            command = ['heavy', 'emulator'] + command
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
        with (output / 'core.log').open('wb') as log:
            proc = subprocess.Popen(command, stdout=log, stderr=log, env=env, start_new_session=True)
            while time.monotonic() < deadline:
                if proc.poll() is not None:
                    raise RuntimeError('core exited; inspect private core.log')
                if re.search(rb'GDB listening on 127\.0\.0\.1:' + str(port).encode(),
                             (output / 'core.log').read_bytes()):
                    break
                time.sleep(.05)
            else:
                raise TimeoutError('startup/slot deadline expired')
            rsp = gdb.Rsp(port)
            if rsp.request('?') != 'S05':
                raise RuntimeError('unexpected initial stop')
            while time.monotonic() < deadline:
                rsp.sock.settimeout(max(.01, min(3, deadline - time.monotonic())))
                status, text = rsp.monitor('pace')
                if status != 'OK':
                    raise RuntimeError('PACE monitor unsupported')
                context = json.loads(text)
                if context['available']:
                    status2, text2 = rsp.monitor('pace')
                    if status2 != 'OK' or json.loads(text2) != context:
                        raise RuntimeError('register snapshot changed while stopped')
                    if len(context['d']) != 8 or len(context['a']) != 8 or context['pc'] & 1:
                        raise RuntimeError('invalid PACE register snapshot')
                    result.update(status='ok', context=context)
                    break
                rsp.send('c')
                time.sleep(min(.1, max(0, deadline - time.monotonic())))
                rsp.sock.sendall(b'\x03')
                if rsp.packet() != 'S02':
                    raise RuntimeError('unexpected interrupt stop')
            else:
                raise TimeoutError('no optimized PACE boundary observed before deadline')
            if rsp.request('D') != 'OK':
                raise RuntimeError('detach failed')
    except (OSError, ValueError, RuntimeError, EOFError, AssertionError) as exc:
        result.update(status='error', error=str(exc))
    finally:
        if rsp:
            rsp.sock.close()
        if proc and proc.poll() is None:
            with suppress(ProcessLookupError):
                os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                with suppress(ProcessLookupError):
                    os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
        (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS private TX PACE inspection' if result['status'] == 'ok' else 'FAIL; inspect private result.json')
    return 0 if result['status'] == 'ok' else 1


if __name__ == '__main__':
    raise SystemExit(main())
