#!/usr/bin/env python3
"""Run a private, staged peripheral qualification plan against owned Palm sessions."""

import argparse
from contextlib import ExitStack
import importlib.util
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

spec = importlib.util.spec_from_file_location('frond_run', Path(__file__).with_name('frond-run.py'))
native = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native)


def seconds(value):
    value = float(value)
    if not math.isfinite(value) or value <= 0:
        raise ValueError('timeouts must be finite and positive')
    return value


def run(plan_path, output, timeout=900):
    started = time.monotonic()
    deadline = started + seconds(timeout)
    report = {'version': 1, 'status': 'fail', 'stage': 'prepare', 'steps': [],
              'inputs': [], 'checks': [], 'error': None}
    processes, nodes, connections, allowed_exits = {}, {}, [], {}
    root = None
    try:
        plan_path = Path(plan_path).resolve()
        plan = json.loads(plan_path.read_text())
        if not isinstance(plan, dict):
            raise ValueError('plan must be an object')
        output = Path(output).resolve()
        if output.exists():
            raise ValueError('use a new output directory')
        if not plan.get('nodes') or not plan.get('steps'):
            raise ValueError('plan needs nodes and steps')
        if not isinstance(plan['nodes'], dict) or not isinstance(plan['steps'], list):
            raise ValueError('nodes must be an object and steps a list')
        if any(not isinstance(s, dict) for s in plan['steps']):
            raise ValueError('each step must be an object')
        names = [step['id'] for step in plan['steps']]
        if len(set(names)) != len(names) or any(not re.fullmatch(r'[A-Za-z0-9_-]+', n) for n in names):
            raise ValueError('step IDs must be unique simple names')
        if not any('check' in step for step in plan['steps']):
            raise ValueError('plan must contain an evidence checker')
        for name in plan['nodes']:
            if not re.fullmatch(r'[A-Za-z0-9_-]+', name):
                raise ValueError('node names must be simple names')
        inputs = [plan_path, Path(__file__).resolve(), Path(native.__file__).resolve(),
                  *[Path(p).resolve() for p in plan.get('inputs', [])]]
        for config in plan['nodes'].values():
            inputs += [Path(config['core']).resolve(), Path(config['session']).resolve()]
        inputs = list(dict.fromkeys(inputs))
        # Hash before starting any process; source files are never writable guest sessions.
        for path in inputs:
            report['inputs'].append({'path': str(path), 'sha256': native.sha256(path)})
        output.mkdir(mode=0o700, parents=True)
        root = output
        (root / 'plan.json').write_text(json.dumps(plan, indent=2) + '\n')
        shutil.copyfile(Path(__file__), root / 'qualification-runner.py')
        shutil.copyfile(Path(native.__file__), root / 'frond-run.py')

        def expand(value):
            return str(value).replace('$OUT', str(root))

        def budget(step):
            return min(deadline, time.monotonic() + seconds(step.get('timeout', 30)))

        def health():
            native.check_deadline(deadline)
            for name, process in processes.items():
                if process.poll() is not None and process.returncode not in allowed_exits.get(name, []):
                    raise RuntimeError(f'{name} exited unexpectedly ({process.returncode}); see its log')

        def wait(seconds_to_wait):
            until = min(deadline, time.monotonic() + seconds_to_wait)
            while time.monotonic() < until:
                health()
                native.pause(min(.1, until - time.monotonic()), deadline)

        with ExitStack() as stack:
            socket_root = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix='frond-qual-')))
            for name, config in plan['nodes'].items():
                report['stage'] = 'start-' + name
                core = Path(config['core']).resolve()
                origin = Path(config['session']).resolve()
                session = root / (name + '-working.frond')
                shutil.copyfile(origin, session)
                if native.sha256(session) != native.sha256(origin):
                    raise RuntimeError('session changed while copying')
                listener = stack.enter_context(socket.socket(socket.AF_UNIX))
                address = str(socket_root / name)
                listener.bind(address)
                listener.listen(1)
                log = stack.enter_context((root / (name + '.log')).open('wb'))
                args = ['heavy', 'emulator', str(core), str(session), '--no-sound',
                        '--desktop-socket', address, *[expand(a) for a in config.get('args', [])]]
                process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=log,
                                           stderr=subprocess.STDOUT, start_new_session=True,
                                           env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'))
                processes[name] = process
                startup = budget({'timeout': config.get('startup_timeout', 60)})
                while not select.select([listener], [], [], .1)[0]:
                    health()
                    native.check_deadline(startup)
                connection, _ = listener.accept()
                connections.append(connection)
                control = native.Control(connection, process, deadline,
                                         seconds(config.get('command_timeout', 10)), [])
                if control.line(startup) != ['0', 'event', 'ready', '']:
                    raise RuntimeError(name + ': missing desktop ready event')
                nodes[name] = control

            for step in plan['steps']:
                health()
                report['stage'] = step['id']
                item = {'id': step['id'], 'status': 'fail', 'responses': []}
                report['steps'].append(item)
                end = budget(step)
                kinds = [k for k in ('command', 'replay', 'wait_log', 'service', 'stop', 'check', 'wait') if k in step]
                if len(kinds) != 1:
                    raise ValueError(step['id'] + ': choose exactly one action')
                kind = kinds[0]
                if kind in ('command', 'replay'):
                    control = nodes[step['node']]
                    control.steps = item['responses']
                    if kind == 'replay':
                        replay = root / (step['id'] + '.replay')
                        replay.write_text(expand(step['replay']))
                        control.require('input-replay', replay, end)
                        while True:
                            state = control.require('replay-state', deadline=end)
                            if state['code'] == 'finished':
                                break
                            if state['code'] != 'running':
                                raise RuntimeError('unexpected replay state')
                            wait(.1)
                            native.check_deadline(end)
                    else:
                        command, argument = step['command'], expand(step.get('argument', ''))
                        while True:
                            reply = control.request(command, argument, end)
                            if reply['ok']:
                                break
                            if reply['code'] != 'not-ready' or not step.get('retry_ready', False):
                                raise RuntimeError(command + ': ' + reply['message'])
                            wait(.2)
                            native.check_deadline(end)
                        if 'expect_code' in step and reply['code'] != step['expect_code']:
                            raise RuntimeError('unexpected reply code: ' + reply['code'])
                        if 'expect_message' in step and reply['message'] != step['expect_message']:
                            raise RuntimeError('unexpected reply: ' + reply['message'])
                elif kind == 'wait_log':
                    path = Path(expand(step['wait_log']))
                    marker = step['contains'].encode()
                    if not marker:
                        raise ValueError('empty readiness marker')
                    while not path.exists() or marker not in path.read_bytes():
                        health()
                        if time.monotonic() >= end:
                            raise RuntimeError(f'Timed out waiting for {step["contains"]!r} in {path}')
                        wait(.1)
                elif kind == 'service':
                    name = step['id']
                    if name in processes:
                        raise ValueError('process name already owned: ' + name)
                    log = stack.enter_context((root / (name + '.log')).open('wb'))
                    args = [expand(a) for a in step['service']]
                    processes[name] = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=log,
                                                       stderr=subprocess.STDOUT, start_new_session=True)
                    allowed_exits[name] = step.get('allow_exit', [])
                elif kind == 'stop':
                    name = step['stop']
                    if name in nodes:
                        raise ValueError('stop applies to owned services; guests exit at the end')
                    native.stop(processes.pop(name))
                elif kind == 'check':
                    args = [expand(a) for a in step['check']]
                    log = stack.enter_context((root / (step['id'] + '.json')).open('wb'))
                    errors = stack.enter_context((root / (step['id'] + '.stderr')).open('wb'))
                    checker = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=log,
                                               stderr=errors, start_new_session=True)
                    processes['checker:' + step['id']] = checker
                    try:
                        code = checker.wait(timeout=max(.001, end - time.monotonic()))
                        if code:
                            raise RuntimeError(f'evidence checker exited {code}; see {step["id"]}.stderr')
                    finally:
                        native.stop(checker)
                        del processes['checker:' + step['id']]
                    verdict = json.loads((root / (step['id'] + '.json')).read_text())
                    if not isinstance(verdict, dict) or verdict.get('status') != 'pass':
                        raise RuntimeError('checker did not report status=pass')
                    report['checks'].append({'id': step['id'], 'verdict': verdict})
                else:
                    duration = seconds(step['wait'])
                    if time.monotonic() + duration > end:
                        raise ValueError('wait exceeds step timeout')
                    wait(duration)
                item['status'] = 'pass'
                (root / 'result.json').write_text(json.dumps(report, indent=2) + '\n')

            health()
            for name, control in nodes.items():
                control.steps = []
                control.require('quit')
                code = processes[name].wait(timeout=max(.001, min(10, deadline - time.monotonic())))
                if code:
                    raise RuntimeError(f'{name}: nonzero exit after quit: {code}')
                del processes[name]
            health()
            report.update(status='pass', stage='complete')
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, native.RunError,
            subprocess.TimeoutExpired, KeyboardInterrupt) as error:
        report['error'] = {'code': getattr(error, 'code', 'qualification'),
                           'message': str(error) or 'interrupted'}
    finally:
        if root and report['status'] != 'pass':
            report['failure_saves'] = {}
            for name, control in nodes.items():
                if processes.get(name) is None or processes[name].poll() is not None:
                    continue
                try:
                    # Save does not need a supported guest API boundary. A separate
                    # small cleanup budget preserves failed runs without retrying UI.
                    control.deadline = time.monotonic() + 5
                    control.command_timeout = 5
                    control.steps = []
                    reply = control.request('save', root / (name + '-failed.frond'))
                    report['failure_saves'][name] = reply
                except (OSError, native.RunError, ValueError, KeyboardInterrupt) as error:
                    report['failure_saves'][name] = {'ok': False, 'message': str(error)}
        for connection in connections:
            connection.close()
        for process in processes.values():
            native.stop(process)
        unchanged = {}
        for record in report['inputs']:
            try:
                unchanged[record['path']] = native.sha256(Path(record['path'])) == record['sha256']
            except OSError:
                unchanged[record['path']] = False
        report['inputs_unchanged'] = unchanged
        if not all(unchanged.values()):
            report.update(status='fail', error={'code': 'input-changed', 'message': 'An original input changed'})
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        if root:
            try:
                artifacts = {}
                for path in root.rglob('*'):
                    if path.is_file() and path.name != 'result.json':
                        artifacts[str(path.relative_to(root))] = native.sha256(path)
                report['artifacts_sha256'] = artifacts
                (root / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
            except OSError as error:
                report.update(status='fail', error={'code': 'report', 'message': str(error)})
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--timeout', type=seconds, default=900)
    args = parser.parse_args()
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    report = run(args.plan, args.output, args.timeout)
    print(json.dumps(report))
    return 0 if report['status'] == 'pass' else 1


if __name__ == '__main__':
    sys.exit(main())
