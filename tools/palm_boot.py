"""Frame checks and setup taps shared by ROM tests and the private app runner."""
import os
import hashlib
import shutil
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
# DragonBall devices, run by frond-68k.
DEVICES_68K = profile_devices(PROFILES, 'runs_68k')
ROM_DEVICES = {**DEVICES, **DEVICES_68K}


def rom_device(device):
    """The ARM or 68k profile entry; looked up each time so test fixtures added to DEVICES count."""
    return DEVICES[device] if device in DEVICES else DEVICES_68K[device]


def palm_images():
    """The private firmware folder (the deviceimages/palm layout) from FROND_DEVICE_IMAGES, else a path that isn't there."""
    return Path(os.environ.get('FROND_DEVICE_IMAGES') or '/nonexistent/set-FROND_DEVICE_IMAGES')


CORE = Path(__file__).resolve().parents[1] / 'out/frond'
CORE_68K = Path(__file__).resolve().parents[1] / 'out/frond-68k'


def core_for(device):
    """The runner binary for a device name."""
    return CORE_68K if device in DEVICES_68K else CORE


def session_file(path):
    """The .frond inside a saved machine folder, or a lone session file."""
    path = Path(path)
    if path.is_dir():
        found = [path / (path.name + '.frond'), path / 'session.frond', *sorted(path.glob('*.frond'))]
        return next((p for p in found if p.is_file()), path / (path.name + '.frond'))
    return path


