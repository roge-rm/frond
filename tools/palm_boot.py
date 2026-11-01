"""Frame checks and setup taps shared by ROM tests and the private app runner."""
import struct
import time
import zlib

DEVICES = {
    'palm-tx': {'nor': 16 << 20, 'nand': 132 << 20, 'height': 480, 'input_height': 480},
    'palm-e2': {'nor': 8 << 20, 'nand': 34603008, 'height': 320, 'input_height': 440},
    'palm-zire-72': {'nor': 14942208, 'nand': 0, 'height': 320, 'input_height': 440},
}


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


def zire_setup(core, root):
    """Calibrate and finish setup, waiting for each form and the launcher."""
    def red(rows, x, y):
        # The target's red rings around its centre.
        return sum(1 for dx in range(-12, 13, 3) for dy in range(-12, 13, 3)
                   if 0 <= x + dx < 320 and 0 <= y + dy < 320 and rows[y + dy][4 * (x + dx)] > 180
                   and rows[y + dy][4 * (x + dx) + 1] < 80) > 10

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
    core.wait_frame(root / 'launcher.png', launcher_wallpaper, 60)


def e2_setup(core, root):
    """Finish the E2's Date & Time form and open Applications from the silkscreen."""
    core.wait_frame(root / 'boot-frame.png', lambda rows: rows is not None and date_time_prefs(rows), 120)
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
