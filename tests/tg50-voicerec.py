#!/usr/bin/env python3
"""TG50 Voice Recorder: a 68k read of address 0 on a normal launch runs on the ROM PACE.

  tests/tg50-voicerec.py

Voice Recorder's PilotMain reads through its NULL launch parameter block. On the device the read
faults in user mode and Palm OS skips it; native PACE hands the instruction to the ROM's own PACE
for that (patch 0835). Launches the app headless on a copy of the TG50 smoke session
(FROND_TG50_SESSION, else FROND_ARM_SESSIONS/sony-clie-tg50, else build/smoke/machines/sony-clie-tg50)
and checks the core log (one 68k fault handed to the ROM PACE, its four byte reads skipped, no
abort loop) and that the screen isn't the blank one of the crash. Skipped without the session.
The run goes in build/step-tg50-voicerec/TIME/ (mode 0700).
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from palm_boot import png_rows  # noqa: E402

CORE = ROOT / 'out/frond'


def session():
    candidates = []
    if os.environ.get('FROND_TG50_SESSION'):
        candidates.append(Path(os.environ['FROND_TG50_SESSION']))
    if os.environ.get('FROND_ARM_SESSIONS'):
        candidates.append(Path(os.environ['FROND_ARM_SESSIONS']) / 'sony-clie-tg50')
    candidates.append(ROOT / 'build/smoke/machines/sony-clie-tg50')
    for path in candidates:
        if path.is_dir() or path.suffix == '.frond' and path.is_file():
            return path
    return None


def main():
    machine = session()
    if not machine or not CORE.exists():
        print('tg50 voice recorder: SKIP, no TG50 smoke session or out/frond')
        return 0
    work = ROOT / 'build/step-tg50-voicerec'
    work.mkdir(mode=0o700, parents=True, exist_ok=True)
    work.chmod(0o700)
    output = work / time.strftime('%Y%m%d-%H%M%S')
    run = subprocess.run([sys.executable, str(ROOT / 'tools/frond-run.py'), '--core', str(CORE),
                          '--device', 'sony-clie-tg50', '--session', str(machine), '--output', str(output),
                          '--app', 'Voice Recorder', '--headless', '--run-for', '6', '--screenshot', '6000'],
                         capture_output=True, text=True, timeout=300)
    try:
        result = json.loads(run.stdout)
    except ValueError:
        print(f'tg50 voice recorder: FAIL, frond-run gave no result: {run.stderr.strip()[-500:]}')
        return 1
    log = (output / 'core.log').read_text(errors='replace') if (output / 'core.log').exists() else ''
    handed = log.count('running it on the ROM PACE')
    skipped = sum(f'R of 1 bytes to 0x0000000{i} failed' in log for i in range(4))
    failed = log.count(' failed (pc=')
    problems = []
    if result['status'] != 'ok':
        problems.append(f"run {result['status']}: {result.get('error')}")
    if 'read of 0x00000000 faulted' not in log or handed != 1:
        problems.append(f'{handed} faults handed to the ROM PACE, expected the one read of 0x00000000')
    # The abort loop logs thousands of failed accesses.
    if skipped != 4 or failed > 20:
        problems.append(f'{failed} failed accesses, expected the four byte reads of 0-3')
    if 'memory fault in PACE' in log:
        problems.append('native PACE raised the fault itself')
    screen = output / 'screenshot-001.png'
    colours = len({bytes(row[i:i + 3]) for row in png_rows(screen) for i in range(0, len(row), 4)}) \
        if screen.exists() else 0
    if colours < 8:
        problems.append(f'the screen has {colours} colours, not the Voice Recorder form')
    if problems:
        print('tg50 voice recorder: FAIL, ' + '; '.join(problems) + f' (see {output})')
        return 1
    # Only the log stays after a pass; the machine copy holds firmware state.
    subprocess.run(['rm', '-rf', str(output / 'working'), str(output / 'core'), str(screen)], check=True)
    print('tg50 voice recorder: ok, the NULL read ran on the ROM PACE and the form came up')
    return 0


if __name__ == '__main__':
    sys.exit(main())