class Core:
    """A core process driven over its console; waits on its output and frames, not on the clock."""

    def __init__(self, image, log, *args):
        self.lines, self.log = [], log.open('w')
        image = list(image)
        # A selected 68k device runs in frond-68k; heavy only where it's installed.
        device = image[image.index('--device') + 1] if '--device' in image[:-1] else None
        runner = ['heavy', 'emulator'] if shutil.which('heavy') else []
        self.process = subprocess.Popen([*runner, str(core_for(device)), *map(str, image), '--no-sound', *map(str, args)],
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


def zire_reminder(rows):
    """The Zire's setup Reminder: a full-width blue title bar, a white body and OK, Snooze and Go To buttons.

    Setup sets it as a one-time alarm an hour after Done; OK at (50, 280) clears it with no further alarm."""
    if rows is None or len(rows) != 320:
        return False
    def px(x, y):
        return tuple(rows[y][4 * x:4 * x + 3])
    title = sum(1 for x in range(320) if px(x, 26) == (0, 101, 206))
    white = sum(1 for x in range(0, 320, 4) for y in range(40, 250, 4) if min(px(x, y)) > 230)
    # The left and right outlines of the three buttons on y = 280, and the OK button's top and bottom.
    outline = [sum(px(x, 280)) < 300 for x in (12, 104, 114, 238)] + [sum(px(60, y)) < 300 for y in (256, 306)]
    return title == 320 and white > 0.7 * 80 * 53 and all(outline)


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


def e2_denvfs_target(rows, x, y):
    """The de-NVFS Digitizer's four black triangles, with white gaps between them."""
    if rows is None or len(rows) != 320:
        return False
    def dark(dx, dy):
        return max(rows[y+dy][4*(x+dx):4*(x+dx)+3]) < 80
    return (all(dark(dx, dy) for dx in (-10, 10) for dy in (-10, 10)) and
            not dark(0, -10) and not dark(-10, 0))


def e2_denvfs_prefs(rows):
    return (rows is not None and len(rows) == 320 and
            sum(tuple(rows[12][4*x:4*x+3]) == (49, 0, 156) for x in range(10, 130)) >= 50 and
            sum(max(rows[60][4*x:4*x+3]) < 180 for x in range(310)) >= 300)


def e2_denvfs_launcher(rows):
    """The purple status strip, category selector and coloured four-row icon grid."""
    if rows is None or len(rows) != 320:
        return False
    purple = sum(tuple(rows[26][4*x:4*x+3]) == (49, 0, 156) for x in range(312))
    category = sum(max(rows[y][4*x:4*x+3]) < 80 for y in range(6, 20) for x in range(272, 316))
    cells = sum(sum(max(rows[y][4*x:4*x+3]) - min(rows[y][4*x:4*x+3]) > 50
                    for y in range(top, top+40, 2) for x in range(left, left+70, 2)) > 30
                for left in (16, 120, 220) for top in (40, 110, 182, 254))
    return purple > 280 and category > 10 and cells >= 8


def e2_denvfs_setup(core, root):
    """Calibrate the RAM-backed E2 firmware, then leave Preferences with physical Home."""
    frame = root / 'boot-frame.png'
    rows = core.wait_frame(frame, lambda rows: e2_denvfs_target(rows, 20, 20) or
                           e2_denvfs_prefs(rows) or e2_denvfs_launcher(rows) or
                           (rows is not None and len(rows) == 320 and date_time_prefs(rows)), 120)
    if e2_denvfs_launcher(rows):
        return
    if e2_denvfs_target(rows, 20, 20):
        for number, (x, y) in enumerate(((20, 20), (300, 300), (160, 120))):
            core.wait_frame(frame, lambda rows: e2_denvfs_target(rows, x, y), 30)
            (root / f'calibrate-{number}.png').write_bytes(frame.read_bytes())
            core.tap(root, x, y)
        core.wait_frame(root / 'preferences.png', e2_denvfs_prefs, 30)
    core.tap(root, 27, 352)
    core.wait_frame(root / 'launcher.png', e2_denvfs_launcher, 60)


def e3_launcher(rows):
    """The E3's stock launcher above its guest-drawn input area and status bar."""
    return rows is not None and len(rows) == 480 and stock_launcher(rows)


def e3_digitizer_done(rows):
    if rows is None or len(rows) != 480:
        return False
    # Success text, the Done outline and the blue input area's left and right panels.
    ink = sum(max(rows[y][4*x:4*x+3]) < 80 for y in range(42, 60) for x in range(5, 120))
    return (ink > 180 and max(rows[292][4*35:4*35+3]) < 80 and
            all(rows[340][4*x+2] > 140 and rows[340][4*x] < 140 for x in (3, 315)))


def e3_setup(core, root):
    """Calibrate the E3, finish Digitizer, then use its virtual Applications button."""
    frame = root / 'boot-frame.png'
    rows = core.wait_frame(frame, lambda rows: calibration_target(rows, 291, 292) or
                           e3_launcher(rows) or (rows is not None and len(rows) == 480 and date_time_prefs(rows)), 120)
    if e3_launcher(rows):
        return
    if calibration_target(rows, 291, 292):
        for number, (x, y) in enumerate(((291, 292), (27, 27), (160, 160))):
            core.wait_frame(frame, lambda rows: calibration_target(rows, x, y), 30)
            (root / f'calibrate-{number}.png').write_bytes(frame.read_bytes())
            core.tap(root, x, y)
        core.wait_frame(root / 'digitizer-done.png', e3_digitizer_done, 30)
    core.tap(root, 35, 305)
    core.tap(root, 32, 352)
    core.wait_frame(root / 'launcher.png', e3_launcher, 60)


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


def iiix_dark(rows, x, y):
    return sum(rows[y][4 * x:4 * x + 3]) < 250


def iiix_target(rows, x, y):
    """The IIIx calibration cross centred on (x, y): four arrow tips 10 pixels out, a dot in the middle."""
    if rows is None or len(rows) != 160 or not (10 <= x < 150 and 10 <= y < 150):
        return False
    tips = all(iiix_dark(rows, x + dx, y + dy) for dx in (-10, 10) for dy in (-10, 10))
    gaps = not any(iiix_dark(rows, x + dx, y + dy) for dx, dy in ((0, -5), (0, 5), (-5, 0), (5, 0)))
    ink = sum(iiix_dark(rows, xx, yy) for yy in range(y - 10, y + 11) for xx in range(x - 10, x + 11))
    return tips and gaps and iiix_dark(rows, x, y) and 120 <= ink <= 220


def iiix_title(rows):
    """An inverted title tab at the top left over a full-width line, as on every IIIx form."""
    return (rows is not None and len(rows) == 160 and
            sum(iiix_dark(rows, x, y) for x in range(2, 30) for y in range(1, 11)) > 150 and
            sum(iiix_dark(rows, x, 13) for x in range(160)) > 150)


def iiix_buttons(rows):
    """Centres of the 10 to 15 pixel high buttons along the bottom of a form, left to right."""
    if rows is None or len(rows) != 160:
        return []
    for top in range(130, 150):
        spans, x = [], 0
        while x < 160:
            if iiix_dark(rows, x, top):
                start = x
                while x < 160 and iiix_dark(rows, x, top):
                    x += 1
                bottom = next((b for b in range(top + 9, min(top + 15, 160))
                               if all(iiix_dark(rows, xx, b) for xx in range(start, x))), None)
                # An outline, not a filled shape: light just inside the top edge.
                inside = range(start + 3, x - 3)
                if x - start >= 12 and bottom and sum(not iiix_dark(rows, xx, top + 2) for xx in inside) > 0.8 * len(inside):
                    spans.append(((start + x) // 2, (top + bottom) // 2))
            x += 1
        if spans:
            return spans
    return []


def iiix_launcher(rows):
    """The launcher: a title tab, the category trigger at the top right and a grid of app icons."""
    if not iiix_title(rows):
        return False
    trigger = any(all(iiix_dark(rows, x + i, y + i) and iiix_dark(rows, x + 7 - i, y + i) and
                      not iiix_dark(rows, x + i - 1, y + i) for i in range(4))
                  for y in range(2, 8) for x in range(100, 150))
    # Three columns of icons, rows about 35 pixels apart.
    icons = sum(1 for top in (20, 55, 90) for cx in (24, 76, 128)
                if sum(iiix_dark(rows, x, y) for x in range(cx - 10, cx + 10) for y in range(top, top + 18)) > 25)
    return trigger and icons >= 7


def iiix_setup(core, root):
    """Palm OS 3.1 to 4.1 setup: two tap-anywhere pages, three calibration targets, Next, then Done."""
    frame = root / 'boot-frame.png'
    rows = core.wait_frame(frame, lambda rows: iiix_title(rows) or iiix_launcher(rows), 120)
    if iiix_launcher(rows):
        return

    def changed(before):
        seen = []

        def settled(rows):
            seen.append(rows)
            return rows is not None and rows != before and len(seen) > 1 and seen[-1] == seen[-2]
        return core.wait_frame(frame, settled, 30)
    rows = core.wait_frame(frame, iiix_title, 30)
    for step in (1, 2):
        (root / f'setup-{step}.png').write_bytes(frame.read_bytes())
        core.tap(root, 80, 120)
        rows = changed(rows)
    for number, (x, y) in enumerate(((10, 10), (149, 149), (79, 60))):
        rows = core.wait_frame(frame, lambda rows: iiix_target(rows, x, y), 30)
        (root / f'calibrate-{number}.png').write_bytes(frame.read_bytes())
        core.tap(root, x, y)
    for step, count in ((3, 2), (4, 3)):
        rows = core.wait_frame(frame, lambda rows: iiix_title(rows) and len(iiix_buttons(rows)) == count, 30)
        (root / f'setup-{step}.png').write_bytes(frame.read_bytes())
        # Next, then Done: the right-most button.
        core.tap(root, *iiix_buttons(rows)[-1])
    core.wait_frame(root / 'launcher.png', iiix_launcher, 60)


def variant_recipe(device, variant=None):
    if variant is None:
        return rom_device(device)['setup']
    name = variant['id'] if isinstance(variant, dict) else variant
    if name != 'e2-denvfs' or not any(v['id'] == name for v in rom_device(device).get('firmware_variants', [])):
        raise ValueError(f'No qualified firmware variant recipe for {device}: {name}')
    return name


def setup_device(device, core, root, variant=None):
    """Run the profile's measured setup recipe; unknown recipes fail before sending input."""
    recipe = variant_recipe(device, variant)
    try:
        run = SETUP_RECIPES[recipe]
    except KeyError:
        raise ValueError(f'No qualified setup recipe for {device}: {recipe}') from None
    return run(core, root)


SETUP_RECIPES = {'tx': tx_setup, 'e2': e2_setup, 'e2-denvfs': e2_denvfs_setup, 'e3': e3_setup,
                 'zire': zire_setup, 'tg50': tg50_setup, 'iiix': iiix_setup}


def launcher_check(device, variant=None):
    recipe = variant_recipe(device, variant) if variant is not None else rom_device(device)['launcher']
    try:
        return LAUNCHER_CHECKS[recipe]
    except KeyError:
        raise ValueError(f'No qualified launcher check for {device}: {recipe}') from None


LAUNCHER_CHECKS = {'wallpaper': launcher_wallpaper,
                   'e2-denvfs': e2_denvfs_launcher,
                   'wallpaper-upper': upper_wallpaper_launcher, 'tg50-clie': tg50_launcher,
                   'iiix': iiix_launcher, 'e3-stock': e3_launcher}


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
    marked_hash = hashlib.sha256(session_file(root / 'marked.session').read_bytes()).hexdigest()
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
    assert hashlib.sha256(session_file(root / 'marked.session').read_bytes()).hexdigest() == marked_hash
    (root / 'postconditions.json').write_text(json.dumps(
        {'marker': 'Formats week start', 'before': 'Sunday', 'marked': 'Monday',
         'after': 'Sunday' if hard else 'Monday', 'new_input': 'Date Book',
         'final_screen': 'Sony launcher', 'home_hold_ms': 600,
         'marked_session_before': marked_hash,
         'marked_session_after': hashlib.sha256(session_file(root / 'marked.session').read_bytes()).hexdigest()}, indent=2) + '\n')


def iiix_reset(core, root, hard):
    """A soft reset keeps the finished setup and comes back to the launcher (4.1 by way of Date &
    Time); a hard reset asks to erase, takes Up, and starts setup again. Then new input: Date Book on the first app key, and
    the silkscreen Applications button back to the launcher."""
    marked = root / 'marked.session'
    core.send(f'save-session "{marked}"', 'wrote session')
    marked = session_file(marked)
    marked_hash = hashlib.sha256(marked.read_bytes()).hexdigest()
    core.send('reset-hard' if hard else 'reset', 'done', timeout=60)
    if hard:
        # The erase prompt is plain text with no title tab; Up confirms.
        core.wait_frame(root / 'reset-erase.png', lambda rows: rows is not None and not iiix_title(rows) and
                        sum(iiix_dark(rows, x, y) for x in range(160) for y in range(160)) > 300, 60)
        core.key(root, 'up', hold_ms=300)
        core.wait_frame(root / 'reset-setup.png', iiix_title, 120)
        iiix_setup(core, root)
    else:
        # 4.1 opens Preferences at Date & Time after a soft reset; Applications leaves it.
        rows = core.wait_frame(root / 'reset-launcher.png', lambda rows: iiix_launcher(rows) or iiix_title(rows), 120)
        if not iiix_launcher(rows):
            (root / 'reset-prefs.png').write_bytes((root / 'reset-launcher.png').read_bytes())
            core.tap(root, 13, 178)
            core.wait_frame(root / 'reset-launcher.png', iiix_launcher, 60)
    core.key(root, 'hard1', hold_ms=300)
    core.wait_frame(root / 'reset-input.png', lambda rows: iiix_title(rows) and not iiix_launcher(rows), 60)
    core.tap(root, 13, 178)
    core.wait_frame(root / 'reset-continued.png', iiix_launcher, 60)
    assert hashlib.sha256(marked.read_bytes()).hexdigest() == marked_hash
    (root / 'postconditions.json').write_text(json.dumps(
        {'setup_after_reset': hard, 'new_input': 'Date Book', 'final_screen': 'launcher',
         'marked_session': marked_hash}, indent=2) + '\n')


def e3_formats(rows):
    return (rows is not None and len(rows) == 480 and
            sum(max(rows[68][4*x:4*x+3]) < 60 for x in range(320)) > 300)


def e3_week_start(rows, monday):
    if not e3_formats(rows):
        return False
    popup = sum(max(rows[y][4*x:4*x+3]) < 80 for y in range(222, 240) for x in range(145, 242))
    m = max(rows[204][4*178:4*178+3]) < 80
    s = max(rows[204][4*183:4*183+3]) < 80
    return popup < 5 and m == monday and s != monday


def e3_open_formats(core, root):
    core.tap(root, 150, 280)
    # Preferences opens its category list; Formats is the second General entry.
    core.tap(root, 60, 104)
    core.wait_frame(root / 'formats.png', e3_formats, 30)


def e3_erase_confirmation(rows):
    if rows is None or len(rows) != 480:
        return False
    ink = lambda top, bottom: sum(max(rows[y][4*x:4*x+3]) < 80
                                 for y in range(top, bottom) for x in range(20, 300))
    return ink(10, 75) > 1500 and ink(250, 310) > 1500 and ink(330, 480) == 0


def e3_reset(core, root, hard):
    """Soft reset retains a Formats marker; hard reset erases it and repeats calibration."""
    e3_open_formats(core, root)
    core.wait_frame(root / 'marker-before.png', lambda rows: e3_week_start(rows, False), 30)
    core.tap(root, 234, 211)
    core.wait_frame(root / 'week-menu.png', lambda rows: e3_formats(rows) and
                    sum(max(rows[y][4*x:4*x+3]) < 80 for y in range(222, 240) for x in range(145, 242)) > 200, 30)
    core.tap(root, 225, 232)
    core.wait_frame(root / 'marker-monday.png', lambda rows: e3_week_start(rows, True), 30)
    machine = root / 'marked'
    core.send(f'save-session "{machine}"', 'wrote session')
    marked = machine / 'marked.frond'
    marked_hash = hashlib.sha256(marked.read_bytes()).hexdigest()
    core.tap(root, 32, 352)
    core.wait_frame(root / 'marked-launcher.png', e3_launcher, 30)
    core.send('reset-hard' if hard else 'reset', 'Set speed mode', timeout=60)
    if hard:
        core.wait_frame(root / 'reset-erase.png', e3_erase_confirmation, 120)
        core.key(root, 'up', hold_ms=600)
    e3_setup(core, root)
    e3_open_formats(core, root)
    core.wait_frame(root / 'marker-after.png', lambda rows: e3_week_start(rows, not hard), 30)
    core.tap(root, 32, 352)
    core.wait_frame(root / 'reset-launcher.png', e3_launcher, 30)
    core.key(root, 'hard1', hold_ms=300)
    core.wait_frame(root / 'reset-input.png', lambda rows: rows is not None and not e3_launcher(rows), 30)
    core.tap(root, 32, 352)
    core.wait_frame(root / 'reset-continued.png', e3_launcher, 30)
    assert hashlib.sha256(marked.read_bytes()).hexdigest() == marked_hash
    (root / 'postconditions.json').write_text(json.dumps(
        {'marker': 'Formats week start', 'before': 'Sunday', 'marked': 'Monday',
         'after': 'Sunday' if hard else 'Monday', 'new_input': 'Calendar',
         'final_screen': 'stock launcher',
         'marked_session': marked_hash}, indent=2) + '\n')


def e2_denvfs_formats(rows):
    return (rows is not None and len(rows) == 320 and
            sum(max(rows[68][4*x:4*x+3]) < 60 for x in range(320)) > 300)


def e2_denvfs_week_start(rows, monday):
    if not e2_denvfs_formats(rows):
        return False
    popup = sum(max(rows[y][4*x:4*x+3]) < 80 for y in range(222, 240) for x in range(145, 242))
    m = max(rows[204][4*178:4*178+3]) < 80
    s = max(rows[204][4*183:4*183+3]) < 80
    return popup < 5 and m == monday and s != monday


def e2_denvfs_open_formats(core, root):
    core.launch('Preferences')
    core.wait_frame(root / 'preferences.png', e2_denvfs_prefs, 30)
    core.tap(root, 60, 104)
    core.wait_frame(root / 'formats.png', e2_denvfs_formats, 30)


def e2_denvfs_erase_confirmation(rows):
    if rows is None or len(rows) != 320:
        return False
    def ink(top, bottom):
        return sum(max(rows[y][4*x:4*x+3]) < 80 for y in range(top, bottom) for x in range(320))
    return (ink(0, 120) == 0 and ink(120, 145) > 700 and ink(146, 168) > 500 and
            ink(170, 194) > 650 and ink(195, 320) == 0)


def e2_denvfs_reset(core, root, hard):
    """Soft reset retains the week-start preference; confirmed hard reset restores Sunday."""
    e2_denvfs_open_formats(core, root)
    core.wait_frame(root / 'marker-before.png', lambda rows: e2_denvfs_week_start(rows, False), 30)
    core.tap(root, 234, 211)
    core.wait_frame(root / 'week-menu.png', lambda rows: e2_denvfs_formats(rows) and
                    sum(max(rows[y][4*x:4*x+3]) < 80 for y in range(222, 240) for x in range(145, 242)) > 200, 30)
    core.tap(root, 225, 232)
    core.wait_frame(root / 'marker-monday.png', lambda rows: e2_denvfs_week_start(rows, True), 30)
    marked = root / 'marked'
    core.send(f'save-session "{marked}"', 'wrote session')
    marked_hash = hashlib.sha256(session_file(marked).read_bytes()).hexdigest()
    core.tap(root, 27, 352)
    core.wait_frame(root / 'marked-launcher.png', e2_denvfs_launcher, 30)
    core.send('reset-hard' if hard else 'reset', 'Set speed mode', timeout=60)
    if hard:
        core.wait_frame(root / 'reset-erase.png', e2_denvfs_erase_confirmation, 120)
        core.key(root, 'up', hold_ms=600)
    e2_denvfs_setup(core, root)
    e2_denvfs_open_formats(core, root)
    core.wait_frame(root / 'marker-after.png', lambda rows: e2_denvfs_week_start(rows, not hard), 30)
    core.tap(root, 27, 352)
    core.wait_frame(root / 'reset-launcher.png', e2_denvfs_launcher, 30)
    core.key(root, 'hard1', hold_ms=300)
    core.wait_frame(root / 'reset-input.png', lambda rows: rows is not None and not e2_denvfs_launcher(rows), 30)
    core.tap(root, 27, 352)
    core.wait_frame(root / 'reset-continued.png', e2_denvfs_launcher, 30)
    assert hashlib.sha256(session_file(marked).read_bytes()).hexdigest() == marked_hash
    (root / 'postconditions.json').write_text(json.dumps(
        {'marker': 'Formats week start', 'before': 'Sunday', 'marked': 'Monday',
         'after': 'Sunday' if hard else 'Monday', 'new_input': 'Calendar',
         'final_screen': 'de-NVFS launcher', 'marked_session': marked_hash}, indent=2) + '\n')


RESET_RECIPES = {'tg50': tg50_reset, 'iiix': iiix_reset, 'e3': e3_reset,
                 'e2-denvfs': e2_denvfs_reset}


def reset_recipe(device, variant=None):
    recipe = variant_recipe(device, variant)
    try:
        return RESET_RECIPES[recipe]
    except KeyError:
        raise ValueError(f'No measured reset recipe for {device}: {recipe}') from None
