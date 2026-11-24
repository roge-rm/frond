#!/usr/bin/env python3
"""Runs TwTime.prc (twtime.c, TwGfxWriteSurface timing) headless through benchmark-replay and prints its
results per engine: full-screen 320x480 mean, min and max and 64x64 mean and min, in guest ms.

Uses run.py's card and cold boot (boot91.state in WORK, redone when the binary, ROM or fixture change), then
launches the program from the card view and reads its result blocks from a snapshot.

Usage: twtime.py --bench BENCHMARK-REPLAY --rom ROM --fixture tests/fixtures/zodiac-doom2.replay
                 --prc TwTime.prc --work DIR [--engines fast,exact]"""
import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run  # noqa: E402

END_MS = 106000


def main():
    parser = argparse.ArgumentParser()
    for name in ('--bench', '--rom', '--fixture', '--prc', '--work'):
        parser.add_argument(name, required=True)
    parser.add_argument('--engines', default='fast,exact')
    args = parser.parse_args()
    os.makedirs(args.work, exist_ok=True)
    runner = run.Runner(args, run.make_card(args.work, args.prc))
    boot = os.path.join(args.work, 'boot91.state')
    if not os.path.exists(boot):
        # states() makes the cold boot, then looks for TwGfxTest's waiting row, which TwTime doesn't draw.
        try:
            runner.states()
        except SystemExit:
            pass
    if not os.path.exists(boot):
        sys.exit('twtime.py: no cold boot state')
    for engine in args.engines.split(','):
        events = run.LAUNCH + ['%d snapshot "s.png"' % (END_MS - 10)]
        stop, out = runner.bench('twtime-' + engine, events, END_MS, engine == 'fast', load=boot)
        words = run.read_result(os.path.join(out, 'snapshot-1.png'))
        if stop or not words or words[2] != 3:
            print('%s: no result (%s, phase %s, see %s)' % (engine, stop, words and words[2], out))
            continue
        shutil.copy(os.path.join(out, 'snapshot-1.png'), os.path.join(args.work, 'twtime-%s.png' % engine))
        shutil.rmtree(out)
        print('%s: 320x480 mean %.2f min %.2f max %.2f ms, 64x64 mean %.2f min %.2f ms' %
              (engine, words[3] / 1000, words[4] / 1000, words[5] / 1000, words[6] / 1000, words[7] / 1000))


if __name__ == '__main__':
    main()
