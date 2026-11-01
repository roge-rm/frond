#!/usr/bin/env python3
"""Measure emulation speed with build/benchmark-replay (fixed guest time).

Workloads: the Bike or Die 2 level-one replay and FrondBench. Writes a text
report to stdout and JSON to build/speed-check/<timestamp>/. A workload whose
private session is missing is skipped. SPEED_HOST=test-host runs on test-host
in ~/frond-speed/ instead of here.
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
SSH_KEY = Path.home() / '.ssh/titan-pocket-test-host'
SSH = ['ssh', '-i', str(SSH_KEY), '-o', 'IdentitiesOnly=yes', 'test-host']
REMOTE = 'frond-speed'
# Real TX figures from Dan's device, in ms.
REAL_TX = {'Integer': 11710, 'Memory': 3270, 'Drawing': 6360, 'Database': 5010, 'Total': 26340}

WORKLOADS = [
    # name, session variable, default session, replay, guest ms
    ('bike', 'BIKE_SESSION', 'debug/mips-cap/level1-paused.frond',
     'bike-level1.replay', 10000),
    ('frondbench', 'FRONDBENCH_SESSION', 'debug/frondbench/run-04/working.frond',
     'frondbench-run.replay', 40000),
]


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


RSH = ' '.join(SSH[:-1])


def remote_lock():
    """Hold ~/frond-speed/.speed-lock on the test host until the returned process is closed."""
    run(SSH + [f'mkdir -p ~/{REMOTE}'])
    p = subprocess.Popen(SSH + [f"cd ~/{REMOTE} && flock -w 3600 .speed-lock sh -c 'echo locked; cat >/dev/null'"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    if p.stdout.readline().strip() != 'locked':
        p.kill()
        sys.exit('could not get the lock on test-host')
    return p


def remote_sync(workloads, rdir):
    """Copy the binary, replays and sessions into this run's own folder on the test host."""
    run(SSH + [f'mkdir -p ~/{rdir}'])
    files = [BENCH] + [FIXTURES / w['replay'] for w in workloads]
    run(['rsync', '-t', '-e', RSH] + [str(f) for f in files] + [f'test-host:{rdir}/'])
    for w in workloads:
        run(SSH + [f"mkdir -p ~/{rdir}/{w['name']}"])
        run(['rsync', '-t', '-e', RSH, w['session'], f"test-host:{rdir}/{w['name']}/session.frond"])


def bench(w, mips, out, host, rdir):
    """Run one workload and return its output folder (local)."""
    env = dict(os.environ, BENCH_DUMP_RAM='1')
    if host == 'test-host':
        name = w['name']
        remote = (f"cd ~/{rdir} && BENCH_DUMP_RAM=1 ./{BENCH.name} {name}/session.frond "
                  f"{w['replay']} {w['ms']} {mips} on {name}/out > {name}.log 2>&1 || tail -n 3 {name}.log >&2; "
                  f"test -f {name}/out/summary.json")
        res = subprocess.run(SSH + [remote], capture_output=True, text=True)
        if res.returncode:
            raise RuntimeError(res.stderr.strip() or 'run failed on test-host')
        run(['rsync', '-a', '-e', RSH, f'test-host:{rdir}/{name}/out/', str(out) + '/'])
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
    host = os.environ.get('SPEED_HOST', '')
    if host not in ('', 'test-host'):
        sys.exit('SPEED_HOST must be empty or test-host')
    if not BENCH.exists():
        sys.exit('build/benchmark-replay is missing: make benchmark-replay')
    stamp = f"{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}"
    base = ROOT / 'build/speed-check' / stamp
    base.mkdir(parents=True)
    ws, skipped = [], []
    for name, var, default, replay, ms in WORKLOADS:
        session = os.environ.get(var) or default
        if not Path(session).is_file():
            print(f'{name}: skipped, no session at {session} (set {var})')
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
                r['frondbench_ratio'] = {k: ms[k] / REAL_TX[k] for k in REAL_TX if k in ms}
                lines.append(f"  {'test':9}{'ms':>8}{'real TX':>9}{'ratio':>7}")
                for k in REAL_TX:
                    if k in ms:
                        lines.append(f"  {k:9}{ms[k]:>8}{REAL_TX[k]:>9}{ms[k] / REAL_TX[k]:>7.2f}")
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
