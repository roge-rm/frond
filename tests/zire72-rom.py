#!/usr/bin/env python3
"""Zire 72 detection, rejection and boot checks on the private Zire NOR, plus the E2 and TX still loading.

Skipped when the firmware isn't there. FROND_ZIRE_LAUNCHER=1 adds the launcher and key runs, FROND_ZIRE_STORAGE=1
the SD card, session and memo runs, FROND_HARD_RESET=1 the TX and E2 erase prompt after reset-hard,
FROND_ZIRE_PERIPHERALS=1 USB HotSync, IrDA beaming, the infrared keyboard, Bluetooth pairing and the camera stub. Mutated copies live in a temporary directory under build/;
the originals are only read and their SHA-256 is checked before and after.
"""
import hashlib
import os
import re
import shutil
import socket
from pathlib import Path
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'out/frond'
PALM = Path('firmware/palm')
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


def date_time_prefs(rows):
    """Prefs > Date & Time: the long blue Preferences tab, the panel title and a white body."""
    def px(x, y):
        return rows[y][4 * x:4 * x + 3]
    tab = sum(1 for x in range(100, 128) if px(x, 12)[2] > 150 and px(x, 12)[0] < 80)
    title = sum(1 for x in range(200, 316) for y in range(6, 20) if sum(px(x, y)) < 200)
    white = sum(1 for x in range(0, 320, 4) for y in range(150, 280, 4) if min(px(x, y)) > 230)
    return tab >= 8 and 400 <= title <= 600 and white > 0.9 * 80 * 33


