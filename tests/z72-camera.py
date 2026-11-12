#!/usr/bin/env python3
"""Zire 72 Camera shows and photographs the core's built-in test pattern through the real ROM.

run --core CORE --session SESSION --output DIR
    opens Camera, checks the viewfinder against the pattern, takes and saves a photo at each
    size the Camera offers, exports each photo and checks its pixels against the pattern. Then
    saves a session while the viewfinder runs, loads it in a new process and checks the
    viewfinder and a photo again. Then the host sources: a picture file in the viewfinder and a
    1280 x 960 photo of it, frames written into the shared frames file, and a save putting the
    camera back to the pattern. SESSION is a Zire 72 session at the launcher with the setup
    Reminder up (the smoke session); private, so it's never copied anywhere else.

Needs ffmpeg to decode the PNG screenshots and JPEG photos.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import mmap
import time
import zipfile
import zlib

spec = importlib.util.spec_from_file_location('voice_memo', Path(__file__).with_name('z72-voice-memo.py'))
voice_memo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(voice_memo)
Core = voice_memo.Core

BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
        (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
DIGITS = [(7, 5, 5, 5, 7), (2, 6, 2, 2, 7), (7, 1, 7, 4, 7), (7, 1, 7, 1, 7), (5, 5, 7, 1, 1),
          (7, 4, 7, 1, 7), (7, 4, 7, 5, 7), (7, 1, 1, 1, 1), (7, 5, 7, 5, 7), (7, 5, 7, 1, 7)]


def geometry(w, h):
    bars_end, band_end = h * 7 // 10, h * 8 // 10
    return bars_end, band_end, max(1, (h - band_end) // 7)


def pattern_pixel(w, h, frame, x, y):
    """The core's cameraTestPattern (uarm/camera_pattern.cpp) at one pixel."""
    bars_end, band_end, cell = geometry(w, h)
    if y < bars_end:
        return BARS[x * 8 // w]
    if y < band_end:
        start = (frame % 64) * w // 64
        return (255,) * 3 if start <= x < start + (w + 15) // 16 else (64,) * 3
    cy, cx = (y - band_end) // cell, x // cell
    if 1 <= cy <= 5 and 1 <= cx < 25:
        digit, column = (cx - 1) // 4, (cx - 1) % 4
        value = frame // 10 ** (5 - digit) % 10
        if column < 3 and DIGITS[value][cy - 1] >> (2 - column) & 1:
            return (255,) * 3
    return (0,) * 3


def picture_pixel(w, h, x, y, invert=False):
    """The host-source picture: eight colour columns, darker in each of six rows."""
    bar = BARS[7 - x * 8 // w if invert else x * 8 // w]
    row = y * 6 // h
    return tuple(c * (6 - row) // 6 for c in bar)


def picture_rgb(w, h, invert=False):
    return b''.join(bytes(picture_pixel(w, h, x, y, invert)) for y in range(h) for x in range(w))


def write_png(path, w, h, rgb):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def write_frame(path, w, h, rgb):
    """Writes one RGB24 frame into the core's frames file the way a frontend does."""
    with open(path, 'r+b') as f, mmap.mmap(f.fileno(), 0) as m:
        assert m[:8] == b'FrondCam' and struct.unpack_from('<I', m, 8)[0] == 1
        seq = struct.unpack_from('<I', m, 16)[0]
        struct.pack_into('<I', m, 16, seq | 1)
        struct.pack_into('<IIIII', m, 20, 1, w, h, 0, 0)
        m[64:64 + len(rgb)] = rgb
        struct.pack_into('<I', m, 16, (seq | 1) + 1)


def compare_picture(w, h, rgb, x0=0, y0=0, stride=None, edge=4, invert=False):
    """As compare, against the host-source picture."""
    stride = stride or w
    total = far = count = strict = worst = 0
    for y in range(h):
        for x in range(w):
            want = picture_pixel(w, h, x, y, invert)
            at = ((y0 + y) * stride + x0 + x) * 3
            d = max(abs(rgb[at + i] - want[i]) for i in range(3))
            total += d
            count += 1
            near = any(picture_pixel(w, h, min(w - 1, max(0, x + dx)), min(h - 1, max(0, y + dy)), invert) != want
                       for dx, dy in ((-edge, 0), (edge, 0), (0, -edge), (0, edge)))
            if not near:
                strict += 1
                worst = max(worst, d)
                far += d > 24
    return {'mean': round(total / count, 2), 'strict_pixels': strict, 'strict_over_24': far, 'strict_worst': worst}


def decode(path):
    """Width, height and rgb24 bytes of an image, through ffmpeg."""
    probe = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'stream=width,height', '-of', 'csv=p=0',
                            str(path)], check=True, capture_output=True, text=True).stdout.split(',')
    w, h = int(probe[0]), int(probe[1])
    rgb = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(path), '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                         check=True, capture_output=True).stdout
    assert len(rgb) == w * h * 3, (path, len(rgb), w, h)
    return w, h, rgb


def read_frame(w, h, rgb, x0=0, y0=0, stride=None):
    """Reads the six-digit frame counter from the centres of its cells."""
    stride = stride or w
    _, band_end, cell = geometry(w, h)
    frame = 0
    for digit in range(6):
        bits = []
        for row in range(5):
            line = 0
            for column in range(3):
                x = x0 + (1 + digit * 4 + column) * cell + cell // 2
                y = y0 + band_end + (1 + row) * cell + cell // 2
                line = line << 1 | (rgb[(y * stride + x) * 3 + 1] > 128)
            bits.append(line)
        assert tuple(bits) in DIGITS, ('unreadable digit', digit, bits)
        frame = frame * 10 + DIGITS.index(tuple(bits))
    return frame


def compare(w, h, rgb, frame, x0=0, y0=0, stride=None, edge=4):
    """Per-channel differences against the pattern. Pixels within edge of a colour change are
    left out of the strict count, since JPEG and chroma subsampling blur them."""
    stride = stride or w
    total = far = count = strict = 0
    worst = 0
    for y in range(h):
        for x in range(w):
            want = pattern_pixel(w, h, frame, x, y)
            at = ((y0 + y) * stride + x0 + x) * 3
            d = max(abs(rgb[at + i] - want[i]) for i in range(3))
            total += d
            count += 1
            near = any(pattern_pixel(w, h, frame, min(w - 1, max(0, x + dx)), min(h - 1, max(0, y + dy))) != want
                       for dx, dy in ((-edge, 0), (edge, 0), (0, -edge), (0, edge)))
            if not near:
                strict += 1
                worst = max(worst, d)
                far += d > 24
    return {'mean': round(total / count, 2), 'strict_pixels': strict, 'strict_over_24': far, 'strict_worst': worst}


def run_replay(core, work, name, text):
    path = work / f'{name}.replay'
    path.write_text(text.replace('S/', str(work) + '/'))
    core.replay(path)


def databases(core):
    deadline = time.monotonic() + 30
    while True:
        ok, code, listing = core.request('db-list')
        if ok or code != 'not-ready' or time.monotonic() > deadline:
            break
        time.sleep(.5)
    assert ok, (code, listing)
    names = []
    for line in listing.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[0] == '43616d72' and parts[1] == '466f746f':  # Camr/Foto
            names.append(bytes.fromhex(parts[3]).decode('latin-1'))
    return names


def photo_from_archive(archive, out):
    """A file stream database: DBLK blocks holding the JPEG file."""
    with zipfile.ZipFile(archive) as z:
        b = z.read(z.namelist()[0])
    n = struct.unpack_from('>H', b, 76)[0]
    offsets = [struct.unpack_from('>I', b, 78 + 8 * i)[0] for i in range(n)] + [len(b)]
    data = bytearray()
    for start, end in zip(offsets, offsets[1:]):
        assert b[start:start + 4] == b'DBLK'
        data += b[start + 8:start + 8 + struct.unpack_from('>I', b, start + 4)[0]]
    assert data[:2] == b'\xff\xd8', data[:4]
    out.write_bytes(data)


DISMISS = '500 pen-down 50 280\n600 pen-up\n1500 pen-up\n'


def shots(name, at):
    """Three screenshots 300 ms apart: one may catch the overlay while DMA is writing it."""
    return ''.join(f'{at + i * 300} snapshot "S/{name}-{i}.png"\n' for i in range(3))


OPEN = '500 pen-down 160 135\n600 pen-up\n6000 pen-up\n' + shots('viewfinder', 6001)
# The size popup's items, from 1280 x 960 down to 160 x 120.
SIZES = {(1280, 960): 238, (640, 480): 261, (320, 240): 283, (160, 120): 305}
SHOOT = ('500 pen-down 255 282\n600 pen-up\n4000 pen-up\n4001 snapshot "S/review.png"\n'
         '4500 pen-down 236 270\n4600 pen-up\n9000 pen-up\n')
HOME = '500 pen-down 23 352\n600 pen-up\n3000 pen-up\n'


def check_viewfinder(work, name, invert=None):
    """The viewfinder is overlay 2 at 0,0, 320 x 240, over the 320 x 320 screen. Takes the
    first of the three shots with a readable counter, or the last one's failure. With invert
    set it's checked against the host-source picture (inverted or not) instead."""
    result = {}
    for i in range(3):
        w, h, rgb = decode(work / f'{name}-{i}.png')
        if invert is not None:
            result = dict(compare_picture(320, 240, rgb, stride=w, edge=2, invert=invert), shot=i)
            if result['strict_over_24'] == 0:
                break
            continue
        try:
            frame = read_frame(320, 240, rgb, stride=w)
        except AssertionError as e:
            result = {'error': str(e), 'shot': i}
            continue
        result = compare(320, 240, rgb, frame, stride=w, edge=2)
        result.update(frame=frame, shot=i)
        if result['strict_over_24'] == 0:
            break
    return result


def shoot(core, work, size, before, picture=False):
    run_replay(core, work, 'size', f'500 pen-down 100 301\n600 pen-up\n1500 pen-down 90 {SIZES[size]}\n'
                                   '1600 pen-up\n3000 pen-up\n')
    run_replay(core, work, 'shoot', SHOOT)
    run_replay(core, work, 'home', HOME)
    photos = [p for p in databases(core) if p not in before]
    assert len(photos) == 1, photos
    archive = work / f'photo-{size[0]}.zip'
    ok, code, message = core.request('export-db', f'{archive}\n{photos[0]}')
    assert ok, (code, message)
    jpeg = work / f'photo-{size[0]}.jpg'
    photo_from_archive(archive, jpeg)
    w, h, rgb = decode(jpeg)
    result = {'photo': photos[0], 'width': w, 'height': h}
    if (w, h) == size and picture:
        result.update(compare_picture(w, h, rgb))
    elif (w, h) == size:
        result['frame'] = read_frame(w, h, rgb)
        result.update(compare(w, h, rgb, result['frame']))
    return result, photos[0]


def session(args, name, source, steps):
    work = args.output / name
    machine = work / 'machine'
    machine.mkdir(parents=True)
    shutil.copy(source, machine / 'machine.frond')
    os.chmod(machine / 'machine.frond', 0o600)
    core = Core(args.core, machine, 0, work / 'core.log')
    try:
        core.wait_ready()
        return steps(core, work)
    finally:
        core.quit()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=['run'])
    parser.add_argument('--core', required=True)
    parser.add_argument('--session', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.core = str(Path(args.core).resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    os.chmod(args.output, 0o700)
    results = {}

    def sizes(core, work):
        run_replay(core, work, 'dismiss', DISMISS)
        core.wait_ready()
        before = databases(core)
        out = {}
        for size in SIZES:
            sub = work / f'{size[0]}'
            sub.mkdir()
            run_replay(core, sub, 'open', OPEN)
            if size == (1280, 960):
                out['viewfinder'] = check_viewfinder(sub, 'viewfinder')
            out[f'{size[0]}x{size[1]}'], photo = shoot(core, sub, size, before)
            before.append(photo)
        return out
    results.update(session(args, 'sizes', args.session, sizes))

    def save_during(core, work):
        run_replay(core, work, 'dismiss', DISMISS)
        core.wait_ready()
        run_replay(core, work, 'open', OPEN)
        before = check_viewfinder(work, 'viewfinder')
        ok, code, message = core.request('save', str(work / 'during.frond'))
        assert ok, (code, message)
        run_replay(core, work, 'later', shots('after-save', 3000))
        return {'before-save': before, 'after-save': check_viewfinder(work, 'after-save')}
    results['save-during'] = session(args, 'save-during', args.session, save_during)

    def load_saved(core, work):
        run_replay(core, work, 'wait', shots('loaded', 3000))
        out = {'viewfinder': check_viewfinder(work, 'loaded')}
        # The photo databases before the load are those of the smoke session's Camera.
        run_replay(core, work, 'shoot', SHOOT)
        run_replay(core, work, 'home', HOME)
        photos = databases(core)
        archive = work / 'photo.zip'
        newest = [p for p in photos if p.startswith('Photo_')]
        assert newest, photos
        ok, code, message = core.request('export-db', f'{archive}\n{sorted(newest)[-1]}')
        assert ok, (code, message)
        photo_from_archive(archive, work / 'photo.jpg')
        w, h, rgb = decode(work / 'photo.jpg')
        frame = read_frame(w, h, rgb)
        out['photo'] = dict(compare(w, h, rgb, frame), width=w, height=h, frame=frame)
        return out
    results['load-saved'] = session(args, 'load-saved', args.output / 'save-during' / 'during' / 'during.frond', load_saved)

    def host_sources(core, work):
        out = {}
        png = work / 'picture.png'
        write_png(png, 1280, 960, picture_rgb(1280, 960))
        # Starts on the pattern; a device that isn't a webcam is refused and says why.
        out['start-state'] = core.request('camera-state')[2]
        out['bad-webcam'] = core.request('camera-mode', 'webcam:/dev/null')[2]
        run_replay(core, work, 'dismiss', DISMISS)
        core.wait_ready()
        before = databases(core)
        ok, code, message = core.request('camera-mode', f'picture:{png}')
        assert ok, (code, message)
        out['picture-state'] = message
        run_replay(core, work, 'open', OPEN)
        out['picture-viewfinder'] = check_viewfinder(work, 'viewfinder', invert=False)
        out['picture-photo'], _ = shoot(core, work, (1280, 960), before, picture=True)
        # Pushed frames: 640 x 480 RGB of the picture turned around, scaled by the core.
        ok, code, frames = core.request('camera-mode', 'frames')
        assert ok, (code, frames)
        out['frames-file'] = Path(frames).name
        write_frame(frames, 640, 480, picture_rgb(640, 480, invert=True))
        run_replay(core, work, 'open-frames', OPEN.replace('viewfinder', 'frames'))
        out['frames-viewfinder'] = check_viewfinder(work, 'frames', invert=True)
        # A save puts the camera back to the pattern.
        ok, code, message = core.request('save', str(work / 'saved.frond'))
        assert ok, (code, message)
        out['after-save-state'] = core.request('camera-state')[2]
        run_replay(core, work, 'after-save', shots('pattern', 3000))
        out['after-save-viewfinder'] = check_viewfinder(work, 'pattern')
        return out
    results['host-sources'] = session(args, 'host-sources', args.session, host_sources)
    (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')

    failures = []

    def check(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            failures.append(what)

    def matches(r, mean, strict):
        return r.get('mean', 999) <= mean and r.get('strict_over_24', 1) <= strict * r.get('strict_pixels', 0)

    print(json.dumps(results, indent=1))
    # The viewfinder goes through RGB565, so up to 8 a channel off.
    check(matches(results['viewfinder'], 6, 0), 'the viewfinder shows the pattern')
    for size in SIZES:
        r = results[f'{size[0]}x{size[1]}']
        check((r['width'], r['height']) == size, f'the {size[0]} x {size[1]} photo has that size')
        check(matches(r, 8, .001), f'the {size[0]} x {size[1]} photo matches the pattern')
    vf = results['save-during']['after-save']
    check(matches(vf, 6, 0), 'the viewfinder runs on after a save')
    loaded = results['load-saved']
    # A fresh start would count from 0; a load goes on from the saved frame.
    check(matches(loaded['viewfinder'], 6, 0) and
          loaded['viewfinder'].get('frame', 0) > results['save-during']['before-save'].get('frame', 1e9),
          'a session saved with the viewfinder up loads with it running')
    check(matches(loaded['photo'], 8, .001), 'a photo after the load matches the pattern')
    host = results['host-sources']
    check(host['start-state'] == 'pattern', 'the camera starts on the pattern')
    check(host['bad-webcam'] == "pattern That device can't capture video", 'a device that is no webcam is refused')
    check(host['picture-state'] == 'picture', 'the picture source arms')
    check(matches(host['picture-viewfinder'], 6, 0), 'the viewfinder shows the picture')
    r = host['picture-photo']
    check((r['width'], r['height']) == (1280, 960) and matches(r, 8, .001), 'a 1280 x 960 photo matches the picture')
    check(host['frames-file'] == '.camera-frames', 'the frames file is in the machine folder')
    check(matches(host['frames-viewfinder'], 6, 0), 'the viewfinder shows pushed frames')
    check(host['after-save-state'] == 'pattern' and matches(host['after-save-viewfinder'], 6, 0),
          'a save puts the camera back to the pattern')
    if failures:
        sys.exit(f'{len(failures)} check(s) failed')


if __name__ == '__main__':
    main()
