#!/usr/bin/env python3
"""Large SD card qualification on real firmware, through the SD probe and the desktop control socket.

  tests/large-card-rom.py DEVICE MIB [MIB...]

Per size: a card made by sd-create in a fresh machine folder, the probe's VolumeSize against the layout,
write, read and delete, a guest format (the probe formats the card when a FrondSDFormat database is
installed) and the same again on the new volume, a save, then a fresh process from that save that reads
the guest's file back. The exported card is checked with mtools. Needs the ready sessions from
`tests/smoke.py sessions` and the Palm OS SDK for the probe. Evidence goes in
build/step-large-card/qualify/DEVICE-MIB/ (mode 0700); the card copies are deleted afterwards.
"""
import argparse
import json
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'tools'))
import smoke  # noqa: E402
from sd_results import export_if_ready, read_log  # noqa: E402

OUT = ROOT / 'build/step-large-card/qualify'


def layout_bytes(mib):
    """Data area bytes of the card frondSd::layout makes (native/SdImage.h)."""
    start, total = 63, (mib << 20) // 512
    sectors = total - start
    cluster = 4 if mib <= 64 else 8 if mib <= 256 else 16 if mib == 512 else 32 if mib == 1024 else 64
    fat = reserved = 1
    while True:
        clusters = min((sectors - reserved - 32 - 2 * fat) // cluster, 65524)
        required = ((clusters + 2) * 2 + 511) // 512
        if fat >= required:
            break
        fat = required
    return clusters * cluster * 512


def format_pdb(path):
    """An empty database named FrondSDFormat; the probe formats the card once while it's there."""
    header = b'FrondSDFormat'.ljust(32, b'\0') + struct.pack('>HHIIIIII4s4sI', 0, 1, 0xb6000000, 0xb6000000, 0, 0, 0,
                                                             0, b'DATA', b'FrUs', 0)
    path.write_bytes(header + struct.pack('>IH', 0, 0) + b'\0\0')
    return path


def probe_run(run, path, runs, timeout=180):
    """The records of the probe's runs-th invocation once it's complete."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if run.request('guest-ready', ok=None)[0] and export_if_ready(run.request, path):
            lines = read_log(path)
            begins = [i for i, line in enumerate(lines) if line.startswith('begin ')]
            if len(begins) >= runs:
                records = lines[begins[runs - 1]:begins[runs] if len(begins) > runs else len(lines)]
                if records[-1].startswith('complete '):
                    if 'err=290d' in records[1].lower() and len(records) == 3:
                        runs += 1  # not mounted yet; the probe tries again each second
                        continue
                    return records
        time.sleep(1)
    raise smoke.Fail(f'probe run {runs} did not complete')


def fields(records):
    out = {}
    for line in records:
        label, error, value = line.split(' ')
        out[label] = (int(error[4:], 16), int(value[6:]))
    return out


def check_io(f, prior):
    """The probe's own file checks. 5.2.8 gives vfsErrFileEOF (2A07) for a read that reaches the end."""
    problems = []
    for key in ('guest-open', 'guest-close', 'guest-reopen', 'guest-read-close', 'scratch-open', 'scratch-write',
                'scratch-close', 'scratch-delete', 'guest-match', 'complete'):
        if f.get(key, (1,))[0] != 0:
            problems.append(f'{key} {f.get(key)}')
    if f.get('guest-write') != (0, 19):
        problems.append(f'guest-write {f.get("guest-write")}')
    if f.get('guest-read', (1,))[0] not in (0, 0x2a07):
        problems.append(f'guest-read {f.get("guest-read")}')
    if f.get('scratch-gone', (0,))[0] != 0x2a08:
        problems.append(f'scratch-gone {f.get("scratch-gone")}')
    if prior and (f.get('prior-open', (1,))[0] != 0 or f.get('prior-match', (1,))[0] != 0):
        problems.append(f'prior {f.get("prior-open")} {f.get("prior-match")}')
    return problems


def partition_offset(image):
    with image.open('rb') as card:
        boot = card.read(512)
    if boot[54:62] in (b'FAT16   ', b'FAT12   ') or boot[82:90] == b'FAT32   ':
        return 0
    return struct.unpack_from('<I', boot, 454)[0] * 512


def qualify(device, mib, probe, guest_format=True):
    work = OUT / f'{device}-{mib}' if guest_format else OUT / f'{device}-{mib}-no-format'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True, mode=0o700)
    result = {'device': device, 'mib': mib, 'guest_format': guest_format, 'layout_bytes': layout_bytes(mib), 'problems': []}
    problems = result['problems']
    session = smoke.OUT / f'{device}.session'
    if smoke.SMOKE[device].get('reminder'):
        session, _ = smoke.clear_reminder(device, work, session)
    folder1, folder2, saved, image = work / 'machine-1', work / 'machine-2', work / 'saved', work / 'card.img'
    try:
        return checks(device, mib, probe, guest_format, work, session, result, folder1, folder2, saved, image)
    finally:
        for big in (image, folder1, folder2, saved):
            if big.is_dir():
                shutil.rmtree(big, ignore_errors=True)
            else:
                big.unlink(missing_ok=True)


