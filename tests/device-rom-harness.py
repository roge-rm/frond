#!/usr/bin/env python3
"""Firmware-free acceptance of ROM harness failures, evidence safety and guest cleanup."""
import argparse
import contextlib
import hashlib
import json
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot
from guest_process import run_guest

spec = importlib.util.spec_from_file_location('device_rom', ROOT / 'tests/device-rom.py')
rom = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rom)


class HarnessTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='device-rom-harness-', dir=ROOT / 'build')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.heavy = self.script('heavy', '''
            import subprocess, sys
            sys.exit(subprocess.call(sys.argv[2:]))
        ''')
        self.env = patch.dict(os.environ, PATH=str(self.root) + os.pathsep + os.environ['PATH'])
        self.env.start()
        self.addCleanup(self.env.stop)
        self.nor = self.root / 'nor'
        self.nor.write_bytes(b'firmware fixture, no boot claim')

    def script(self, name, code):
        path = self.root / name
        path.write_text('#!' + sys.executable + '\n' + textwrap.dedent(code))
        path.chmod(0o700)
        return path

    def hung_core(self):
        lock = self.root / 'slot'
        core = self.script('core', f'''
            import fcntl, os, signal, sys, time
            slot = open({str(lock)!r}, 'w')
            fcntl.flock(slot, fcntl.LOCK_EX)
            child = os.fork()
            if child == 0:
                signal.signal(signal.SIGTERM, signal.SIG_IGN)
                while True:
                    signal.pause()
            print('ready', flush=True)
            for line in sys.stdin:
                print('reply', flush=True)
                if line.strip() == 'quit':
                    time.sleep(600)
            time.sleep(600)
        ''')
        return core, lock

    def assert_slot_released(self, path):
        # The descendant inherited the flock. Acquiring it proves the whole guest group released it.
        result = subprocess.run([sys.executable, '-c',
            'import fcntl,sys; f=open(sys.argv[1],"a"); fcntl.flock(f,fcntl.LOCK_EX)', str(path)], timeout=3)
        self.assertEqual(result.returncode, 0)

    def args(self, **kw):
        values = dict(device='palm-e2', nor=self.nor, nand=None, sha256=None,
                      launcher=False, evidence=self.root / 'evidence', boot_timeout=0.5)
        values.update(kw)
        return argparse.Namespace(**values)

    def run_suite(self, args, core):
        with patch.object(rom, 'CORE', core):
            return unittest.TextTestRunner(stream=io.StringIO()).run(rom.suite(args))

    def test_timeout_kills_descendant_and_releases_slot(self):
        core, lock = self.hung_core()
        with self.assertRaises(subprocess.TimeoutExpired) as error:
            run_guest([core], timeout=0.5, capture_output=True, text=True)
        self.assertIn('ready', error.exception.output)
        self.assert_slot_released(lock)

    def test_console_close_kills_hung_guest_and_releases_slot(self):
        core, lock = self.hung_core()
        with patch.object(palm_boot, 'CORE', core):
            guest = palm_boot.Core([], self.root / 'console.log')
        try:
            guest.send('ping', 'reply', timeout=3)
        finally:
            guest.close(timeout=0.1)
        self.assertIsNotNone(guest.process.poll())
        self.assertFalse(guest.reader.is_alive())
        self.assert_slot_released(lock)

    def test_generic_boot_failure_is_a_failure_with_log(self):
        core = self.script('core', "import sys\nprint('fixture failure', file=sys.stderr)\nsys.exit(5)\n")
        result = self.run_suite(self.args(), core)
        self.assertFalse(result.wasSuccessful())
        self.assertIn('fixture failure', (self.root / 'evidence/boot.log').read_text())

    def test_generic_timeout_fails_and_releases_slot(self):
        core, lock = self.hung_core()
        result = self.run_suite(self.args(), core)
        self.assertFalse(result.wasSuccessful())
        self.assertTrue(any('TimeoutExpired' in trace for _, trace in result.errors))
        self.assert_slot_released(lock)

    def test_default_zire_hash_is_checked_before_delegation(self):
        with patch.object(rom, 'ZIRE_NOR', self.nor):
            with self.assertRaisesRegex(ValueError, 'requested SHA-256'):
                rom.suite(self.args(device='palm-zire-72', nor=None, sha256='wrong'))

    def test_explicit_hash_failure_starts_no_guest(self):
        marker = self.root / 'started'
        core = self.script('core', f'from pathlib import Path\nPath({str(marker)!r}).touch()\n')
        result = self.run_suite(self.args(sha256='wrong'), core)
        self.assertFalse(result.wasSuccessful())
        self.assertFalse(marker.exists())

    def test_explicit_fault_qualification_rejects_missing_inputs_before_guest(self):
        marker = self.root / 'started'
        core = self.script('core-marker', f'from pathlib import Path\nPath({str(marker)!r}).touch()\n')
        expectation = self.root / 'expected.json'
        data = {'version': 1, 'device': 'sony-clie-tg50', 'nor_sha256': rom.sha(self.nor),
                'phases': {'boot': [], 'setup': [], 'reload': [], 'rollback': [], 'reset-soft': [], 'reset-hard': []}}
        expectation.write_text(json.dumps(data))
        probe = self.root / 'build/test-device-session'
        probe.parent.mkdir()
        for missing in ('core', 'original', 'probe', 'phase'):
            if missing == 'phase':
                probe.touch()
                bad = dict(data, phases={'boot': [], 'setup': [], 'rollback': []})
                expectation.write_text(json.dumps(bad))
            argv = ['device-rom', '--device', 'sony-clie-tg50', '--launcher', '--resets', '--nor',
                    str(self.root / 'missing-nor' if missing == 'original' else self.nor),
                    '--sha256', data['nor_sha256'], '--expected-faults', str(expectation)]
            with self.subTest(missing=missing), patch.object(rom, 'ROOT', self.root), \
                 patch.object(rom, 'CORE', self.root / 'missing-core' if missing == 'core' else core), \
                 patch.object(sys, 'argv', argv), contextlib.redirect_stderr(io.StringIO()), \
                 self.assertRaises(SystemExit) as error:
                rom.main()
            self.assertEqual(error.exception.code, 2)
            self.assertFalse(marker.exists())

    def test_explicit_fault_qualification_cannot_skip_unsupported_rollback(self):
        probe = self.root / 'build/test-device-session'
        probe.parent.mkdir()
        probe.touch()
        args = self.args(launcher=True)
        args.fault_contract = {}
        with patch.object(rom, 'ROOT', self.root):
            tests = [test for test in rom.suite(args) if test._testMethodName == 'test_running_session_rollback']
            with patch.object(rom, 'run_guest', return_value=subprocess.CompletedProcess([], 77, '', 'unsupported')):
                result = unittest.TextTestRunner(stream=io.StringIO()).run(unittest.TestSuite(tests))
        self.assertFalse(result.wasSuccessful())
        self.assertEqual(len(result.failures), 1)
        self.assertFalse(result.skipped)

    def test_contract_archives_the_parsed_byte_snapshot(self):
        core = self.script('unused-core', 'raise RuntimeError("must not execute")\n')
        probe = self.root / 'build/test-device-session'
        probe.parent.mkdir()
        probe.touch()
        expectation = self.root / 'expect.json'
        data = {'version': 1, 'device': 'sony-clie-tg50', 'nor_sha256': rom.sha(self.nor),
                'phases': {name: [] for name in ('boot', 'setup', 'reload', 'rollback', 'reset-soft', 'reset-hard')}}
        payload = json.dumps(data, separators=(',', ':')).encode() + b'\n'
        expectation.write_bytes(payload)
        evidence = self.root / 'snapshot-evidence'
        original_suite = rom.suite

        def replace_source(args):
            expectation.write_bytes(b'{}')
            return unittest.TestSuite(test for test in original_suite(args)
                                      if test._testMethodName == 'test_running_session_rollback')

        argv = ['device-rom', '--device', 'sony-clie-tg50', '--nor', str(self.nor),
                '--sha256', data['nor_sha256'], '--launcher', '--resets',
                '--expected-faults', str(expectation), '--evidence', str(evidence)]
        with patch.object(rom, 'ROOT', self.root), patch.object(rom, 'CORE', core), \
             patch.object(rom, 'suite', side_effect=replace_source), patch.object(sys, 'argv', argv), \
             patch.object(rom, 'run_guest', return_value=subprocess.CompletedProcess([], 0, 'continued guest input', '')), \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(rom.main(), 0)
        self.assertEqual((evidence / 'expected-faults.json').read_bytes(), payload)
        identity = json.loads((evidence / 'expected-faults-identity.json').read_text())
        self.assertEqual(identity['sha256'], hashlib.sha256(payload).hexdigest())
        self.assertEqual(expectation.read_bytes(), b'{}')

    def test_existing_evidence_is_refused_unchanged(self):
        evidence = self.root / 'evidence'
        evidence.mkdir()
        sentinel = evidence / 'keep'
        sentinel.write_text('earlier evidence')
        result = subprocess.run([sys.executable, str(ROOT / 'tests/device-rom.py'), '--device', 'palm-e2',
                                 '--evidence', str(evidence)], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertIn('Evidence directory already exists', result.stderr)
        self.assertEqual(sentinel.read_text(), 'earlier evidence')
        self.assertEqual(list(evidence.iterdir()), [sentinel])

    def test_variant_mismatch_starts_no_guest(self):
        evidence = self.root / 'mismatched-variant'
        argv = ['device-rom', '--device', 'palm-e2', '--nor', str(self.nor), '--variant', 'e2-denvfs',
                '--evidence', str(evidence)]
        with patch.object(sys, 'argv', argv), patch.object(rom, 'suite') as suite, \
             contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            rom.main()
        self.assertEqual(error.exception.code, 2)
        suite.assert_not_called()
        self.assertFalse(evidence.exists())

    def test_detected_variant_reaches_generic_harness(self):
        variant = {'id': 'e2-denvfs'}
        for explicit in ([], ['--variant', 'e2-denvfs']):
            argv = ['device-rom', '--device', 'palm-e2', '--nor', str(self.nor), *explicit]
            with self.subTest(explicit=explicit), patch.object(sys, 'argv', argv), \
                 patch.object(rom, 'firmware_variant', return_value=variant), \
                 patch.object(rom, 'suite', return_value=unittest.TestSuite()) as suite, \
                 contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(rom.main(), 0)
            self.assertIs(suite.call_args.args[0].firmware_variant, variant)


if __name__ == '__main__':
    unittest.main()
