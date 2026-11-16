#!/usr/bin/env python3
"""Measure emulation speed with build/benchmark-replay (fixed guest time).

Workloads: the Bike or Die 2 level-one replay and FrondBench. Writes a text
report to stdout and JSON to build/speed-check/<timestamp>/. A workload whose
session isn't given is skipped.

  BIKE_SESSION, FRONDBENCH_SESSION  session files for the two workloads (no defaults)
  BIKE_REPLAY, FRONDBENCH_REPLAY    another replay for a workload, like frondbench-run.replay for a
                                    core before patch 0414 (T|X touch on the ROM's default calibration)
  SPEED_MIPS                        MIPS cap, default 100
  SPEED_BENCH                       another benchmark-replay binary (default: the clean-tree commit build
                                    from tools/commit-build.sh when it exists, else build/benchmark-replay)
  SPEED_68K                         another frond-68k binary for the 68k recipes (default: the one next to
                                    the benchmark-replay in use, else out/frond-68k)
  SPEED_HOST                        an ssh destination (user@host) to run on instead of here, in ~/frond-speed/
  SPEED_SSH_KEY                     optional ssh key for SPEED_HOST
  SPEED_REAL_MS                     optional FrondBench times from a real device, like Integer=1000,Memory=500,
                                    to print a ratio column; Total is added up from the rest

--counts runs each recipe in tools/speed-baseline.json once per engine and compares its guest
instruction and cycle counts, frame and PCM counts and hashes against the baseline there. It
needs no quiet machine. Exact always repeats; Fast runs with sync compiles (BENCH_JIT_ASYNC is
dropped), which repeat too. Async Fast doesn't, so it's never compared. Only recipes whose counts
moved then get wall-clock rounds; --timing gives rounds for every recipe, for release-notes figures.
A recipe with "measure": true is run but not compared or stored; "read": "speedy" or "frondbench" prints what
the guest showed (Speedy's three times, FrondBench's four). Exits 1 when a count moved or a run failed.

  speed-check.py --counts [--only a,b] [--engines exact,fast] [--jobs 4] [--rounds N] [--against OLD-BENCH]
                 [--timing] [--async-rounds] [--update] [--baseline FILE]

  FROND_DEVICE_IMAGES   firmware folder (deviceimages/palm) for the cold-boot recipes
  ZODIAC_CARDS          folder of Zodiac game cards for the recipes with a card (s1/, s2/)
  BIKE_SESSION, ...     session files for the session recipes, named by each recipe's "session"
  SPEED_LOCK            a lock file (tinydeck: /var/lib/lab/frond-speed/.speed-lock): count runs
                        hold it shared, wall-clock rounds exclusive
"""

import argparse
import concurrent.futures
import fcntl
import hashlib
import json
import os
import statistics
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def commit_build():
    """The shared build folder for HEAD (tools/commit-build.sh) when the tree is clean and it exists."""
    try:
        git = lambda *a: subprocess.run(['git', '-C', str(ROOT), *a], capture_output=True, text=True, check=True).stdout.strip()
        if git('status', '--porcelain'):
            return None
        common = Path(git('rev-parse', '--git-common-dir'))
        common = common if common.is_absolute() else ROOT / common
        folder = common.resolve().parent / 'build' / ('out-' + git('rev-parse', 'HEAD')[:10])
        return folder if (folder / 'COMMIT').exists() else None
    except (OSError, subprocess.CalledProcessError):
        return None


