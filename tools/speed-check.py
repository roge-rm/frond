#!/usr/bin/env python3
"""Measure emulation speed with build/benchmark-replay (fixed guest time).

Workloads: the Bike or Die 2 level-one replay and FrondBench. Writes a text
report to stdout and JSON to build/speed-check/<timestamp>/. A workload whose
session isn't given is skipped.

  BIKE_SESSION, FRONDBENCH_SESSION  session files for the two workloads (no defaults)
  SPEED_MIPS                        MIPS cap, default 100
  SPEED_BENCH                       another benchmark-replay binary
  SPEED_HOST                        an ssh destination (user@host) to run on instead of here, in ~/frond-speed/
  SPEED_SSH_KEY                     optional ssh key for SPEED_HOST
  SPEED_REAL_MS                     optional FrondBench times from a real device, like Integer=1000,Memory=500,
                                    to print a ratio column; Total is added up from the rest
"""

import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BENCH = Path(os.environ.get('SPEED_BENCH') or ROOT / 'build/benchmark-replay').absolute()
FIXTURES = ROOT / 'tests/fixtures'
HOST = os.environ.get('SPEED_HOST', '')
KEY = os.environ.get('SPEED_SSH_KEY', '')
SSH = ['ssh', *(['-i', KEY, '-o', 'IdentitiesOnly=yes'] if KEY else []), HOST]
REMOTE = 'frond-speed'
REAL = {}
for item in filter(None, os.environ.get('SPEED_REAL_MS', '').split(',')):
    name, _, value = item.partition('=')
    try:
        REAL[name.strip()] = int(value)
    except ValueError:
        raise SystemExit(f'SPEED_REAL_MS: expected NAME=MILLISECONDS, got {item!r}')
if REAL and 'Total' not in REAL:
    REAL['Total'] = sum(REAL.values())

WORKLOADS = [
    # name, session variable, replay, guest ms
    ('bike', 'BIKE_SESSION', 'bike-level1.replay', 10000),
    ('frondbench', 'FRONDBENCH_SESSION', 'frondbench-run.replay', 40000),
]


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


RSH = ' '.join(SSH[:-1])


