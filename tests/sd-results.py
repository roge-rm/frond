#!/usr/bin/env python3
"""Firmware-free regressions for the SD acceptance result checker."""
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

from sd_results import ControlError, check_records, export_if_ready, read_log


def invocation(size=32, stamp=100, host=True, prior=False):
    entries = [('begin', 0, stamp), ('enumerate', 0, 2), ('volume-size', 0, (size << 20) - 65536)]
    entries += [('host-open', 0 if host else 0x2a08, 0)]
    if host:
        entries += [('host-read', 0, 16), ('host-match', 0, 0), ('host-close', 0, 0)]
    entries += [('prior-open', 0 if prior else 0x2a08, 0)]
    if prior:
        entries += [('prior-read', 0, 19), ('prior-match', 0, 0), ('prior-close', 0, 0)]
    entries += [('guest-open', 0, 0), ('guest-write', 0, 19), ('guest-close', 0, 0),
                ('guest-reopen', 0, 0), ('guest-read', 0, 19), ('guest-match', 0, 0),
                ('guest-read-close', 0, 0), ('complete', 0, stamp)]
    return [f'{label} err={error:04X} value={value}' for label, error, value in entries]


class Results(unittest.TestCase):
    def test_only_not_ready_can_be_retried(self):
        replies = iter(['not-ready', 'not-ready', None, 'export'])
        calls = []
        def request(command, path):
            calls.append((command, path))
            error = next(replies)
            if error:
                raise ControlError(error, 'test failure')
        self.assertFalse(export_if_ready(request, 'log.zip'))
        self.assertFalse(export_if_ready(request, 'log.zip'))
        self.assertTrue(export_if_ready(request, 'log.zip'))
        with self.assertRaises(ControlError) as raised:
            export_if_ready(request, 'log.zip')
        self.assertEqual(raised.exception.code, 'export')
        self.assertEqual(calls, [('export', 'log.zip')] * 4)

    def test_fresh_invocations_and_capacity(self):
        first = invocation()
        check_records(first, 32)
        second = invocation(stamp=200, prior=True)
        check_records(first + second, 32, previous=first, prior=True)
        check_records(first + invocation(128, 300, host=False), 128, previous=first, host=False)

    def test_stale_and_incomplete_results_fail(self):
        old = invocation()
        for lines in (old, old + invocation(stamp=200)[:-1], old + ['begin err=0000 value=200']):
            with self.subTest(lines=lines), self.assertRaises(AssertionError):
                check_records(lines, 32, previous=old)

    def test_io_failures_and_wrong_card_fail(self):
        good = invocation()
        for label in ('guest-open', 'guest-write', 'guest-close', 'guest-reopen', 'guest-read', 'guest-read-close'):
            broken = [line.replace('err=0000', 'err=2A01') if line.startswith(label + ' ') else line for line in good]
            with self.subTest(label=label), self.assertRaises(AssertionError):
                check_records(broken, 32)
        with self.assertRaises(AssertionError):
            check_records(good, 128)
        with self.assertRaises(AssertionError):
            check_records(good, 32, host=False)
        for label in ('guest-read', 'guest-write'):
            short = [line.replace('value=19', 'value=18') if line.startswith(label + ' ') else line for line in good]
            with self.subTest(label=label), self.assertRaises(AssertionError):
                check_records(short, 32)
        with self.assertRaises(AssertionError):
            check_records(good[:-1] + ['complete err=0000 value=99'], 32)

    def test_palm_record_archive(self):
        lines = invocation()
        raw = bytearray(78 + len(lines) * 8 + 2)
        struct.pack_into('>H', raw, 76, len(lines))
        for i, line in enumerate(lines):
            struct.pack_into('>I', raw, 78 + i * 8, len(raw))
            raw.extend(line.encode() + b'\0')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'log.zip'
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr('FrondSDLog.pdb', raw)
            self.assertEqual(read_log(path), lines)
            struct.pack_into('>I', raw, 78, len(raw) + 1)
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr('FrondSDLog.pdb', raw)
            with self.assertRaises(AssertionError):
                read_log(path)


if __name__ == '__main__':
    unittest.main()