COMMIT_BUILD = commit_build()
BENCH = Path(os.environ.get('SPEED_BENCH') or (COMMIT_BUILD / 'benchmark-replay' if COMMIT_BUILD else ROOT / 'build/benchmark-replay')).absolute()
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
    # name, session variable, replay, guest ms. The FrondBench session was calibrated by hand before
    # 0414 moved T|X touches, so its Run tap is at 34,293 now (38,302 before).
    ('bike', 'BIKE_SESSION', 'bike-level1.replay', 10000),
    ('frondbench', 'FRONDBENCH_SESSION', 'frondbench-run-tx0414.replay', 40000),
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
    files = [BENCH] + [w['replay'] for w in workloads]
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
                  f"{w['replay'].name} {w['ms']} {mips} on {name}/out > {name}.log 2>&1 || tail -n 3 {name}.log >&2; "
                  f"test -f {name}/out/summary.json")
        res = subprocess.run(SSH + [remote], capture_output=True, text=True)
        if res.returncode:
            raise RuntimeError(res.stderr.strip() or 'run failed on the remote host')
        run(['rsync', '-a', '-e', RSH, f'{HOST}:{rdir}/{name}/out/', str(out) + '/'])
    else:
        res = subprocess.run([str(BENCH), w['session'], str(w['replay']), str(w['ms']),
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
            replay = Path(os.environ.get(name.upper() + '_REPLAY') or FIXTURES / replay).absolute()
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


# Count check (--counts). Fields that must match the baseline exactly, per recipe and engine.
COUNTS = ('guest_ns', 'cycles', 'instructions', 'jit_instructions', 'lcd_frames', 'changed_frames',
          'pcm_frames', 'frame_sequence_hash', 'pcm_hash')
BASELINE = ROOT / 'tools/speed-baseline.json'


def speedy_times(out):
    """Speedy's detail form (opened by the replay) as read from guest RAM: Calc, Mem, Graph and Total seconds."""
    data = (out / 'ram-end.bin').read_bytes()
    found = {}
    for key, label in (('calc', b'Calc Test :'), ('mem', b'Mem Test :'), ('graph', b'Graph Test :'), ('total', b'Total :')):
        for at in re.finditer(re.escape(label), data):  # the last copy that has a time after it
            m = re.search(rb'(\d+\.\d\d) sec', data[at.end():at.end() + 80], re.S)
            if m:
                found[key] = float(m[1])
    return found


READERS = {'speedy': speedy_times, 'frondbench': lambda out: (frondbench_results(out) or {}).get('ms', {})}


def digest(path):
    """First 16 hex digits of the SHA-256 of a session or ROM, or of a machine folder's files."""
    h = hashlib.sha256()
    path = Path(path)
    files = sorted(f for f in path.rglob('*') if f.is_file()) if path.is_dir() else [path]
    for f in files:
        if path.is_dir():
            h.update(str(f.relative_to(path)).encode() + b'\0')
        with open(f, 'rb') as data:
            for block in iter(lambda: data.read(1 << 20), b''):
                h.update(block)
    return h.hexdigest()[:16]


def images_path(name):
    """A file under FROND_DEVICE_IMAGES."""
    return Path(os.environ.get('FROND_DEVICE_IMAGES', '')) / name


def card_path(name):
    """A Zodiac card under ZODIAC_CARDS."""
    return Path(os.environ.get('ZODIAC_CARDS', '')) / name


def recipe_key(r, path):
    """Digest of everything a recipe starts from: its ROM or session, its NAND image and its card."""
    key = digest(path)
    if 'nand' in r:
        key = hashlib.sha256((key + digest(images_path(r['nand']))).encode()).hexdigest()[:16]
    if 'card' in r:
        key = hashlib.sha256((key + digest(card_path(r['card']))).encode()).hexdigest()[:16]
    return key


def recipe_input(r):
    """The recipe's session or ROM path, or (None, why it's skipped)."""
    if 'rom' in r:
        images = os.environ.get('FROND_DEVICE_IMAGES')
        if not images:
            return None, 'set FROND_DEVICE_IMAGES'
        path = Path(images) / r['rom']
    else:
        value = os.environ.get(r['session'])
        if not value:
            return None, f"set {r['session']}"
        path = Path(value)
    if r.get('runner') == 'frond-68k' and not CORE_68K.exists():
        return None, f'{CORE_68K} is missing (set SPEED_68K or use a tools/commit-build.sh folder)'
    if 'nand' in r and not images_path(r['nand']).exists():
        return None, f"{images_path(r['nand'])} is missing"
    if 'card' in r:
        if not os.environ.get('ZODIAC_CARDS'):
            return None, 'set ZODIAC_CARDS'
        if not card_path(r['card']).exists():
            return None, f"{card_path(r['card'])} is missing"
    return (path, None) if path.exists() else (None, f'{path} is missing')


class Lock:
    """SPEED_LOCK held shared or exclusive while a run goes, or nothing without it."""
    def __init__(self, exclusive):
        self.path, self.mode = os.environ.get('SPEED_LOCK'), fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH

    def __enter__(self):
        if self.path:
            self.file = open(self.path, 'a')
            fcntl.flock(self.file, self.mode)

    def __exit__(self, *exc):
        if self.path:
            self.file.close()


# SPEED_68K, else the frond-68k next to the benchmark-replay in use (commit-build folders hold
# both, so a copied folder brings it along), else the checkout's out/frond-68k.
CORE_68K = Path(os.environ.get('SPEED_68K') or (BENCH.parent / 'frond-68k' if (BENCH.parent / 'frond-68k').exists()
                else ROOT / 'out/frond-68k')).absolute()


def run_68k(r, path, out, exclusive):
    """A frond-68k --run-for run: its cycle count and hashes of the last frame and the sound."""
    out.mkdir(mode=0o700)
    seconds = r['ms'] / 1000
    cmd = [str(CORE_68K), str(path), '--device', r['device'], '--run-for', f'{seconds:g}', '--speed', 'original',
           '--framebuffer-output', str(out / 'frame.png'), '--audio-output', str(out / 'audio.wav')]
    with Lock(exclusive):
        start = time.monotonic()
        res = subprocess.run(cmd, capture_output=True, text=True)
        wall = time.monotonic() - start
    (out.parent / (out.name + '.log')).write_text(res.stdout + res.stderr)
    m = re.search(r'^ran (\d+) cycles, ([0-9.]+) guest seconds', res.stdout, re.M)
    if res.returncode or not m:
        raise RuntimeError((res.stderr.strip().splitlines() or ['failed'])[-1])
    files = {k: out / f for k, f in (('frame_sequence_hash', 'frame.png'), ('pcm_hash', 'audio.wav'))}
    s = {'engine': 'exact', 'cycles': int(m[1]), 'guest_ns': round(float(m[2]) * 1e9), 'wall_seconds': wall,
         **{k: digest(f) if f.exists() else 'none' for k, f in files.items()}}
    return s


def replay_run(bench, r, path, engine, out, exclusive, asynchronous=False):
    """One benchmark-replay run, or a frond-68k one for a 68k recipe; returns its summary."""
    if r.get('runner') == 'frond-68k':
        return run_68k(r, path, out, exclusive)
    env = dict(os.environ, FROND_JIT='1' if engine == 'fast' else '0')
    env.pop('BENCH_JIT_ASYNC', None)
    if r.get('read'):
        env['BENCH_DUMP_RAM'] = '1'
    if asynchronous:
        env['BENCH_JIT_ASYNC'] = '1'
    replay = r.get('replay', '-')
    cmd = [str(bench), *(['--device', r['device']] if 'rom' in r else []),
           *(['--nand', str(images_path(r['nand']))] if 'nand' in r else []),
           *(['--sd', str(card_path(r['card']))] if 'card' in r else []), str(path),
           replay if replay == '-' else str(FIXTURES / replay), str(r['ms']), str(r['mips']),
           r.get('sound', 'on'), str(out)]
    with Lock(exclusive):
        res = subprocess.run(cmd, env=env, capture_output=True, text=True)
    (out.parent / (out.name + '.log')).write_text(res.stdout + res.stderr)
    shutil.rmtree(out / 'machine', ignore_errors=True)
    if res.returncode:
        raise RuntimeError((res.stderr.strip().splitlines() or ['failed'])[-1])
    s = json.loads((out / 'summary.json').read_text())
    if r.get('read'):
        s['read'] = READERS[r['read']](out)
        for big in ('ram-start.bin', 'ram-end.bin'):
            (out / big).unlink(missing_ok=True)
    if s.get('engine', engine) != engine:
        raise RuntimeError(f"ran {s.get('engine')}, not {engine}: is the JIT built in?")
    return s


def rounds(r, path, engine, n, base, against, asynchronous):
    """n wall-clock rounds, interleaved with an older binary when given; median host s per guest s."""
    times = {'new': [], 'old': []}
    base.mkdir(mode=0o700)
    for i in range(n):
        for which, bench in (('new', BENCH), ('old', against)):
            if bench is None:
                continue
            s = replay_run(bench, r, path, engine, base / f'round-{which}-{i + 1}', True, asynchronous)
            times[which].append(s['wall_seconds'] / (s['guest_ns'] / 1e9))
    return {k: statistics.median(v) for k, v in times.items() if v}, times


def counts_main(args):
    if not BENCH.exists():
        sys.exit('build/benchmark-replay is missing: make benchmark-replay')
    against = Path(args.against).absolute() if args.against else None
    if against and not against.exists():
        sys.exit(f'{against} is missing')
    baseline_path = Path(args.baseline)
    baseline = json.loads(baseline_path.read_text())
    recipes = baseline['recipes']
    names = args.only.split(',') if args.only else list(recipes)
    for name in names:
        if name not in recipes:
            sys.exit(f'no recipe {name} in {baseline_path}')
    engines = args.engines.split(',')
    stamp = f"{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}"
    base = ROOT / 'build/speed-check' / f'counts-{stamp}'
    base.mkdir(parents=True, mode=0o700)
    report, moved, failed, lines = {}, [], [], []
    compared, skipped, missing = 0, 0, 0
    # Count runs don't need a quiet machine, so they run side by side.
    jobs, keys = [], {}
    for name in names:
        path, why = recipe_input(recipes[name])
        if path is None:
            jobs.append((name, None, why))
            continue
        keys[name] = recipe_key(recipes[name], path)
        jobs += [(name, engine, path) for engine in engines if engine in recipes[name].get('engines', engines)]
    with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
        runs = {(n, e): pool.submit(replay_run, BENCH, recipes[n], p, e, base / f'{n}-{e}', False)
                for n, e, p in jobs if e}
    for name, engine, path in jobs:
        if engine is None:
            lines.append(f'{name}: skipped, {path}')
            skipped += 1
            continue
        r, label = recipes[name], f'{name} {engine}'
        try:
            s = runs[(name, engine)].result()
        except Exception as e:
            lines.append(f'{label}: FAILED, {e}')
            failed.append(label)
            continue
        got = {k: s[k] for k in COUNTS if k in s}
        got['input'] = keys[name]
        report[label] = got
        want = r.get('results', {}).get(engine)
        if r.get('measure'):
            # Measure-only: nothing stored or compared; the readings are printed.
            got['read'] = s.get('read', {})
            lines.append(f"{label}: measured, {got.get('instructions', got.get('cycles'))} instructions, "
                         f"{got['cycles']} cycles, {s['guest_ns'] / 1e9:.3f} guest s, read {got['read']}")
            continue
        jit = f", {got['jit_instructions']} in the JIT" if engine == 'fast' else ''
        count = f"{got['instructions']} instructions" if 'instructions' in got else f"{got['cycles']} cycles"
        summary = f"{count}{jit}, frames {s['frame_sequence_hash']}, pcm {s['pcm_hash']}"
        if args.update:
            r.setdefault('results', {})[engine] = got
            lines.append(f'{label}: baseline set, {summary}')
        elif not want:
            missing += 1
            lines.append(f'{label}: no baseline (run with --update), {summary}')
        elif want.get('input') != got['input']:
            skipped += 1
            lines.append(f"{label}: skipped, input {got['input']} isn't the baseline's {want.get('input')}")
        else:
            compared += 1
            diffs = [k for k in COUNTS if got.get(k) != want.get(k)]
            if not diffs:
                lines.append(f'{label}: same, {summary}')
            else:
                moved.append((name, engine, path))
                lines.append(f'{label}: MOVED')
                for k in diffs:
                    old, new = want.get(k), got.get(k)
                    if k not in got:
                        lines.append(f'  {k} {old} -> missing from this run')
                        continue
                    pct = f' ({(new - old) / old * 100:+.3f} %)' if isinstance(old, int) and old else ''
                    lines.append(f'  {k} {old} -> {new}{pct}')
    if args.update:
        baseline_path.write_text(json.dumps(baseline, indent=2) + '\n')
        lines.append(f'wrote {baseline_path}')
    print('\n'.join(lines), flush=True)
    timed = moved if not args.timing else [
        (n, e, recipe_input(recipes[n])[0]) for n in names for e in engines
        if recipe_input(recipes[n])[0] and e in recipes[n].get('engines', engines)]
    timing = {}
    if args.rounds and timed:
        print(f'wall-clock rounds ({args.rounds} each{", against " + str(against) if against else ""}):', flush=True)
        for name, engine, path in timed:
            label = f'{name} {engine}'
            try:
                med, all_times = rounds(recipes[name], path, engine, args.rounds, base / f'{name}-{engine}-rounds',
                                        against, args.async_rounds)
            except Exception as e:
                print(f'{label}: rounds FAILED, {e}')
                failed.append(label)
                continue
            timing[label] = all_times
            text = f"{label}: {med['new']:.3f} s host per guest second"
            if 'old' in med:
                text += f", old {med['old']:.3f} ({(med['new'] - med['old']) / med['old'] * 100:+.1f} %)"
            print(text, flush=True)
    (base / 'counts.json').write_text(json.dumps({'results': report, 'moved': [f'{n} {e}' for n, e, _ in moved],
                                                  'failed': failed, 'rounds': timing}, indent=2) + '\n')
    print(f'JSON: {base.relative_to(ROOT)}/counts.json')
    if not report and not failed:
        print('nothing ran: every recipe was skipped')
        return 1
    if not args.update:
        print(f'{compared} compared, {skipped} skipped, {missing} without a baseline, {len(moved)} moved, {len(failed)} failed')
        if missing:
            print('no baseline for some recipes: run with --update')
        if not compared:
            print('nothing was compared against a baseline')
    return 1 if (not args.update and (moved or missing or not compared)) or failed else 0


def parse():
    p = argparse.ArgumentParser(description='Emulation speed and count checks with benchmark-replay.')
    p.add_argument('--counts', action='store_true', help='compare counts against the baseline')
    p.add_argument('--baseline', default=str(BASELINE))
    p.add_argument('--only', help='comma-separated recipe names')
    p.add_argument('--engines', default='exact,fast')
    p.add_argument('--jobs', type=int, default=4, help='count runs at once')
    p.add_argument('--update', action='store_true', help='write these counts as the baseline')
    p.add_argument('--rounds', type=int, default=3, help='wall-clock rounds for a moved recipe, 0 for none')
    p.add_argument('--timing', action='store_true', help='wall-clock rounds for every recipe')
    p.add_argument('--against', help='an older benchmark-replay to interleave the rounds with')
    p.add_argument('--async-rounds', action='store_true', help='Fast rounds compile on a thread, as the app does')
    return p.parse_args()


ARGS = parse()
if ARGS.counts:
    sys.exit(counts_main(ARGS))
main()
