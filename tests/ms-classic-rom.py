#!/usr/bin/env python3
"""Classic Memory Stick sizes on the real TG50 ROM, through the desktop control socket.

  tests/ms-classic-rom.py MB [MB...]

Per size: ms-create in a fresh machine folder from the ready TG50 session, the Palm's "Unrecognized
Format" prompt, its format (OK), the capacity ms-files reports and a Card Info screenshot, a file put
and got back, a save, then a fresh process from that save that gets the file back and exports the
stick. Needs `tests/smoke.py sessions`. Evidence goes in build/step-large-card/ms-classic/MB/ (mode
0700); the stick copies are deleted afterwards.
"""
import argparse
import json
import os
import shutil
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'tools'))
import smoke  # noqa: E402
import palm_boot  # noqa: E402

DEVICE = 'sony-clie-tg50'
OUT = ROOT / 'build/step-large-card/ms-classic'
MIB = 1 << 20
GEOMETRY = {4: (512, 16), 8: (1024, 16), 16: (1024, 32), 32: (2048, 32), 64: (4096, 32), 128: (8192, 32)}


def format_prompt(rows):
    """The Unrecognized Format alert: its blue title bar fills row 140."""
    if rows is None or len(rows) != 320:
        return False
    return sum(1 for x in range(320) if rows[140][4*x] < 80 and rows[140][4*x+1] < 40 and rows[140][4*x+2] > 120) > 250


def black(rows):
    """The LCD is off (RGBA rows; alpha is always 255)."""
    return rows is None or not any(max(row[i] for i in range(0, len(row), 4)) > 40 or
                                   max(row[i] for i in range(1, len(row), 4)) > 40 or
                                   max(row[i] for i in range(2, len(row), 4)) > 40 for row in rows[::16])


def formatting(rows):
    """Date Book under the Formatting... box (its blue border crosses the middle rows)."""
    return palm_boot.tg50_datebook(rows) and any(
        rows[y][4*x] < 80 and rows[y][4*x+1] < 40 and rows[y][4*x+2] > 120 for y in (140, 160, 180) for x in range(80, 240))


def hexpath(path):
    return str(path).encode().hex()


def space(run):
    message = run.control.retry_ready('ms-files', '/', 300)['message']
    first = message.splitlines()[0].split('\t')
    return int(first[1]), int(first[2]), message


def datebook(run, work, name, ready=60):
    """Date Book's day view. hard1 opens Date Book, and in Date Book moves on to the next view."""
    for attempt in range(6):
        rows = run.frame(work / f'{name}.png')
        if not black(rows) and palm_boot.tg50_datebook(rows):
            run.ready(ready)
            return
        run.boot.key(work, 'power' if black(rows) else 'hard1', hold_ms=600 if not black(rows) else 200)
        time.sleep(3)
    raise smoke.Fail(f'{name}: no Date Book day view')


def qualify(mb):
    work = OUT / str(mb)
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True, mode=0o700)
    result = {'mb': mb, 'problems': []}
    folder1, folder2, saved, image = work / 'machine-1', work / 'machine-2', work / 'saved', work / 'stick.frondms'
    try:
        checks(mb, work, result, folder1, folder2, saved, image)
    except Exception as error:  # noqa: BLE001 - recorded with what came before it
        result['problems'].append(f'{type(error).__name__}: {error}')
    finally:
        for big in (image, folder1, folder2, saved):
            if big.is_dir():
                shutil.rmtree(big, ignore_errors=True)
            else:
                big.unlink(missing_ok=True)
    return result


