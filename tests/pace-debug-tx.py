#!/usr/bin/env python3
"""Private TX smoke or SDK stepping acceptance (no bundled fixture).

Build the acceptance app with tools/build-palm-probe.py --probe debugger, then
supply --probe build/debugger-probe/FrondDebugProbe.prc. This mode installs and
launches through the normal developer workflow, requires an actual ARM GDB
(FROND_GDB or gdb-multiarch), and retains private mapping/context/transcripts.
The overall --timeout includes normal preparation and the heavy emulator slot.
Without --probe this remains only a PACE snapshot smoke check.
"""
import argparse
from contextlib import suppress
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('gdbstub_tests', Path(__file__).with_name('gdbstub.py'))
gdb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gdb)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def descriptor(path):
    """Find the initialized probe descriptor in a private v6 session RAM."""
    data = path.read_bytes()
    magic, version = struct.unpack_from('<II', data)
    if magic != 0x19800819 or version != 6:
        raise ValueError('expected a v6 session')
    # Magic, version, 32-byte device name, display, RAM size, then six section sizes.
    header = 72
    sizes = struct.unpack_from('<6I', data, 48)
    decoder = zlib.decompressobj()
    payload = decoder.decompress(data[header:], 512*1024*1024+1)
    if not decoder.eof or len(payload) != sum(sizes):
        raise ValueError('invalid session payload')
    ram = payload[sum(sizes[:3]):sum(sizes[:4])]
    found = []
    at = ram.find(b'FrDBSTEP-RAM-v01')
    while at >= 0:
        if at + 48 <= len(ram):
            values = struct.unpack_from('>8I', ram, at+16)
            if values[0] == 1 and values[1] <= values[3] < values[4] < values[1]+values[2]:
                found.append(dict(zip(('version', 'code', 'codeSize', 'entry', 'exception',
                                       'calls', 'stores', 'buffer'), values)))
        at = ram.find(b'FrDBSTEP-RAM-v01', at+1)
    if len(found) != 1:
        raise ValueError(f'expected one initialized probe descriptor, found {len(found)}')
    return found[0]


