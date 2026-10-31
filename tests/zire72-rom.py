#!/usr/bin/env python3
"""Zire 72 detection, rejection and boot checks on the private Zire NOR, plus the E2 and TX still loading.

Skipped when the firmware isn't there. Mutated copies live in a temporary directory under build/;
the originals are only read and their SHA-256 is checked before and after.
"""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'out/frond'
PALM = Path('/srv/downloads/backup/deviceimages/palm')
ZIRE = PALM / 'Palm/Zire-72/Palm-Zire72-nor.bin'
ZIRE_SHA = 'f44dd41217682740b86714e77b5ca0ef41b1d0bed18361e80bcc833a40f6659b'
E2 = PALM / 'Palm/Tungsten-E2/Palm-Tungsten-E2-nor.bin'
TX = PALM / 'Palm/TX/Palm-TX-5.4-nor.bin'
TX_NAND = PALM / 'Palm/TX/Palm-TX-5.4-nand.bin'
HSPR = struct.pack('<I', 0x68737072)  # HAL IDs are stored little endian


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def png_rows(path):
    """Unfiltered rows of an 8-bit RGBA, non-interlaced PNG."""
    data, at, idat = path.read_bytes(), 8, b''
    while at < len(data):
        length, kind = struct.unpack('>I4s', data[at:at + 8])
        if kind == b'IHDR':
            width, height = struct.unpack('>II', data[at + 8:at + 16])
        elif kind == b'IDAT':
            idat += data[at + 8:at + 8 + length]
        at += 12 + length
    raw, stride, rows, prev = zlib.decompress(idat), width * 4, [], bytearray(width * 4)
    for y in range(height):
        kind, row = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = row[i - 4] if i >= 4 else 0
            b, c = prev[i], prev[i - 4] if i >= 4 else 0
            if kind == 1:
                row[i] = (row[i] + a) & 255
            elif kind == 2:
                row[i] = (row[i] + b) & 255
            elif kind == 3:
                row[i] = (row[i] + (a + b) // 2) & 255
            elif kind == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                row[i] = (row[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(row))
        prev = row
    return rows


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

    def test_unchanged_image_boots(self):
        for args in ([ZIRE], [ZIRE, '--device', 'palm-zire-72']):
            result = self.run_core([*args, '--no-sound', '--run-for', '12'])
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('HAL ID: aZ72', result.stderr)
            self.assertIn('palm-zire-72 (Zire 72)', result.stderr)
            self.assertIn('using RAM size: 33554432 bytes', result.stderr)
            self.assertIn('LCD=1', result.stdout)
            self.assertNotIn('Invalid instr', result.stderr)
            self.assertNotIn('failed (pc=', result.stderr)
            self.assertNotIn('Someone tried to set processor power mode', result.stderr)
            self.assertNotIn('framebuffer not in RAM', result.stderr)

    @unittest.skipUnless(os.environ.get('FROND_ZIRE_LAUNCHER'), 'set FROND_ZIRE_LAUNCHER=1 for the 70 s launcher run')
    def test_cold_boot_to_launcher_and_calc(self):
        # Calibration (three targets), language OK, Welcome Next, Next, Not now, Done, then Calc.
        out = Path(os.environ.get('FROND_ZIRE_EVIDENCE', self.root))
        out.mkdir(parents=True, exist_ok=True)
        taps = [(25500, 291, 292), (28500, 27, 27), (31500, 160, 160), (36000, 40, 300),
                (39000, 280, 304), (42500, 280, 304), (45500, 45, 302), (48500, 35, 304), (61000, 268, 70)]
        lines = []
        for at, x, y in taps:
            lines += [f'{at} pen-down {x} {y}', f'{at + 150} pen-up']
        lines.insert(-2, f'60000 snapshot "{out / "zire72-launcher.png"}"')
        lines.append(f'66000 snapshot "{out / "zire72-calc.png"}"')
        replay = self.root / 'launcher.replay'
        replay.write_text('\n'.join(lines) + '\n')
        result = self.run_core([ZIRE, '--no-sound', '--run-for', '67', '--input-replay', replay], timeout=180)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('black frame', result.stderr)
        launcher, calc = (out / 'zire72-launcher.png').read_bytes(), (out / 'zire72-calc.png').read_bytes()
        self.assertNotEqual(launcher, calc)

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


@unittest.skipUnless(ZIRE.is_file() and CORE.is_file() and os.environ.get('FROND_ZIRE_LAUNCHER'),
                     'set FROND_ZIRE_LAUNCHER=1 with the private Zire 72 firmware for the 4 minute key run')
class Zire72KeysTest(unittest.TestCase):
    """Hard keys, 5-way, sleep and wake, soft and hard reset from a session saved at the launcher.

    Frames are compared from row 30, below the clock in the title bar.
    """

    @classmethod
    def setUpClass(cls):
        cls.before = sha(ZIRE)
        assert cls.before == ZIRE_SHA, 'Zire 72 NOR is not the inventoried image'
        cls.temp = tempfile.TemporaryDirectory(prefix='zire72-keys-', dir=ROOT / 'build')
        cls.root = Path(cls.temp.name)
        cls.session = cls.root / 'launcher.session'
        # Calibration (three targets), language OK, Welcome Next, Next, Not now, Done.
        taps = [(25500, 291, 292), (28500, 27, 27), (31500, 160, 160), (36000, 40, 300),
                (39000, 280, 304), (42500, 280, 304), (45500, 45, 302), (48500, 35, 304)]
        lines = [f'24000 snapshot "{cls.root / "calibrate.png"}"']
        for at, x, y in taps:
            lines += [f'{at} pen-down {x} {y}', f'{at + 150} pen-up']
        lines.append(f'58000 snapshot "{cls.root / "launcher.png"}"')
        cls.drive([ZIRE], cls.replay('boot', lines), [(62, f'save-session "{cls.session}"'), (3, None)])
        assert cls.session.is_file(), 'launcher session not saved'

    @classmethod
    def tearDownClass(cls):
        if os.environ.get('FROND_ZIRE_EVIDENCE'):
            out = Path(os.environ['FROND_ZIRE_EVIDENCE'])
            out.mkdir(parents=True, exist_ok=True)
            for png in cls.root.glob('*.png'):
                (out / png.name).write_bytes(png.read_bytes())
        cls.temp.cleanup()
        assert sha(ZIRE) == cls.before, 'Zire 72 NOR changed'

    @classmethod
    def replay(cls, name, lines):
        path = cls.root / f'{name}.replay'
        path.write_text('\n'.join(lines) + '\n')
        return path

    @classmethod
    def drive(cls, image, replay, steps):
        """Runs the core in real time, sending console commands after each wait in seconds."""
        log = (cls.root / f'{replay.stem}.log').open('w')
        core = subprocess.Popen([str(CORE), *map(str, image), '--no-sound', '--input-replay', str(replay)],
                                stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, text=True,
                                env={**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy'})
        try:
            for wait, command in steps:
                time.sleep(wait)
                if command:
                    core.stdin.write(command + '\n')
                    core.stdin.flush()
            core.stdin.write('quit\n')
            core.stdin.flush()
            core.wait(timeout=30)
        finally:
            if core.poll() is None:
                core.kill()
                core.wait()
            core.stdin.close()
            log.close()
        return (cls.root / f'{replay.stem}.log').read_text()

    def keys(self, name, events, until):
        """Presses keys from the launcher session and returns the core's log."""
        lines = []
        for at, key in events:
            lines += [f'{at} key-down {key}', f'{at + 200} key-up {key}'] if key else []
        lines += [f'{at} snapshot "{self.root / f"{name}-{at}.png"}"' for at, key in events if not key]
        lines.sort(key=lambda line: int(line.split()[0]))
        result = subprocess.run([str(CORE), self.session, '--no-sound', '--run-for', str(until), '--input-replay',
                                 str(self.replay(name, lines))], capture_output=True, text=True, timeout=120,
                                env={**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy'})
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout + result.stderr

    def frame(self, name):
        return png_rows(self.root / f'{name}.png')

    def test_hard_keys_open_apps(self):
        launcher, seen = self.frame('launcher'), []
        for key in ('hard1', 'hard2', 'hard3'):
            self.keys(key, [(2000, key), (7000, None)], 8)
            frame = self.frame(f'{key}-7000')
            self.assertNotEqual(frame[30:], launcher[30:], key)
            self.assertNotIn(frame, seen, key)
            seen.append(frame)

    def test_five_way_moves_on_launcher(self):
        launcher = self.frame('launcher')
        for key in ('down', 'right'):
            self.keys(key, [(2000, key), (5000, None)], 6)
            self.assertNotEqual(self.frame(f'{key}-5000')[30:], launcher[30:], key)

    def test_sleep_and_wake(self):
        launcher = self.frame('launcher')
        log = self.keys('power', [(1000, 'power'), (4000, None), (8000, 'power'), (11000, None)], 12)
        self.assertIn('replay snapshot lcd-off', log)
        self.assertNotIn('framebuffer not in RAM', log)
        self.assertEqual(self.frame('power-11000')[30:], launcher[30:])
        log = self.keys('wake2', [(1000, 'power'), (4000, None), (8000, 'hard2'), (11000, None)], 12)
        self.assertIn('replay snapshot lcd-off', log)
        self.assertNotEqual(self.frame('wake2-11000')[30:], launcher[30:])

    def test_soft_and_hard_reset(self):
        # Soft reset keeps the store and lands on Prefs; hard reset erases it and boots to calibration.
        self.drive([self.session], self.replay('soft', [f'26000 snapshot "{self.root / "soft.png"}"']),
                   [(1, 'reset'), (27, None)])
        self.drive([self.session], self.replay('hard', [f'26000 snapshot "{self.root / "hard.png"}"']),
                   [(1, 'reset-hard'), (27, None)])
        soft, hard, calibrate = self.frame('soft'), self.frame('hard'), self.frame('calibrate')
        self.assertNotEqual(soft, hard)
        self.assertNotEqual(soft[30:], self.frame('launcher')[30:])
        self.assertEqual(hard[:100], calibrate[:100])  # the instructions; the device picture below is animated


if __name__ == '__main__':
    unittest.main()
