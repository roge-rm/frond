#!/usr/bin/env python3
"""Memory Stick Pro on the real TG50 ROM, through the desktop control socket.

  tests/ms-pro-rom.py MIB [MIB...]      (256, 512, 1024 or 1886)

Per size, in a fresh machine folder from the ready TG50 session: ms-create's preformatted stick
mounts (ms-files capacity); then a blank raw image of that size goes in with ms-insert, the Palm
says "Unrecognized Format", formats it (OK), and the capacity ms-files reports, a Card Info
screenshot, a file put and got back and a save follow; a fresh process from that save gets the
file back and exports the stick, which mtools reads. Needs `tests/smoke.py sessions`. Evidence goes
in build/step-large-card/ms-pro/MIB/ (mode 0700); the stick copies are deleted afterwards.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'tools'))
import smoke  # noqa: E402
import palm_boot  # noqa: E402

DEVICE = 'sony-clie-tg50'
OUT = ROOT / 'build/step-large-card/ms-pro'
SIZES = (256, 512, 1024, 1886)


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


def qualify(mib):
    work = OUT / str(mib)
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True, mode=0o700)
    result = {'mib': mib, 'problems': []}
    folder1, folder2, saved = work / 'machine-1', work / 'machine-2', work / 'saved'
    blank, image = work / 'blank.img', work / 'stick.img'
    try:
        checks(mib, work, result, folder1, folder2, saved, blank, image)
    except Exception as error:  # noqa: BLE001 - recorded with what came before it
        result['problems'].append(f'{type(error).__name__}: {error}')
    finally:
        for big in (blank, image, folder1, folder2, saved):
            if big.is_dir():
                shutil.rmtree(big, ignore_errors=True)
            else:
                big.unlink(missing_ok=True)
    return result


def wait_format(run, work, result, started):
    """Taps OK on the Unrecognized Format prompt, then waits for the Formatting... box to go."""
    problems = result['problems']
    dark = 0
    while not format_prompt(rows := run.frame(work / 'unrecognized-format.png')):
        dark = dark + 1 if black(rows) else 0
        if dark == 2:
            result.setdefault('woke', []).append(round(time.monotonic() - started))
            run.boot.key(work, 'power', hold_ms=200)
            dark = 0
        if time.monotonic() - started > 600:
            problems.append('no Unrecognized Format prompt')
            return False
        time.sleep(2)
    result['prompt_seconds'] = round(time.monotonic() - started, 1)
    started = time.monotonic()
    run.tap(work, 49, 294)
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
        return False
    shutil.copy(work / f'format-{n:03d}.png', work / 'formatted.png')
    result['format_box_seconds'] = round(time.monotonic() - started, 1)
    datebook(run, work, 'datebook-formatted', ready=600)
    result['format_seconds'] = round(time.monotonic() - started, 1)
    return True


def checks(mib, work, result, folder1, folder2, saved, blank, image):
    problems = result['problems']
    payload = work / 'put.bin'
    payload.write_bytes(os.urandom(70000) + b'Written by Frond\n')
    with open(blank, 'wb') as f:
        f.truncate(mib << 20)
    folder1.mkdir()
    run = smoke.Run(DEVICE, work, smoke.session_path(DEVICE), extra=['--work-folder', folder1])
    run.control.command_timeout = 600
    try:
        run.ready()
        datebook(run, work, 'datebook-1')
        if run.require('ms-state')['message'] == 'inserted':
            run.require('ms-eject')
            time.sleep(2)
        created = run.control.request('ms-create', str(mib))
        result['create'] = {k: created[k] for k in ('ok', 'code', 'message')}
        if not created['ok']:
            problems.append(f'ms-create refused: {created["message"]}')
            return
        result['type'] = run.require('ms-type')['message']
        time.sleep(5)
        datebook(run, work, 'datebook-created', ready=300)
        used, total, _ = space(run)
        result['created_space'] = {'used': used, 'total': total}
        if not (mib << 20) * 0.9 <= total <= mib << 20 or used > total:
            problems.append(f'preformatted capacity {used}/{total} is outside 90-100% of {mib} MiB')
        run.require('ms-eject')
        time.sleep(2)
        started = time.monotonic()
        inserted = run.control.request('ms-insert', str(blank))
        result['insert_blank'] = {k: inserted[k] for k in ('ok', 'code', 'message')}
        if not inserted['ok']:
            problems.append(f'ms-insert refused: {inserted["message"]}')
            return
        if not wait_format(run, work, result, started):
            return
        used, total, listing = space(run)
        result['space'] = {'used': used, 'total': total}
        if not (mib << 20) * 0.9 <= total <= mib << 20 or used > total:
            problems.append(f'capacity {used}/{total} is outside 90-100% of {mib} MiB')
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
        result['saved_files'] = sorted(p.name for p in saved.iterdir()) if saved.is_dir() else []
    finally:
        run.close()
    reload(mib, work, result, payload, saved, folder2, image)


def reload(mib, work, result, payload, saved, folder2, image):
    problems = result['problems']
    folder2.mkdir()
    run = smoke.Run(DEVICE, work, saved, extra=['--work-folder', folder2])
    run.control.command_timeout = 600
    try:
        run.ready()
        result['reload_state'] = run.require('ms-state')['message']
        result['reload_type'] = run.require('ms-type')['message']
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
    size = image.stat().st_size
    result['export'] = {'bytes': size}
    if size != mib << 20:
        problems.append(f'export is {size} bytes, not {mib} MiB')
    # The partition starts at sector 63, as in the attribute area's MBR entry.
    for tool in (['minfo', '-i', f'{image}@@32256', '::'], ['mdir', '-i', f'{image}@@32256', '::']):
        done = subprocess.run(tool, capture_output=True, text=True)
        (work / f'{tool[0]}.txt').write_text(done.stdout + done.stderr)
        if done.returncode:
            problems.append(f'{tool[0]} failed on the export')
    if 'FROND' not in (work / 'mdir.txt').read_text():
        problems.append('the export has no FROND.BIN')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('sizes', nargs='+', type=int, choices=SIZES)
    args = p.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    OUT.parent.chmod(0o700)
    OUT.chmod(0o700)
    failed = False
    for mib in args.sizes:
        started = time.monotonic()
        try:
            result = qualify(mib)
        except Exception as error:  # noqa: BLE001 - every failure is recorded
            result = {'mib': mib, 'problems': [f'{type(error).__name__}: {error}']}
        result['seconds'] = round(time.monotonic() - started)
        (OUT / f'{mib}.json').write_text(json.dumps(result, indent=2) + '\n')
        failed |= bool(result['problems'])
        print(f'{"FAIL" if result["problems"] else "PASS"} {mib} MiB: space {result.get("space")} '
              f'({result["seconds"]} s) {"; ".join(result["problems"])}', flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
