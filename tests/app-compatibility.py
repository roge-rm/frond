#!/usr/bin/env python3
"""Firmware-free tests for private application evidence and identity guards."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import contextlib
import io
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('app_matrix', ROOT / 'tools/app-compatibility.py')
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def database(records, name='Test Data', kind=b'DATATest'):
    raw = bytearray(78 + 8 * len(records) + 2)
    raw[:len(name)] = name.encode()
    raw[60:68] = kind
    struct.pack_into('>H', raw, 76, len(records))
    for i, record in enumerate(records):
        struct.pack_into('>I', raw, 78 + 8 * i, len(raw))
        raw[83 + 8 * i:86 + 8 * i] = (i + 1).to_bytes(3, 'big')
        raw.extend(record)
    return raw


class MatrixTests(unittest.TestCase):
    def setUp(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='app-matrix-test-', dir=ROOT / 'build')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.core = self.root / 'core'; self.core.write_bytes(b'fake executable'); self.core.chmod(0o700)
        self.fixture = self.root / 'fixture.frond'; self.fixture.write_bytes(b'original fixture')
        self.prc = self.root / 'test.prc'; self.prc.write_bytes(b'app')
        self.replay = self.root / 'input.replay'; self.replay.write_text('0 pen-down 10 10\n10 pen-up\n')
        self.manifest = self.root / 'private.json'
        self.output = self.root / 'results'
        self.raw = {'version': 1, 'core': self.ident(self.core), 'apps': [
            {'id': 'test', 'version': '1.2', 'app': 'Test', 'fixture': self.ident(self.fixture),
             'installs': [self.ident(self.prc)], 'stages': {
                 'cold-boot': {'skip': 'No cold boot runner'},
                 'launch': self.stage(b'before'), 'interaction': self.stage(b'after', True),
                 'persistence': self.stage(b'after'), 'restore': self.stage(b'after')}}]}
        self.calls = 0
        self.fault = None

    def ident(self, path):
        return {'path': str(path), 'sha256': matrix.runner.sha256(path)}

    def stage(self, value, replay=False):
        return {'run_for': 0, 'assertions': [{'kind': 'database-records', 'database': 'Test Data',
                 'type': 'DATA', 'creator': 'Test', 'records_sha256': [sha(value)]}],
                **({'replay': self.ident(self.replay)} if replay else {})}

    def fake_run(self, args):
        self.calls += 1
        root = args.output
        root.mkdir(parents=True, mode=0o700)
        origins = [args.session] if args.session else [args.nor, args.nand]
        sources = [args.core, *origins, *args.install, *([args.replay] if args.replay else [])]
        result = {'status': 'ok', 'stage': 'complete', 'core_exit_code': 0,
                  'inputs': [self.ident(p) for p in sources],
                  'origin': {'mode': 'session-restore' if args.session else 'cold-boot',
                             'saved_ram_cpu_loaded': bool(args.session)}, 'artifacts': {
                      'session_saved': True, 'session': str(root / 'working.frond'),
                      'exports': [], 'screenshots': []}}
        (root / 'working.frond').write_bytes(b'saved' + bytes([self.calls]))
        for index, name in enumerate(args.export_db, 1):
            archive = root / f'database-{index:03d}.zip'
            value = b'before' if self.calls == 1 else b'after'
            if self.fault == 'stale-interaction':
                value = b'before'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr(name + '.pdb', database([value], name=name))
            result['artifacts']['exports'].append({'database': name, 'path': str(archive)})
        for index, at in enumerate(args.screenshot):
            p = root / f'frame-{index}.png'; p.write_bytes(b'frame')
            result['artifacts']['screenshots'].append({'at_ms': at, 'path': str(p)})
        if self.fault == 'wrong-origin':
            result['origin']['saved_ram_cpu_loaded'] = not bool(args.session)
        if self.fault == 'forged-identity':
            result['inputs'][0]['sha256'] = '0' * 64
        if self.fault == 'detached-identity':
            result['inputs'][1]['path'] = str(self.fixture)
        if self.fault == 'failed':
            result['status'] = 'error'
        if self.fault == 'partial':
            result['artifacts']['session_saved'] = False
        if self.fault == 'modify-original':
            self.fixture.write_bytes(b'changed')
        if self.fault == 'modify-frozen-core':
            args.core.write_bytes(b'changed')
        (root / 'result.json').write_text(json.dumps(result))
        if self.fault == 'detached-result':
            result['extra'] = 'not on disk'
        return result

    def run_case(self):
        self.manifest.write_text(json.dumps(self.raw))
        with patch.object(matrix.runner, 'run', self.fake_run):
            return matrix.run(self.manifest, self.output, 1)

    def test_exact_records_and_session_chain_are_narrow_evidence(self):
        report = self.run_case()
        self.assertEqual(report['execution_status'], 'ok')
        self.assertEqual(report['coverage_status'], 'incomplete')
        self.assertEqual(self.calls, 4)
        rows = report['apps'][0]['stages']
        self.assertEqual(rows['cold-boot']['status'], 'skipped')
        self.assertTrue(all(rows[s]['status'] == 'pass' for s in matrix.STAGES[1:]))
        self.assertEqual(len({rows[s]['run_id'] for s in matrix.STAGES}), 5)
        self.assertTrue(all(c['unchanged'] and c['snapshot_unchanged'] for c in report['input_preservation']))
        self.assertEqual(self.output.stat().st_mode & 0o777, 0o700)
        self.assertEqual(json.loads((self.output / 'matrix.json').read_text()), report)

    def test_runner_failures_never_pass_and_block_following_stages(self):
        for fault in ('forged-identity', 'detached-identity', 'failed', 'partial', 'detached-result', 'wrong-origin'):
            with self.subTest(fault=fault):
                self.output = self.root / fault; self.calls = 0; self.fault = fault
                report = self.run_case()
                rows = report['apps'][0]['stages']
                self.assertEqual(report['execution_status'], 'error')
                self.assertEqual(rows['launch']['status'], 'error')
                self.assertEqual(rows['interaction']['status'], 'blocked')
                self.assertEqual(self.calls, 1)
                self.assertTrue((self.output / 'test/launch/result.json').exists())

    def test_same_records_do_not_prove_interaction(self):
        self.fault = 'stale-interaction'
        self.raw['apps'][0]['stages']['interaction']['assertions'][0]['records_sha256'] = [sha(b'before')]
        report = self.run_case()
        row = report['apps'][0]['stages']['interaction']
        self.assertEqual(row['status'], 'error')
        self.assertIn('No observed interaction', row['error'])

    def test_restore_requires_preceding_database_identity(self):
        self.raw['apps'][0]['stages']['persistence']['assertions'][0]['database'] = 'Other Data'
        report = self.run_case()
        self.assertIn('preceding', report['apps'][0]['stages']['persistence']['error'])

    def test_visual_frames_never_pass(self):
        for label in matrix.STAGES[1:]:
            self.raw['apps'][0]['stages'][label]['assertions'] = [
                {'kind': 'visual-review', 'expected': 'Editor responds to declared input', 'at_ms': [0]}]
        report = self.run_case()
        self.assertTrue(all(report['apps'][0]['stages'][s]['status'] == 'needs-review' for s in matrix.STAGES[1:]))
        self.assertEqual(report['execution_status'], 'ok')
        self.assertEqual(report['coverage_status'], 'needs-review')

    def test_skip_blocks_subsequent_stages(self):
        self.raw['apps'][0]['stages']['interaction'] = {'skip': 'No authorized fixture'}
        report = self.run_case()
        self.assertEqual(self.calls, 1)
        self.assertEqual(report['apps'][0]['stages']['restore']['status'], 'blocked')

    def test_live_developer_runner_with_synthetic_socket_peer(self):
        # Exercises real private copies, control socket, replay, save and chaining.
        # This executable is the existing firmware-free fake peer, not an emulator.
        peer = ROOT / 'tests/developer-run.py'
        self.core.write_text('#!' + sys.executable + '\nimport runpy\n'
                             + 'runpy.run_path(' + repr(str(peer)) + ', run_name="__main__")\n')
        self.fixture.write_text('{}')
        self.raw['core'] = self.ident(self.core)
        app = self.raw['apps'][0]
        app['fixture'] = self.ident(self.fixture)
        for label in matrix.STAGES[1:]:
            app['stages'][label]['assertions'] = [
                {'kind': 'visual-review', 'expected': 'Synthetic frame requires review', 'at_ms': [0]}]
        self.manifest.write_text(json.dumps(self.raw))
        with patch.object(matrix.runner.shutil, 'which', return_value=None):
            report = matrix.run(self.manifest, self.output, 8)
        self.assertEqual(report['execution_status'], 'ok', report)
        self.assertEqual(report['coverage_status'], 'needs-review')
        self.assertTrue(all(report['apps'][0]['stages'][s]['status'] == 'needs-review'
                            for s in matrix.STAGES[1:]))
        for label in matrix.STAGES[1:]:
            self.assertTrue((self.output / 'test' / label / 'core').stat().st_mode & 0o100)

    def test_raw_boot_pins_two_images_then_chains_saved_state(self):
        nor, nand = self.root / 'nor.bin', self.root / 'nand.bin'
        nor.write_bytes(b'authorized NOR'); nand.write_bytes(b'authorized NAND')
        app = self.raw['apps'][0]
        app['fixture'] = {'mode': 'cold-boot', 'nor': self.ident(nor), 'nand': self.ident(nand)}
        visual = {'run_for': 0, 'assertions': [
            {'kind': 'visual-review', 'expected': 'Setup completes and expected UI responds', 'at_ms': [0]}]}
        app['stages']['cold-boot'] = copy.deepcopy(visual)
        for label in matrix.STAGES[1:]:
            app['stages'][label]['assertions'] = copy.deepcopy(visual['assertions'])
        report = self.run_case()
        self.assertEqual(report['execution_status'], 'ok', report)
        rows = report['apps'][0]['stages']
        self.assertEqual(rows['cold-boot']['status'], 'needs-review')
        self.assertEqual(rows['launch']['input_session']['source_stage'], 'cold-boot')
        self.assertEqual(report['coverage_status'], 'needs-review')
        self.assertEqual(self.calls, 5)

    def test_raw_boot_static_records_do_not_prove_guest_execution(self):
        app = self.raw['apps'][0]
        app['fixture'] = {'mode': 'cold-boot', 'nor': self.ident(self.fixture), 'nand': self.ident(self.prc)}
        app['stages']['cold-boot'] = self.stage(b'before')
        with self.assertRaisesRegex(ValueError, 'static records'):
            matrix.validate_manifest(self.raw, self.root)

    def test_fixture_resume_is_not_new_launch_coverage(self):
        app = self.raw['apps'][0]
        app['installs'] = []
        app['stages']['launch']['resume_app'] = True
        report = self.run_case()
        row = report['apps'][0]['stages']['launch']
        self.assertEqual(row['operation'], 'fixture-resume')
        self.assertEqual(row['status'], 'needs-review')
        self.assertTrue(row['install_launch_coverage'].startswith('untested'))

    def test_fixture_resume_rejects_installs(self):
        self.raw['apps'][0]['stages']['launch']['resume_app'] = True
        with self.assertRaisesRegex(ValueError, 'cannot install'):
            matrix.validate_manifest(self.raw, self.root)

    def test_persistence_skip_preserves_explicit_gap_and_restores_interaction(self):
        self.raw['apps'][0]['stages']['persistence'] = {'skip': 'App database save/relaunch untested'}
        report = self.run_case()
        rows = report['apps'][0]['stages']
        self.assertEqual(rows['persistence']['status'], 'skipped')
        self.assertEqual(rows['restore']['status'], 'pass')
        self.assertEqual(rows['restore']['input_session']['source_stage'], 'interaction')
        self.assertEqual(report['coverage_status'], 'incomplete')

    def test_successful_execution_cli_does_not_claim_complete_coverage(self):
        self.manifest.write_text(json.dumps(self.raw))
        with patch.object(matrix.runner, 'run', self.fake_run), patch.object(sys, 'argv',
                ['app-compatibility.py', '--manifest', str(self.manifest), '--output', str(self.output)]), \
                contextlib.redirect_stdout(io.StringIO()) as stdout:
            self.assertEqual(matrix.main(), 0)
        report = json.loads(stdout.getvalue())
        self.assertEqual(report['execution_status'], 'ok')
        self.assertEqual(report['coverage_status'], 'incomplete')
        self.assertNotIn('status', report)

    def test_duplicate_json_keys_rejected(self):
        with self.assertRaises(ValueError):
            matrix.strict_json('{"version":1,"version":1}')

    def test_stale_output_preserved(self):
        self.output.mkdir(); marker = self.output / 'marker'; marker.write_text('keep')
        report = self.run_case()
        self.assertEqual(report['execution_status'], 'error')
        self.assertEqual(marker.read_text(), 'keep')
        self.assertEqual(self.calls, 0)

    def test_private_output_outside_checkout_is_supported(self):
        with tempfile.TemporaryDirectory(prefix='app-private-output-') as directory:
            self.output = Path(directory) / 'new-result'
            self.assertEqual(self.run_case()['execution_status'], 'ok')
            self.assertEqual(self.output.stat().st_mode & 0o777, 0o700)

    def test_forged_manifest_identity_rejected_before_run(self):
        self.raw['apps'][0]['fixture']['sha256'] = '0' * 64
        self.assertEqual(self.run_case()['execution_status'], 'error')
        self.assertEqual(self.calls, 0)

    def test_original_and_snapshot_modifications_are_errors(self):
        for fault in ('modify-original', 'modify-frozen-core'):
            with self.subTest(fault=fault):
                self.fixture.write_bytes(b'original fixture')
                self.fault = fault; self.output = self.root / fault
                self.assertEqual(self.run_case()['execution_status'], 'error')

    def test_schema_rejects_shells_expressions_unbounded_and_bad_types(self):
        for mutation in (lambda r: r.update(version=True),
                         lambda r: r.update(command='echo yes'),
                         lambda r: r['apps'][0]['stages']['launch'].update(run_for=601),
                         lambda r: r['apps'][0]['stages']['launch'].update(run_for=float('nan')),
                         lambda r: r['apps'][0]['stages']['launch'].update(run_for=10**400),
                         lambda r: r['apps'][0]['stages']['cold-boot'].update(command='boot')):
            raw = copy.deepcopy(self.raw); mutation(raw)
            with self.assertRaises(ValueError):
                matrix.validate_manifest(raw, self.root)

    def test_replay_rejects_unbounded_and_nonphysical_commands(self):
        for content in ('0 shell touch /tmp/x\n', '600001 pen-up\n', '1 pen-up\n0 pen-up\n', '0 key-down bogus\n'):
            self.replay.write_text(content)
            with self.assertRaises(ValueError):
                matrix.validate_replay(self.replay)

    def test_strict_database_identity_and_directory(self):
        oracle = self.stage(b'good')['assertions'][0]
        archive = self.root / 'export.zip'
        for name, raw in [('Other.pdb', database([b'good'])),
                          ('Test Data.pdb', database([b'good'], name='Other')),
                          ('Test Data.pdb', database([b'good'], kind=b'BAD!Test')),
                          ('Test Data.pdb', b'truncated')]:
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr(name, raw)
            with self.assertRaises(ValueError):
                matrix.read_database(archive, oracle)
        raw = database([b'good']); struct.pack_into('>I', raw, 78, 0)
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('Test Data.pdb', raw)
        with self.assertRaises(ValueError):
            matrix.read_database(archive, oracle)

    def test_deleted_busy_and_duplicate_record_identities_are_not_retention(self):
        oracle = self.stage(b'good')['assertions'][0]
        archive = self.root / 'bad-records.zip'
        variants = []
        for flags in (0x80, 0x20):
            raw = database([b'good']); raw[82] = flags; variants.append(raw)
        raw = database([b'good']); raw[83:86] = b'\0\0\0'; variants.append(raw)
        raw = database([b'good', b'good']); raw[91:94] = raw[83:86]; variants.append(raw)
        for raw in variants:
            with zipfile.ZipFile(archive, 'w') as z: z.writestr('Test Data.pdb', raw)
            with self.assertRaises(ValueError): matrix.read_database(archive, oracle)

    def test_snapshot_failure_and_interruption_never_report_complete(self):
        self.manifest.write_text(json.dumps(self.raw))
        for failure in (OSError('copy failed'), KeyboardInterrupt()):
            self.output = self.root / ('failed-copy' if isinstance(failure, OSError) else 'interrupted')
            with patch.object(matrix, 'snapshot_inputs', side_effect=failure):
                report = matrix.run(self.manifest, self.output, 1)
            self.assertEqual(report['execution_status'], 'error')
            self.assertEqual(report['coverage_status'], 'incomplete')
            self.assertTrue((self.output / 'matrix.json').is_file())

    def test_unsupported_archive_is_structured_stage_failure(self):
        original = matrix.read_database
        def unreadable(path, oracle):
            with patch.object(zipfile.ZipFile, 'read', side_effect=NotImplementedError('compression')):
                return original(path, oracle)
        with patch.object(matrix, 'read_database', side_effect=unreadable):
            report = self.run_case()
        self.assertEqual(report['execution_status'], 'error')
        rows = report['apps'][0]['stages']
        self.assertEqual(rows['launch']['status'], 'error')
        self.assertEqual(rows['interaction']['status'], 'blocked')

    def test_corrupt_deflate_is_a_validation_error(self):
        archive = self.root / 'deflate.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('Test Data.pdb', database([b'good']))
        with patch.object(zipfile.ZipFile, 'read', side_effect=zlib.error('invalid block type')):
            with self.assertRaises(ValueError):
                matrix.read_database(archive, self.stage(b'good')['assertions'][0])

    def test_live_runner_archive_decode_failure_blocks_later_stages(self):
        peer = ROOT / 'tests/developer-run.py'
        self.core.write_text('#!' + sys.executable + '\nimport runpy\n'
                            + 'runpy.run_path(' + repr(str(peer)) + ', run_name="__main__")\n')
        self.core.chmod(0o700)
        self.raw['core'] = self.ident(self.core)
        for kind in ('compression', 'encrypted', 'deflate'):
            with self.subTest(kind=kind):
                self.fixture.write_text(json.dumps({'bad_archive': kind}))
                self.raw['apps'][0]['fixture'] = self.ident(self.fixture)
                self.manifest.write_text(json.dumps(self.raw))
                with patch.object(matrix.runner.shutil, 'which', return_value=None):
                    report = matrix.run(self.manifest, self.root / kind, 8)
                self.assertEqual(report['execution_status'], 'error')
                rows = report['apps'][0]['stages']
                self.assertEqual(rows['launch']['status'], 'error')
                self.assertEqual(rows['interaction']['status'], 'blocked')
                result = json.loads((self.root / kind / 'test/launch/result.json').read_text())
                self.assertEqual(result['error']['code'], 'export')
                self.assertFalse(result['artifacts']['session_saved'])


if __name__ == '__main__':
    unittest.main()
