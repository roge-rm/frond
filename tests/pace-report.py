#!/usr/bin/env python3
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('report', Path(__file__).resolve().parents[1] / 'tools/pace-report.py')
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class Report(unittest.TestCase):
    def test_program_identity_is_explicit_and_unverified(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'probe.prc'
            data = bytearray(90)
            data[:8] = b'Test\x1bApp'
            struct.pack_into('>H', data, 32, 1)
            data[60:68] = b'applFRND'
            struct.pack_into('>H', data, 76, 1)
            data[78:82] = b'code'
            struct.pack_into('>HI', data, 82, 1, 88)
            data[88:] = b'\x4e\x75'
            path.write_bytes(data)
            result = report.program_identity(path)
            self.assertFalse(result['active_app_verified'])
            self.assertIsNone(result['guest_load_address'])
            self.assertEqual(result['sha256'], hashlib.sha256(data).hexdigest())
            self.assertEqual(result['creator_hex'], b'FRND'.hex())
            self.assertEqual(result['resources'], [{'type_hex': b'code'.hex(), 'id': 1, 'file_offset': 88}])
            self.assertIn('\\u001b', json.dumps(result))
            for bad in [data[:30], data[:80], data[:88] + b'', bytearray(b'X' * 90)]:
                # A zero-length resource at EOF is valid; use a bad offset instead.
                if len(bad) == 88:
                    bad[84:88] = b'\xff' * 4
                path.write_bytes(bad)
                with self.assertRaises(ValueError):
                    report.program_identity(path)

    def test_context_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'context.json'
            valid = {'version': 1, 'available': True, 'pc': 42, 'sr': 0,
                     'state_pointer': 0, 'a': [0] * 8, 'd': [0] * 8}
            path.write_text(json.dumps(valid))
            self.assertEqual(report.read_context(path), valid)
            for invalid in [{}, [], dict(valid, d=[0] * 7), dict(valid, pc=-1), dict(valid, sr=65536)]:
                path.write_text(json.dumps(invalid))
                with self.assertRaises(ValueError): report.read_context(path)
            path.write_text(' ' * 16385)
            with self.assertRaises(ValueError): report.read_context(path)


if __name__ == '__main__':
    unittest.main()