def acceptance(args, output, deadline):
    """Real SDK/firmware acceptance uses a real GDB, not the smoke RSP client."""
    result = {'version': 2, 'status': 'error', 'mode': 'sdk-firmware-gdb',
              'gdb': None, 'inputs': {}, 'gaps': []}
    process = client = None
    started = time.monotonic()
    paths = [args.session.resolve(), args.probe.resolve(), args.core.resolve(),
             Path(__file__).resolve(), ROOT / 'tests/palm/debugger-probe-gdb.py']
    paths += sorted((ROOT / 'tests/palm').glob('debugger-probe*'))
    paths += [ROOT / 'tools/build-palm-probe.py']
    paths = list(dict.fromkeys(paths))
    hashes = {}
    result['inputs'] = hashes

    def remaining():
        seconds = deadline - time.monotonic()
        if seconds <= 0:
            raise TimeoutError('overall deadline expired (includes preparation/heavy slot)')
        return seconds

    try:
        for path in paths:
            hashes[str(path)] = digest(path)
        sources = output / 'sources'
        sources.mkdir(mode=0o700)
        for path in paths[3:]:
            shutil.copyfile(path, sources / path.name)
        executable = os.environ.get('FROND_GDB') or shutil.which('gdb-multiarch')
        if not executable:
            result['gaps'].append('real ARM GDB unavailable; no firmware stepping acceptance')
            raise RuntimeError('set FROND_GDB or install gdb-multiarch')
        result['gdb'] = str(Path(executable).resolve())
        hashes[result['gdb']] = digest(result['gdb'])
        version = subprocess.run([executable, '-batch', '-nx', '-ex', 'show version'],
                                 capture_output=True, text=True, timeout=remaining())
        (output / 'gdb-version.txt').write_text(version.stdout+version.stderr)
        if version.returncode:
            raise RuntimeError('GDB failed to start')
        # The normal developer workflow installs/launches on copied state. Its
        # empty replay contains no key/pen or manufactured wake events.
        prep = output / 'prepare'
        command = [sys.executable, str(ROOT / 'tools/frond-run.py'),
                   '--session', str(args.session.resolve()), '--core', str(args.core.resolve()),
                   '--install', str(args.probe.resolve()), '--app', 'Frond debugger probe',
                   '--settle', '3', '--run-for', '1', '--headless',
                   '--timeout', str(remaining()), '--startup-timeout', str(remaining()),
                   '--output', str(prep)]
        with (output / 'prepare-runner.log').open('wb') as log:
            process = subprocess.Popen(command, stdout=log, stderr=log, start_new_session=True)
            process.wait(timeout=remaining())
        if process.returncode:
            raise RuntimeError('normal app install/launch failed; inspect prepare/result.json')
        prepared = prep / 'working.frond'
        witness = descriptor(prepared)
        result['descriptor'] = witness
        remaining()
        session = output / 'input.session'
        shutil.copyfile(prepared, session)
        session.chmod(0o600)
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        command = [str(args.core.resolve()), str(session), '--gdb', str(port), '--no-sound',
                   '--run-for', str(int(remaining())+1)]
        if shutil.which('heavy'):
            command = ['heavy', 'emulator'] + command
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
        with (output / 'core.log').open('wb') as log:
            # Unbuffer output so ROM-qualified handler addresses are available
            # while the CPU is stopped waiting for the first GDB connection.
            if shutil.which('stdbuf'):
                command = ['stdbuf', '-oL', '-eL'] + command
            process = subprocess.Popen(command, stdout=log, stderr=log, env=env, start_new_session=True)
            while True:
                remaining()
                text = (output / 'core.log').read_text(errors='replace')
                if process.poll() is not None:
                    raise RuntimeError('core exited before debugger connection')
                if 'GDB listening on 127.0.0.1:' + str(port) in text:
                    break
                time.sleep(min(.05, remaining()))
            text += (prep / 'core.log').read_text(errors='replace')
            entry = re.search(r'found PACE entry at (0x[0-9a-fA-F]+)', text)
            divu = re.search(r'found PACE callout for divu at (0x[0-9a-fA-F]+)', text)
            if not entry or not divu:
                raise RuntimeError('ROM qualification did not report PACE/DIVU addresses')
            config = dict(output=str(output), port=port, descriptor=witness,
                          probe=str(args.probe.resolve()), pace_entry=int(entry[1],16),
                          divu_callout=int(divu[1],16))
            config_path = output / 'gdb-config.json'
            config_path.write_text(json.dumps(config, indent=2)+'\n')
            env['FROND_TX_DEBUG_CONFIG'] = str(config_path)
            with (output / 'gdb.log').open('wb') as transcript:
                client = subprocess.Popen([executable, '-batch', '-nx', '-x',
                    str(ROOT / 'tests/palm/debugger-probe-gdb.py')], stdout=transcript,
                    stderr=transcript, env=env, start_new_session=True)
                client.wait(timeout=remaining())
            report = json.loads((output / 'gdb-result.json').read_text())
            if client.returncode or report['status'] != 'ok':
                raise RuntimeError(report.get('error', 'real GDB acceptance failed'))
            result.update(status='ok', runtime_map=str(output / 'runtime-map.json'),
                          context=str(output / 'pace-context.json'),
                          checks=report['checks'],
                          firmware_exception_recovery_verified=False)
    except (OSError, ValueError, RuntimeError, EOFError, AssertionError, struct.error, subprocess.TimeoutExpired) as exc:
        result.update(status='error', error=str(exc))
    finally:
        for child in (client, process):
            if child and child.poll() is None:
                with suppress(ProcessLookupError):
                    os.killpg(child.pid, signal.SIGTERM)
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    with suppress(ProcessLookupError):
                        os.killpg(child.pid, signal.SIGKILL)
                    child.wait()
        unchanged = {}
        for path, original in hashes.items():
            try:
                unchanged[path] = digest(path) == original
            except OSError:
                unchanged[path] = False
        result['inputs_unchanged'] = unchanged
        if not all(unchanged.values()):
            result.update(status='error', error='an input changed during validation')
        result['elapsed_seconds'] = round(time.monotonic()-started, 3)
        (output / 'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print('PASS private TX SDK/GDB stepping' if result['status'] == 'ok' else
          'FAIL; inspect private result.json')
    return 0 if result['status'] == 'ok' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path, help='new private directory outside the checkout')
    parser.add_argument('--core', type=Path, default=ROOT / 'out/frond')
    parser.add_argument('--timeout', type=float, default=90)
    parser.add_argument('--probe', type=Path, help='SDK FrondDebugProbe.prc: prepare copied session and require real GDB stepping acceptance')
    args = parser.parse_args()
    if not 1 <= args.timeout <= 3600:
        parser.error('--timeout must be 1..3600 seconds')
    output = args.output.resolve()
    if output == ROOT or ROOT in output.parents:
        parser.error('private fixture results must be outside the checkout')
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    deadline = time.monotonic() + args.timeout
    if args.probe:
        return acceptance(args, output, deadline)
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
