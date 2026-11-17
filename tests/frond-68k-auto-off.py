#!/usr/bin/env python3
"""frond-68k auto-off on the private IIIx 3.1 and m515 ROMs: left alone after a cold boot, the Palm
keeps the LCD on at 115 guest seconds and has turned itself off by 150, from the OS's default
2 minute auto-off (the timer starts a few seconds into the boot). Skips a ROM that isn't in
FROND_DEVICE_IMAGES."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
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
    print(f'frond-68k auto-off: {ran} ROMs checked')


if __name__ == '__main__':
    sys.exit(main())