class Core:
    """A core process driven over its console; waits on its output and frames, not on the clock."""

    def __init__(self, image, log, *args):
        self.lines, self.log = [], log.open('w')
        self.process = subprocess.Popen([str(CORE), *map(str, image), '--no-sound', *map(str, args)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, errors='replace',
                                        env={**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy'})
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
        self.snaps = 0

    def read(self):
        for line in self.process.stdout:
            self.lines.append(line)
            self.log.write(line)

    def send(self, command, expect=None, timeout=30):
        """Sends a console command; with expect, returns the first later line holding one of its strings."""
        mark = len(self.lines)
        self.process.stdin.write(command + '\n')
        self.process.stdin.flush()
        if expect is None:
            return None
        expect = (expect,) if isinstance(expect, str) else expect
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for line in self.lines[mark:]:
                if any(e in line for e in expect):
                    return line
            if self.process.poll() is not None:
                break
            time.sleep(0.1)
        raise AssertionError(f'no {expect} after {command}')

    def until(self, command, expect, done='', timeout=60):
        """Repeats a command until its reply holds done, e.g. while Palm OS isn't ready for host calls."""
        end = time.monotonic() + timeout
        while True:
            line = self.send(command, expect)
            if done in line:
                return line
            if time.monotonic() > end:
                raise AssertionError(f'{command}: no {done!r} after {timeout} s; last reply {line.strip()!r}')
            time.sleep(1)

    def launch(self, name, timeout=60):
        """Launches an app by database name, retrying while Palm OS isn't at a point that takes host calls."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            mark = len(self.lines)
            self.send(f'launch "{name}"')
            time.sleep(3)
            if not any('launch' in line and ('could not' in line or 'failed' in line) for line in self.lines[mark:]):
                return
        raise AssertionError(f'{name} did not launch')

    def replay(self, root, lines):
        self.snaps += 1
        path = root / f'console-{self.snaps}.replay'
        path.write_text('\n'.join(lines) + '\n')
        return self.send(f'input-replay "{path}"', 'input replay loaded')

    # A new replay replaces the running one, so these wait for their release to go out.
    def released(self, mark):
        end = time.monotonic() + 10
        while time.monotonic() < end:
            if any('replay event 2 ' in line for line in self.lines[mark:]):
                return
            time.sleep(0.05)
        raise AssertionError('replay release not sent')

    def key(self, root, name):
        mark = len(self.lines)
        self.replay(root, [f'0 key-down {name}', f'200 key-up {name}'])
        self.released(mark)

    def tap(self, root, x, y):
        mark = len(self.lines)
        self.replay(root, [f'0 pen-down {x} {y}', f'150 pen-up'])
        self.released(mark)

    def frame(self, path, timeout=10):
        """Saves a frame and returns its rows, or None with the LCD off."""
        path.unlink(missing_ok=True)
        mark = len(self.lines)
        self.replay(path.parent, [f'0 snapshot "{path}"'])
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if any('lcd-off' in line for line in self.lines[mark:]):
                return None
            if path.is_file():
                time.sleep(0.2)
                return png_rows(path)
            time.sleep(0.1)
        raise AssertionError('no frame')

    def wait_frame(self, path, check, timeout=60):
        """Polls frames until check(rows) holds; rows is None while the LCD is off."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            rows = self.frame(path)
            if check(rows):
                return rows
            time.sleep(1)
        raise AssertionError(f'{path.name}: frame never matched')

    def output(self):
        return ''.join(self.lines)

    def close(self):
        try:
            if self.process.poll() is None:
                self.process.stdin.write('quit\n')
                self.process.stdin.flush()
                self.process.wait(timeout=30)
        except (BrokenPipeError, subprocess.TimeoutExpired):
            self.process.kill()
            self.process.wait()
        self.reader.join(timeout=5)
        self.log.close()
        return self.process.returncode


def boot_to_launcher(root):
    """Cold boots the real NOR through calibration and setup and saves a session at the launcher.

    Each tap waits for its screen: the red target under the pen for calibration, then a new title that has
    stopped changing. With the console running, guest time drifts from host time too much for fixed times.
    """
    session = root / 'launcher.session'
    core = Core([ZIRE], root / 'boot.log')

    def red(rows, x, y):
        # The target's red rings around its centre.
        return sum(1 for dx in range(-12, 13, 3) for dy in range(-12, 13, 3)
                   if 0 <= x + dx < 320 and 0 <= y + dy < 320 and rows[y + dy][4 * (x + dx)] > 180
                   and rows[y + dy][4 * (x + dx) + 1] < 80) > 10

    try:
        frame = root / 'boot-frame.png'
        for x, y in [(291, 292), (27, 27), (160, 160)]:
            rows = core.wait_frame(frame, lambda rows: rows is not None and red(rows, x, y), 120)
            if x == 291:
                (root / 'calibrate.png').write_bytes(frame.read_bytes())
            core.tap(root, x, y)
        title = rows[:24]
        # Language OK, Welcome Next, Next, Not now, Done.
        for step, (x, y) in enumerate([(40, 300), (280, 304), (280, 304), (45, 302), (35, 304)]):
            seen = []

            def settled(rows):
                seen.append(None if rows is None else rows[:24])
                # A title bar, not a blank screen between forms.
                inked = rows is not None and sum(1 for row in rows[:24] for i in range(0, 1280, 16) if row[i] < 128) > 20
                return inked and rows[:24] != title and len(seen) > 1 and seen[-1] == seen[-2]
            rows = core.wait_frame(frame, settled, 30)
            (root / f'setup-{step}.png').write_bytes(frame.read_bytes())
            title = rows[:24]
            core.tap(root, x, y)
        # The launcher's wallpaper: most of the lower screen is coloured, not white or grey.
        core.wait_frame(root / 'launcher.png', lambda rows: rows is not None and sum(
            1 for x in range(0, 320, 8) for y in range(150, 280, 8)
            if max(rows[y][4 * x:4 * x + 3]) - min(rows[y][4 * x:4 * x + 3]) > 30) > 400, 60)
        time.sleep(1)
        core.send(f'save-session "{session}"', 'wrote session')
    finally:
        core.close()
    assert session.is_file(), 'launcher session not saved'
    return session

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
        # Sessions hold cards up to 128 MiB, so the Zire takes 32-128 MiB like the TX.
        for mib in (16, 129, 256):
            card = self.root / f'card-{mib}.img'
            with card.open('wb') as f:
                f.truncate(mib << 20)
            self.rejected([ZIRE, '--sd', card], 'sd card image has bad size')
            card.unlink()

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
        cls.session = boot_to_launcher(cls.root)

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

    def test_short_power_presses(self):
        # 100 ms presses, like a phone tap: one as soon as the LCD goes off (the DAL may still be going
        # to sleep), one once it's well asleep. Both wake it.
        core = Core([self.session], self.root / 'short.log')

        def tap_power():
            mark = len(core.lines)
            core.replay(self.root, ['0 key-down power', '100 key-up power'])
            core.released(mark)
        try:
            for name, settle in (('entry', 0), ('asleep', 3)):
                core.key(self.root, 'power')
                core.wait_frame(self.root / f'short-{name}-off.png', lambda rows: rows is None, 20)
                time.sleep(settle)
                tap_power()
                core.wait_frame(self.root / f'short-{name}-on.png', lambda rows: rows is not None, 10)
        finally:
            self.assertEqual(core.close(), 0)

    def test_soft_and_hard_reset(self):
        # Soft reset keeps the store and lands on Prefs > Date & Time; hard reset erases it and boots to
        # calibration. Frames are polled until they match, with the LCD off or the logo in between.
        calibrate = self.frame('calibrate')
        core = Core([self.session], self.root / 'soft.log')
        try:
            core.send('reset')
            core.wait_frame(self.root / 'soft.png', lambda rows: rows is not None and date_time_prefs(rows))
        finally:
            self.assertEqual(core.close(), 0)
        core = Core([self.session], self.root / 'hard.log')
        try:
            core.send('reset-hard')
            # The instructions; the device picture below is animated.
            core.wait_frame(self.root / 'hard.png', lambda rows: rows is not None and rows[:100] == calibrate[:100])
        finally:
            self.assertEqual(core.close(), 0)
        self.assertFalse(date_time_prefs(self.frame('launcher')))
        self.assertFalse(date_time_prefs(calibrate))

def erase_prompt(rows, tx):
    """The DAL's "Erase all data?" prompt: on the TX the red Yes and blue No rows, on the E2 a block of
    black text in the middle of a white screen."""
    if rows is None:
        return False
    def px(x, y):
        return rows[y][4 * x:4 * x + 3]
    if tx:
        red = sum(1 for x in range(320) for y in range(288, 312) if px(x, y)[0] > 150 and max(px(x, y)[1:]) < 90)
        blue = sum(1 for x in range(320) for y in range(448, 472) if px(x, y)[2] > 120 and px(x, y)[0] < 90)
        return red > 100 and blue > 100
    dark = [y for y in range(320) for x in range(320) if sum(px(x, y)) < 384]
    colour = sum(1 for y in range(320) for x in range(320) if max(px(x, y)) - min(px(x, y)) > 40)
    return 1500 < len(dark) < 4000 and colour == 0 and 100 < min(dark) and max(dark) < 210 and max(dark) - min(dark) > 40


@unittest.skipUnless(E2.is_file() and TX.is_file() and TX_NAND.is_file() and CORE.is_file()
                     and os.environ.get('FROND_HARD_RESET'),
                     'set FROND_HARD_RESET=1 with the E2 and TX firmware for the 2 minute hard reset run')
class HardResetPromptTest(unittest.TestCase):
    """reset-hard holds power through the boot, so the TX and E2 DAL ask to erase; a soft reset during the
    hold cancels it. Frames are only taken once the hold is over."""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hard-reset-', dir=ROOT / 'build')
        cls.root = Path(cls.temp.name)

    @classmethod
    def tearDownClass(cls):
        if os.environ.get('FROND_ZIRE_EVIDENCE'):
            out = Path(os.environ['FROND_ZIRE_EVIDENCE'])
            out.mkdir(parents=True, exist_ok=True)
            for path in cls.root.glob('*.png'):
                (out / path.name).write_bytes(path.read_bytes())
        cls.temp.cleanup()

    def run_reset(self, name, image, tx, soft_after=None):
        core = Core(image, self.root / f'{name}.log')
        try:
            core.wait_frame(self.root / f'{name}-boot.png', lambda rows: rows is not None, 60)
            core.send('reset-hard')
            if soft_after is not None:
                time.sleep(soft_after)
                core.send('reset')
            time.sleep(12)
            path = self.root / f'{name}.png'
            # The prompt waits for a key, so it is still up after the 9 s hold.
            end = time.monotonic() + 20
            while time.monotonic() < end:
                if erase_prompt(core.frame(path), tx):
                    return True
                time.sleep(1)
            return False
        finally:
            self.assertEqual(core.close(), 0)

    def test_e2_prompt(self):
        self.assertTrue(self.run_reset('e2-hard', [E2], False), 'no erase prompt on the E2')

    def test_tx_prompt(self):
        self.assertTrue(self.run_reset('tx-hard', [TX, '--device', 'palm-tx', '--nand', TX_NAND], True),
                        'no erase prompt on the TX')

    def test_soft_reset_cancels_hold(self):
        self.assertFalse(self.run_reset('e2-soft', [E2], False, soft_after=1), 'the hold outlived a soft reset')


SDK = ROOT.parent / 'palm-os-sdk/sdk-5r3'
PROBE_READY = (SDK / 'include/PalmOS.h').is_file() and all(shutil.which(t) for t in ('m68k-palmos-gcc', 'pilrc'))
MTOOLS = all(shutil.which(t) for t in ('mformat', 'mcopy', 'mtype', 'mdir'))


@unittest.skipUnless(ZIRE.is_file() and CORE.is_file() and os.environ.get('FROND_ZIRE_STORAGE') and PROBE_READY and MTOOLS,
                     'set FROND_ZIRE_STORAGE=1 with the Zire 72 firmware, the Palm OS SDK and mtools for the 6 minute '
                     'card and session run')
class Zire72StorageTest(unittest.TestCase):
    """SD card VFS through the SD probe, eject, sleep with a card and a file open, sessions with an embedded
    card and a memo, and refused damaged sessions. The tests run in order, each from the last one's session."""

    @classmethod
    def setUpClass(cls):
        cls.before = sha(ZIRE)
        assert cls.before == ZIRE_SHA, 'Zire 72 NOR is not the inventoried image'
        cls.temp = tempfile.TemporaryDirectory(prefix='zire72-storage-', dir=ROOT / 'build')
        cls.root = Path(cls.temp.name)
        subprocess.run(['python3', str(ROOT / 'tools/build-palm-probe.py'), '--sdk', str(SDK), '--probe', 'sd',
                        '--output', str(cls.root / 'probe')], cwd=ROOT, check=True, capture_output=True)
        cls.probe = cls.root / 'probe/FrondSDProbe.prc'
        cls.card = cls.card_image('card.img', {'HOST.TXT': b'Hello from host\n', 'DELETE.TXT': b'delete me\n'})
        cls.session = boot_to_launcher(cls.root)

    @classmethod
    def tearDownClass(cls):
        if os.environ.get('FROND_ZIRE_EVIDENCE'):
            out = Path(os.environ['FROND_ZIRE_EVIDENCE'])
            out.mkdir(parents=True, exist_ok=True)
            for path in [*cls.root.glob('*.png'), *cls.root.glob('*.log')]:
                (out / path.name).write_bytes(path.read_bytes())
        cls.temp.cleanup()
        assert sha(ZIRE) == cls.before, 'Zire 72 NOR changed'

    @classmethod
    def card_image(cls, name, files, mib=32):
        path = cls.root / name
        with path.open('wb') as card:
            card.truncate(mib << 20)
        subprocess.run(['mformat', '-i', str(path), '-v', 'FROND', '::'], check=True, capture_output=True)
        for file, data in files.items():
            (cls.root / file).write_bytes(data)
            subprocess.run(['mcopy', '-i', str(path), str(cls.root / file), '::/' + file], check=True)
        return path

    def card_file(self, card, name):
        result = subprocess.run(['mtype', '-i', str(card), '::/' + name], capture_output=True)
        return result.stdout if result.returncode == 0 else None

    def core(self, image, name, *args):
        core = Core([image], self.root / f'{name}.log', *args)
        self.addCleanup(lambda: self.assertNotIn('Invalid instr', core.output()))
        return core

    def run_probe(self, core, name, runs):
        """Starts the probe and returns the log records of its newest run once that run is complete."""
        core.launch('Frond SD probe')
        path = self.root / f'{name}.zip'
        end = time.monotonic() + 90
        while time.monotonic() < end:
            path.unlink(missing_ok=True)
            core.send(f'db-export "{path}"', ('export written', 'not ready', 'FAILED'))
            if path.is_file():
                records = probe_log(path)
                begins = [i for i, line in enumerate(records) if line.startswith('begin ')]
                if len(begins) >= runs and any(r.startswith('complete ') for r in records[begins[runs - 1]:]):
                    return records[begins[runs - 1]:]
            time.sleep(2)
        raise AssertionError(f'probe run {runs} did not complete')

    def check_probe(self, records, prior):
        fields = dict(line.split(' ', 1) for line in records)
        for key in ('enumerate', 'host-open', 'host-close', 'guest-open', 'guest-write', 'guest-close', 'guest-reopen',
                    'guest-read-close', 'scratch-open', 'scratch-write', 'scratch-close', 'scratch-delete'):
            self.assertTrue(fields[key].startswith('err=0000'), (key, fields[key]))
        # The 5.2.8 VFS gives vfsErrFileEOF (2A07) for a read that reaches the end, like the E2.
        for key in ('host-read', 'guest-read'):
            self.assertIn(fields[key][:8], ('err=0000', 'err=2A07'), key)
        for key in ('host-match', 'guest-match', 'complete'):
            self.assertTrue(fields[key].startswith('err=0000'), (key, fields[key]))
        self.assertTrue(fields['scratch-gone'].startswith('err=2A08'))
        self.assertTrue(fields['prior-open'].startswith('err=0000' if prior else 'err=2A08'))
        if prior:
            self.assertTrue(fields['prior-match'].startswith('err=0000'))
        return fields

    def sleep_and_wake(self, core, name):
        core.key(self.root, 'power')
        core.wait_frame(self.root / f'{name}-asleep.png', lambda rows: rows is None, 20)
        # Pressed while the DAL may still be going to sleep; the press waits until it's asleep.
        core.key(self.root, 'power')
        return core.wait_frame(self.root / f'{name}-awake.png', lambda rows: rows is not None, 10)

    def test_1_card_read_write_delete(self):
        core = self.core(self.session, 'card')
        try:
            core.send(f'mount "{self.card}"')
            core.until(f'install "{self.probe}"', ('installation', 'not'), 'successful')
            fields = self.check_probe(self.run_probe(core, 'card', 1), prior=False)
            self.assertTrue(fields['host-delete'].startswith('err=0000'))
            self.assertTrue(fields['host-delete-gone'].startswith('err=2A08'))
            core.send(f'save-sd "{self.root / "card-out.img"}"', 'wrote SD image')
            core.send(f'save-session "{self.root / "card.session"}"', 'wrote session')
        finally:
            self.assertEqual(core.close(), 0)
        out = self.root / 'card-out.img'
        self.assertEqual(self.card_file(out, 'GUEST.TXT'), b'Written by Palm TX\n')
        self.assertIsNone(self.card_file(out, 'DELETE.TXT'))
        self.assertEqual(self.card_file(out, 'HOST.TXT'), b'Hello from host\n')
        self.assertIsNone(self.card_file(self.card, 'GUEST.TXT'))  # the mounted image is a copy

    def test_2_card_session_eject_and_sleep(self):
        # A fresh process from the session with the card in it, then eject, reinsert, sleep and wake.
        core = self.core(self.root / 'card.session', 'restored')
        try:
            self.check_probe(self.run_probe(core, 'restored', 2), prior=True)
            core.send(f'save-sd "{self.root / "ejected.img"}"', 'wrote SD image')
            core.send('unmount')
            core.send(f'mount "{self.root / "ejected.img"}"')
            self.check_probe(self.run_probe(core, 'reinserted', 3), prior=True)
            self.sleep_and_wake(core, 'card')
            self.check_probe(self.run_probe(core, 'woken', 4), prior=True)
            core.send(f'save-session "{self.root / "woken.session"}"', 'wrote session')
        finally:
            self.assertEqual(core.close(), 0)

    def test_3_sleep_with_a_file_open(self):
        # HOLD.TXT has the probe keep HOLD.DAT open and write a 512-byte block every 10 event passes.
        hold = self.card_image('hold.img', {'HOST.TXT': b'Hello from host\n', 'HOLD.TXT': b'40 10'})
        core = self.core(self.root / 'woken.session', 'hold')
        try:
            core.send('unmount')
            core.send(f'mount "{hold}"')
            core.launch('Frond SD probe')
            core.until(f'db-export "{self.root / "hold-early.zip"}"', ('export written', 'not ready'), 'written')
            self.sleep_and_wake(core, 'hold')
            end = time.monotonic() + 120
            while time.monotonic() < end:
                path = self.root / 'hold.zip'
                core.until(f'db-export "{path}"', ('export written', 'not ready'), 'written')
                records = probe_log(path)
                if any(r.startswith('hold-size') for r in records):
                    break
                time.sleep(3)
            self.assertIn('hold-size err=0000 value=20480', records)
            self.assertIn('hold-close err=0000 value=40', records)
            core.send(f'save-sd "{self.root / "hold-out.img"}"', 'wrote SD image')
        finally:
            self.assertEqual(core.close(), 0)
        data = self.card_file(self.root / 'hold-out.img', 'HOLD.DAT')
        self.assertEqual(data, b''.join(bytes([i]) * 512 for i in range(40)))

    def test_4_memo_survives_a_new_process(self):
        core = self.core(self.session, 'memo')
        memo = self.root / 'memo.session'
        try:
            core.launch('Memos-PMem')
            # New, then the title bar menu, Edit, Keyboard; the keyboard's keys type "Frind neni".
            for x, y in [(30, 304), (60, 10), (120, 13), (150, 159)]:
                core.tap(self.root, x, y)
                time.sleep(2)
            for x, y in [(112, 193), (85, 171), (196, 171), (173, 215), (90, 193), (160, 258), (195, 215),
                         (63, 171), (195, 215), (196, 171), (37, 294), (30, 304)]:
                core.tap(self.root, x, y)
                time.sleep(0.1)
            time.sleep(2)
            core.send(f'save-session "{memo}"', 'wrote session')
        finally:
            self.assertEqual(core.close(), 0)
        core = self.core(memo, 'memo-restored')
        try:
            # Memos waits for events forever, so host calls are taken from Calendar.
            core.key(self.root, 'hard1')
            path = self.root / 'memo.zip'
            core.until(f'db-export "{path}"', ('export written', 'not ready'), 'written')
        finally:
            self.assertEqual(core.close(), 0)
        with zipfile.ZipFile(path) as archive:
            self.assertIn(b'Frind neni', archive.read('MemosDB-PMem.pdb'))

    def test_5_damaged_sessions_refused(self):
        data = (self.root / 'card.session').read_bytes()
        for name, bad in [('empty', b''), ('truncated-64', data[:64]), ('truncated-half', data[:len(data) // 2]),
                          ('truncated-last', data[:-1]), ('flipped', data[:len(data) // 2] + bytes([data[len(data) // 2] ^ 0x40])
                                                           + data[len(data) // 2 + 1:])]:
            path = self.root / f'bad-{name}.session'
            path.write_bytes(bad)
            result = subprocess.run([str(CORE), path, '--no-sound', '--run-for', '1'], capture_output=True, text=True,
                                    timeout=60, env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'})
            self.assertEqual(result.returncode, 1, name)
            self.assertNotIn('palm-zire-72 (Zire 72)', result.stdout + result.stderr, name)


KEYBOARD_DRIVER = ROOT / 'build/keyboard-baseline/Keyboard_v113.prc'  # Palm Universal Wireless Keyboard 1.13
PILOT = all(shutil.which(t) for t in ('pilot-xfer',))
BTVIRT = Path(os.environ.get('FROND_BTVIRT', ROOT / 'build/bluetooth-provider/emulator/btvirt'))  # tools/build-bluetooth-provider.py


def blue(rows, x, y):
    """A form title or selection colour, not grey or white."""
    r, g, b = rows[y][4 * x:4 * x + 3]
    return b > 150 and r < 100


def small_pdb(path, name):
    """An empty record database to install over HotSync."""
    header = name.encode().ljust(32, b'\0') + struct.pack('>HHIIIIII4s4sI', 0, 1, 0xb6000000, 0xb6000000, 0, 0, 0, 0,
                                                           b'DATA', b'FrUs', 0)
    path.write_bytes(header + struct.pack('>IH', 0, 0) + b'\0\0')
    return path


def records(path, name):
    """The records of database name in a db-export archive, as strings."""
    with zipfile.ZipFile(path) as archive:
        data = next((archive.read(n) for n in archive.namelist() if n.startswith(name)), b'')
    if len(data) < 78:
        return []
    count = struct.unpack('>H', data[76:78])[0]
    offsets = [struct.unpack('>I', data[78 + 8 * i:82 + 8 * i])[0] for i in range(count)] + [len(data)]
    return [data[offsets[i]:offsets[i + 1]].split(b'\0')[0].decode() for i in range(count)]


@unittest.skipUnless(ZIRE.is_file() and CORE.is_file() and os.environ.get('FROND_ZIRE_PERIPHERALS') and PROBE_READY and PILOT,
                     'set FROND_ZIRE_PERIPHERALS=1 with the Zire 72 firmware, the Palm OS SDK and pilot-link for the '
                     '8 minute peripheral run')
class Zire72PeripheralsTest(unittest.TestCase):
    """USB HotSync both ways, IrDA beaming and Bluetooth pairing between two Zires, the infrared keyboard driver and the
    camera stub."""

    @classmethod
    def setUpClass(cls):
        cls.before = sha(ZIRE)
        assert cls.before == ZIRE_SHA, 'Zire 72 NOR is not the inventoried image'
        cls.temp = tempfile.TemporaryDirectory(prefix='zire72-peripherals-', dir=ROOT / 'build')
        cls.root = Path(cls.temp.name)
        cls.probes = {}
        for probe, name in (('irda', 'FrondIRProbe'), ('keyboard', 'FrondKeyboardProbe')):
            subprocess.run(['python3', str(ROOT / 'tools/build-palm-probe.py'), '--sdk', str(SDK), '--probe', probe,
                            '--output', str(cls.root / probe)], cwd=ROOT, check=True, capture_output=True)
            cls.probes[probe] = cls.root / probe / f'{name}.prc'
        cls.session = boot_to_launcher(cls.root)

    @classmethod
    def tearDownClass(cls):
        if os.environ.get('FROND_ZIRE_EVIDENCE'):
            out = Path(os.environ['FROND_ZIRE_EVIDENCE'])
            out.mkdir(parents=True, exist_ok=True)
            for path in [*cls.root.glob('*.png'), *cls.root.glob('*.log')]:
                (out / path.name).write_bytes(path.read_bytes())
        cls.temp.cleanup()
        assert sha(ZIRE) == cls.before, 'Zire 72 NOR changed'

    def core(self, image, name, *args):
        core = Core([image], self.root / f'{name}.log', *args)
        self.addCleanup(lambda: self.assertNotIn('failed (pc=', core.output()))
        return core

    def export(self, core, name):
        path = self.root / f'{name}.zip'
        path.unlink(missing_ok=True)
        core.until(f'db-export "{path}"', ('export written', 'not ready'), 'written')
        return path

    def sleep_and_wake(self, core, name):
        core.key(self.root, 'power')
        core.wait_frame(self.root / f'{name}-asleep.png', lambda rows: rows is None, 20)
        core.key(self.root, 'power')
        core.wait_frame(self.root / f'{name}-awake.png', lambda rows: rows is not None, 10)

    def usb_pty(self, core):
        end = time.monotonic() + 30
        while time.monotonic() < end:
            if match := re.search(r'usb: pty (\S+)', core.output()):
                return match.group(1)
            time.sleep(0.2)
        raise AssertionError('no USB PTY')

    def hotsync(self, core, pty, *argv, stdin=None):
        """Taps the HotSync button and runs one pilot-link command on the USB endpoint."""
        process = subprocess.Popen([*argv[:1], '-p', pty, *argv[1:]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, cwd=self.root)
        time.sleep(1)
        core.tap(self.root, 160, 165)
        try:
            out = process.communicate(stdin, timeout=90)[0]
        except subprocess.TimeoutExpired:
            process.kill()
            out = process.communicate()[0]
        # Back on the HotSync app's main screen before the next one.
        time.sleep(6)
        return out

    def test_usb_hotsync(self):
        pdb = small_pdb(self.root / 'FrondUsbTest.pdb', 'FrondUsbTest')
        core = self.core(self.session, 'usb', '--usb', 'pty')
        try:
            pty = self.usb_pty(core)
            core.launch('HotSync')
            self.assertRegex(self.hotsync(core, pty, 'pilot-xfer', '-l'), r'List complete\. \d+ files found')
            self.assertIn('enumerated 0830:0061', core.output())
            self.assertRegex(self.hotsync(core, pty, 'pilot-xfer', '-f', 'MemosDB-PMem'), r'complete')
            self.assertGreater((self.root / 'MemosDB-PMem.pdb').stat().st_size, 78)
            self.hotsync(core, pty, 'pilot-xfer', '-i', str(pdb))
            self.sleep_and_wake(core, 'usb')
            self.assertIn('FrondUsbTest', self.hotsync(core, pty, 'pilot-xfer', '-l'))
            core.send(f'save-session "{self.root / "usb.session"}"', 'wrote session')
        finally:
            self.assertEqual(core.close(), 0)
        core = self.core(self.root / 'usb.session', 'usb-restored', '--usb', 'pty')
        try:
            pty = self.usb_pty(core)
            self.assertIn('FrondUsbTest', self.hotsync(core, pty, 'pilot-xfer', '-l'))
        finally:
            self.assertEqual(core.close(), 0)

    def beam(self, cores, name, runs):
        """Beams the IR probe's payload from the first core to the second and checks both logs."""
        before = [self.export(core, f'{name}-before{i}') for i, core in enumerate(cores)]
        cores[0].tap(self.root, 40, 132)
        end = time.monotonic() + 60
        while time.monotonic() < end:
            time.sleep(3)
            after = [self.export(core, f'{name}-after{i}') for i, core in enumerate(cores)]
            sent, received = (records(path, 'FrondIRLog') for path in after)
            if sent.count('send-complete') >= runs and received.count('receive-complete') >= runs:
                break
        result = subprocess.run(['python3', str(ROOT / 'tools/check-irda-probe.py'), *map(str, after),
                                 '--sender-before', str(before[0]), '--receiver-before', str(before[1])],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('"status": "pass"', result.stdout)

    def test_irda_beam(self):
        ports = []
        for _ in range(2):
            with socket.socket() as probe:
                probe.bind(('127.0.0.1', 0))
                ports.append(probe.getsockname()[1])
        cores = [self.core(self.session, f'ir{i}', '--irda', f'tcp:{port}') for i, port in enumerate(ports)]
        link = None
        try:
            for core in cores:
                core.until(f'install "{self.probes["irda"]}"', ('installation', 'not'), 'successful')
                core.launch('Frond IR probe')
            link = subprocess.Popen(['python3', str(ROOT / 'tools/link-irda.py'), *map(str, ports)],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.beam(cores, 'ir', 1)
            for i, core in enumerate(cores):
                self.sleep_and_wake(core, f'ir{i}')
            # link-irda.py connects again to a port that closed while its Palm slept.
            self.beam(cores, 'ir-woken', 2)
        finally:
            if link:
                link.terminate()
                link.wait()
            for core in cores:
                self.assertEqual(core.close(), 0)

    @unittest.skipUnless(KEYBOARD_DRIVER.is_file(), 'needs the Palm Universal Wireless Keyboard 1.13 PRC')
    def test_ir_keyboard(self):
        # The Zire's ROM has no keyboard driver; the 1.13 add-on driver is installed as on the E2.
        core = self.core(self.session, 'keyboard')
        try:
            core.until(f'install "{KEYBOARD_DRIVER}"', ('installation', 'not'), 'successful')
            time.sleep(3)
            core.replay(self.root, ['0 keyboard-attach'])
            core.send('reset')
            core.wait_frame(self.root / 'keyboard-reset.png', lambda rows: rows is not None and date_time_prefs(rows), 60)
            core.launch('Keyboard')
            time.sleep(3)
            core.tap(self.root, 150, 50)  # driver on
            time.sleep(2)
            core.key(self.root, 'hard1')
            core.until(f'install "{self.probes["keyboard"]}"', ('installation', 'not'), 'successful')
            core.launch('Frond keyboard probe')
            time.sleep(2)
            core.replay(self.root, ['500 keyboard-down Z', '650 keyboard-up Z', '1000 keyboard-down X', '1150 keyboard-up X',
                                    '1500 keyboard-down "Left Shift"', '1600 keyboard-down Z', '1750 keyboard-up Z',
                                    '1850 keyboard-up "Left Shift"', '2200 keyboard-down Z', '2350 keyboard-up Z'])
            time.sleep(5)
            typed = records(self.export(core, 'keyboard'), 'FrondKeyboardLog')
        finally:
            self.assertEqual(core.close(), 0)
        chars = ''.join(chr(int(m[1], 16)) for line in typed if (m := re.search(r'event=4 chr=([0-9A-Fa-f]+)', line))
                        and chr(int(m[1], 16)) in 'zxZ')
        self.assertEqual(chars, 'zxZz')

    def passkey(self, core, name):
        """Types 1234 into the open passkey dialog with the silkscreen's number keyboard, then taps OK."""
        core.tap(self.root, 90, 229)
        core.tap(self.root, 253, 430)  # 123 on the silkscreen
        core.wait_frame(self.root / f'{name}-keyboard.png', lambda rows: rows is not None and blue(rows, 20, 40), 15)
        for x, y in [(120, 160), (160, 160), (198, 160), (120, 193), (50, 294)]:
            core.tap(self.root, x, y)
            time.sleep(1)
        core.frame(self.root / f'{name}-passkey.png')
        core.tap(self.root, 50, 294)

    @unittest.skipUnless(BTVIRT.is_file(), 'needs btvirt from tools/build-bluetooth-provider.py')
    def test_bluetooth_pairing(self):
        # Two Zires on one virtual controller network: Trusted Devices > Add Device on the first finds the second,
        # both take the passkey and the second shows up as a trusted device. Each session turns Bluetooth on for
        # the first time here, so each makes up its own address.
        sockets = self.root / 'bt-sockets'
        sockets.mkdir(mode=0o700)
        provider = subprocess.Popen([str(BTVIRT), f'-s{sockets}'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cores = []
        try:
            end = time.monotonic() + 10
            while not (sockets / 'bt-server-bredr').exists():
                self.assertLess(time.monotonic(), end, 'btvirt did not start')
                time.sleep(0.1)
            cores = [self.core(self.session, f'bt{i}', '--bluetooth', sockets / 'bt-server-bredr') for i in range(2)]
            for i, core in enumerate(cores):
                core.launch('BtManPnl')
                core.tap(self.root, 178, 60)  # On
            for i, core in enumerate(cores):
                # Discoverable: Yes once the radio is up, with no Please wait over it.
                core.wait_frame(self.root / f'bt{i}-on.png', lambda rows: rows is not None and not blue(rows, 89, 160)
                                and any(rows[y][4 * x] < 80 for x in range(176, 206) for y in range(158, 172)), 90)
            first, second = cores
            first.tap(self.root, 70, 305)  # Setup Devices
            time.sleep(3)
            first.tap(self.root, 175, 295)  # Trusted Devices
            time.sleep(3)
            first.tap(self.root, 252, 294)  # Add Device
            first.wait_frame(self.root / 'bt-found.png', lambda rows: rows is not None and any(
                rows[y][4 * x] < 80 for x in range(40, 200) for y in range(100, 114)), 90)
            first.tap(self.root, 100, 107)
            first.tap(self.root, 35, 294)  # OK
            first.wait_frame(self.root / 'bt0-pin.png', lambda rows: rows is not None and blue(rows, 20, 151), 60)
            self.passkey(first, 'bt0')
            second.wait_frame(self.root / 'bt1-pin.png', lambda rows: rows is not None and blue(rows, 20, 151), 60)
            self.passkey(second, 'bt1')
            # The second Zire, selected in the first one's Trusted Devices list.
            first.wait_frame(self.root / 'bt-trusted.png', lambda rows: rows is not None and blue(rows, 250, 80)
                             and not blue(rows, 20, 151), 60)
        finally:
            for core in cores:
                self.assertEqual(core.close(), 0)
            provider.terminate()
            provider.wait()

    def test_camera_stub(self):
        # No sensor answers on I2C: Camera shows a blank screen while its driver looks, then goes back to the
        # launcher. Before the capture interface stub it faulted in CameraDriver.
        launcher = png_rows(self.root / 'launcher.png')
        core = self.core(self.session, 'camera')
        try:
            core.key(self.root, 'hard4')
            core.wait_frame(self.root / 'camera-blank.png',
                            lambda rows: rows is not None and all(min(row[4 * x:4 * x + 3]) > 240
                                                                  for row in rows[40:300:20] for x in range(0, 320, 20)), 15)
            core.wait_frame(self.root / 'camera-back.png', lambda rows: rows is not None and rows[30:] == launcher[30:], 40)
            core.key(self.root, 'hard1')
            core.wait_frame(self.root / 'camera-hard1.png', lambda rows: rows is not None and rows[30:] != launcher[30:], 15)
        finally:
            self.assertEqual(core.close(), 0)
        self.assertNotIn('Fatal', core.output())


def probe_log(path):
    """The SD probe's FrondSDLog records from a db-export archive."""
    with zipfile.ZipFile(path) as archive:
        name = next((n for n in archive.namelist() if n.startswith('FrondSDLog')), None)
        if not name:
            return []
        data = archive.read(name)
    count = struct.unpack('>H', data[76:78])[0]
    offsets = [struct.unpack('>I', data[78 + 8 * i:82 + 8 * i])[0] for i in range(count)] + [len(data)]
    return [data[offsets[i]:offsets[i + 1]].split(b'\0')[0].decode() for i in range(count)]


if __name__ == '__main__':
    unittest.main()
