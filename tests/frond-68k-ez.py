#!/usr/bin/env python3
"""An EZ Palm device boots to the launcher, takes a PRC, and keeps it over a save and a reload.

Boots frond-68k on the ROM with the profile's setup recipe, installs a PRC at the launcher, saves
the session, loads it in a fresh process and expects the launcher to list the app as before and the
app to launch. A colour device's launcher has to show colour and a grey one none. Frames of each step
go in --evidence.

Run: python3 tests/frond-68k-ez.py --device palm-v --rom PRIVATE.rom --prc FrondSync68k.prc --evidence DIR
The PRC comes from tools/build-palm-probe.py --probe sync68k. Needs the private ROM.
"""
import argparse
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402


def body(rows):
    """A frame below the title bar, which holds the launcher's clock."""
    return rows[14:] if rows else rows


def saturated(rows):
    """Pixels whose channels differ by more than 60: none on a grey LCD."""
    return sum(1 for row in rows for i in range(0, len(row), 4) if max(row[i:i + 3]) - min(row[i:i + 3]) > 60)


def settle(core, path, wanted, seconds=15):
    end = time.monotonic() + seconds
    while True:
        rows = core.frame(path)
        if wanted(rows) or time.monotonic() > end:
            return rows
        time.sleep(0.5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True, choices=sorted(palm_boot.DEVICES_68K))
    parser.add_argument('--rom', required=True, type=Path)
    parser.add_argument('--prc', required=True, type=Path)
    parser.add_argument('--app', default='Frond sync', help='the PRC\'s launcher name')
    parser.add_argument('--evidence', required=True, type=Path, help='new directory')
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=False)
    root, device = args.evidence, args.device
    launcher = palm_boot.launcher_check(device)
    session, saved = root / 'launcher.frond', root / 'installed.frond'

    core = palm_boot.Core(['--rom', args.rom, '--device', device], root / 'boot.log')
    try:
        palm_boot.setup_device(device, core, root)
        before = core.wait_frame(root / 'launcher.png', launcher, 60)
        core.send(f'save-session "{session}"', 'wrote session')
        print(f'{device}: booted to the launcher', flush=True)
        # Palm OS 4's colour launcher (blue title tab, coloured icons) on a colour LCD; a grey one stays grey.
        colour = saturated(before)
        if palm_boot.rom_device(device)['colour_lcd'] != (colour > 1000):
            raise AssertionError(f'{colour} saturated launcher pixels')
        print(f'{device}: {colour} saturated launcher pixels', flush=True)
    finally:
        core.close()

    core = palm_boot.Core([session, '--device', device], root / 'install.log')
    try:
        before = core.wait_frame(root / 'reloaded-before-install.png', launcher, 60)
        reply = core.send(f'install "{args.prc}"', 'install', 60)
        if 'successful' not in reply:
            raise AssertionError(f'install: {reply.strip()}')
        after = settle(core, root / 'after-install.png', lambda rows: body(rows) != body(before) and launcher(rows))
        if body(after) == body(before):
            raise AssertionError('the launcher did not change after the install')
        time.sleep(2)
        after = core.frame(root / 'after-install.png')
        core.send(f'save-session "{saved}"', 'wrote session')
        print(f'{device}: PRC installed and listed', flush=True)
    finally:
        core.close()

    core = palm_boot.Core([saved, '--device', device], root / 'reload.log')
    try:
        reloaded = settle(core, root / 'reloaded.png', lambda rows: launcher(rows) and body(rows) == body(after), 60)
        if body(reloaded) != body(after):
            raise AssertionError('the reloaded launcher differs from the one that was saved')
        core.launch(args.app)
        app = settle(core, root / 'app.png', lambda rows: rows is not None and not launcher(rows))
        if not app or launcher(app):
            raise AssertionError('the installed app did not launch after the reload')
        print(f'{device}: saved, reloaded, app launches', flush=True)
    finally:
        core.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
