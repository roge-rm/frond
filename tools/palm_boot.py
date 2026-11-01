"""Frame checks and setup taps shared by ROM tests and the private app runner."""
import os
import hashlib
import subprocess
import threading
import struct
import time
import zlib

from guest_process import stop_guest

import json
from pathlib import Path

PROFILES = json.loads((Path(__file__).with_name('device-profiles.json')).read_text())['profiles']
def profile_devices(profiles, eligibility):
    return {
        p['name']: dict(p, nor=p['nor_bytes'], nand=p['raw_nand_bytes'],
                       input_height=p['height'] + p['silkscreen_height'])
        for p in profiles if p[eligibility]
    }


def runner_devices(profiles):
    return profile_devices(profiles, 'runner_eligible')


def frontend_devices(profiles):
    return profile_devices(profiles, 'available')


DEVICES = runner_devices(PROFILES)
FRONTEND_DEVICES = frontend_devices(PROFILES)


CORE = Path(__file__).resolve().parents[1] / 'out/frond'


class Core:
    """A core process driven over its console; waits on its output and frames, not on the clock."""

    def __init__(self, image, log, *args):
        self.lines, self.log = [], log.open('w')
        self.process = subprocess.Popen(['heavy', 'emulator', str(CORE), *map(str, image), '--no-sound', *map(str, args)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, errors='replace', start_new_session=True,
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

    def key(self, root, name, hold_ms=200):
        mark = len(self.lines)
        self.replay(root, [f'0 key-down {name}', f'{hold_ms} key-up {name}'])
        self.released(mark)

    def tap(self, root, x, y, hold_ms=150):
        mark = len(self.lines)
        self.replay(root, [f'0 pen-down {x} {y}', f'{hold_ms} pen-up'])
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

    def close(self, timeout=30):
        try:
            if self.process.poll() is None:
                self.process.stdin.write('quit\n')
                self.process.stdin.flush()
                self.process.wait(timeout=timeout)
        except (BrokenPipeError, subprocess.TimeoutExpired):
            pass
        finally:
            stop_guest(self.process)
        self.reader.join(timeout=5)
        self.process.stdin.close()
        self.process.stdout.close()
        self.log.close()
        return self.process.returncode


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


def launcher_wallpaper(rows, top=150):
    """A large part of the launcher is coloured wallpaper."""
    return rows is not None and sum(
        1 for x in range(0, 320, 8) for y in range(top, 280, 8)
        if max(rows[y][4 * x:4 * x + 3]) - min(rows[y][4 * x:4 * x + 3]) > 30) > 400


def stock_launcher(rows):
    """The stock E2 launcher: blue separator, category selector and a coloured icon grid."""
    if rows is None or len(rows) < 320:
        return False
    blue = sum(1 for x in range(0, 312) if rows[26][4*x+2] > 150 and
               rows[26][4*x+2] - rows[26][4*x] > 50)
    category = sum(1 for y in range(6, 20) for x in range(296, 316)
                   if sum(rows[y][4*x:4*x+3]) < 200)
    cells = sum(1 for left in (16, 120, 220) for top in (40, 110, 182, 254)
                if sum(1 for y in range(top, top+40, 2) for x in range(left, left+70, 2)
                       if max(rows[y][4*x:4*x+3]) - min(rows[y][4*x:4*x+3]) > 50) > 30)
    return blue > 280 and category > 10 and cells >= 8


def upper_wallpaper_launcher(rows):
    return launcher_wallpaper(rows, 40) or stock_launcher(rows)


def calibration_target(rows, x, y):
    # The target's red rings around its centre.
    return rows is not None and sum(1 for dx in range(-12, 13, 3) for dy in range(-12, 13, 3)
        if 0 <= x + dx < 320 and 0 <= y + dy < 320 and rows[y + dy][4 * (x + dx)] > 180
        and rows[y + dy][4 * (x + dx) + 1] < 80) > 10


def factory_setup(core, root, taps, launcher):
    """Calibrate and finish the measured factory forms, waiting for each screen."""
    frame = root / 'boot-frame.png'
    for x, y in [(291, 292), (27, 27), (160, 160)]:
        rows = core.wait_frame(frame, lambda rows: calibration_target(rows, x, y), 120)
        if x == 291:
            (root / 'calibrate.png').write_bytes(frame.read_bytes())
        core.tap(root, x, y)
    title = rows[:24]
    for step, (x, y) in enumerate(taps):
        seen = []

        def settled(rows):
            seen.append(None if rows is None else rows[:24])
            # A title bar, not a blank screen between forms.
            inked = rows is not None and sum(1 for row in rows[:24] for i in range(0, 1280, 16) if row[i] < 128) > 20
            body = rows is not None and any(min(rows[y][4 * x:4 * x + 3]) > 230
                                            for x in range(0, 320, 16) for y in range(80, 280, 16))
            return inked and body and rows[:24] != title and len(seen) > 1 and seen[-1] == seen[-2]
        rows = core.wait_frame(frame, settled, 30)
        (root / f'setup-{step}.png').write_bytes(frame.read_bytes())
        title = rows[:24]
        core.tap(root, x, y)
    core.wait_frame(root / 'launcher.png', launcher, 60)


def zire_setup(core, root):
    # Language OK, Welcome Next, Location Next, Not now, Done.
    factory_setup(core, root, [(40, 300), (280, 304), (280, 304), (45, 302), (35, 304)], launcher_wallpaper)


def e2_setup(core, root):
    """Finish factory setup on blank NAND, or Date & Time on configured NAND."""
    rows = core.wait_frame(root / 'boot-frame.png', lambda rows: calibration_target(rows, 291, 292) or
                           (rows is not None and date_time_prefs(rows)), 120)
    if calibration_target(rows, 291, 292):
        # Welcome Next, Location Next, Not now, Done.
        return factory_setup(core, root, [(280, 304), (280, 304), (45, 304), (35, 304)],
                             upper_wallpaper_launcher)
    core.tap(root, 35, 305)
    core.tap(root, 27, 352)
    core.wait_frame(root / 'launcher.png', lambda rows: launcher_wallpaper(rows, 40), 60)


def tx_setup(core, root):
    """Finish the TX's Date & Time form, then open the launcher with the Home key."""
    def prefs(rows):
        # The blue Preferences tab at the top left, the Date & Time title and a white form body.
        if rows is None:
            return False
        title = sum(1 for x in range(200, 316) for y in range(6, 20) if sum(rows[y][4 * x:4 * x + 3]) < 200)
        blue = sum(1 for x in range(10, 120) if rows[12][4 * x + 2] > 150 and rows[12][4 * x] < 80)
        white = sum(1 for x in range(0, 320, 4) for y in range(100, 180, 4) if min(rows[y][4 * x:4 * x + 3]) > 230)
        return 400 <= title <= 600 and blue >= 40 and white > 0.7 * 80 * 20
    core.wait_frame(root / 'boot-frame.png', prefs, 120)
    core.tap(root, 35, 305)
    core.wait_frame(root / 'setup-frame.png', lambda rows: rows is not None and not prefs(rows), 30)
    time.sleep(2)
    core.key(root, 'home')
    core.wait_frame(root / 'launcher.png', launcher_wallpaper, 60)


def tg50_setup_step(rows, step):
    if rows is None or len(rows) != 320:
        return False
    red, green, blue = rows[3][12:15]
    if not (red < 100 and green < 50 and blue > 100):
        return False
    white = lambda x, y: min(rows[y][4*x:4*x+3]) > 230
    if step == 1:
        return white(89, 10) and not white(90, 8)
    if step == 2:
        return white(90, 8) and white(90, 17)
    if step == 3:
        return white(90, 8) and not white(90, 17)
    return step == 4 and not white(89, 10) and white(94, 17)


def tg50_calibration_target(rows, x, y):
    if rows is None or len(rows) != 320 or min(rows[0][1276:1279]) < 230:
        return False
    return sum(1 for yy in range(y-14, y+15) for xx in range(x-14, x+15)
               if max(rows[yy][4*xx:4*xx+3]) < 80) > 280


def tg50_launcher(rows):
    if rows is None or len(rows) != 320:
        return False
    # The grey Sony sidebar, lower CLIÉ lettering and selected blue application row.
    sidebar = sum(1 for y in range(175, 318, 4) for x in range(2, 104, 4)
                  if 160 < min(rows[y][4*x:4*x+3]) and max(rows[y][4*x:4*x+3]) < 230)
    lettering = sum(1 for y in range(295, 313) for x in range(12, 90)
                    if max(rows[y][4*x:4*x+3]) < 130)
    selected = sum(1 for y in range(80, 120, 4) for x in range(124, 310, 4)
                   if rows[y][4*x+2] > 140 and rows[y][4*x] < 100)
    return sidebar > 500 and lettering > 70 and selected > 250


def tg50_setup(core, root):
    frame = root / 'boot-frame.png'
    rows = core.wait_frame(frame, lambda rows: tg50_setup_step(rows, 1) or tg50_launcher(rows), 120)
    if tg50_launcher(rows):
        return
    (root / 'setup-1.png').write_bytes(frame.read_bytes())
    core.tap(root, 160, 150)
    core.wait_frame(frame, lambda rows: tg50_setup_step(rows, 2), 30)
    (root / 'setup-2.png').write_bytes(frame.read_bytes())
    core.tap(root, 160, 150)
    for number, (x, y) in enumerate(((20, 20), (300, 300), (160, 120))):
        core.wait_frame(frame, lambda rows: tg50_calibration_target(rows, x, y), 30)
        (root / f'calibrate-{number}.png').write_bytes(frame.read_bytes())
        core.tap(root, x, y)
    core.wait_frame(frame, lambda rows: tg50_setup_step(rows, 3), 30)
    (root / 'setup-3.png').write_bytes(frame.read_bytes())
    core.tap(root, 135, 306)
    core.wait_frame(frame, lambda rows: tg50_setup_step(rows, 4), 30)
    (root / 'setup-4.png').write_bytes(frame.read_bytes())
    core.tap(root, 219, 306)
    core.wait_frame(root / 'launcher.png', tg50_launcher, 60)


def setup_device(device, core, root):
    """Run the profile's measured setup recipe; unknown recipes fail before sending input."""
    recipe = DEVICES[device]['setup']
    try:
        run = SETUP_RECIPES[recipe]
    except KeyError:
        raise ValueError(f'No qualified setup recipe for {device}: {recipe}') from None
    return run(core, root)


SETUP_RECIPES = {'tx': tx_setup, 'e2': e2_setup, 'zire': zire_setup, 'tg50': tg50_setup}


def launcher_check(device):
    recipe = DEVICES[device]['launcher']
    try:
        return LAUNCHER_CHECKS[recipe]
    except KeyError:
        raise ValueError(f'No qualified launcher check for {device}: {recipe}') from None


LAUNCHER_CHECKS = {'wallpaper': launcher_wallpaper,
                   'wallpaper-upper': upper_wallpaper_launcher, 'tg50-clie': tg50_launcher}


def tg50_date_time_prefs(rows):
    if rows is None or len(rows) != 320:
        return False
    purple = rows[12][480:483]
    white = sum(1 for y in range(180, 290, 5) for x in range(5, 315, 5)
                if min(rows[y][4*x:4*x+3]) > 230)
    return (purple[0] < 100 and purple[1] < 50 and purple[2] > 100 and
            min(rows[12][560:563]) > 230 and white > 1330)


def tg50_datebook(rows):
    if rows is None or len(rows) != 320:
        return False
    # The day view's dotted hour lines, spaced 22 pixels apart.
    return sum(1 for y in range(56, 277, 22)
               if sum(1 for x in range(50, 318) if max(rows[y][4*x:4*x+3]) < 220) > 120) >= 9


def tg50_formats(rows):
    return (rows is not None and len(rows) == 320 and
            sum(1 for x in range(320) if max(rows[68][4*x:4*x+3]) < 60) > 300)


def tg50_week_start(rows, monday):
    if not tg50_formats(rows):
        return False
    popup = sum(1 for y in range(222, 240) for x in range(145, 242)
                if max(rows[y][4*x:4*x+3]) < 80)
    m = max(rows[216][716:719]) < 80
    s = max(rows[205][736:739]) < 80
    return popup < 5 and m == monday and s != monday


def tg50_pref_menu(rows):
    return (rows is not None and len(rows) == 320 and
            sum(1 for y in range(30, 242, 2) if max(rows[y][1272:1275]) < 80) > 90)


def tg50_setting_category(rows):
    return (tg50_launcher(rows) and
            sum(1 for y in range(76, 93) for x in range(245, 311)
                if min(rows[y][4*x:4*x+3]) > 230) < 8)


def tg50_formats_from_prefs(core, root):
    frame = root / 'formats-frame.png'
    rows = core.wait_frame(frame, lambda rows: tg50_date_time_prefs(rows) or tg50_formats(rows), 60)
    if not tg50_formats(rows):
        core.tap(root, 245, 12, hold_ms=300)
        core.wait_frame(root / 'pref-menu.png', tg50_pref_menu, 30)
        core.tap(root, 254, 124, hold_ms=300)
        core.wait_frame(frame, tg50_formats, 30)


def tg50_open_formats(core, root):
    core.tap(root, 55, 71, hold_ms=300)
    core.wait_frame(root / 'setting-category.png', tg50_setting_category, 30)
    core.tap(root, 218, 98, hold_ms=300)
    tg50_formats_from_prefs(core, root)


def tg50_home(core, root, name):
    core.key(root, 'home-key', hold_ms=600)
    core.wait_frame(root / name, tg50_launcher, 60)


def tg50_erase_confirmation(rows):
    if rows is None:
        return False
    def black(left, top, right, bottom):
        return sum(max(rows[y][4*x:4*x+3]) < 80
                   for y in range(top, bottom) for x in range(left, right))
    return (black(0, 0, 320, 100) == 0 and black(0, 220, 320, 320) == 0 and
            black(50, 120, 270, 142) > 250 and black(75, 145, 245, 190) > 300)


def tg50_reset(core, root, hard):
    # A fixed Formats preference is guest-owned data, independent of the date and time zone.
    tg50_open_formats(core, root)
    core.wait_frame(root / 'marker-before.png', lambda rows: tg50_week_start(rows, False), 30)
    core.tap(root, 234, 211, hold_ms=300)
    core.wait_frame(root / 'week-menu.png', lambda rows: tg50_formats(rows) and
                    sum(1 for y in range(222, 240) for x in range(145, 242)
                        if max(rows[y][4*x:4*x+3]) < 80) > 200, 30)
    core.tap(root, 225, 232, hold_ms=300)
    core.wait_frame(root / 'marker-monday.png', lambda rows: tg50_week_start(rows, True), 30)
    core.send(f'save-session "{root / "marked.session"}"', 'wrote session')
    marked_hash = hashlib.sha256((root / 'marked.session').read_bytes()).hexdigest()
    tg50_home(core, root, 'marked-launcher.png')
    core.send('reset-hard' if hard else 'reset', 'Set speed mode', timeout=60)
    if hard:
        core.wait_frame(root / 'reset-erase.png', tg50_erase_confirmation, 60)
        core.key(root, 'up', hold_ms=600)
        core.wait_frame(root / 'reset-factory.png', lambda rows: tg50_setup_step(rows, 1), 120)
        tg50_setup(core, root)
        tg50_open_formats(core, root)
        core.wait_frame(root / 'marker-erased.png', lambda rows: tg50_week_start(rows, False), 30)
    else:
        core.wait_frame(root / 'reset-prefs.png', tg50_date_time_prefs, 120)
        tg50_formats_from_prefs(core, root)
        core.wait_frame(root / 'marker-retained.png', lambda rows: tg50_week_start(rows, True), 30)
    tg50_home(core, root, 'reset-launcher.png')
    core.key(root, 'hard1', hold_ms=600)
    core.wait_frame(root / 'reset-input.png', tg50_datebook, 60)
    tg50_home(core, root, 'reset-continued.png')
    assert hashlib.sha256((root / 'marked.session').read_bytes()).hexdigest() == marked_hash
    (root / 'postconditions.json').write_text(json.dumps(
        {'marker': 'Formats week start', 'before': 'Sunday', 'marked': 'Monday',
         'after': 'Sunday' if hard else 'Monday', 'new_input': 'Date Book',
         'final_screen': 'Sony launcher', 'home_hold_ms': 600,
         'marked_session_before': marked_hash,
         'marked_session_after': hashlib.sha256((root / 'marked.session').read_bytes()).hexdigest()}, indent=2) + '\n')


RESET_RECIPES = {'tg50': tg50_reset}


def reset_recipe(device):
    recipe = DEVICES[device]['setup']
    try:
        return RESET_RECIPES[recipe]
    except KeyError:
        raise ValueError(f'No measured reset recipe for {device}: {recipe}') from None
