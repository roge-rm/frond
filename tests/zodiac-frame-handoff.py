#!/usr/bin/env python3
"""The Zodiac's frames reach the window a fixed time after the guest drew them: the core hands each slice's frame
over when the main loop is due to run the next slice, not as soon as the slice's emulation ends. Cold boots the
real ROM (ZODIAC_ROM) for a few seconds in Fast with FROND_FRAME_TRACE and checks each frame's host time against
the host time its guest time stands for. Skipped without the ROM."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORE = Path(os.environ.get('FROND_CORE') or ROOT / 'out/frond')
SLICE_US = 1000000 // 60


def main():
    rom = os.environ.get('ZODIAC_ROM', '')
    if not rom or not Path(rom).is_file():
        print('zodiac-frame-handoff: skipped, no ZODIAC_ROM')
        return 0
    with tempfile.TemporaryDirectory(prefix='frond-frame-handoff-') as work:
        trace = Path(work) / 'frames.txt'
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', FROND_FRAME_TRACE=str(trace))
        env.pop('FROND_JIT', None)
        run = subprocess.run([str(CORE), '--device', 'tapwave-zodiac-2', '--engine', 'fast', '--run-for', '8', rom],
                             env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
        if run.returncode != 0:
            print(run.stdout[-2000:])
            print('zodiac-frame-handoff: the core failed')
            return 1
        rows = [tuple(int(v) for v in line.split()) for line in trace.read_text().splitlines() if line.strip()]
    # Lateness of each hand-over against the end of its slice in real time. The main loop runs a slice's guest
    # time one slice behind the host clock, so a frame handed over when the next slice is due is about one slice
    # late; one handed over as soon as the emulation ends is late only by the emulation time.
    late = [host - end for host, _, end in rows]
    if len(late) < 8:
        print('zodiac-frame-handoff: only %d frames' % len(late))
        return 1
    due = sum(1 for d in late if d >= SLICE_US - 1500)
    print('zodiac-frame-handoff: %d frames, %d handed over when the next slice was due, lateness ms %s'
          % (len(late), due, ' '.join('%.1f' % (d / 1000) for d in late)))
    if due < 0.8 * len(late):
        print('zodiac-frame-handoff: frames handed over before their slice ended in real time')
        return 1
    print('zodiac-frame-handoff: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
