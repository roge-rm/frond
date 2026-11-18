#!/usr/bin/env python3
"""Zodiac YMF768 ready bit: the boot's power cycle, sleep and wake end their power waits in a few tries.

The ROM's power routine waits on bank 1 register 38 (200bdcfc). Each case runs Fast from a cold boot to just before
it and saves the machine there, then runs Exact and Fast from that state through it, and checks:
- the ready register reads (each a try of a wait) stay far below the ROM's limits, where a wait that never ends
  reads it 340001 or 500001 times;
- the ready register reads, the screen on and off times, the frames after the last event and the chip registers at
  the end are the same in Exact and Fast;
- in the boot case, in each engine, a run loaded from a state saved in the middle of the power up (a power step under
  way) reads the register as many times, last at the same guest time, and ends with the same chip registers as the
  run that saved it and carried on from it loaded in place. A load lets go of the held pen, and the tap's pen is
  still held at the save (it stays down 125 ms at least), so only a run that loaded too can match it exactly.
- the run that saved and carried on without a load saved at the same read. In Exact it's the same as the run that
  never saved. In Fast it can be a read off:
  saving drops the JIT's translated code, and the JIT charges its blocks to the board at their ends, so new block
  bounds move the guest's timer reads a little (one read and 1 us here).
Memory and PCM hashes aren't compared between the engines, whose guest timing differs as in count-check, nor across a
load, after which benchmark-replay takes frames, traces and ends at other slice points.

Cases: the boot powers the chip down at about 35 s and up again for the first tap's click at 40.04 s; sleep and wake (power at 12 s and 14.5 s, a tap at 16.5 s).

  zodiac-ymf-ready.py [--rom ROM] [--keep DIR]

The ROM defaults to Tapwave/Zodiac-2/Tapwave-Zodiac-2-v1.1.rom in FROND_DEVICE_IMAGES; without it the test is
skipped. --keep keeps the runs' logs and states in DIR (a new folder).
"""
import argparse
import os
import re
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
parser.add_argument('--keep', type=Path)
args = parser.parse_args()
if not args.rom.is_file():
    print('zodiac-ymf-ready: skipped, no Zodiac ROM (FROND_DEVICE_IMAGES or --rom)')
    sys.exit(0)

# A power down reads 38 about 1000 times and a power up about 6000 (2 and 2 + 10 ms of tries).
READ_LIMIT = 20000
failures = []


def bench(work, name, replay, end, env):
    """One benchmark-replay run with BENCH_ZODIAC_TRACE: its log lines and wall time."""
    environment = dict(os.environ, BENCH_ZODIAC_TRACE='1000', **env)
    environment.pop('BENCH_JIT_ASYNC', None)
    started = time.monotonic()
    result = subprocess.run([str(BENCH), '--device', 'tapwave-zodiac-2', str(args.rom), str(replay), str(end), 'original',
                             'on', str(Path(work) / name)], capture_output=True, text=True, env=environment)
    (Path(work) / f'{name}.log').write_text(result.stderr)
    if result.returncode:
        sys.exit(f'{name}: benchmark-replay failed\n{result.stderr[-2000:]}')
    return result.stderr.splitlines(), time.monotonic() - started


def parse(log, after_ms):
    lcd = [re.sub(r' wall .*', '', line) for line in log if line.startswith('lcd ') and 'at the end' not in line]
    frames = [line for line in log if line.startswith('frame ') and float(line.split()[1]) > after_ms]
    tail = log[next(i for i, line in enumerate(log) if line.startswith('lcd ') and 'at the end' in line):]
    reads = int(tail[-1].split()[-1]) if tail[-1].startswith('ready reads') else -1
    last = next((line for line in tail if line.startswith('ready last read')), None)
    chip = [line for line in tail if not line.startswith(('pcm hash', 'ready reads', 'memory hash', 'ready last read'))]
    saved = next((int(line.split()[-1]) for line in log if line.startswith('saved at')), None)
    return lcd, frames, chip, reads, last, saved


def differ(what, a, b):
    failures.append(f'{what}:\n  ' + '\n  '.join(a) + '\n against\n  ' + '\n  '.join(b))


