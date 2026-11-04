#!/usr/bin/env python3
"""Checks that a IIIx session saved by a frond-68k from before whole-clock cycle counts loads in
this one without a jump in the cycle counter.

An older core counted in two-clock units. Its sessions have no "clock 1" metadata line, and
loading one doubles the counter and the timer marks kept against it. The check saves a session
with the older core, runs one guest second of it on both cores and expects this core's counter
at twice the older core's. A session this core saves again must then keep its count.

Run: python3 tests/frond-68k-old-session.py --rom PRIVATE.rom --old-core OLD/frond-68k --output DIR
"""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import palm_boot  # noqa: E402

STEP = 100000  # headless runs stop on a step of this many cycles


def ran(core, session):
    """Runs session for one guest second headless and returns the cycle counter at the end."""
    out = subprocess.run([str(core), '--device', 'palm-iiix', '--run-for', '1', '--no-sound', str(session)],
                         capture_output=True, text=True, timeout=120, stdin=subprocess.DEVNULL)
    match = re.search(r'ran (\d+) cycles', out.stdout)
    if out.returncode or not match:
        raise SystemExit(f'{core} did not run {session}: {out.stdout[-400:]}{out.stderr[-400:]}')
    return int(match.group(1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--old-core', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    old, new = args.old_core.resolve(), palm_boot.CORE_68K
    saved, resaved = (args.output / 'old.session').resolve(), (args.output / 'resaved.session').resolve()

    palm_boot.CORE_68K = old
    core = palm_boot.Core(['--device', 'palm-iiix', args.rom], args.output / 'old.log')
    try:
        core.frame(args.output / 'old-boot.png', timeout=60)
        time.sleep(3)  # a few guest seconds in, so a missed conversion is far outside the margin
        core.send(f'save-session "{saved}"', 'wrote session')
    finally:
        core.close()
    if b'clock 1' in saved.read_bytes()[:4096]:
        raise SystemExit('the older core already writes whole clocks')

    palm_boot.CORE_68K = new
    core = palm_boot.Core(['--device', 'palm-iiix', saved], args.output / 'new.log')
    try:
        core.frame(args.output / 'new-load.png')
        core.send(f'save-session "{resaved}"', 'wrote session')
    finally:
        core.close()

    before, after, again = ran(old, saved), ran(new, saved), ran(new, resaved)
    print(f'older core {before} cycles, this core {after} from the same session, {again} once resaved')
    failures = []
    if abs(after - 2 * before) > 2 * STEP:
        failures.append(f'expected about {2 * before} cycles from the old session, got {after}')
    if again < after - 2 * STEP:
        failures.append(f'the resaved session went back to {again} cycles from {after}')
    if again > after + 2 * 16_580_000:
        failures.append(f'the resaved session jumped to {again} cycles from {after}')
    if failures:
        raise SystemExit('\n'.join(failures))
    print('old session loads without a cycle jump')


if __name__ == '__main__':
    main()
