#!/usr/bin/env python3
"""Firmware-free qualification barriers, late failures and ownership tests."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('qualification', ROOT / 'tools/qualify-peripherals.py')
qualification = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qualification)


class QualificationTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='peripheral-qualification-')
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.core = self.root / 'fake core'
        self.core.write_text('#!' + sys.executable + '\nimport runpy\nrunpy.run_path(' +
                             repr(str(ROOT / 'tests/developer-run.py')) + ', run_name="__main__")\n')
        self.core.chmod(0o700)
        self.session = self.root / 'original.frond'
        self.session.write_text('{}')
        heavy = self.root / 'heavy'
        heavy.write_text('#!/bin/sh\nshift\nexec "$@"\n')
        heavy.chmod(0o700)
        self.output = self.root / 'private results 音'
        self.plan = {'nodes': {'tx': {'core': str(self.core), 'session': str(self.session)}},
                     'steps': [{'id': 'ready', 'node': 'tx', 'command': 'guest-ready',
                                'retry_ready': True, 'timeout': 2}, self.check()]}

    def check(self, status='pass', code=0):
        return {'id': 'evidence', 'check': [sys.executable, '-c',
                f'import json; print(json.dumps({{"status": {status!r}}})); raise SystemExit({code})']}

    def run_plan(self, config=None):
        self.session.write_text(json.dumps(config or {}))
        plan_path = self.root / 'input-plan.json'
        plan_path.write_text(json.dumps(self.plan))
        with patch.dict(os.environ, PATH=str(self.root) + os.pathsep + os.environ['PATH']):
            result = qualification.run(plan_path, self.output, timeout=6)
        self.assertEqual(json.loads((self.output / 'result.json').read_text()), result)
        # Each core prints its PID on start; all owned guests must be reaped.
        for log in self.output.glob('*.log'):
            first = log.read_text().splitlines()
            if first and first[0].isdigit():
                with self.assertRaises(ProcessLookupError):
                    os.kill(int(first[0]), 0)
        return result

    def test_two_node_readiness_barrier_and_endpoint_events(self):
        self.plan['nodes']['e2'] = dict(self.plan['nodes']['tx'])
        self.plan['steps'].insert(1, {'id': 'e2-ready', 'node': 'e2', 'command': 'guest-ready',
                                      'retry_ready': True, 'timeout': 2})
        result = self.run_plan({'ready_retries': 2, 'endpoint_events': ['infrared', 'bluetooth']})
        self.assertEqual(result['status'], 'pass', result)
        for step in result['steps'][:2]:
            self.assertEqual([r['ok'] for r in step['responses']], [False, False, True])
        self.assertTrue(all(result['inputs_unchanged'].values()))
        self.assertEqual(self.output.stat().st_mode & 0o777, 0o700)

    def test_readiness_timeout_blocks_checker_and_cleans_guest(self):
        self.plan['steps'][0]['timeout'] = .15
        result = self.run_plan({'ready_retries': 999})
        self.assertEqual(result['status'], 'fail')
        self.assertEqual(result['stage'], 'ready')
        self.assertEqual(result['checks'], [])
        self.assertEqual(len(result['steps']), 1)
        self.assertTrue(result['failure_saves']['tx']['ok'])
        self.assertTrue((self.output / 'tx-failed.frond').is_file())

    def test_late_checker_failure_blocks_downstream(self):
        self.plan['steps'][1] = self.check(code=7)
        self.plan['steps'].append({'id': 'forbidden', 'node': 'tx', 'command': 'save',
                                   'argument': '$OUT/should-not-exist'})
        result = self.run_plan()
        self.assertEqual(result['status'], 'fail')
        self.assertEqual(result['stage'], 'evidence')
        self.assertFalse((self.output / 'should-not-exist').exists())

    def test_exit_zero_without_passing_evidence_fails(self):
        self.plan['steps'][1] = self.check(status='fail')
        result = self.run_plan()
        self.assertEqual(result['status'], 'fail')
        self.assertIn('status=pass', result['error']['message'])

    def test_checker_json_must_be_a_verdict_object(self):
        self.plan['steps'][1]['check'] = [sys.executable, '-c', 'print("[]")']
        result = self.run_plan()
        self.assertEqual(result['status'], 'fail')
        self.assertIn('status=pass', result['error']['message'])

    def test_completed_peer_exit_must_be_explicitly_allowed(self):
        self.plan['steps'][0:0] = [
            {'id': 'peer', 'service': [sys.executable, '-c', 'raise SystemExit(0)'],
             'allow_exit': [0]}, {'id': 'settle', 'wait': .1}]
        result = self.run_plan()
        self.assertEqual(result['status'], 'pass', result)

    def test_original_input_mutation_overrides_pass(self):
        result = self.run_plan({'mutate_core': str(self.core)})
        self.assertEqual(result['status'], 'fail')
        self.assertEqual(result['error']['code'], 'input-changed')

    def test_peer_readiness_marker_and_owned_cleanup(self):
        self.plan['steps'][0:0] = [
            {'id': 'peer', 'service': [sys.executable, '-u', '-c',
              'import os,time; print(os.getpid()); print("READY"); time.sleep(30)']},
            {'id': 'peer-ready', 'wait_log': '$OUT/peer.log', 'contains': 'READY', 'timeout': 2}]
        result = self.run_plan()
        self.assertEqual(result['status'], 'pass', result)

    def test_peer_exit_blocks_downstream(self):
        self.plan['steps'][0:0] = [
            {'id': 'peer', 'service': [sys.executable, '-c', 'raise SystemExit(8)']},
            {'id': 'peer-ready', 'wait_log': '$OUT/peer.log', 'contains': 'NEVER', 'timeout': 2}]
        result = self.run_plan()
        self.assertEqual(result['status'], 'fail')
        self.assertIn('exited unexpectedly', result['error']['message'])

    def test_replay_waits_for_completion_before_evidence(self):
        self.plan['steps'].insert(1, {'id': 'capture', 'node': 'tx',
            'replay': '0 key-down home\n10 key-up home\n40 snapshot "$OUT/captured.png"\n'})
        result = self.run_plan()
        self.assertEqual(result['status'], 'pass', result)
        self.assertTrue((self.output / 'captured.png').is_file())
        self.assertIn('captured.png', result['artifacts_sha256'])


if __name__ == '__main__':
    unittest.main()
