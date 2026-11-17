#!/usr/bin/env python3
"""frond-68k auto-off on the private IIIx 3.1 and m515 ROMs: left alone after a cold boot, the Palm
keeps the LCD on at 115 guest seconds and has turned itself off by 150, from the OS's default
2 minute auto-off (the timer starts a few seconds into the boot). Fails without FROND_DEVICE_IMAGES
or with neither ROM in it.

Then, on the IIIx in a console run, the power button puts the Palm to sleep and a db-list wakes it
with a power press and answers, with the LCD back on."""
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

CORE = Path(os.environ.get('FROND_68K') or ROOT / 'out/frond-68k')
ROMS = [('palm-iiix', 'Palm/IIIx/Palm-IIIx-3.1-en.rom'), ('palm-m515', 'Palm/m515/Palm-m515-4.1-en.rom')]
OFF = 'framebuffer output with the LCD off'


def lcd_off(rom, device, seconds, folder):
    result = subprocess.run([CORE, '--rom', rom, '--device', device, '--run-for', str(seconds), '--no-sound',
                             '--framebuffer-output', folder / f'{device}-{seconds}.png'],
                            capture_output=True, text=True, timeout=600,
                            env={**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy'})
    if result.returncode != 0:
        raise AssertionError(f'{device} {seconds} s run failed:\n{result.stdout}{result.stderr}')
    return OFF in result.stdout + result.stderr


def asleep_call(rom, folder):
    palm_boot.CORE_68K = CORE
    core = palm_boot.Core(['--rom', rom, '--device', 'palm-iiix'], folder / 'asleep.txt')
    try:
        # Waits for a heavy emulator slot first.
        while not any(line.strip() and not line.startswith(('heavy', 'emulator ')) for line in core.lines):
            if core.process.poll() is not None:
                raise AssertionError('frond-68k exited before starting')
            time.sleep(1)
        core.wait_frame(folder / 'boot.png', lambda rows: rows is not None, 120)
        time.sleep(10)  # the Welcome screen, past the boot
        core.key(folder, 'power', 200)
        end = time.monotonic() + 20
        while core.frame(folder / 'sleep.png') is not None:
            if time.monotonic() > end:
                raise AssertionError('power button did not put the IIIx to sleep')
            time.sleep(0.5)
        line = core.send('db-list', ('db: ', 'db-list:'), 60)
        if not line or not line.startswith('db: '):
            raise AssertionError(f'db-list on a sleeping IIIx: {line!r}')
        if core.frame(folder / 'woken.png') is None:
            raise AssertionError('LCD still off after the db-list woke the IIIx')
        print('palm-iiix: db-list woke the sleeping Palm and answered')
    finally:
        core.close()


def main():
    images = Path(os.environ.get('FROND_DEVICE_IMAGES') or '/nonexistent')
    ran = 0
    with tempfile.TemporaryDirectory() as folder:
        for device, name in ROMS:
            rom = images / name
            if not rom.is_file():
                print(f'skip {device}: no {name} in FROND_DEVICE_IMAGES')
                continue
            if lcd_off(rom, device, 115, Path(folder)):
                raise AssertionError(f'{device} turned off before 2 minutes')
            if not lcd_off(rom, device, 150, Path(folder)):
                raise AssertionError(f'{device} still on after 2 minutes idle')
            print(f'{device}: on at 115 s, off at 150 s')
            ran += 1
            if device == 'palm-iiix':
                asleep_call(rom, Path(folder))
    if not ran:
        raise SystemExit('frond-68k auto-off: no ROM checked, set FROND_DEVICE_IMAGES to the firmware folder')
    print(f'frond-68k auto-off: {ran} ROMs checked')


if __name__ == '__main__':
    sys.exit(main())
