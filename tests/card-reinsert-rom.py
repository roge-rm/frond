#!/usr/bin/env python3
"""Card reinsert on real firmware: a card put back right after an eject, with no guest run between.

  tests/card-reinsert-rom.py [--device DEVICE ...]

The frontend pauses the core while a card's files are edited, so the eject and the next insert can
come with no guest time between them. The core runs the guest until it has handled the removal
before it inserts (patch 0320). Each device runs on a copy of its smoke session
(FROND_ARM_SESSIONS/DEVICE, else build/smoke/machines/DEVICE); a device without one is skipped.

SD (T|X, Tungsten E2, Zire 72): the SD probe runs on a card, the Palm sleeps, the card is ejected and taken out, a
card file made in another machine (a DELETE.TXT, no GUEST.TXT) is put in, and a second probe run has
to see the new card: no GUEST.TXT, and the DELETE.TXT it removes. Also sd-eject then sd-insert of an
image on the T|X.

Memory Stick (TG50, classic and Pro): a stick with A.TXT is ejected with the Palm asleep and taken out, the stick from
another machine (A.TXT and B.TXT) is put in, and the files listed have to include B.TXT.

Machine copies go in build/step-card-reinsert/rom-TIME/ (mode 0700); only the logs stay after a pass.
"""
import argparse
import importlib.util
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'tools'))
import smoke  # noqa: E402

_spec = importlib.util.spec_from_file_location('card_swap_rom', ROOT / 'tests/card-swap-rom.py')
swap = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(swap)

DEVICES = ('palm-tx', 'palm-e2', 'palm-zire-72', 'sony-clie-tg50')


def image(work, name, extra):
    """A card image with HOST.TXT and, if `extra`, a DELETE.TXT."""
    host = work / 'HOST.TXT'
    host.write_bytes(b'Hello from host\n')
    path = work / name
    smoke.card_image(path, host)
    if extra:
        delete = work / 'DELETE.TXT'
        delete.write_bytes(b'Delete me\n')
        subprocess.run(['mcopy', '-i', str(path), str(delete), '::/DELETE.TXT'], check=True)
    return path


def start_probe(case, run, image_path):
    """The card in, the SD probe installed; the first probe run is the caller's."""
    run.ready()
    run.require('sd-insert', image_path)
    time.sleep(3)
    run.ready()
    installed = run.control.retry_ready('install', str(swap.PROBE), 60)
    if installed['code'] == 'needs-reset':
        run.require('reset')
        run.ready()


def new_card_seen(block, why):
    smoke.need(block['complete'][0] == '0000', f'{why}: the probe failed: {block}')
    smoke.need(block.get('host-delete', ('', 1))[0] == '0000', f'{why}: the new card\'s DELETE.TXT was not seen: {block}')
    smoke.need(block['prior-open'][0] != '0000', f'{why}: the old card\'s GUEST.TXT is still there: {block}')


def sd_reinsert(case):
    work = case.root
    if smoke.SMOKE[case.device].get('reminder'):
        case.session, _ = smoke.clear_reminder(case.device, work, case.session)
    old = image(work, 'old.img', False)
    # The card the other machine leaves: DELETE.TXT, no GUEST.TXT.
    b = case.machine('reinsert-b')
    run = case.run(b)
    try:
        run.ready()
        run.require('sd-insert', image(work, 'new.img', True))
        run.require('sd-eject')
        identity = run.require('sd-take-out')['message'].split()[0]
    finally:
        run.close()
    swap.settled(b, 'sd.frondsd')

    a = case.machine('reinsert-a')
    run = case.run(a)
    try:
        start_probe(case, run, old)
        first = swap.sd_probe(case, run, 'a-first')
        smoke.need(first['prior-open'][0] != '0000', 'the first probe found a GUEST.TXT')
        # Eject, take out, swap the card file, put in: the commands come back to back, with the Palm
        # asleep, so it runs only when the put-in is done.
        swap.power_off(run, work, 'a-sleep')
        run.require('sd-eject')
        run.require('sd-take-out')
        (a / 'sd.frondsd').unlink(missing_ok=True)
        (b / 'sd.frondsd').rename(a / 'sd.frondsd')
        run.require('sd-put-in', identity)
        swap.power_on(run, work, 'a-wake')
        new_card_seen(swap.sd_probe(case, run, 'a-put-in'), 'put-in')
        case.notes.append('SD put-in after eject, Palm asleep: the probe saw the new card')
        if case.device == 'palm-tx':
            swap.power_off(run, work, 'a-sleep-2')
            run.require('sd-eject')
            run.require('sd-insert', image(work, 'again.img', True))
            swap.power_on(run, work, 'a-wake-2')
            new_card_seen(swap.sd_probe(case, run, 'a-insert'), 'insert')
            case.notes.append('SD insert after eject: the probe saw the new card')
    finally:
        run.close()


