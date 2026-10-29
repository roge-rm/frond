#!/usr/bin/env python3
"""Firmware-free rejection tests for the guest audio acceptance checker."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location(
    'audio_results', Path(__file__).resolve().parents[1] / 'tools/check-audio-probe.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def result(label, value=0):
    return f'{label} err=0000 value={value} tick=100'


def block(seed, sequence, channel, capacity=4096):
    actual = capacity // 2 if seed == 2 and sequence == 1 else 0 if seed == 2 and sequence == 2 else capacity
    frames = actual // 4
    # Independent closed-form oracle: opposite stereo samples sum to 65536,
    # except at the (at most one) zero crossing in this block.
    zero = ((2048 - seed * 97 - sequence * 131) * pow(17, -1, 4096)) & 4095
    checksum = (frames - (zero < frames)) * 65536
    return (f'block seed={seed} seq={sequence} capacity={capacity} actual={actual} '
            f'checksum={checksum} channel={channel}')


def invocation(capacity=4096):
    lines = ['audio-probe-v1 signed16le stereo44100 full-half-zero-full']
    for seed in (1, 2, 3):
        lines += [result('case', seed), result('create-extended' if seed == 2 else 'create-basic', 100),
                  result('volume-unity'), result('start'), result('pause'), result('pause-return-calls', 2),
                  result('paused-calls', 2), result('resume'), result('volume-half'),
                  result('get-volume', 512), result('pan-left'), result('get-pan', 0xfffffc00),
                  result('pan-center'), result('stop'), result('stop-return-calls', 3),
                  result('stopped-calls', 3), result('restart'), result('delete'),
                  result('callback-count', 4)]
        lines += [block(seed, sequence, 101 if seed in (1, 3) else 102, capacity) for sequence in range(4)]
    lines += ['pair-begin', result('pair-create', 0), result('pair-start', 0),
              result('pair-create', 1), result('pair-start', 1)]
    for seed in (4, 5):
        lines += [result('pair-delete', seed - 4), result('callback-count', 4)]
        lines += [block(seed, sequence, 100 + seed, capacity) for sequence in range(4)]
    return lines + ['pair-end', 'complete']


class Results(unittest.TestCase):
    def test_complete_and_reused_address(self):
        verdict = checker.check(invocation())
        self.assertEqual(verdict['status'], 'pass')
        self.assertTrue(verdict['address_reused'])
        self.assertEqual(verdict['callbacks'], {str(i): 4 for i in range(1, 6)})

    def test_corruptions_cannot_pass(self):
        good = invocation()
        cases = {
            'incomplete': good[:-1],
            'reordered': [x.replace('seed=1 seq=1 ', 'seed=1 seq=2 ') for x in good],
            'missing': [x for x in good if not x.startswith('block seed=1 seq=1 ')],
            'wrong-length': [x.replace('actual=2048', 'actual=2044') for x in good],
            'bad-checksum': [x.replace('checksum=0 ', 'checksum=1 ') for x in good],
            'api-error': [x.replace('start err=0000', 'start err=0801') for x in good],
            'overflow': [x.replace('callback-count err=0000', 'callback-count err=0001') for x in good],
            'post-pause': [x.replace('paused-calls err=0000 value=2', 'paused-calls err=0000 value=3') for x in good],
            'post-stop': [x.replace('stopped-calls err=0000 value=3', 'stopped-calls err=0000 value=4') for x in good],
            'missing-second-start': [x for x in good if x != result('pair-start', 1)],
            'wrong-volume': [x.replace('get-volume err=0000 value=512', 'get-volume err=0000 value=513') for x in good],
            'unknown-seed': [x.replace('seed=5 ', 'seed=6 ') for x in good],
            'invalid-channel': [x.replace('channel=105', 'channel=0') for x in good],
            'unknown-record': good[:-1] + ['unknown'] + good[-1:],
            'orphan-operation': good[:1] + [result('unknown-operation')] + good[1:],
            'unknown-pair-operation': [result('unknown-pair') if x == result('pair-start', 1) else x for x in good],
        }
        for label, lines in cases.items():
            with self.subTest(label=label):
                self.assertNotEqual(lines, good)
                with self.assertRaises(ValueError):
                    checker.check(lines)

    def test_preferred_buffer_size_is_not_a_guarantee(self):
        self.assertEqual(checker.check(invocation(2048))['status'], 'pass')

    def test_archive_boundaries(self):
        lines = invocation()
        raw = bytearray(78 + len(lines) * 8)
        struct.pack_into('>H', raw, 76, len(lines))
        for i, line in enumerate(lines):
            struct.pack_into('>I', raw, 78 + i * 8, len(raw))
            raw.extend(line.encode() + b'\0')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'log.zip'
            def write(data, name='FrondAudioLog.pdb'):
                with zipfile.ZipFile(path, 'w') as archive:
                    archive.writestr(name, data)
            write(raw)
            self.assertEqual(checker.read_log(path), lines)
            for data in (raw[:77], raw[:80]):
                write(data)
                with self.assertRaises(ValueError):
                    checker.read_log(path)
            damaged = raw[:]
            struct.pack_into('>I', damaged, 78, 0)
            write(damaged)
            with self.assertRaises(ValueError):
                checker.read_log(path)
            write(raw, 'OtherLog.pdb')
            with self.assertRaises(ValueError):
                checker.read_log(path)


if __name__ == '__main__':
    unittest.main()
