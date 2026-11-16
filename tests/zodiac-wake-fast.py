#!/usr/bin/env python3
"""Zodiac sleep and wake on Fast, with the YMF768 ready wait passed at once and run try by try.

Presses power to sleep, presses it again to wake and taps the screen, then checks both runs turn the screen off
and on at the same guest times, show the same frames after the tap and end with the same chip registers, and that
the fast run passed the ready waits' tries. With --exact, an Exact run has to end the same as well (minutes).

  zodiac-wake-fast.py [--rom ROM] [--state FILE] [--exact]

The ROM defaults to Tapwave/Zodiac-2/Tapwave-Zodiac-2-v1.1.rom in FROND_DEVICE_IMAGES; without it the test is
skipped. --state starts from a state saved by benchmark-replay's BENCH_ZODIAC_SAVE instead of a cold boot.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BENCH = Path(os.environ.get('SPEED_BENCH') or ROOT / 'build/benchmark-replay')

parser = argparse.ArgumentParser()
parser.add_argument('--rom', type=Path,
                    default=Path(os.environ.get('FROND_DEVICE_IMAGES', '/nonexistent')) / 'Tapwave/Zodiac-2/Tapwave-Zodiac-2-v1.1.rom')
parser.add_argument('--state', type=Path)
parser.add_argument('--exact', action='store_true')
args = parser.parse_args()
if not args.rom.is_file():
    print('zodiac-wake-fast: skipped, no Zodiac ROM (FROND_DEVICE_IMAGES or --rom)')
    sys.exit(0)

# Events start half a second after the state's time, or at 12 s from a cold boot (the launcher is up by then).
base = 12000
if args.state:
    base = struct.unpack('<Q', args.state.read_bytes()[:8])[0] // 1000000 + 500
events = [(0, 'key-down power'), (200, 'key-up power'), (2500, 'key-down power'), (2700, 'key-up power'),
          (4500, 'pen-down 240 160'), (4600, 'pen-up')]
end = base + 6500


def run(work, name, env):
    out = Path(work) / name
    environment = dict(os.environ, BENCH_ZODIAC_TRACE='500', **env)
    environment.pop('BENCH_JIT_ASYNC', None)
    if args.state:
        environment['BENCH_ZODIAC_LOAD'] = str(args.state)
    started = time.monotonic()
    result = subprocess.run([str(BENCH), '--device', 'tapwave-zodiac-2', str(args.rom), str(Path(work) / 'wake.replay'),
                             str(end), 'original', 'off', str(out)], capture_output=True, text=True, env=environment)
    wall = time.monotonic() - started
    if result.returncode:
        sys.exit(f'{name}: benchmark-replay failed\n{result.stderr[-2000:]}')
    log = result.stderr.splitlines()
    lcd = [re.sub(r' wall .*', '', line) for line in log if line.startswith('lcd ') and 'at the end' not in line]
    frames = [line for line in log if line.startswith('frame ') and float(line.split()[1]) > base + 4500]
    tail = log[next(i for i, line in enumerate(log) if line.startswith('lcd ') and 'at the end' in line):]
    tries = max([int(m.group(1)) for line in log for m in [re.search(r' ready (\d+)', line)] if m] or [0])
    print(f'{name}: wall {wall:.2f} s, ready wait tries passed {tries}, {tail[0]}')
    return lcd, frames, tail, tries


failures = []
with tempfile.TemporaryDirectory(prefix='zodiac-wake-fast-', dir=ROOT / 'build') as work:
    Path(work, 'wake.replay').write_text(''.join(f'{base + at} {what}\n' for at, what in events))
    fast = run(work, 'fast', {'FROND_JIT': '1'})
    slow = run(work, 'fast-try-by-try', {'FROND_JIT': '1', 'BENCH_ZODIAC_READY_SKIP': '0'})
    if fast[3] < 700000:
        failures.append(f'the fast run passed {fast[3]} ready wait tries, expected most of the 840002')
    if slow[3]:
        failures.append('BENCH_ZODIAC_READY_SKIP=0 still passed tries')
    off = [line for line in fast[0] if line.startswith('lcd off') and float(line.split()[3]) > base]
    on = [line for line in fast[0] if line.startswith('lcd on') and float(line.split()[3]) > base + 2500]
    if not off or not on or fast[2][0] != 'lcd on at the end':
        failures.append(f'the screen didn\'t go off and come back: {fast[0]}')
    if not fast[1]:
        failures.append('no frame after the tap')
    others = [('Fast try by try', slow)]
    if args.exact:
        others.append(('Exact', run(work, 'exact', {'FROND_JIT': '0'})))
    for name, other in others:
        for what, index in (('screen on and off times', 0), ('frames after the tap', 1), ('chip registers at the end', 2)):
            if fast[index] != other[index]:
                failures.append(f'{what} differ from {name}:\n  ' + '\n  '.join(fast[index]) + '\n against\n  ' +
                                '\n  '.join(other[index]))
for failure in failures:
    print('FAIL:', failure)
sys.exit(1 if failures else 0)
