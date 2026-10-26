#!/usr/bin/env python3
"""Firmware-free runner tests with a fragmented desktop socket peer."""

import importlib.util
import json
import os
from pathlib import Path
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('frond_run', ROOT / 'tools/frond-run.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def fake_core():
    print(os.getpid(), flush=True)
    session = Path(sys.argv[1])
    config = json.loads(session.read_text())
    if config.get('early_exit'):
        return 7
    time.sleep(config.get('startup_delay', 0))
    with socket.socket(socket.AF_UNIX) as connection:
        connection.connect(sys.argv[3])

        def send(frame):
            data = frame.encode()
            connection.sendall(data[:3])
            time.sleep(.001)
            connection.sendall(data[3:])

        send('0\tevent\tready\t\n')
        stream = connection.makefile('rb')
        counts, replay = {}, []
        for line in stream:
            serial, command, encoded = line.decode().strip('\n').split('\t')
            argument = bytes.fromhex(encoded).decode()
            counts[command] = counts.get(command, 0) + 1
            if config.get('hang') == command:
                time.sleep(30)
            if config.get('disconnect') == command:
                return 9
            if config.get('malformed') == command:
                send('999\tok\tdone\tzz\n')
                continue
            ok, code, message = True, 'done', ''
            if config.get('unsupported') == command:
                ok, code = False, 'command'
            elif command == 'guest-ready':
                ok = counts[command] > config.get('ready_retries', 0)
                code = 'guest-ready' if ok else 'not-ready'
            elif command == 'install':
                code = 'needs-reset' if config.get('reset') else 'installed'
                if config.get('corrupt'):
                    ok, code, message = False, 'corrupt', 'The Palm database is corrupt.'
                assert Path(argument).parent == session.parent
            elif command == 'launch-app':
                if config.get('missing_app'):
                    ok, code = False, 'launch'
                elif counts[command] <= config.get('launch_retries', 0):
                    ok, code = False, 'not-ready'
                else:
                    code = 'launched'
            elif command == 'input-replay':
                replay = Path(argument).read_text().splitlines()
                replay_start = time.monotonic()
                code = 'loaded'
            elif command == 'replay-state':
                last = max((int(line.split()[0]) for line in replay), default=0)
                complete = (time.monotonic() - replay_start) * 1000 >= last
                code = 'finished' if complete else 'running'
                if complete and not config.get('missing_screenshot'):
                    for event in replay:
                        parts = event.split(maxsplit=2)
                        if parts[1] == 'snapshot':
                            Path(shlex.split(parts[2])[0]).write_bytes(b'fake PNG')
            elif command == 'export-db':
                destination, name = argument.split('\n')
                if config.get('export_error'):
                    ok, code, message = False, 'export', 'Database not found'
                else:
                    with zipfile.ZipFile(destination, 'w') as archive:
                        archive.writestr(name + '.pdb', b'synthetic log')
                    code = 'exported'
            elif command == 'save':
                Path(argument).write_text('{"saved":true}')
            send(f'{serial}\t{"ok" if ok else "error"}\t{code}\t{message.encode().hex()}\n')
            if command == 'quit':
                return 0
    return 0


class DeveloperRunTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='developer-run-')
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.core = self.root / 'fake core'
        self.core.write_text('#!' + sys.executable + '\nimport runpy\n'
                             + 'runpy.run_path(' + repr(str(Path(__file__).resolve())) + ', run_name="__main__")\n')
        self.core.chmod(0o700)
        self.session = self.root / 'input.session'
        self.session.write_text('{}')
        self.output = self.root / 'result 音 space'

    def run_case(self, config=None, extra=()):
        self.session.write_text(json.dumps(config or {}))
        args = runner.parser().parse_args([
            '--core', str(self.core), '--session', str(self.session), '--output', str(self.output),
            '--app', 'Test App', '--settle', '0', '--timeout', '4',
            '--command-timeout', '1', '--ready-timeout', '1', *extra])
        # The peer is synthetic and does not run an emulator or occupy a shared slot.
        with patch.object(runner.shutil, 'which', return_value=None):
            result = runner.run(args)
        if 'command' in result:
            # No queued wrapper or fake core may remain after success or failure.
            self.assertIsNotNone(result['core_exit_code'])
        if self.output.exists():
            self.assertEqual(json.loads((self.output / 'result.json').read_text()), result)
        self.assertEqual(json.loads(self.session.read_text()), config or {})
        return result

    def test_full_workflow_preserves_inputs_and_remaps_all_snapshots(self):
        prc, pdb = self.root / 'app.prc', self.root / 'data.pdb'
        prc.write_bytes(b'private app'); pdb.write_bytes(b'private data')
        protected = self.root / 'never overwrite.png'
        protected.write_bytes(b'original')
        replay = self.root / 'source.replay'
        source = f'0 pen-up\n100 snapshot "{protected}"\n'
        replay.write_text(source)
        result = self.run_case({'ready_retries': 1, 'launch_retries': 1, 'reset': True}, [
            '--install', str(prc), '--install', str(pdb), '--replay', str(replay),
            '--screenshot', '50', '--screenshot', '200', '--export-db', 'Test log', '--run-for', '.25'])
        self.assertEqual(result['status'], 'ok', result)
        self.assertGreaterEqual(result['elapsed_seconds'], .25)
        self.assertEqual(self.output.stat().st_mode & 0o777, 0o700)
        self.assertEqual(prc.read_bytes(), b'private app')
        self.assertEqual(pdb.read_bytes(), b'private data')
        self.assertEqual(protected.read_bytes(), b'original')
        self.assertEqual(replay.read_text(), source)
        self.assertEqual(len(result['artifacts']['screenshots']), 3)
        self.assertEqual(len(result['artifacts']['exports']), 1)
        self.assertTrue(result['artifacts']['session_saved'])
        self.assertEqual(json.loads((self.output / 'working.frond').read_text()), {'saved': True})
        commands = [step['command'] for step in result['steps']]
        self.assertEqual(commands.count('install'), 2)
        self.assertEqual(commands.count('reset'), 2)
        self.assertLess(commands.index('input-replay'), commands.index('export-db'))
        self.assertLess(commands.index('export-db'), commands.index('save'))

    def test_terminal_install_failure_is_not_retried(self):
        prc = self.root / 'bad.prc'; prc.write_bytes(b'bad')
        result = self.run_case({'corrupt': True}, ['--install', str(prc)])
        self.assertEqual(result['error']['code'], 'corrupt')
        self.assertEqual(result['stage'], 'install')
        self.assertEqual([s['command'] for s in result['steps']], ['guest-ready', 'install'])

    def test_readiness_timeout(self):
        result = self.run_case({'ready_retries': 999}, ['--ready-timeout', '.3'])
        self.assertEqual(result['error']['code'], 'timeout')
        self.assertEqual(result['stage'], 'ready')

    def test_hung_command_timeout(self):
        result = self.run_case({'hang': 'launch-app'}, ['--command-timeout', '.15'])
        self.assertEqual(result['error']['code'], 'timeout')
        self.assertEqual(result['stage'], 'launch')
        self.assertLess(result['elapsed_seconds'], 2)

    def test_overall_replay_timeout(self):
        result = self.run_case(extra=['--screenshot', '10000', '--timeout', '.5'])
        self.assertEqual(result['error']['code'], 'timeout')
        self.assertEqual(result['stage'], 'replay')
        self.assertFalse(result['artifacts']['session_saved'])

    def test_startup_timeout(self):
        result = self.run_case({'startup_delay': 30}, ['--startup-timeout', '.15'])
        self.assertEqual(result['error']['code'], 'timeout')
        self.assertEqual(result['stage'], 'startup')

    def test_old_core_is_reported_without_readiness_retries(self):
        result = self.run_case({'unsupported': 'guest-ready'})
        self.assertEqual(result['error']['code'], 'command')
        self.assertEqual(len(result['steps']), 1)

    def test_bad_response(self):
        result = self.run_case({'malformed': 'launch-app'})
        self.assertEqual(result['error']['code'], 'protocol')

    def test_disconnect(self):
        result = self.run_case({'disconnect': 'launch-app'})
        self.assertEqual(result['error']['code'], 'disconnected')

    def test_early_exit(self):
        result = self.run_case({'early_exit': True})
        self.assertEqual(result['error']['code'], 'process-exit')
        self.assertEqual(result['core_exit_code'], 7)

    def test_missing_snapshot(self):
        result = self.run_case({'missing_screenshot': True}, ['--screenshot', '0'])
        self.assertEqual(result['error']['code'], 'screenshot')

    def test_export_failure(self):
        result = self.run_case({'export_error': True}, ['--export-db', 'Missing log'])
        self.assertEqual(result['error']['code'], 'export')
        self.assertFalse(result['artifacts']['session_saved'])

    def test_unusual_output_path_is_quoted_for_native_replay(self):
        self.output = self.root / 'quote" back\\slash\ttab 音'
        result = self.run_case(extra=['--screenshot', '0'])
        self.assertEqual(result['status'], 'ok', result)
        self.assertTrue(Path(result['artifacts']['screenshots'][0]['path']).is_file())

    def test_sigterm_reports_interruption_and_reaps_only_owned_core(self):
        self.session.write_text('{"hang":"launch-app"}')
        process = subprocess.Popen([
            sys.executable, str(ROOT / 'tools/frond-run.py'), '--core', str(self.core),
            '--session', str(self.session), '--output', str(self.output), '--app', 'App', '--settle', '0'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=dict(os.environ, PATH=str(self.root)))
        try:
            until = time.monotonic() + 5
            log = self.output / 'core.log'
            while not log.exists() or not log.read_text().strip():
                self.assertLess(time.monotonic(), until)
                time.sleep(.01)
            core_pid = int(log.read_text().strip())
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 1, stderr)
            result = json.loads(stdout)
            self.assertEqual(result['error']['code'], 'interrupted')
            with self.assertRaises(ProcessLookupError):
                os.kill(core_pid, 0)
        finally:
            if process.poll() is None:
                process.kill(); process.wait()

    def test_invalid_replay_order_rejected_before_core_starts(self):
        replay = self.root / 'bad.replay'; replay.write_text('10 pen-up\n0 pen-up\n')
        result = self.run_case(extra=['--replay', str(replay)])
        self.assertEqual(result['error']['code'], 'replay')
        self.assertNotIn('command', result)

    def test_existing_output_is_untouched(self):
        self.output.mkdir(); marker = self.output / 'marker'; marker.write_bytes(b'keep')
        args = runner.parser().parse_args(['--core', str(self.core), '--session', str(self.session),
                                           '--app', 'App', '--output', str(self.output)])
        result = runner.run(args)
        self.assertEqual(result['error']['code'], 'output-exists')
        self.assertEqual(list(self.output.iterdir()), [marker])
        self.assertEqual(marker.read_bytes(), b'keep')


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[2] == '--desktop-socket':
        sys.exit(fake_core())
    unittest.main()