def checks(device, mib, probe, guest_format, work, session, result, folder1, folder2, saved, image):
    problems = result['problems']
    folder1.mkdir()
    run = smoke.Run(device, work, session, extra=['--work-folder', folder1])
    run.control.command_timeout = 600
    try:
        run.ready()
        state = run.require('sd-state')['message']
        if state == 'inserted':
            run.require('sd-eject')
            time.sleep(2)
        started = time.monotonic()
        created = run.control.request('sd-create', str(mib))
        result['create'] = {'ok': created['ok'], 'code': created['code'], 'message': created['message'],
                            'seconds': round(time.monotonic() - started, 1)}
        if not created['ok']:
            problems.append(f'sd-create refused: {created["message"]}')
            return result
        time.sleep(3)
        run.install_and_launch(probe, 'Frond SD probe')
        first = probe_run(run, work / 'probe-1.zip', 1)
        result['probe_frond_layout'] = first
        (work / 'partial.json').write_text(json.dumps(result, indent=2) + '\n')
        f = fields(first)
        size = f.get('volume-size', (1, 0))
        result['volume_size'] = size[1]
        if size[0] != 0 or size[1] != layout_bytes(mib):
            problems.append(f'VolumeSize {size} is not the layout {layout_bytes(mib)}')
        problems += [f'frond layout: {p}' for p in check_io(f, prior=False)]
        result['formatted_size'] = size[1]
        if guest_format:
            guest_formats(run, work, mib, result)
        started = time.monotonic()
        run.require('save', saved)
        result['save_seconds'] = round(time.monotonic() - started, 1)
    finally:
        run.close()
    return reload(device, mib, work, saved, folder2, image, result, prior_runs=3 if guest_format else 2)


def guest_formats(run, work, mib, result):
    """Guest format, then the same checks on the new volume."""
    problems = result['problems']
    run.control.retry_ready('install', str(format_pdb(work / 'FrondSDFormat.pdb')), 60)
    run.control.retry_ready('launch-app', 'Frond SD probe', 60)
    # Palm OS asks before formatting; OK is at the same place on all three screens.
    second = None
    for attempt in range(5):
        time.sleep(4)
        run.frame(work / f'format-{attempt}.png')
        run.tap(work, 48, 293)
        try:
            second = probe_run(run, work / 'probe-2.zip', 2, timeout=30)
            break
        except smoke.Fail:
            continue
    if second is None:
        run.frame(work / 'format-stuck.png')
        problems.append('the probe did not finish after the guest format')
        return
    result['probe_guest_format'] = second
    f = fields(second)
    result['format'] = {k: f.get(k) for k in ('format-info', 'format-unmount', 'format', 'format-size', 'format-used')}
    if f.get('format', (1,))[0] != 0:
        problems.append(f'guest format failed: {f.get("format")}')
    formatted = f.get('format-size', (1, 0))
    result['formatted_size'] = formatted[1]
    if formatted[0] != 0 or not (mib << 20) * 0.95 <= formatted[1] <= mib << 20:
        problems.append(f'formatted VolumeSize {formatted} is outside 95-100% of {mib} MiB')
    problems += [f'guest format: {p}' for p in check_io(f, prior=False)]


