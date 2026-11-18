#!/usr/bin/env python3
"""frond-68k's Faster speed on the private IIIx 3.1 ROM. Headless, --speed Nx runs N times the cycles of
Original in the same guest seconds, and Original reports the IIIx's own clock. On the console the speed
command and speed-state report the setting, and at 2x the measured rate follows a limit twice the clock,
so guest time keeps to real time. Skips without FROND_DEVICE_IMAGES or the ROM in it."""
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

CORE = ROOT / 'out/frond-68k'
ROM = 'Palm/IIIx/Palm-IIIx-3.1-en.rom'
CLOCK = palm_boot.rom_device('palm-iiix')['cpu_hz']


def headless(rom, speed, seconds=2):
    args = [CORE, '--rom', rom, '--device', 'palm-iiix', '--run-for', str(seconds), '--no-sound']
    if speed:
        args += ['--speed', speed]
    result = subprocess.run(args, capture_output=True, text=True, timeout=300,
                            env={**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy'})
    found = re.search(r'ran (\d+) cycles, ([\d.]+) guest seconds, now at ([\d.e+]+) clocks per second', result.stdout)
    assert result.returncode == 0 and found, (speed, result.returncode, result.stdout[-2000:], result.stderr[-2000:])
    return int(found.group(1)), float(found.group(2)), float(found.group(3))


def main():
    rom = palm_boot.palm_images() / ROM
    if not rom.is_file():
        print('SKIP: no IIIx ROM (set FROND_DEVICE_IMAGES)')
        return
    # Original, and a bad value refused.
    base_cycles, base_seconds, base_rate = headless(rom, None)
    # The rate prints with six digits.
    assert abs(base_rate / CLOCK - 1) < 1e-5, base_rate
    cycles, seconds, rate = headless(rom, 'original')
    assert (cycles, rate) == (base_cycles, base_rate), (cycles, rate, base_cycles, base_rate)
    bad = subprocess.run([CORE, '--rom', rom, '--device', 'palm-iiix', '--run-for', '1', '--speed', '5x'],
                         capture_output=True, text=True, timeout=60)
    assert bad.returncode != 0
    # 8x is Uncapped's speed on the 68k, so Faster stops at 4x.
    eight = subprocess.run([CORE, '--rom', rom, '--device', 'palm-iiix', '--run-for', '1', '--speed', '8x'],
                           capture_output=True, text=True, timeout=60)
    assert eight.returncode != 0 and "8x is Uncapped's speed" in eight.stderr, eight.stderr[-500:]
    print(f'original: {base_cycles} cycles in {base_seconds:.3f} guest s at {base_rate:.0f} Hz')
    for speed, factor in (('1.5x', 1.5), ('2x', 2), ('3x', 3), ('4x', 4)):
        cycles, seconds, rate = headless(rom, speed)
        ratio = (cycles / seconds) / (base_cycles / base_seconds)
        print(f'{speed}: {cycles} cycles in {seconds:.3f} guest s at {rate:.0f} Hz, {ratio:.4f}x')
        assert abs(rate / (CLOCK * factor) - 1) < 1e-5, (speed, rate)
        # Each step is 100000 cycles, so the run's end lands within one step of the guest seconds asked for.
        assert abs(ratio - factor) < 0.01 * factor, (speed, ratio)

    with tempfile.TemporaryDirectory(dir=ROOT / 'build') as folder:
        core = palm_boot.Core(['--rom', rom, '--device', 'palm-iiix'], Path(folder) / 'console.txt', '--speed', '3x')
        try:
            assert core.send('speed', 'speed:').strip() == 'speed: 3x'
            assert core.send('speed bogus', 'usage: speed').strip().startswith('usage: speed original|1.5x')
            assert core.send('speed', 'speed:').strip() == 'speed: 3x'
            assert core.send('speed 2x', 'speed:').strip() == 'speed: 2x'
            time.sleep(4)
            state = core.send('speed-state', 'speed:').strip()
            print(state)
            found = re.search(r'([\d.]+) MHz, limit ([\d.]+) MHz, (\d+)%, (\S+)$', state)
            assert found and found.group(4) == '2x', state
            assert abs(float(found.group(2)) - 2 * CLOCK / 1e6) < 0.01, state
            # The measured rate (cycles a wall second) is the limit's: guest time keeps to real time.
            assert 90 <= int(found.group(3)) <= 110, state
            assert core.send('speed original', 'speed:').strip() == 'speed: original'
            state = core.send('speed-state', 'speed:').strip()
            assert state.endswith(', original') and f'limit {CLOCK / 1e6:.2f} MHz' in state, state
            assert "8x is Uncapped's speed" in core.send('speed 8x', 'usage: speed')
            assert core.send('speed uncapped', 'speed:').strip() == 'speed: uncapped'
            state = core.send('speed-state', 'speed:').strip()
            assert state.endswith(', uncapped') and f'limit {8 * CLOCK / 1e6:.2f} MHz' in state, state
        finally:
            core.close()
    print('PASS: frond-68k Faster speeds, Original unchanged, speed command and option')


if __name__ == '__main__':
    main()
