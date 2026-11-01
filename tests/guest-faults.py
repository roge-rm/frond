#!/usr/bin/env python3
"""Fault contracts bind private fixture identity, exact signatures and phase counts."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from guest_faults import check_diagnostics, parse_contract, observed_faults


class FaultContractTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'expect.json'
        self.data = {'version': 1, 'device': 'fixture', 'nor_sha256': 'a' * 64,
                     'phases': {'boot': [{'access': 'write', 'size': 4, 'va': '0x4',
                                          'pc': '0x20001000', 'fsr': 47, 'count': 1}],
                                'setup': [], 'reload': [], 'rollback': [], 'reset-soft': [], 'reset-hard': []}}
        self.line = 'W of 4 bytes to 0x00000004 failed (pc=0x20001000, fsr=47)!\n'

    def load(self, data=None, **changes):
        self.path.write_text(json.dumps(self.data if data is None else data))
        args = dict(device='fixture', actual_sha256='a' * 64, pinned_sha256='a' * 64,
                    launcher=True, resets=True)
        args.update(changes)
        return parse_contract(self.path.read_bytes(), **args)

    def test_exact_contract_and_empty_phase(self):
        contract = self.load()
        check_diagnostics('ordinary log\n> ' + self.line, 'boot', contract)
        check_diagnostics('ordinary log\n', 'reload', contract)
        self.assertEqual(sum(observed_faults(self.line).values()), 1)

    def test_strict_default_and_functional_diagnostics(self):
        with self.assertRaises(ValueError):
            check_diagnostics(self.line, 'boot')
        for message in ('Invalid instr', 'framebuffer not in RAM'):
            with self.assertRaises(ValueError):
                check_diagnostics(self.line + message, 'boot', self.load())

    def test_pinned_identity_and_functional_run_are_required(self):
        for change in ({'device': 'another'}, {'actual_sha256': 'b' * 64},
                       {'pinned_sha256': 'b' * 64}, {'pinned_sha256': None}, {'launcher': False}, {'resets': False}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.load(**change)
        bad = copy.deepcopy(self.data)
        bad['nor_sha256'] = 'b' * 64
        with self.assertRaises(ValueError):
            self.load(bad)

    def test_every_signature_field_and_count_is_exact(self):
        contract = self.load()
        for line in (self.line.replace('W of', 'R of'), self.line.replace('4 bytes', '2 bytes'),
                     self.line.replace('00000004', '00000000'), self.line.replace('20001000', '20001004'),
                     self.line.replace('fsr=47', 'fsr=10'), self.line * 2, '',
                     self.line + self.line.replace('20001000', '20002000')):
            with self.subTest(line=line), self.assertRaises(ValueError):
                check_diagnostics(line, 'boot', contract)
        with self.assertRaises(ValueError):
            check_diagnostics(self.line, 'reload', contract)

    def test_malformed_and_duplicate_input(self):
        for key, value in [('version', 2), ('version', True), ('phases', []), ('extra', 1)]:
            bad = copy.deepcopy(self.data)
            bad[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                self.load(bad)
        for key, value in [('access', 'execute'), ('size', 3), ('size', True), ('va', '4'),
                           ('pc', '0x100000000'), ('fsr', -1), ('count', 0), ('count', True)]:
            bad = copy.deepcopy(self.data)
            bad['phases']['boot'][0][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                self.load(bad)
        for phase in ('missing', 'duplicate', 'unknown'):
            bad = copy.deepcopy(self.data)
            if phase == 'missing': del bad['phases']['setup']
            if phase == 'duplicate': bad['phases']['boot'] *= 2
            if phase == 'unknown': bad['phases']['invented'] = []
            with self.subTest(phase=phase), self.assertRaises(ValueError):
                self.load(bad)
        self.path.write_text('{"version":1,"version":1}')
        with self.assertRaises(ValueError):
            parse_contract(self.path.read_bytes(), device='fixture', actual_sha256='a'*64, pinned_sha256='a'*64,
                          launcher=True, resets=True)

    def test_unknown_diagnostic_is_not_silently_ignored(self):
        for line in (self.line.rstrip() + ' extra', self.line.replace('fsr=47', 'fsr=?'),
                     'memory fault in PACE writing at 0x00000000',
                     'W of 4 bytes to 0x4 failed without a decoded PC'):
            with self.assertRaises(ValueError):
                check_diagnostics(line, 'boot', self.load())


if __name__ == '__main__':
    unittest.main()