def reload(device, mib, work, saved, folder2, image, result, prior_runs):
    """A fresh process from the save reads the guest's file back; the exported card is checked with mtools."""
    problems = result['problems']
    folder2.mkdir()
    run = smoke.Run(device, work, saved, extra=['--work-folder', folder2])
    run.control.command_timeout = 600
    try:
        run.ready()
        result['reload_state'] = run.require('sd-state')['message']
        run.control.retry_ready('launch-app', 'Frond SD probe', 60)
        try:
            third = probe_run(run, work / 'probe-3.zip', prior_runs)
        except smoke.Fail:
            third = None
            problems.append('reload: the card never mounted (the probe kept getting 290D)')
        if third:
            result['probe_reload'] = third
            f = fields(third)
            if f.get('volume-size') != (0, result.get('formatted_size')):
                problems.append(f'reloaded VolumeSize {f.get("volume-size")} is not {result.get("formatted_size")}')
            problems += [f'reload: {p}' for p in check_io(f, prior=True)]
        run.require('sd-eject')
        time.sleep(2)
        run.require('sd-export', image)
    finally:
        run.close()
    offset = partition_offset(image)
    target = f'{image}@@{offset}'
    info = subprocess.run(['minfo', '-i', target, '::'], capture_output=True, text=True)
    listing = subprocess.run(['mdir', '-i', target, '::/'], capture_output=True, text=True)
    guest = subprocess.run(['mtype', '-i', target, '::/GUEST.TXT'], capture_output=True)
    result['export'] = {'bytes': image.stat().st_size, 'partition_offset': offset,
                        'minfo': info.stdout[-2000:], 'mdir': listing.stdout[-2000:]}
    (work / 'minfo.txt').write_text(info.stdout + info.stderr)
    if image.stat().st_size != mib << 20:
        problems.append(f'export is {image.stat().st_size} bytes')
    if listing.returncode or guest.stdout != b'Written by Palm TX\n':
        problems.append(f'exported card: mdir rc {listing.returncode}, GUEST.TXT {guest.stdout[:40]!r}')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('device', choices=['palm-tx', 'palm-e2', 'palm-zire-72'])
    p.add_argument('sizes', nargs='+', type=int)
    p.add_argument('--no-format', action='store_true', help='skip the guest format; save and reload the card as Frond made it')
    args = p.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    # The probe with the format step, built here so build/smoke/probes stays as it is.
    probe = OUT / 'probe/FrondSDProbe.prc'
    subprocess.run([sys.executable, str(ROOT / 'tools/build-palm-probe.py'), '--sdk', str(smoke.SDK), '--probe', 'sd',
                    '--output', str(OUT / 'probe')], cwd=ROOT, check=True, capture_output=True)
    OUT.parent.chmod(0o700)
    OUT.chmod(0o700)
    failed = False
    for mib in args.sizes:
        started = time.monotonic()
        try:
            result = qualify(args.device, mib, probe, guest_format=not args.no_format)
        except Exception as error:  # noqa: BLE001 - every failure is recorded
            result = {'device': args.device, 'mib': mib, 'problems': [f'{type(error).__name__}: {error}']}
        result['seconds'] = round(time.monotonic() - started)
        (OUT / f'{args.device}-{mib}{"-no-format" if args.no_format else ""}.json').write_text(json.dumps(result, indent=2) + '\n')
        verdict = 'PASS' if not result['problems'] else 'FAIL'
        failed |= bool(result['problems'])
        print(f'{verdict} {args.device} {mib} MiB: volume {result.get("volume_size")} layout {result.get("layout_bytes")}, '
              f'formatted {result.get("formatted_size")} ({result["seconds"]} s) {"; ".join(result["problems"])}', flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
