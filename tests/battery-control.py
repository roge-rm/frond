#!/usr/bin/env python3
"""The battery control command on out/frond-68k (an EZ ROM, palm-iiix) and out/frond (a Zire 72 or T|X smoke
machine): battery-state answers Maximum and not charging by default (with no-charging where the board can't take it), battery sets each mode live, bad arguments
are refused, --battery sets it at start, and the setting isn't saved with the session.

Private firmware and sessions: --rom, else the generic OS 3.5 EZ ROM in the firmware folder; --arm-session, else
FROND_ARM_SESSION, else build/smoke/machines/palm-zire-72 or palm-tx here or in the main checkout. A side without
its input is skipped. Scratch goes in build/battery-control-*.
"""
import argparse
import importlib.util
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

spec = importlib.util.spec_from_file_location('core_control', ROOT / 'tests/core-control.py')
core_control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(core_control)
Client = core_control.Client


def state(c, want, command='battery-state', argument=''):
    got = c.ok(command, argument, code='battery')
    assert got == want, f'{command} {argument!r}: {got!r}, wanted {want!r}'


def check_commands(c, charging=True):
    """charging False: a board whose ROM reads its own dock status from an input Frond doesn't model."""
    tail = '' if charging else ' no-charging'
    state(c, 'max 100 battery' + tail)
    if charging:
        state(c, 'level 20 charging', 'battery', 'level 20 charging')
        state(c, 'level 20 charging')
    else:
        c.fails('battery', 'level 20 charging', code='unsupported')
        state(c, 'max 100 battery' + tail)
    state(c, 'host 57 battery' + tail, 'battery', 'host 57')
    state(c, 'level 3 battery' + tail, 'battery', 'level 3')
    for bad in ('', 'level', 'level 101', 'max 50', 'low', 'level 5 maybe'):
        c.fails('battery', bad, code='argument')
    state(c, 'level 3 battery' + tail)
    state(c, 'max 100 battery' + tail, 'battery', 'max')


def check_68k(core, rom, root):
    # The IIIx's ROM reads its own dock status from the board, which Frond doesn't model: no Charging there.
    c = Client(core, [rom, '--device', 'palm-iiix'], root, 'iiix')
    try:
        check_commands(c, charging=False)
    finally:
        c.quit()
    c = Client(core, [rom, '--device', 'palm-iiix', '--battery', 'level 40 charging'], root, 'iiix-start')
    try:
        state(c, 'level 40 battery no-charging')
    finally:
        c.quit()


def check_dana(core, rom, root):
    c = Client(core, [rom, '--device', 'alphasmart-dana'], root, 'dana')
    try:
        check_commands(c)
    finally:
        c.quit()


def check_arm(core, session, root):
    work = root / 'arm'
    work.mkdir()
    copy = work / session.name
    shutil.copytree(session, copy, ignore=shutil.ignore_patterns('*.tmp-*'))
    saved = work / 'saved.frond'
    c = Client(core, [copy, '--battery', 'level 40 charging'], root, 'arm')
    try:
        state(c, 'level 40 charging')
        state(c, 'max 100 battery', 'battery', 'max')
        check_commands(c)
        c.ok('battery', 'level 10 charging')
        c.ok('save', saved, code='saved')
    finally:
        c.quit()
    # The session keeps no battery setting: it loads at Maximum unless told otherwise.
    reload = work / 'saved' if (work / 'saved').is_dir() else saved
    c = Client(core, [reload], root, 'arm-reload')
    try:
        state(c, 'max 100 battery')
    finally:
        c.quit()


def arm_session(given):
    if given:
        return given
    for base in (ROOT, Path.home() / 'Projects/palm/frond'):
        for name in ('palm-zire-72', 'palm-tx'):
            if (base / 'build/smoke/machines' / name).is_dir():
                return base / 'build/smoke/machines' / name
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--arm-session', type=Path)
    parser.add_argument('--rom', type=Path)
    args = parser.parse_args()
    rom = args.rom or palm_boot.palm_images() / 'Palm/Generic-OS-3.5-EZ/Palm-OS-3.5-en-ez.rom'
    session = arm_session(args.arm_session)
    failed = ran = 0
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='battery-control-', dir=ROOT / 'build') as work:
        root = Path(work).resolve()
        dana = palm_boot.palm_images() / 'AlphaSmart/Dana/AlphaSmart-Dana-1.5.rom'
        for name, ready, run in (('frond-68k', rom.is_file(), lambda: check_68k(ROOT / 'out/frond-68k', rom, root)),
                                 ('frond-68k dana', dana.is_file(), lambda: check_dana(ROOT / 'out/frond-68k', dana, root)),
                                 ('frond', session is not None,
                                  lambda: check_arm(ROOT / 'out/frond', session.resolve(), root))):
            if not ready:
                print(f'battery-control {name}: skipped, no input')
                continue
            ran += 1
            try:
                run()
                print(f'battery-control {name}: ok', flush=True)
            except AssertionError as error:
                failed += 1
                print(f'battery-control {name}: FAIL, {error}', flush=True)
                for log in sorted(root.glob('*.log')):
                    print(f'--- {log.name}\n' + log.read_text(errors='replace')[-2000:])
    if not ran:
        print('battery-control: FAIL, nothing was checked')
        return 1
    print('battery-control: ' + ('FAIL' if failed else 'ok'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
