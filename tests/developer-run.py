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
    raw = sys.argv[1] == '--experimental-tx'
    session = Path(sys.argv[4] if raw else sys.argv[1])
    config = json.loads(session.open().readline() if raw else session.read_text())
    if config.get('mutate_core'):
        Path(config['mutate_core']).write_text('changed original executable')
    if config.get('early_exit'):
        return 7
    time.sleep(config.get('startup_delay', 0))
    with socket.socket(socket.AF_UNIX) as connection:
        connection.connect(sys.argv[sys.argv.index('--desktop-socket') + 1])

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
                if config.get('unsupported_after_launch') and counts.get('launch-app', 0):
                    ok = False
                if config.get('slow_ready_retry') and counts[command] > 1:
                    time.sleep(.5)
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
                if counts[command] <= config.get('export_retries', 0):
                    ok, code, message = False, 'not-ready', 'Palm OS is busy.'
                elif config.get('export_error'):
                    ok, code, message = False, 'export', 'Database not found'
                else:
                    with zipfile.ZipFile(destination, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
                        archive.writestr(name + '.pdb', b'synthetic log')
                    if config.get('bad_archive'):
                        raw = bytearray(Path(destination).read_bytes())
                        central = raw.index(b'PK\x01\x02')
                        if config['bad_archive'] == 'compression':
                            raw[8:10] = raw[central + 10:central + 12] = b'\x63\0'
                        elif config['bad_archive'] == 'encrypted':
                            raw[6] |= 1; raw[central + 8] |= 1
                        else:
                            data_start = 30 + int.from_bytes(raw[26:28], 'little') + int.from_bytes(raw[28:30], 'little')
                            raw[data_start] = 7  # Invalid DEFLATE block type.
                        Path(destination).write_bytes(raw)
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
            *([] if '--resume-app' in extra else ['--app', 'Test App']),
            '--settle', '0', '--timeout', '4',
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

    def test_resume_does_not_relaunch_saved_workload(self):
        result = self.run_case({'missing_app': True, 'ready_retries': 999}, ['--resume-app', '--screenshot', '0'])
        self.assertEqual(result['status'], 'ok', result)
        commands = [step['command'] for step in result['steps']]
        self.assertNotIn('launch-app', commands)
        self.assertNotIn('guest-ready', commands)
        self.assertIn('input-replay', commands)
        self.assertTrue(result['artifacts']['session_saved'])

    def test_cold_boot_uses_only_copied_flash_and_frozen_core(self):
        nor, nand = self.root / 'nor.bin', self.root / 'nand.bin'
        for path, size in [(nor, 16 << 20), (nand, 132 << 20)]:
            with path.open('wb') as stream:
                stream.write(b'{}\n'); stream.truncate(size)
        args = runner.parser().parse_args(['--core', str(self.core), '--nor', str(nor),
            '--nand', str(nand), '--resume-app', '--output', str(self.output),
            '--settle', '0', '--timeout', '5'])
        with patch.object(runner.shutil, 'which', return_value=None):
            result = runner.run(args)
        self.assertEqual(result['status'], 'ok', result)
        self.assertEqual(result['origin'], {'mode': 'cold-boot', 'saved_ram_cpu_loaded': False})
        self.assertEqual([r['path'] for r in result['inputs']], [str(self.core), str(nor), str(nand)])
        self.assertTrue(all(result['inputs_unchanged'].values()))
        self.assertEqual(result['command'][0], str(self.output / 'core'))
        self.assertNotIn(str(self.session), result['command'])
        self.assertIn(str(self.output / 'input-nand.bin'), result['command'])
        self.assertTrue((self.output / 'working.frond').is_file())
        self.assertEqual(nor.stat().st_size, 16 << 20)
        self.assertEqual(nand.stat().st_size, 132 << 20)

    def test_raw_flash_requires_pair_and_exact_sizes(self):
        for extra in [[], ['--nand', str(self.session)]]:
            args = runner.parser().parse_args(['--core', str(self.core), '--nor', str(self.session),
                '--resume-app', '--output', str(self.output), *extra])
            result = runner.run(args)
            self.assertEqual(result['status'], 'error', result)
            self.assertFalse(self.output.exists())

    def test_original_core_change_cannot_change_executed_snapshot_or_pass(self):
        original = runner.sha256(self.core)
        result = self.run_case({'mutate_core': str(self.core)})
        self.assertEqual(result['status'], 'error', result)
        self.assertEqual(result['error']['code'], 'input-changed')
        self.assertFalse(result['inputs_unchanged'][str(self.core)])
        self.assertEqual(runner.sha256(self.output / 'core'), original)

    def test_replay_does_not_require_a_second_guest_boundary(self):
        result = self.run_case({'unsupported_after_launch': True}, ['--screenshot', '0'])
        self.assertEqual(result['status'], 'ok', result)
        commands = [step['command'] for step in result['steps']]
        self.assertEqual(commands.count('guest-ready'), 1)
        self.assertLess(commands.index('launch-app'), commands.index('input-replay'))

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

    def test_readiness_timeout_during_retry_explains_idle_wait(self):
        result = self.run_case({'ready_retries': 999, 'slow_ready_retry': True}, ['--ready-timeout', '.3'])
        self.assertEqual(result['error']['code'], 'timeout')
        self.assertIn('waiting indefinitely', result['error']['message'])

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
        self.assertEqual(sum(s['command'] == 'export-db' for s in result['steps']), 1)

    def test_undecodable_export_is_structured_and_stops_before_save(self):
        for kind in ('compression', 'encrypted', 'deflate'):
            with self.subTest(kind=kind):
                self.output = self.root / kind
                result = self.run_case({'bad_archive': kind}, ['--export-db', 'Test log'])
                self.assertEqual(result['status'], 'error')
                self.assertEqual(result['stage'], 'export')
                self.assertEqual(result['error']['code'], 'export')
                self.assertFalse(result['artifacts']['session_saved'])
                self.assertNotIn('save', [s['command'] for s in result['steps']])

    def test_export_not_ready_is_bounded_and_retried(self):
        result = self.run_case({'export_retries': 2}, ['--export-db', 'Test log'])
        self.assertEqual(result['status'], 'ok', result)
        exports = [s for s in result['steps'] if s['command'] == 'export-db']
        self.assertEqual(len(exports), 3)

    def test_export_not_ready_timeout(self):
        result = self.run_case({'export_retries': 999}, ['--export-db', 'Test log', '--ready-timeout', '.3'])
        self.assertEqual(result['error']['code'], 'timeout')
        self.assertIn('waiting indefinitely', result['error']['message'])
        self.assertEqual(result['stage'], 'export')
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
    if '--desktop-socket' in sys.argv:
        sys.exit(fake_core())
    unittest.main()
