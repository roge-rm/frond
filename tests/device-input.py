#!/usr/bin/env python3
"""Reject synthetic device/cohort inputs before creating a SoC or SDL window."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'out/frond'


def header(hal=0x68737072, size=256, at=0):
    data = bytearray(size)
    struct.pack_into('<I', data, at + 8, 0xfeedbeef)
    struct.pack_into('<II', data, at + 0x80, 0x50616c6d, hal)
    return data


def e2_nor(size=8 << 20, at=0x20000):
    """E2 NOR layout: card header after the 128 KiB boot block."""
    return header(size=size, at=at)


def session(path, device, *, nor=None, nand=0, display=6, ram=32 << 20, sd=False):
    nor = header() if nor is None else nor
    # A hand-built 0.2.0 envelope, independent of the C++ writer.
    memory, state, card = b'RAM', b'STATE', bytes(512) if sd else b''
    fields = [0x19800819, 5 if sd else 4, device & 0xffffffff, display, ram,
              0, len(nor), nand, len(memory), len(state)]
    if sd:
        fields.append(len(card))
    compressor = zlib.compressobj()
    with path.open('wb') as stream:
        stream.write(struct.pack('<' + 'I' * len(fields), *fields))
        stream.write(compressor.compress(nor))
        block = bytes(1 << 20)
        remaining = nand
        while remaining:
            size = min(remaining, len(block))
            stream.write(compressor.compress(block[:size]))
            remaining -= size
        stream.write(compressor.compress(memory + state + card))
        stream.write(compressor.flush())
    return path


class DeviceInputTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='device-input-', dir=ROOT / 'build')
        cls.root = Path(cls.temp.name)
        cls.nor = cls.root / 'nor'
        cls.nand = cls.root / 'nand'
        cls.e2 = cls.root / 'e2'
        cls.e2.write_bytes(e2_nor())
        for file, size in ((cls.nor, 16 << 20), (cls.nand, 138412032)):
            with file.open('wb') as stream:
                stream.truncate(size)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def rejected(self, args, message):
        result = subprocess.run([str(CORE), *map(str, args)], capture_output=True, text=True,
                                timeout=10, env={**os.environ, 'SDL_VIDEODRIVER': 'dummy',
                                                 'SDL_AUDIODRIVER': 'dummy'})
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertIn(message, result.stderr + result.stdout)
        self.assertNotIn('SDL', result.stderr)
        return result

    def test_inventory(self):
        data = json.loads(subprocess.check_output([ROOT / 'out/frond-device-profiles']))
        self.assertEqual([p['id'] for p in data['profiles'] if p['status'] == 'tx-desktop'], ['palm-tx'])
        self.assertEqual(len(data['profiles']), 4)
        self.assertEqual({p['cohort'] for p in data['profiles']},
                         {'tx-nor-nand', 'e2-nor-nand', 'patched-e2', 'paravirtual'})
        self.assertTrue(all(p['status'] == 'inherited-unverified' for p in data['profiles'][:3]))

    def test_tx_raw_geometry_and_options(self):
        base = [self.nor, '--experimental-tx']
        self.rejected(base, 'explicit --nand')
        self.rejected(base + ['--nand', self.e2], 'invalid NAND size')
        self.rejected([self.e2, '--experimental-tx', '--nand', self.nand], 'does not match ROM')
        self.rejected(base + ['--nand', self.nand, '--ram-size', '16'], 'unsupported RAM size')
        self.rejected(base + ['--nand', self.nand, '--ram-size', '0'], 'unsupported RAM size')
        self.rejected(base + ['--nand', self.nand, '--ram-size', '4096'], 'overflows')
        self.rejected(base + ['--nand', self.nand, '--display', '320x320'], 'unsupported display mode')
        self.rejected([self.e2, '--nand', self.nand], 'invalid NAND size')

    def test_e2_nor_size_and_layout(self):
        cases = ((e2_nor(4 << 20), 'invalid NOR size; expected 8388608 bytes'),
                 (e2_nor((8 << 20) + 1), 'invalid NOR size; expected 8388608 bytes'),
                 (header(), 'invalid NOR size; expected 8388608 bytes'),
                 (e2_nor(at=0), 'invalid NOR layout'),
                 (e2_nor(at=0x40000), 'invalid NOR layout'))
        for index, (data, message) in enumerate(cases):
            with self.subTest(case=index):
                rom = self.root / f'e2-bad-{index}'
                rom.write_bytes(data)
                self.rejected([rom, '--run-for', '1'], message)
        # E2 sessions saved before these checks keep their NOR; the synthetic state then fails.
        for index, nor in enumerate((e2_nor(4 << 20), e2_nor(16 << 20), e2_nor(at=0))):
            with self.subTest(session=index):
                path = session(self.root / f'e2-old-session-{index}', 0, nor=nor, nand=34603008,
                               display=5)
                result = self.rejected([path, '--run-for', '1'], 'failed to restore savestate')
                self.assertNotIn('invalid', result.stderr)

    def test_inherited_cohorts_cannot_use_desktop(self):
        for hal in (0x68737072, 0x446d4752, 0x50765076):
            with self.subTest(hal=hal):
                rom = self.root / 'inherited'
                rom.write_bytes(e2_nor() if hal == 0x68737072 else header(hal))
                self.rejected([rom, '--desktop-socket', self.root / 'unused'], 'Palm TX only')

    def test_e2_replay_bounds_and_physical_app_buttons(self):
        replay = self.root / 'e2.replay'
        # A duration rejection proves parsing succeeded, without executing synthetic ROM.
        # Existing aliases keep their TX meaning; physical names avoid claiming an E2 Home key.
        for key in ('hard1', 'hard2', 'hard3', 'hard4', 'home', 'calendar', 'contacts', 'web',
                    'up', 'down', 'left', 'right', 'center', 'power'):
            replay.write_text(f'0 key-down {key}\n1000 key-up {key}\n')
            self.rejected([self.e2, '--input-replay', replay, '--run-for', '1'],
                          'input replay extends beyond')
        replay.write_text('0 pen-down 319 439\n1000 pen-up\n')
        self.rejected([self.e2, '--input-replay', replay, '--run-for', '1'],
                      'input replay extends beyond')
        for x, y in ((320, 319), (319, 440), (-1, 0)):
            replay.write_text(f'0 pen-down {x} {y}\n')
            self.rejected([self.e2, '--input-replay', replay, '--run-for', '1'],
                          'invalid replay event')

    def test_unknown_and_mismatched_session_ids(self):
        for device in (-1, 4, 0x7fffffff):
            with self.subTest(device=device):
                path = session(self.root / 'unknown', device)
                self.rejected([path], 'unknown session device profile')
        path = session(self.root / 'mismatch', 1)
        self.rejected([path], 'does not match ROM')
        path = session(self.root / 'tx-tag-e2-rom', 3)
        self.rejected([path], 'does not match ROM')
        path = session(self.root / 'e2', 0)
        self.rejected([path, '--experimental-tx'], "different device's session")

    def test_session_ram_display_and_corruption(self):
        path = session(self.root / 'display', 2, nor=header(0x50765076), display=99)
        self.rejected([path], 'unsupported display mode')
        path = session(self.root / 'ram', 2, nor=header(0x50765076), ram=24 << 20)
        self.rejected([path], 'unsupported RAM size')
        path = session(self.root / 'broken', 0)
        path.write_bytes(path.read_bytes()[:-1])
        self.rejected([path], 'invalid session image')

    def test_020_tx_v4_v5_envelopes(self):
        # Check legacy decoding and TX routing using an intentional pre-SoC option rejection.
        for sd in (False, True):
            with self.subTest(sd=sd):
                path = session(self.root / 'tx', 3, nor=bytes(16 << 20), nand=138412032, sd=sd)
                self.rejected([path, '--ram-size', '32'], 'cannot specify RAM size for an existing session')
                self.rejected([path, '--nand', self.nand], 'separate NAND image cannot be used')


if __name__ == '__main__':
    unittest.main()
