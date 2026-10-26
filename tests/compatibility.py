#!/usr/bin/env python3
"""Firmware-free negative tests for the compatibility matrix and fresh-log contract."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('matrix', ROOT / 'tools/check-compatibility.py')
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)
results = matrix.results
TOKEN, NEXT = 'a' * 32, 'b' * 32


def invocation(token=TOKEN, resumed=False):
    values = [256, 2 if resumed else 1, 2 if resumed else 1, 2, 3,
              results.checksum(), results.checksum(), 1]
    return ([f'begin token={token} version=1'] +
            [f'{label} err=0000 value={value}' for label, value in zip(results.CASES, values)] +
            [f'complete token={token} failures=0'])


def log_archive(path, lines, corrupt=None):
    raw = bytearray(78 + len(lines) * 8 + 2)
    raw[:len(results.LOG)] = results.LOG.encode()
    raw[60:68] = b'LOGSFrCp'
    struct.pack_into('>H', raw, 76, len(lines))
    for i, line in enumerate(lines):
        struct.pack_into('>I', raw, 78 + i * 8, len(raw))
        raw.extend(line.encode() + b'\0')
    if corrupt:
        corrupt(raw)
    with zipfile.ZipFile(path, 'w') as archive:
        archive.writestr(results.LOG + '.pdb', raw)


class Compatibility(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_fresh_initial_and_restored(self):
        for token, resumed in ((TOKEN, False), (NEXT, True)):
            path = self.root / 'log.zip'
            log_archive(path, invocation(token, resumed))
            checks = results.check_records(results.read_log(path), token, resumed=resumed)
            self.assertEqual(len(checks), 8)
            self.assertTrue(all(c['status'] == 'pass' for c in checks))

    def test_stale_incomplete_duplicate_and_failed_logs_rejected(self):
        good = invocation()
        broken = [invocation(NEXT), good[:-1], good + good, good[:-1] + [good[1]],
                  good[:2] + [good[1]] + good[3:], list(reversed(good))]
        for i in range(1, len(good) - 1):
            broken.append(good[:i] + [good[i].replace('err=0000', 'err=0001')] + good[i+1:])
            broken.append(good[:i] + [good[i].rsplit('=', 1)[0] + '=0'] + good[i+1:])
        broken += [good[:-1] + [good[-1].replace('failures=0', 'failures=1')]]
        for lines in broken:
            with self.subTest(lines=lines), self.assertRaises(ValueError):
                results.check_records(lines, TOKEN)
        with self.assertRaises(ValueError):
            results.check_records(good, TOKEN, resumed=True)

    def test_malformed_databases_rejected(self):
        path = self.root / 'log.zip'
        for corrupt in (lambda b: b.__setitem__(0, 0), lambda b: b.__setitem__(60, 0),
                        lambda b: struct.pack_into('>I', b, 78, 1),
                        lambda b: struct.pack_into('>I', b, 86, len(b) + 1),
                        lambda b: b.__setitem__(-1, 65),
                        lambda b: struct.pack_into('>H', b, 76, 65535)):
            log_archive(path, invocation(), corrupt)
            with self.assertRaises(ValueError):
                results.read_log(path)
        with zipfile.ZipFile(path, 'w') as archive:
            archive.writestr('unrelated.pdb', b'bad')
        with self.assertRaises(ValueError):
            results.read_log(path)

    def test_configuration_encoding(self):
        raw = results.config_database(NEXT, TOKEN)
        self.assertEqual(raw[60:68], b'DATAFrCp')
        self.assertEqual(raw[88:], NEXT.encode() + b'\0' + TOKEN.encode() + b'\0')
        self.assertEqual(len(results.config_database(TOKEN)[88:]), 66)
        with self.assertRaises(ValueError):
            results.config_database('../bad')

    def args(self, *extra):
        return matrix.parser().parse_args(['--output', str(self.root / 'output'),
                                          '--core-test', '/bin/true', *extra])

    def test_without_firmware_rows_are_skipped(self):
        report = matrix.run(self.args())
        self.assertEqual(report['status'], 'pass', report)
        self.assertEqual([r['status'] for r in report['rows']], ['pass'] + ['skipped'] * 8)

    def test_existing_output_untouched_and_failed_core_stops(self):
        args = self.args()
        args.output.mkdir(); marker = args.output / 'keep'; marker.write_text('private')
        self.assertEqual(matrix.run(args)['status'], 'error')
        self.assertEqual(list(args.output.iterdir()), [marker])
        args.output = self.root / 'failed'; args.core_test = Path('/bin/false')
        self.assertEqual(matrix.run(args)['status'], 'error')
        self.assertFalse((args.output / 'initial-config.pdb').exists())

    def test_matrix_restores_session_and_requires_new_guest_results(self):
        calls = []
        def fake_run(args):
            calls.append(args)
            args.output.mkdir()
            archive = args.output / 'database-001.zip'
            raw = args.install[0].read_bytes()
            token = raw[88:120].decode()
            previous = raw[121:153].rstrip(b'\0').decode()
            self.assertEqual(bool(previous), len(calls) == 2)
            log_archive(archive, invocation(token, bool(previous)))
            session = args.output / 'working.frond'
            session.write_bytes(b'synthetic saved state')
            (args.output / 'result.json').write_text('{}')
            return {'status': 'ok', 'artifacts': {'exports': [{'path': str(archive)}],
                                                'session': str(session)}}
        with patch.object(matrix.runner, 'run', side_effect=fake_run):
            report = matrix.run(self.args('--session', str(self.root / 'source.session')))
        self.assertEqual(report['status'], 'pass', report)
        self.assertEqual(calls[1].session, calls[0].output / 'working.frond')
        self.assertEqual(len(calls[0].install), 2)
        self.assertEqual(len(calls[1].install), 1)
        self.assertEqual(len(report['rows']), 17)
        self.assertEqual(json.loads((self.root / 'output/matrix.json').read_text()), report)
        self.assertEqual((self.root / 'output').stat().st_mode & 0o777, 0o700)

    def test_stale_export_does_not_pass_matrix(self):
        def stale(args):
            args.output.mkdir()
            archive = args.output / 'log.zip'; log_archive(archive, invocation(TOKEN))
            return {'status': 'ok', 'artifacts': {'exports': [{'path': str(archive)}]}}
        with patch.object(matrix.runner, 'run', side_effect=stale):
            report = matrix.run(self.args('--session', str(self.root / 'source.session')))
        self.assertEqual(report['status'], 'error')
        self.assertIn('Stale', report['error'])


if __name__ == '__main__':
    unittest.main()
