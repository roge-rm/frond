#!/usr/bin/env python3
"""Firmware-free regressions for the SD acceptance result checker."""
from pathlib import Path
import json
import struct
import tempfile
import unittest
import zipfile

from sd_results import ControlError, ProbeTimeout, ResetMountReadiness, check_records, export_if_ready, read_log, wait_for_probe


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


def empty_mount(stamp=200):
    return [f'begin err=0000 value={stamp}', 'enumerate err=290D value=0', f'complete err=290D value={stamp}']


def write_log(path, lines):
    raw = bytearray(78 + len(lines) * 8 + 2)
    struct.pack_into('>H', raw, 76, len(lines))
    for i, line in enumerate(lines):
        struct.pack_into('>I', raw, 78 + i * 8, len(raw))
        raw.extend(line.encode() + b'\0')
    with zipfile.ZipFile(path, 'w') as archive:
        archive.writestr('FrondSDLog.pdb', raw)


class Results(unittest.TestCase):
    def test_reset_requires_new_complete_success_after_empty(self):
        old, empty, good = invocation(prior=True), empty_mount(), invocation(stamp=300, prior=True)
        monitor = ResetMountReadiness(old)
        self.assertIsNone(monitor.observe(old + empty))
        self.assertIsNone(monitor.observe(old + empty))  # A stale failed run cannot count as a new attempt.
        self.assertIsNone(monitor.observe(old + empty + good[:-1]))
        self.assertEqual(monitor.observe(old + empty + good), old + empty + good)
        self.assertEqual([x['state'] for x in monitor.observations], ['mount-pending', 'mounted'])

    def test_reset_does_not_hide_io_capacity_or_other_enumeration_errors(self):
        old, empty = invocation(prior=True), empty_mount()
        good = invocation(stamp=300, prior=True)
        bad_io = [x.replace('value=19', 'value=18') if x.startswith('guest-write ') else x for x in good]
        bad_enum = [x.replace('290D', '2903') for x in empty_mount(300)]
        for bad in (bad_io, invocation(64, 300, prior=True), bad_enum):
            with self.subTest(bad=bad), self.assertRaises(AssertionError):
                # Even an earlier bad invocation followed by good results must fail.
                ResetMountReadiness(old).observe(old + empty + bad + good)

    def test_non_reset_rejects_even_recovered_empty_and_guest_deadline_is_fatal(self):
        old, empty, good = invocation(prior=True), empty_mount(), invocation(stamp=300, prior=True)
        with self.assertRaises(AssertionError):
            ResetMountReadiness(old, allow_empty=False).observe(old + empty + good)
        deadline = ['begin err=0000 value=3200', 'mount-timeout err=290D value=30', 'complete err=290D value=3200']
        with self.assertRaisesRegex(AssertionError, 'guest deadline'):
            ResetMountReadiness(old).observe(old + empty + deadline + good)

    def run_mount_wait(self, mode):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / 'reset.zip'
        old, empty, good = invocation(prior=True), empty_mount(), invocation(stamp=300, prior=True)
        clock, calls, apps, exports = [0.0], [], [], [0]
        def request(command, argument='', ok=True, *, deadline=None):
            calls.append((command, deadline))
            if mode == 'late':
                clock[0] = 3
                return True, ''
            if command == 'launch-app':
                apps.append(argument)
            if command == 'export':
                exports[0] += 1
                lines = old + empty
                if exports[0] > 1 and mode != 'stale':
                    if mode == 'io-error':
                        lines += [x.replace('err=0000', 'err=2A01') if x.startswith('guest-read ') else x for x in good]
                    else:
                        lines += good
                write_log(argument, lines)
            return (True, '') if ok is None else ''
        def sleep(seconds):
            clock[0] += seconds
        try:
            result = wait_for_probe(request, path, previous=old, prior=True, reset_mount=True,
                                    timeout=2, now=lambda: clock[0], sleep=sleep)
        except (AssertionError, ProbeTimeout) as error:
            result = error
        report = json.loads(path.with_name('reset-mount-readiness.json').read_text())
        return result, report, path, calls, apps, clock[0]

    def test_mount_wait_archives_failures_then_requires_fresh_success(self):
        result, report, path, calls, apps, elapsed = self.run_mount_wait('success')
        self.assertIsInstance(result, list)
        self.assertEqual(report['status'], 'mounted')
        self.assertEqual(apps, ['Frond SD probe'])  # No input or third-party app is used.
        self.assertEqual(len(report['observations']), 2)
        first = report['observations'][0]
        self.assertEqual(first['invocations'][0]['records'], empty_mount())
        self.assertEqual(read_log(Path(first['archive']))[-3:], empty_mount())
        self.assertTrue(all(deadline == 2 for _, deadline in calls))
        self.assertLess(elapsed, 2)

    def test_mount_wait_deadline_rejects_stale_and_late_results(self):
        for mode in ('stale', 'late'):
            with self.subTest(mode=mode):
                result, report, path, calls, apps, elapsed = self.run_mount_wait(mode)
                self.assertIsInstance(result, ProbeTimeout)
                self.assertEqual(report['status'], 'timed-out')
                if mode == 'stale':
                    self.assertEqual(elapsed, 2)
                    self.assertTrue(report['observations'])
                    self.assertTrue(all(Path(x['archive']).is_file() for x in report['observations']))

    def test_mount_wait_preserves_real_io_failure_without_retry(self):
        result, report, path, calls, apps, elapsed = self.run_mount_wait('io-error')
        self.assertIsInstance(result, AssertionError)
        self.assertEqual(report['status'], 'failed')
        self.assertEqual(len(report['observations']), 2)
        self.assertEqual(report['observations'][-1]['state'], 'failed')
        self.assertEqual(sum(command == 'export' for command, _ in calls), 2)

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