def checks(mb, work, result, folder1, folder2, saved, image):
    problems = result['problems']
    payload = work / 'put.bin'
    payload.write_bytes(os.urandom(70000) + b'Written by Frond\n')
    folder1.mkdir()
    run = smoke.Run(DEVICE, work, smoke.session_path(DEVICE), extra=['--work-folder', folder1])
    run.control.command_timeout = 600
    try:
        run.ready()
        datebook(run, work, 'datebook-1')
        if run.require('ms-state')['message'] == 'inserted':
            run.require('ms-eject')
            time.sleep(2)
        inserted = time.monotonic()
        created = run.control.request('ms-create', str(mb))
        result['create'] = {k: created[k] for k in ('ok', 'code', 'message')}
        if not created['ok']:
            problems.append(f'ms-create refused: {created["message"]}')
            return
        # The driver scans the whole stick first, longer the bigger it is; the TG50 may sleep
        # meanwhile, so a second black frame in a row gets the power button.
        dark = 0
        while not format_prompt(rows := run.frame(work / 'unrecognized-format.png')):
            dark = dark + 1 if black(rows) else 0
            if dark == 2:
                result.setdefault('woke', []).append(round(time.monotonic() - inserted))
                run.boot.key(work, 'power', hold_ms=200)
                dark = 0
            if time.monotonic() - inserted > 600:
                problems.append('no Unrecognized Format prompt')
                return
            time.sleep(2)
        result['prompt_seconds'] = round(time.monotonic() - inserted, 1)
        started = time.monotonic()
        run.tap(work, 49, 294)
        # Frames every 2 s until the Formatting... box goes. The TG50 may sleep: a second black frame
        # in a row gets the power button.
        rows, n, dark = None, 0, 0
        while time.monotonic() - started < 600:
            n += 1
            rows = run.frame(work / f'format-{n:03d}.png')
            dark = dark + 1 if black(rows) else 0
            if not dark and not formatting(rows) and not format_prompt(rows):
                break
            if dark == 2:
                result.setdefault('woke', []).append(round(time.monotonic() - started))
                run.boot.key(work, 'power', hold_ms=200)
                dark = 0
            time.sleep(2)
        if black(rows) or formatting(rows):
            problems.append('the format did not finish')
            return
        shutil.copy(work / f'format-{n:03d}.png', work / 'formatted.png')
        result['format_box_seconds'] = round(time.monotonic() - started, 1)
        # The driver scans the whole stick again after the format.
        datebook(run, work, 'datebook-formatted', ready=600)
        result['format_seconds'] = round(time.monotonic() - started, 1)
        used, total, listing = space(run)
        result['space'] = {'used': used, 'total': total}
        if not (mb << 20) * 0.85 <= total <= mb << 20 or used > total:
            problems.append(f'capacity {used}/{total} is outside 85-100% of {mb} MB')
        run.control.retry_ready('launch-app', 'Card Info', 60)
        time.sleep(4)
        run.frame(work / 'card-info.png')
        datebook(run, work, 'datebook-2')
        run.control.retry_ready('ms-put', hexpath(payload) + '\n' + hexpath('/FROND.BIN'), 240)
        got = work / 'got-1.bin'
        run.control.retry_ready('ms-get', hexpath(got) + '\n' + hexpath('/FROND.BIN'), 240)
        if got.read_bytes() != payload.read_bytes():
            problems.append('the file got back differs from the one put')
        result['after_put'] = space(run)[2]
        started = time.monotonic()
        run.require('save', saved)
        result['save_seconds'] = round(time.monotonic() - started, 1)
    finally:
        run.close()
    reload(mb, work, result, payload, saved, folder2, image)


def reload(mb, work, result, payload, saved, folder2, image):
    problems = result['problems']
    folder2.mkdir()
    run = smoke.Run(DEVICE, work, saved, extra=['--work-folder', folder2])
    run.control.command_timeout = 600
    try:
        run.ready()
        result['reload_state'] = run.require('ms-state')['message']
        datebook(run, work, 'datebook-reload', ready=600)
        used, total, _ = space(run)
        result['reload_space'] = {'used': used, 'total': total}
        if total != result['space']['total']:
            problems.append(f'reloaded capacity {total} is not {result["space"]["total"]}')
        got = work / 'got-2.bin'
        run.control.retry_ready('ms-get', hexpath(got) + '\n' + hexpath('/FROND.BIN'), 240)
        if got.read_bytes() != payload.read_bytes():
            problems.append('reload: the file differs')
        run.require('ms-eject')
        time.sleep(2)
        run.require('ms-export', image)
    finally:
        run.close()
    blocks, pages = GEOMETRY[mb]
    header = image.read_bytes()[:64]
    size = image.stat().st_size
    # A version 2 .frondms: the logical blocks (494 in the first segment, 496 in each later one)
    # as the payload, the side region after the next 1 MiB, then a CRC32 per 1 MiB chunk.
    payload, side = struct.unpack_from('<Q', header, 16)[0], struct.unpack_from('<Q', header, 48)[0]
    stored = -(-payload // MIB) * MIB + side
    result['export'] = {'bytes': size, 'version': struct.unpack_from('<I', header, 8)[0], 'payload': payload, 'side': side}
    logical = 494 + 496 * (blocks // 512 - 1)
    if (header[:8] != b'FRONDMS\0' or result['export']['version'] != 2 or payload != logical * pages * 512
            or not side or size != 64 + stored + 4 * -(-stored // MIB)):
        problems.append(f'export {result["export"]} is not a {mb} MB version 2 stick')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('sizes', nargs='+', type=int, choices=sorted(GEOMETRY))
    args = p.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    OUT.parent.chmod(0o700)
    OUT.chmod(0o700)
    failed = False
    for mb in args.sizes:
        started = time.monotonic()
        try:
            result = qualify(mb)
        except Exception as error:  # noqa: BLE001 - every failure is recorded
            result = {'mb': mb, 'problems': [f'{type(error).__name__}: {error}']}
        result['seconds'] = round(time.monotonic() - started)
        (OUT / f'{mb}.json').write_text(json.dumps(result, indent=2) + '\n')
        failed |= bool(result['problems'])
        print(f'{"FAIL" if result["problems"] else "PASS"} {mb} MB: space {result.get("space")} '
              f'({result["seconds"]} s) {"; ".join(result["problems"])}', flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