def case(work, name, events, save_ms, mid_ms, end, after_ms, sleeps=False):
    replay = Path(work) / f'{name}.replay'
    replay.write_text(''.join(f'{line}\n' for line in events))
    state = Path(work) / f'{name}.state'
    bench(work, f'{name}-save', replay, end, {'FROND_JIT': '1', 'BENCH_ZODIAC_SAVE': f'{save_ms}:{state}'})
    runs = {}
    for engine, jit in (('fast', '1'), ('exact', '0')):
        log, wall = bench(work, f'{name}-{engine}', replay, end, {'FROND_JIT': jit, 'BENCH_ZODIAC_LOAD': str(state)})
        lcd, frames, chip, reads, last, _ = runs[engine] = parse(log, after_ms)
        print(f'{name} {engine}: wall {wall:.2f} s, ready register reads {reads}, {chip[0]}')
        if not 0 < reads <= READ_LIMIT:
            failures.append(f'{name} {engine}: {reads} ready register reads, expected 1 to {READ_LIMIT}')
        if sleeps and (not any(line.startswith('lcd off') for line in lcd) or not any(line.startswith('lcd on') for line in lcd)
                       or chip[0] != 'lcd on at the end'):
            failures.append(f'{name} {engine}: the screen didn\'t go off and come back: {lcd}')
        if not frames:
            failures.append(f'{name} {engine}: no frame after {after_ms} ms')
        if not mid_ms:
            continue
        # Saved mid-wait and carried on.
        mid = Path(work) / f'{name}-{engine}-mid.state'
        kept = parse(bench(work, f'{name}-{engine}-mid-save', replay, end,
                           {'FROND_JIT': jit, 'BENCH_ZODIAC_LOAD': str(state), 'BENCH_ZODIAC_SAVE': f'{mid_ms}:{mid}',
                            'BENCH_ZODIAC_SAVE_CONTINUE': '1'})[0], after_ms)
        if b'YMFR' not in mid.read_bytes():
            failures.append(f'{name} {engine}: the state saved at {mid_ms} ms has no power step under way')
        # Saved mid-wait and carried on from the state loaded in place, and loaded in a new run: the same run.
        reloaded = parse(bench(work, f'{name}-{engine}-mid-reload', replay, end,
                               {'FROND_JIT': jit, 'BENCH_ZODIAC_LOAD': str(state),
                                'BENCH_ZODIAC_SAVE': f'{mid_ms}:{Path(work) / f"{name}-{engine}-mid-reload.state"}',
                                'BENCH_ZODIAC_SAVE_CONTINUE': 'reload'})[0], after_ms)
        resumed = parse(bench(work, f'{name}-{engine}-mid', replay, end,
                              {'FROND_JIT': jit, 'BENCH_ZODIAC_LOAD': str(mid)})[0], after_ms)
        if kept[5] is None or reloaded[5] != kept[5]:
            failures.append(f'{name} {engine}: saved at {reloaded[5]} ready register reads loading in place, '
                            f'{kept[5]} carrying on')
        if resumed[3] != reloaded[3] or resumed[4] != reloaded[4]:
            failures.append(f'{name} {engine}: {resumed[3]} ready register reads after the load, {resumed[4]}; '
                            f'{reloaded[3]} loading in place, {reloaded[4]}')
        if resumed[2] != reloaded[2]:
            differ(f'{name} {engine}: chip registers after a load mid-wait differ from the run that loaded in place',
                   resumed[2], reloaded[2])
        # Saving changes nothing in Exact; in Fast it can move the guest a read (see above).
        if abs(kept[3] - reads) > (engine == 'fast') or (engine == 'exact' and kept[4] != last):
            failures.append(f'{name} {engine}: {kept[3]} ready register reads saving on the way ({kept[4]}), '
                            f'{reads} without ({last})')
        if kept[2] != chip:
            differ(f'{name} {engine}: chip registers differ saving on the way', kept[2], chip)
    for what, index in (('screen on and off times', 0), ('frames', 1), ('chip registers at the end', 2)):
        if runs['fast'][index] != runs['exact'][index]:
            differ(f'{name}: {what} differ between Fast and Exact', runs['fast'][index], runs['exact'][index])
    if runs['fast'][3] != runs['exact'][3]:
        failures.append(f'{name}: {runs["fast"][3]} ready register reads in Fast, {runs["exact"][3]} in Exact')


def cases(work):
    case(work, 'boot', ['40000 pen-down 240 160', '40020 pen-up'], 34000, 40047, 41500, 40000)
    case(work, 'sleep-wake', ['12000 key-down power', '12200 key-up power', '14500 key-down power',
                              '14700 key-up power', '16500 pen-down 240 160', '16600 pen-up'],
         11500, None, 18500, 16500, sleeps=True)


if args.keep:
    args.keep.mkdir()
    cases(args.keep)
else:
    with tempfile.TemporaryDirectory(prefix='zodiac-ymf-ready-', dir=ROOT / 'build') as work:
        cases(work)
for failure in failures:
    print('FAIL:', failure)
sys.exit(1 if failures else 0)
