#!/usr/bin/env python3
"""Desktop socket saves write in the background: on out/frond (an ARM smoke session, the Zire 72 by
default) and out/frond-68k (the IIIx ROM booting), each save is checked byte for byte against the
whole save done on the emulation thread at the same instant (FROND_SAVE_REFERENCE), the guest is
checked to run on while the file is written, and the saved session is loaded again. Prints the
emulation thread's stall for each save beside the synchronous one.

Private firmware and sessions: --arm-session, else FROND_ARM_SESSION, else the palm-zire-72 smoke
machine; --rom, else the generic OS 3.5 EZ ROM in FROND_DEVICE_IMAGES. A side without its input is
skipped. Logs and saves go in build/bg-save/ (mode 0700). --timing-only skips the reference, for a
session with a card or NAND (its card generations differ in another folder), and reports the
whole save in place of the synchronous one.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import shutil
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

spec = importlib.util.spec_from_file_location('core_control', ROOT / 'tests/core-control.py')
core_control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(core_control)
Client = core_control.Client

SAVES = 3


def stalls(log):
    text = log.read_text(errors='replace')
    sync = [float(m) for m in re.findall(r'save timing: synchronous reference ([0-9.]+) ms', text)]
    blocked = [float(m) for m in re.findall(r'emulation thread ([0-9.]+) ms', text)]
    guest = [(int(a), int(b)) for a, b in re.findall(r'(?:guest time|guest cycles) (\d+) at snapshot, (\d+) when written', text)]
    assert 'synchronous reference failed' not in text, 'the reference save failed'
    return sync, blocked, guest


TIMING_ONLY = False


def run_side(name, core, args, saved, reference, root):
    os.environ['FROND_SAVE_TIMING'] = '1'
    if not TIMING_ONLY:
        os.environ['FROND_SAVE_REFERENCE'] = str(reference)
    try:
        c = Client(core, args, root, name)
    finally:
        os.environ.pop('FROND_SAVE_REFERENCE', None)
    try:
        c.ok('device-state')
        for n in range(SAVES):
            time.sleep(2)
            c.ok('save', saved, code='saved')
            assert TIMING_ONLY or saved.read_bytes() == reference.read_bytes(), \
                f'save {n + 1}: background save differs from the reference'
        c.ok('device-state')
        c.quit()
    finally:
        c.close()
    sync, blocked, guest = stalls(root / f'{name}.log')
    if TIMING_ONLY:
        sync = [float(m) for m in re.findall(r'whole save ([0-9.]+) ms', (root / f'{name}.log').read_text(errors='replace'))]
    assert len(sync) == SAVES and len(blocked) == SAVES and len(guest) == SAVES, (sync, blocked, guest)
    for before, after in guest:
        assert after > before, f'the guest did not run while the file was written: {before} -> {after}'
    # The saved session loads again.
    c = Client(core, [saved, *args[1:]], root, name + '-reload')
    try:
        c.ok('device-state')
        c.quit()
    finally:
        c.close()
    return (f'{saved.stat().st_size} bytes; {"whole" if TIMING_ONLY else "synchronous"} save {", ".join(f"{s:.0f}" for s in sync)} ms; '
            f'emulation thread now {", ".join(f"{b:.0f}" for b in blocked)} ms')


def check_arm(core, session, root):
    work = root / 'arm'
    work.mkdir()
    copy = work / session.name
    shutil.copytree(session, copy, ignore=shutil.ignore_patterns('*.tmp-*'))
    saved = next(copy.glob('*.frond'))
    (work / 'ref').mkdir()
    return run_side('arm', core, [saved], saved, work / 'ref/ref.frond', root)


def check_68k(core, rom, root):
    work = root / 'iiix'
    work.mkdir()
    return run_side('iiix', core, [rom, '--device', 'palm-iiix'], work / 'iiix.frond', work / 'ref.frond', root)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--arm-session', type=Path)
    parser.add_argument('--rom', type=Path)
    parser.add_argument('--timing-only', action='store_true')
    args = parser.parse_args()
    global TIMING_ONLY
    TIMING_ONLY = args.timing_only
    session = args.arm_session or Path(os.environ.get('FROND_ARM_SESSION') or ROOT / 'build/smoke/machines/palm-zire-72')
    rom = args.rom or palm_boot.palm_images() / 'Palm/Generic-OS-3.5-EZ/Palm-OS-3.5-en-ez.rom'
    base = ROOT / 'build/bg-save'
    base.mkdir(parents=True, exist_ok=True)
    os.chmod(base, 0o700)
    root = base / time.strftime('run-%Y%m%d-%H%M%S')
    root.mkdir()
    failed = ran = 0
    for name, ready, run in (('frond', session.is_dir(), lambda: check_arm(ROOT / 'out/frond', session.resolve(), root)),
                             ('frond-68k', rom.is_file(), lambda: check_68k(ROOT / 'out/frond-68k', rom.resolve(), root))):
        if not ready:
            print(f'background-save {name}: skipped, no input')
            continue
        ran += 1
        try:
            print(f'background-save {name}: ok ({run()})', flush=True)
        except AssertionError as error:
            failed += 1
            print(f'background-save {name}: FAIL, {error}', flush=True)
    if not ran:
        print('background-save: FAIL, every side was skipped')
        return 1
    print('background-save: ' + ('FAIL' if failed else 'ok') + f' (logs in {root})')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