def remote_lock():
    """Hold ~/frond-speed/.speed-lock on the remote host until the returned process is closed."""
    run(SSH + [f'mkdir -p ~/{REMOTE}'])
    p = subprocess.Popen(SSH + [f"cd ~/{REMOTE} && flock -w 3600 .speed-lock sh -c 'echo locked; cat >/dev/null'"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    if p.stdout.readline().strip() != 'locked':
        p.kill()
        sys.exit('could not get the lock on the remote host')
    return p


def remote_sync(workloads, rdir):
    """Copy the binary, replays and sessions into this run's own folder on the remote host."""
    run(SSH + [f'mkdir -p ~/{rdir}'])
    files = [BENCH] + [FIXTURES / w['replay'] for w in workloads]
    run(['rsync', '-t', '-e', RSH] + [str(f) for f in files] + [f'{HOST}:{rdir}/'])
    for w in workloads:
        run(SSH + [f"mkdir -p ~/{rdir}/{w['name']}"])
        run(['rsync', '-t', '-e', RSH, w['session'], f"{HOST}:{rdir}/{w['name']}/session.frond"])


def bench(w, mips, out, host, rdir):
    """Run one workload and return its output folder (local)."""
    env = dict(os.environ, BENCH_DUMP_RAM='1')
    if host:
        name = w['name']
        remote = (f"cd ~/{rdir} && BENCH_DUMP_RAM=1 ./{BENCH.name} {name}/session.frond "
                  f"{w['replay']} {w['ms']} {mips} on {name}/out > {name}.log 2>&1 || tail -n 3 {name}.log >&2; "
                  f"test -f {name}/out/summary.json")
        res = subprocess.run(SSH + [remote], capture_output=True, text=True)
        if res.returncode:
            raise RuntimeError(res.stderr.strip() or 'run failed on the remote host')
        run(['rsync', '-a', '-e', RSH, f'{HOST}:{rdir}/{name}/out/', str(out) + '/'])
    else:
        res = subprocess.run([str(BENCH), w['session'], str(FIXTURES / w['replay']), str(w['ms']),
                              str(mips), 'on', str(out)], env=env, capture_output=True, text=True)
        if res.returncode:
            raise RuntimeError(res.stderr.strip())
    return out


def frondbench_results(out):
    """Find the results record that the run wrote, by diffing guest RAM before and after."""
    pat = re.compile(rb'FrondBench 1\.0\n(?:[^\0]{0,380}?)Checksum [0-9A-Fa-f]+\n')
    before = {m.group() for m in pat.finditer((out / 'ram-start.bin').read_bytes())}
    after = [m.group() for m in pat.finditer((out / 'ram-end.bin').read_bytes()) if m.group() not in before]
    if not after:
        return None
    text = after[-1].decode('latin-1')
    ms = {m[0]: int(m[1]) for m in re.findall(r'^(\w+) (\d+) ms', text, re.M)}
    return {'ms': ms, 'text': text}


def main():
    mips = int(os.environ.get('SPEED_MIPS', '100'))
    host = HOST
    if not BENCH.exists():
        sys.exit('build/benchmark-replay is missing: make benchmark-replay')
    stamp = f"{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}"
    base = ROOT / 'build/speed-check' / stamp
    base.mkdir(parents=True)
    ws, skipped = [], []
    for name, var, replay, ms in WORKLOADS:
        session = os.environ.get(var)
        if not session or not Path(session).is_file():
            print(f'{name}: skipped, {"no session file at " + session if session else "no session"} (set {var})')
            skipped.append(name)
        else:
            ws.append(dict(name=name, session=session, replay=replay, ms=ms))
    rdir = f'{REMOTE}/run-{stamp}'
    lock = None
    if host and ws:
        lock = remote_lock()
        remote_sync(ws, rdir)
    report = {'mips': mips, 'host': host or 'local', 'skipped': skipped, 'workloads': {}}
    lines = [f'speed-check at {mips} MIPS on {host or "this machine"}']
    for w in ws:
        out = base / w['name']
        try:
            bench(w, mips, out, host, rdir)
            s = json.loads((out / 'summary.json').read_text())
        except Exception as e:
            print(f"{w['name']}: failed: {e}")
            report['workloads'][w['name']] = {'error': str(e)}
            continue
        wall, guest = s['wall_seconds'], s['guest_ns'] / 1e9
        r = {'wall_seconds': wall, 'guest_seconds': guest, 'host_per_guest_second': wall / guest,
             'host_mips': s['cycles'] / wall / 1e6, 'cycles': s['cycles'], 'lcd_frames': s['lcd_frames'],
             'changed_frames': s['changed_frames'], 'pcm_frames': s['pcm_frames'],
             'frame_sequence_hash': s['frame_sequence_hash'], 'pcm_hash': s['pcm_hash']}
        lines.append(f"{w['name']}: {wall:.2f} s host for {guest:.2f} s guest "
                     f"({r['host_per_guest_second']:.3f} s per guest second), host {r['host_mips']:.1f} MIPS")
        lines.append(f"  frames {s['lcd_frames']} ({s['changed_frames']} changed)  frame-sequence {s['frame_sequence_hash']}"
                     f"  pcm {s['pcm_hash']} ({s['pcm_frames']} frames)")
        if w['name'] == 'frondbench':
            fb = frondbench_results(out)
            if not fb:
                lines.append('  FrondBench results not found in guest RAM')
            else:
                ms = dict(fb['ms'])
                ms['Total'] = sum(v for k, v in ms.items() if k != 'Total')
                r['frondbench_ms'] = ms
                if REAL:
                    r['frondbench_ratio'] = {k: ms[k] / REAL[k] for k in REAL if k in ms}
                    lines.append(f"  {'test':9}{'ms':>8}{'real':>9}{'ratio':>7}")
                    for k in REAL:
                        if k in ms:
                            lines.append(f"  {k:9}{ms[k]:>8}{REAL[k]:>9}{ms[k] / REAL[k]:>7.2f}")
                else:
                    lines += [f"  {k:9}{v:>8}" for k, v in ms.items()]
        for f in ('ram-start.bin', 'ram-end.bin'):
            (out / f).unlink(missing_ok=True)
        report['workloads'][w['name']] = r
    if lock:
        subprocess.run(SSH + [f'rm -rf ~/{rdir}'])
        lock.stdin.close()
        lock.wait()
    (base / 'speed-check.json').write_text(json.dumps(report, indent=2) + '\n')
    lines.append(f'JSON: {base.relative_to(ROOT)}/speed-check.json')
    print('\n'.join(lines))


main()
