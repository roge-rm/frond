#!/usr/bin/env python3
"""Report a FROND_COST_PROFILE file (tools/debug/cost-profile.patch) as a mix table.

  cost-profile-report.py FILE                       whole run
  cost-profile-report.py FILE NAME=MS ... [--rate MIPS] [--start CYCLES]
      windows in order, each MS long at MIPS (busy cycles = ms * MIPS * 1000), the first one
      starting at --start busy cycles (default 0), or where --start-pace PACE-OPS first
      show up: FrondBench's tests, for example.
"""
import argparse
import re
import sys


def load(path):
    snaps, final, hist = [], None, {}
    for line in open(path):
        if line.startswith(('SNAP', 'FINAL')):
            f = {k: int(v) for k, v in re.findall(r'(\S+)=(\d+)', line)}
            (snaps if line[0] == 'S' else []).append(f)
            if line[0] == 'F':
                final = f
        elif line.startswith('PCHIST'):
            _, a, n = line.split()
            hist[a] = int(n)
    return snaps, final, hist


def at(snaps, final, busy):
    """Counters at a busy-cycle count, linearly between two snapshots."""
    prev = {k: 0 for k in final}
    for s in snaps + [final]:
        if s['busy'] >= busy:
            span = s['busy'] - prev['busy']
            w = (busy - prev['busy']) / span if span else 1.0
            return {k: prev[k] + (s[k] - prev[k]) * w for k in s}
        prev = s
    return final


def diff(a, b):
    return {k: b[k] - a[k] for k in b}


def show(name, d):
    arm = d['arm'] + d['thumb']
    out = [f"{name}: busy {d['busy']:.0f}  arm+thumb {arm:.0f}  pace ops {d['pace']:.0f}  thumb {100 * d['thumb'] / max(arm, 1):.1f}%  "
           f"failed {d['failed']:.0f} ({100 * d['failed'] / max(arm, 1):.1f}%)  ldm words {d['ldmw']:.0f}  stm words {d['stmw']:.0f}  memcpy bytes {d['memcpy']:.0f}"]
    cls = sorted(((k[2:], v) for k, v in d.items() if k.startswith('c.') and v >= 1), key=lambda x: -x[1])
    out.append('  classes: ' + ', '.join(f'{k} {100 * v / max(arm, 1):.1f}%' for k, v in cls if v / max(arm, 1) >= 0.0005))
    regs = []
    for r in ('sdram', 'sram', 'flash', 'device'):
        l = sum(d.get(f'r.{r}.l{s}', 0) for s in range(3))
        st = sum(d.get(f'r.{r}.s{s}', 0) for s in range(3))
        pl, ps = d.get(f'pace.{r}.l', 0), d.get(f'pace.{r}.s', 0)
        regs.append(f'{r} L {l:.0f} S {st:.0f}' + (f' (pace L {pl:.0f} S {ps:.0f})' if pl or ps else ''))
    out.append('  region accesses: ' + '; '.join(regs))
    return '\n'.join(out)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('file')
    p.add_argument('windows', nargs='*')
    p.add_argument('--rate', type=float, default=100, help='MIPS the run was capped at')
    p.add_argument('--start', type=float, default=0, help='busy cycles before the first window')
    p.add_argument('--start-pace', type=int, default=0, help='start the first window where this many PACE ops have run')
    a = p.parse_args()
    snaps, final, hist = load(a.file)
    if not a.windows:
        print(show('whole run', final))
        return
    pos = a.start
    if a.start_pace:
        prev = {k: 0 for k in final}
        for s in snaps:
            if s['pace'] >= a.start_pace:
                pos = prev['busy'] + (s['busy'] - prev['busy']) * (a.start_pace - prev['pace']) / max(s['pace'] - prev['pace'], 1)
                break
            prev = s
    base = at(snaps, final, pos)
    for w in a.windows:
        name, ms = w.split('=')
        end = pos + float(ms) * a.rate * 1000
        top = at(snaps, final, end)
        print(show(name, diff(base, top)))
        base, pos = top, end


if __name__ == "__main__":
    main()
