#!/usr/bin/env python3
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('report', Path(__file__).resolve().parents[1] / 'tools/pace-report.py')
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class Report(unittest.TestCase):
    def test_symbols_require_unique_exact_resource_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            program = Path(tmp) / 'probe.prc'
            program.write_bytes(b'headerXXCODEYY')
            executable = Path(tmp) / 'compiler-output'
            executable.write_bytes(b'compiler output')
            identity = {'size': 14, 'sha256': hashlib.sha256(program.read_bytes()).hexdigest(),
                        'resources': [{'type_hex': '636f6465', 'id': 1, 'file_offset': 6}]}
            def extract(args, check):
                self.assertNotEqual(Path(args[-2]), executable)
                self.assertEqual(Path(args[-2]).read_bytes(), b'compiler output')
                Path(args[-1]).write_bytes(b'CODE')
            def listing(args, text):
                self.assertNotEqual(Path(args[-1]), executable)
                self.assertEqual(Path(args[-1]).read_bytes(), b'compiler output')
                executable.write_bytes(b'concurrent rebuild with different symbols')
                if '-f' in args:
                    return 'architecture: m68k:68000, flags 0x1\n'
                if args[0].endswith('objdump'):
                    return '  0 .text 00000004 00000020 00000020\n'
                return '00000020 T entry\n00000022 t local\n00000020 t .text\n00000028 T outside\n'
            with patch.object(report.subprocess, 'check_output', side_effect=listing), \
                    patch.object(report.subprocess, 'run', side_effect=extract):
                symbols = report.executable_symbols(executable, program, identity, 'test-')
                self.assertEqual(symbols['text_resource_offset'], 2)
                self.assertEqual(symbols['symbols'], [{'name': 'entry', 'resource_offset': 2},
                                                     {'name': 'local', 'resource_offset': 4}])
                for payload in (b'headerXXCODfYY', b'headerCODECODE'):
                    executable.write_bytes(b'compiler output')
                    program.write_bytes(payload)
                    identity['sha256'] = hashlib.sha256(payload).hexdigest()
                    with self.assertRaises(ValueError):
                        report.executable_symbols(executable, program, identity, 'test-')

    def test_runtime_mapping_rejects_stale_or_mismatched_evidence(self):
        context = {'version': 1, 'available': True, 'pc': 0x1003, 'sr': 0,
                   'state_pointer': 64, 'a': [0] * 8, 'd': [0] * 8}
        payload = b'abcdefgh'
        identity = {'sha256': 'program'}
        resource = {'type_hex': '636f6465', 'id': 1, 'address': 0x1000,
                    'size': len(payload), 'sha256': hashlib.sha256(payload).hexdigest()}
        mapping = {'version': 1, 'program_sha256': 'program', 'context_pc': 0x1003,
                   'context': context, 'resources': [resource]}
        symbols = {'type_hex': '636f6465', 'id': 1, 'text_resource_offset': 0,
                   'text_size': 8, 'symbols': [{'name': 'entry', 'resource_offset': 2}]}
        resolve = lambda value: report.resolve_pc(context, value, identity, {('636f6465', 1): payload}, symbols)
        location = resolve(mapping)
        self.assertEqual(location['nearest_text_symbol']['displacement'], 1)
        self.assertFalse(location['active_app_verified'])
        for bad in [dict(mapping, program_sha256='other'), dict(mapping, context_pc=0x1004),
                    dict(mapping, version=True), dict(mapping, context_pc=float(context['pc'])),
                    dict(mapping, context=dict(context, state_pointer=64.0)),
                    dict(mapping, context=dict(context, d=[False] * 8)),
                    dict(mapping, context=dict(context, state_pointer=65)),
                    dict(mapping, resources=[dict(resource, sha256='wrong')]),
                    dict(mapping, resources=[dict(resource, size=7)]),
                    dict(mapping, resources=[dict(resource, id=[])]),
                    dict(mapping, resources=[dict(resource, address=0xfffffffc)]),
                    dict(mapping, resources=[resource, resource])]:
            with self.assertRaises(ValueError): resolve(bad)
        self.assertFalse(resolve(dict(mapping, resources=[dict(resource, address=0x2000)]))['available'])

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
            # A rebuild on rewind used to pair the old directory with a new hash.
            replacement = bytearray(data)
            struct.pack_into('>H', replacement, 82, 1001)
            class RebuiltOnRewind(io.BytesIO):
                def seek(self, offset, whence=0):
                    if offset == 0 and whence == 0:
                        self.getbuffer()[:] = replacement
                    return super().seek(offset, whence)
            with patch.object(Path, 'open', return_value=RebuiltOnRewind(data)):
                captured = report.program_identity(path)
            self.assertEqual(captured['resources'][0]['id'], 1)
            self.assertEqual(captured['sha256'], hashlib.sha256(data).hexdigest())
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
