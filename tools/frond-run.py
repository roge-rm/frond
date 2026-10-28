#!/usr/bin/env python3
"""Run a Palm TX app in a private session copy and print one JSON result."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import zipfile
import zlib


class RunError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def positive(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError('must be finite and positive')
    return number


def nonnegative(value):
    number = float(value)
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError('must be finite and nonnegative')
    return number


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--core', type=Path, default=Path(__file__).resolve().parents[1] / 'out/frond')
    origin = p.add_mutually_exclusive_group(required=True)
    origin.add_argument('--session', type=Path, help='restore a complete saved session')
    origin.add_argument('--nor', type=Path, help='cold boot TX from this NOR and --nand, without saved RAM/CPU')
    p.add_argument('--nand', type=Path, help='TX NAND paired with --nor; copied before execution')
    p.add_argument('--output', type=Path, required=True, help='new private result directory')
    p.add_argument('--install', type=Path, action='append', default=[], help='PRC/PDB; repeat in install order')
    launch = p.add_mutually_exclusive_group(required=True)
    launch.add_argument('--app', help='exact Palm application database name')
    launch.add_argument('--resume-app', action='store_true',
                        help='continue the saved app without relaunching it')
    p.add_argument('--replay', type=Path)
    p.add_argument('--screenshot', type=int, action='append', default=[], metavar='MS',
                   help='capture at milliseconds after replay starts; repeat for several')
    p.add_argument('--export-db', action='append', default=[], metavar='NAME',
                   help='export one named database/log into a ZIP; repeat for several')
    p.add_argument('--run-for', type=nonnegative, default=0, metavar='SECONDS',
                   help='minimum replay interval, extended to include the last event/screenshot')
    p.add_argument('--settle', type=nonnegative, default=1, metavar='SECONDS',
                   help='wait after launch before checking readiness and starting replay (default 1)')
    p.add_argument('--timeout', type=positive, default=180, help='overall wall time, including slot wait (default 180)')
    p.add_argument('--startup-timeout', type=positive, default=60, help='slot wait and socket connection budget (default 60)')
    p.add_argument('--ready-timeout', type=positive, default=30, help='budget for each readiness/retry phase (default 30)')
    p.add_argument('--command-timeout', type=positive, default=30, help='maximum time per socket command (default 30)')
    p.add_argument('--headless', action='store_true', help='use SDL dummy video/audio drivers')
    p.add_argument('--sound', action='store_true', help='enable sound (disabled by default)')
    p.add_argument('--serial', metavar='off|pty|tcp:PORT', help='host serial endpoint for the core')
    p.add_argument('--usb', metavar='off|pty|tcp:PORT', help='host endpoint for the USB HotSync port')
    return p


def prepare_replay(source, root, screenshots):
    events, captures = [], []
    previous = 0
    if source:
        for number, line in enumerate(source.read_text().splitlines(), 1):
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            fields = line.split(maxsplit=2)
            if len(fields) < 2 or not re.fullmatch(r'[0-9]+', fields[0]):
                raise RunError('replay', f'Invalid replay time at line {number}')
            at = int(fields[0])
            if at < previous or at > 2**64 - 1:
                raise RunError('replay', f'Invalid replay order/time at line {number}')
            previous = at
            if fields[1] == 'snapshot':
                # Match std::quoted's path grammar before replacing the destination.
                if len(fields) != 3 or not re.fullmatch(r'(?:"(?:\\.|[^"\\])+"|[^"\s]\S*)(?:\s*#.*)?\s*', fields[2]):
                    raise RunError('replay', f'Invalid snapshot at line {number}')
                path = root / f'replay-snapshot-{len(captures) + 1:03d}.png'
                captures.append({'at_ms': at, 'path': str(path)})
                line = f'{at} snapshot {quote_replay_path(path)}'
            events.append((at, line))
    for index, at in enumerate(screenshots, 1):
        if at < 0 or at > 2**64 - 1:
            raise RunError('argument', 'Screenshot times must be unsigned 64-bit milliseconds')
        path = root / f'screenshot-{index:03d}.png'
        captures.append({'at_ms': at, 'path': str(path)})
        events.append((at, f'{at} snapshot {quote_replay_path(path)}'))
    events.sort(key=lambda event: event[0])
    target = root / 'input.replay'
    target.write_text(''.join(line + '\n' for _, line in events))
    return target, events[-1][0] if events else 0, captures


def quote_replay_path(path):
    return '"' + str(path).replace('\\', '\\\\').replace('"', '\\"') + '"'


class Control:
    def __init__(self, connection, process, deadline, command_timeout, steps):
        self.connection, self.process = connection, process
        self.deadline, self.command_timeout = deadline, command_timeout
        self.steps = steps
        self.buffer = b''
        self.serial = 0

    def line(self, deadline):
        while b'\n' not in self.buffer:
            check_deadline(deadline)
            if not select.select([self.connection], [], [], max(0, min(.1, deadline - time.monotonic())))[0]:
                if self.process.poll() is not None:
                    raise RunError('process-exit', f'Core exited with status {self.process.returncode}')
                continue
            data = self.connection.recv(4096)
            if not data:
                raise RunError('disconnected', 'Core closed the control socket; see core.log')
            self.buffer += data
            if len(self.buffer) > 65536:
                raise RunError('protocol', 'Oversized socket frame')
        line, self.buffer = self.buffer.split(b'\n', 1)
        try:
            return line.decode('ascii').split('\t')
        except UnicodeError as error:
            raise RunError('protocol', 'Invalid socket frame') from error

    def request(self, command, argument='', deadline=None):
        deadline = min(self.deadline, deadline or self.deadline, time.monotonic() + self.command_timeout)
        check_deadline(deadline)
        self.serial += 1
        frame = f'{self.serial}\t{command}\t{str(argument).encode().hex()}\n'.encode()
        if len(frame) > 16384:
            raise RunError('argument', 'Socket command exceeds the core frame limit')
        self.connection.settimeout(max(.001, deadline - time.monotonic()))
        try:
            self.connection.sendall(frame)
            while True:
                fields = self.line(deadline)
                if fields == ['0', 'event', 'close', '']:
                    raise RunError('closed', 'Palm window was closed during the run')
                if fields[:3] in (['0', 'event', 'serial'], ['0', 'event', 'usb']):
                    continue  # live serial/USB state, sent only with --serial/--usb
                if len(fields) != 4 or fields[0] != str(self.serial) or fields[1] not in ('ok', 'error'):
                    raise RunError('protocol', f'Unexpected response: {fields!r}')
                try:
                    message = bytes.fromhex(fields[3]).decode('utf-8')
                except (ValueError, UnicodeError) as error:
                    raise RunError('protocol', 'Invalid response text') from error
                result = {'command': command, 'argument': str(argument), 'ok': fields[1] == 'ok',
                          'code': fields[2], 'message': message}
                self.steps.append(result)
                return result
        except TimeoutError as error:
            raise RunError('timeout', f'Timed out waiting for {command}') from error

    def require(self, command, argument='', deadline=None):
        result = self.request(command, argument, deadline)
        if not result['ok']:
            raise RunError(result['code'], result['message'])
        return result

    def retry_ready(self, command, argument, seconds):
        deadline = min(self.deadline, time.monotonic() + seconds)
        saw_not_ready = False
        while True:
            try:
                result = self.request(command, argument, deadline)
                if result['ok']:
                    return result
                if result['code'] != 'not-ready':
                    raise RunError(result['code'], result['message'])
                saw_not_ready = True
                pause(.2, deadline)
            except RunError as error:
                if error.code != 'timeout' or not saw_not_ready or time.monotonic() < deadline:
                    raise
                raise RunError('timeout', f'{command}: Palm OS did not become ready. '
                               'An app waiting indefinitely for input may need normal user interaction.') from error


def check_deadline(deadline):
    if time.monotonic() >= deadline:
        raise RunError('timeout', 'Wall time budget exhausted')


def pause(seconds, deadline):
    check_deadline(deadline)
    time.sleep(min(seconds, max(0, deadline - time.monotonic())))
    check_deadline(deadline)


def stop(process):
    # Only the process group created for this run, including a queued heavy wrapper.
    if process is not None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=3)


def run(args):
    started = time.monotonic()
    deadline = started + args.timeout
    result = {'version': 1, 'status': 'error', 'stage': 'validate', 'steps': [],
              'artifacts': {}, 'inputs': [], 'error': None}
    root = process = connection = None
    try:
        if bool(args.nor) != bool(args.nand):
            raise RunError('argument', '--nor and --nand must be supplied together')
        for name in ([args.app] if args.app is not None else []) + args.export_db:
            if not name or len(name.encode()) > 31 or '\n' in name or '\0' in name:
                raise RunError('argument', 'Palm database names must be 1–31 UTF-8 bytes without newline/NUL')
        output = args.output.resolve()
        if any(c in str(output) for c in ('\n', '\r', '\0')):
            raise RunError('argument', 'Output paths cannot contain newline/NUL')
        origins = [args.session.resolve()] if args.session else [args.nor.resolve(), args.nand.resolve()]
        paths = [args.core.resolve(), *origins, *[p.resolve() for p in args.install]]
        install_start = 1 + len(origins)
        if args.replay:
            paths.append(args.replay.resolve())
        for path in paths:
            if not path.is_file():
                raise RunError('input', f'Input file does not exist: {path}')
        if not args.session and (origins[0].stat().st_size != 16 * 1024 * 1024 or
                                 origins[1].stat().st_size != 132 * 1024 * 1024):
            raise RunError('input', 'Cold boot requires 16 MiB TX NOR and 132 MiB raw NAND')
        if not os.access(paths[0], os.X_OK):
            raise RunError('input', 'Core is not executable')
        for path in args.install:
            if path.suffix.lower() not in ('.prc', '.pdb'):
                raise RunError('input', f'Expected PRC/PDB input: {path}')
        if output.exists():
            raise RunError('output-exists', f'Use a new result directory: {output}')
        output.mkdir(mode=0o700, parents=True)
        root = output
        result['artifacts'] = {'directory': str(root), 'log': str(root / 'core.log'),
                               'session': str(root / 'working.frond'), 'session_saved': False}
        result['stage'] = 'prepare'
        installs = []
        replay_source = None
        for index, path in enumerate(paths):
            check_deadline(deadline)
            record = {'path': str(path), 'sha256': sha256(path)}
            if index == 0:
                copy = root / 'core'
            elif index < install_start:
                copy = root / ('working.frond' if args.session else 'input-nor.bin' if index == 1 else 'input-nand.bin')
            elif install_start <= index < install_start + len(args.install):
                copy = root / f'install-{index - install_start + 1:03d}{path.suffix.lower()}'
                installs.append(copy)
            elif args.replay and index == len(paths) - 1:
                copy = replay_source = root / 'source.replay'
            else:
                copy = None
            if copy:
                shutil.copyfile(path, copy)
                copy.chmod(0o700 if index == 0 else 0o600)
                if sha256(copy) != record['sha256']:
                    raise RunError('input-changed', f'Input changed while copying: {path}')
                record['copy'] = str(copy)
            result['inputs'].append(record)
        replay, last_ms, captures = prepare_replay(replay_source, root, args.screenshot)
        result['artifacts']['screenshots'] = captures
        result['artifacts']['exports'] = []
        result['origin'] = {'mode': 'session-restore' if args.session else 'cold-boot',
                            'saved_ram_cpu_loaded': bool(args.session)}
        with tempfile.TemporaryDirectory(prefix='frond-run-') as socket_dir, (root / 'core.log').open('wb') as log:
            with socket.socket(socket.AF_UNIX) as listener:
                address = str(Path(socket_dir) / 'control')
                listener.bind(address)
                listener.listen(1)
                boot = [str(root / 'working.frond')] if args.session else [
                    '--experimental-tx', str(root / 'input-nor.bin'), '--nand', str(root / 'input-nand.bin')]
                command = (['heavy', 'emulator'] if shutil.which('heavy') else []) + [
                    str(root / 'core'), *boot, '--desktop-socket', address]
                if not args.sound:
                    command.append('--no-sound')
                if args.serial:
                    command += ['--serial', args.serial]
                if args.usb:
                    command += ['--usb', args.usb]
                result['command'] = command
                env = dict(os.environ)
                if args.headless:
                    env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
                result['stage'] = 'startup'
                startup = min(deadline, time.monotonic() + args.startup_timeout)
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                           stdin=subprocess.DEVNULL, env=env, start_new_session=True)
                while not select.select([listener], [], [], .1)[0]:
                    check_deadline(startup)
                    if process.poll() is not None:
                        raise RunError('process-exit', f'Core exited with status {process.returncode}; see core.log')
                connection, _ = listener.accept()
                control = Control(connection, process, deadline, args.command_timeout, result['steps'])
                if control.line(startup) != ['0', 'event', 'ready', '']:
                    raise RunError('protocol', 'Missing desktop ready event')
                result['stage'] = 'ready'
                if installs or args.app is not None:
                    control.retry_ready('guest-ready', '', args.ready_timeout)
                for path in installs:
                    result['stage'] = 'install'
                    installed = control.retry_ready('install', path, args.ready_timeout)
                    if installed['code'] == 'needs-reset':
                        control.require('reset')
                        control.retry_ready('guest-ready', '', args.ready_timeout)
                result['stage'] = 'launch'
                if args.app is not None:
                    control.retry_ready('launch-app', args.app, args.ready_timeout)
                pause(args.settle, deadline)
                result['stage'] = 'replay'
                control.require('input-replay', replay)
                until = time.monotonic() + max(args.run_for, last_ms / 1000)
                while True:
                    state = control.require('replay-state')
                    if state['code'] not in ('running', 'finished'):
                        raise RunError('protocol', 'Unknown replay state')
                    if state['code'] == 'finished' and time.monotonic() >= until:
                        break
                    pause(.1, deadline)
                # Cores list each snapshot as ok, lcd-off, no-frame or failed. A black frame
                # saved with the LCD off is still a screenshot. Missing ones fail the run
                # after the session is saved.
                snapshots = control.request('replay-snapshots')
                states = snapshots['message'].split(',') if snapshots['ok'] and snapshots['code'] == 'snapshots' else []
                missing = []
                for index, capture in enumerate(sorted(captures, key=lambda c: c['at_ms'])):
                    if index < len(states) and states[index]:
                        capture['state'] = states[index]
                    path = Path(capture['path'])
                    capture['written'] = path.is_file() and path.stat().st_size > 0
                    if not capture['written']:
                        missing.append(str(path))
                result['stage'] = 'export'
                for index, name in enumerate(args.export_db, 1):
                    archive = root / f'database-{index:03d}.zip'
                    control.retry_ready('export-db', str(archive) + '\n' + name, args.ready_timeout)
                    try:
                        with zipfile.ZipFile(archive) as z:
                            entries = z.infolist()
                            if (len(entries) != 1 or entries[0].flag_bits & 1 or
                                    entries[0].compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED) or
                                    z.testzip() is not None):
                                raise RunError('export', f'Invalid named database archive: {archive}')
                    except (RuntimeError, NotImplementedError, EOFError, zipfile.BadZipFile,
                            zlib.error) as error:
                        raise RunError('export', f'Unreadable named database archive: {archive}: {error}') from error
                    result['artifacts']['exports'].append({'database': name, 'path': str(archive)})
                result['stage'] = 'save'
                control.require('save', root / 'working.frond')
                result['artifacts']['session_saved'] = True
                result['stage'] = 'quit'
                control.require('quit')
                try:
                    code = process.wait(timeout=max(.001, min(args.command_timeout, deadline - time.monotonic())))
                except subprocess.TimeoutExpired as error:
                    raise RunError('timeout', 'Core did not exit after quit') from error
                if code:
                    raise RunError('process-exit', f'Core exited with status {code}')
                if missing:
                    raise RunError('screenshot', 'Screenshot was not written: ' + ', '.join(missing))
        result.update(status='ok', stage='complete')
    except KeyboardInterrupt:
        result['error'] = {'code': 'interrupted', 'message': 'Run interrupted'}
    except (RunError, OSError, ValueError, zipfile.BadZipFile) as error:
        result['error'] = {'code': getattr(error, 'code', 'io'), 'message': str(error)}
    finally:
        if connection:
            connection.close()
        stop(process)
        unchanged = {}
        for record in result['inputs']:
            try:
                unchanged[record['path']] = sha256(Path(record['path'])) == record['sha256']
            except OSError:
                unchanged[record['path']] = False
        result['inputs_unchanged'] = unchanged
        if not all(unchanged.values()):
            result['status'] = 'error'
            if result['error'] is None:
                result['stage'] = 'validate-inputs'
                result['error'] = {'code': 'input-changed', 'message': 'An original input changed during execution'}
        result['elapsed_seconds'] = round(time.monotonic() - started, 3)
        result['core_exit_code'] = process.returncode if process else None
        if root:
            try:
                (root / 'result.json').write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n')
            except OSError as error:
                result['status'] = 'error'
                result['report_error'] = str(error)
                if result['error'] is None:
                    result['error'] = {'code': 'report', 'message': 'Could not write result.json'}
    return result


def main():
    def interrupted(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    result = run(parser().parse_args())
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result['status'] == 'ok' else 1


if __name__ == '__main__':
    sys.exit(main())
