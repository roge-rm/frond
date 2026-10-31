#!/usr/bin/env python3
"""Zire 72 detection and rejection checks on the private Zire NOR, plus the E2 and TX still loading.

Skipped when the firmware isn't there. Mutated copies live in a temporary directory under build/;
the originals are only read and their SHA-256 is checked before and after.
"""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'out/frond'
PALM = Path('/srv/downloads/backup/deviceimages/palm')
ZIRE = PALM / 'Palm/Zire-72/Palm-Zire72-nor.bin'
ZIRE_SHA = 'f44dd41217682740b86714e77b5ca0ef41b1d0bed18361e80bcc833a40f6659b'
E2 = PALM / 'Palm/Tungsten-E2/Palm-Tungsten-E2-nor.bin'
TX = PALM / 'Palm/TX/Palm-TX-5.4-nor.bin'
TX_NAND = PALM / 'Palm/TX/Palm-TX-5.4-nand.bin'
HSPR = struct.pack('<I', 0x68737072)  # HAL IDs are stored little endian
REFUSED = "the Zire 72 isn't supported yet"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


@unittest.skipUnless(ZIRE.is_file() and CORE.is_file(), 'private Zire 72 firmware or core missing')
class Zire72RomTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.before = sha(ZIRE)
        assert cls.before == ZIRE_SHA, 'Zire 72 NOR is not the inventoried image'
        cls.temp = tempfile.TemporaryDirectory(prefix='zire72-rom-', dir=ROOT / 'build')
        cls.root = Path(cls.temp.name)
        cls.good = ZIRE.read_bytes()
        assert len(cls.good) == 14942208

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()
        assert sha(ZIRE) == cls.before, 'Zire 72 NOR changed'

    def run_core(self, args, timeout=60):
        return subprocess.run([str(CORE), *map(str, args)], capture_output=True, text=True, timeout=timeout,
                              env={**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy'})

    def rejected(self, args, message):
        result = self.run_core(args)
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertIn(message, result.stderr + result.stdout)
        if message != REFUSED:
            self.assertNotIn('supported yet', result.stderr)
        return result

    def mutated(self, name, data):
        path = self.root / name
        path.write_bytes(bytes(data))
        return path

    def patched(self, name, *edits):
        data = bytearray(self.good)
        for at, value in edits:
            data[at:at + len(value)] = value
        return self.mutated(name, data)

    def test_unchanged_image_is_recognised_then_refused(self):
        for args in ([ZIRE], [ZIRE, '--device', 'palm-zire-72']):
            result = self.rejected(args, REFUSED)
            self.assertIn('HAL ID: aZ72', result.stderr)
            self.assertIn('palm-zire-72 (Zire 72)', result.stderr)
            self.assertIn('using RAM size: 33554432 bytes', result.stderr)

    def test_nor_size(self):
        message = 'invalid NOR size; expected 14942208 bytes'
        for length in (len(self.good) - 1, 14876672, 8 << 20, 0x20000):
            self.rejected([self.mutated('short', self.good[:length])], message)
        self.rejected([self.mutated('padded', self.good + b'\xff' * ((16 << 20) - len(self.good)))], message)
        self.rejected([self.mutated('empty', b'')], 'neither a supported ROM nor a session image')
        self.rejected([self.mutated('tiny', self.good[:0x80])], 'neither a supported ROM nor a session image')
        self.rejected([self.mutated('small-card-only', self.good[:0x10000])], message)

    def test_hal_ids(self):
        both = self.patched('both-e2', (0x84, HSPR), (0x20084, HSPR))
        result = self.rejected([both], 'invalid NOR size; expected 8388608 bytes')
        self.assertIn('palm-e2', result.stderr)
        unknown = self.patched('both-unknown', (0x84, b'QQQQ'), (0x20084, b'QQQQ'))
        self.rejected([unknown], 'device: unknown')
        self.rejected([self.patched('big-only', (0x20084, b'QQQQ'))], 'invalid NOR layout')
        self.rejected([self.patched('big-e2', (0x20084, HSPR))], 'invalid NOR layout')
        self.rejected([self.patched('small-only', (0x84, b'QQQQ'))], 'device: unknown')

    def test_layout(self):
        self.rejected([self.patched('no-signature', (0x8, bytes(4)))], 'invalid NOR layout')
        for length in (0xe20001, 0xe1ffff, 0x10000, 0xffffffff):
            self.rejected([self.patched('big-size', (0x2006c, struct.pack('<I', length)))],
                          'invalid NOR layout')
        self.rejected([self.patched('big-name', (0x20010, b'OtherCard\0'))], 'invalid NOR layout')

    def test_other_devices_and_options(self):
        self.rejected([ZIRE, '--nand', self.mutated('nand', bytes(1 << 20))], 'invalid NAND size; expected 0 bytes')
        for ram in (16, 64, 8):
            self.rejected([ZIRE, '--ram-size', ram], 'unsupported RAM size')
        for display in ('320x480', '160x160'):
            self.rejected([ZIRE, '--display', display], 'unsupported display mode')
        for option in ('--fast-audio', '--experimental-fast-audio'):
            self.rejected([ZIRE, option], 'requires a verified TX 5.4 or Tungsten E2 NOR')
        self.rejected([ZIRE, '--device', 'palm-tx'], 'does not match ROM')
        self.rejected([ZIRE, '--device', 'palm-e2'], 'does not match ROM')
        self.rejected([ZIRE, '--experimental-tx'], 'Unknown argument: --experimental-tx')

    @unittest.skipUnless(E2.is_file() and TX.is_file() and TX_NAND.is_file(), 'E2 or TX firmware missing')
    def test_e2_and_tx_unchanged(self):
        hashes = {p: sha(p) for p in (E2, TX, TX_NAND)}
        self.rejected([TX], 'neither a supported ROM nor a session image')
        self.rejected([TX, '--device', 'palm-zire-72'], 'neither a supported ROM nor a session image')
        self.rejected([E2, '--device', 'palm-zire-72'], 'does not match ROM')
        self.rejected([E2, '--device', 'palm-tx', '--nand', TX_NAND], 'does not match ROM')
        e2 = self.run_core([E2, '--no-sound', '--run-for', '1'])
        self.assertEqual(e2.returncode, 0, e2.stderr)
        self.assertIn('palm-e2 (Tungsten E2)', e2.stderr)
        self.assertIn('using RAM size: 16777216 bytes', e2.stderr)
        tx = self.run_core([TX, '--device', 'palm-tx', '--nand', TX_NAND, '--no-sound', '--run-for', '1'])
        self.assertEqual(tx.returncode, 0, tx.stderr)
        self.assertIn('EXPERIMENTAL Palm TX', tx.stderr)
        self.assertEqual(hashes, {p: sha(p) for p in hashes})


if __name__ == '__main__':
    unittest.main()