def stick_reinsert(case, kind):
    work = case.root
    classic = kind == 'classic'
    name = 'ms.frondms' if classic else 'ms.frondmsp'
    a = case.machine(f'reinsert-{kind}-a')
    run = case.run(a, timeout=1200)
    run.control.command_timeout = 600
    try:
        swap.datebook(run, work, f'{kind}-a-wake')
        if swap.card_state(run, 'ms') == 'inserted':
            run.require('ms-eject')
            time.sleep(2)
        run.require('ms-create', '16' if classic else '256')
        if classic:
            swap.format_stick(run, work)
        swap.stick_put(run, work, 'A.TXT')
        smoke.need('A.TXT' in swap.stick_files(run, work), f'{kind}: A.TXT not listed')
        run.require('save', a)
    finally:
        run.close()

    # The same stick with B.TXT added in another machine.
    b = case.root / f'reinsert-{kind}-b'
    shutil.copytree(a, b)
    run = case.run(b, timeout=900)
    run.control.command_timeout = 600
    try:
        swap.datebook(run, work, f'{kind}-b-wake')
        swap.stick_put(run, work, 'B.TXT')
        run.require('save', b)
        run.require('ms-eject')
        identity = run.require('ms-take-out')['message'].split()[0]
    finally:
        run.close()

    run = case.run(a, timeout=900)
    run.control.command_timeout = 600
    try:
        swap.datebook(run, work, f'{kind}-a-again')
        smoke.need('B.TXT' not in swap.stick_files(run, work), f'{kind}: B.TXT listed before the swap')
        # With the Palm asleep it runs only when the put-in is done.
        swap.power_off(run, work, f'{kind}-a-sleep')
        run.require('ms-eject')
        run.require('ms-take-out')
        (a / name).unlink()
        (b / name).rename(a / name)
        run.require('ms-put-in', identity)
        swap.datebook(run, work, f'{kind}-a-wake-again')
        listed = swap.stick_files(run, work)
        smoke.need({'A.TXT', 'B.TXT'} <= listed, f'{kind}: the stick lists {sorted(listed)} after the put-in')
        case.notes.append(f'{kind} stick put-in after eject lists {sorted(listed)}')
    finally:
        run.close()
    for folder in (a, b):
        shutil.rmtree(folder)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--device', action='append', choices=DEVICES)
    args = parser.parse_args()
    found = swap.sessions(args.device or DEVICES)
    if not found:
        print('card reinsert on firmware: SKIP, no smoke sessions (make smoke-sessions or FROND_ARM_SESSIONS)')
        return 0
    root = ROOT / 'build/step-card-reinsert' / time.strftime('rom-%Y%m%d-%H%M%S')
    root.mkdir(parents=True)
    root.chmod(0o700)
    failed = False
    for device in args.device or DEVICES:
        if device not in found:
            print(f'{device}: SKIP, no smoke session')
            continue
        case = swap.Case(device, found[device], root)
        case.notes = []
        try:
            if device == 'sony-clie-tg50':
                stick_reinsert(case, 'classic')
                stick_reinsert(case, 'pro')
            else:
                sd_reinsert(case)
            print(f'{device}: card reinsert ok ({case.count} runs)')
        except (smoke.Fail, Exception) as error:  # noqa: BLE001 - report and go on to the next device
            failed = True
            print(f'{device}: FAIL {error}')
        for note in case.notes:
            print(f'  {note}')
    if not failed:
        for path in root.rglob('*'):
            if path.is_file() and path.suffix != '.log':
                path.unlink()
    print(f'logs in {root}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
