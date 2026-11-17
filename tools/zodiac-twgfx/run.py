#!/usr/bin/env python3
"""Runs the Zodiac TwGfx test program headless through benchmark-replay and writes its table: per test, on
Fast and Exact, ok, the ROM's error code, refused (Frond stopped, with its stop text) or crashed (no result
before the run's time ran out). Compares the table with a golden list (--golden) or rewrites it (--update).

Every test is its own run from one saved state of the program waiting for the pen; the tap's cell picks the
test (twgfxtest.c), and the program draws its result as a row of coloured blocks that a snapshot at the
run's end catches. The card and saved states are made in WORK:
  boot91.state     cold boot through the setup wizard (the fixture's events before 91 s), Fast
  launched.state   card in, the card view, the program launched and waiting for the pen, Fast
Exact runs load the same state. Runs go JOBS at a time."""
import argparse
import concurrent.futures
import difflib
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
BOOT_MS = 91000
LAUNCH = ['91500 card-insert', '95500 pen-down 370 80', '95700 pen-up', '96500 pen-down 144 75', '96700 pen-up']
LAUNCHED_MS = 99000
TAP_MS = LAUNCHED_MS + 20
CELL, COLUMNS = 20, 20
# The result row in landscape display coordinates (twgfxtest.c showResult): 10 blocks of 16, 40 from the right.
DISPLAY_W, DISPLAY_H, BLOCK, BLOCKS, MARGIN = 480, 320, 16, 10, 40


def log(text):
    print(text, flush=True)


def tests_from_source():
    """Test names in table order, from twgfxtest.c."""
    with open(os.path.join(HERE, 'twgfxtest.c')) as f:
        text = f.read()
    table = text[text.index('static const Test tests[] = {'):]
    table = table[:table.index('};')]
    return re.findall(r'\{"([^"]+)", t\w+, [^}]+\}', table)


def make_card(work, prc):
    """A 64 MB card like the game pass's: one FAT16 partition at sector 2048, the program in /PALM/Launcher."""
    card = os.path.join(work, 'card.img')
    size, start = 64 << 20, 2048
    mbr = bytearray(512)
    mbr[446:462] = struct.pack('<B3sB3sII', 0, b'\0\0\0', 6, b'\0\0\0', start, size // 512 - start)
    mbr[510:512] = b'\x55\xaa'
    with open(card, 'wb') as f:
        f.write(mbr)
        f.truncate(size)
    at = '%s@@%d' % (card, start * 512)
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
    subprocess.run(['mformat', '-N', '5a0d1ac0', '-i', at, '-T', str(size // 512 - start), '-h', '64', '-s', '32', '-v', 'ZODIAC', '::'],
                   check=True, env=env)
    subprocess.run(['mmd', '-i', at, '::/PALM', '::/PALM/Launcher'], check=True, env=env)
    subprocess.run(['mcopy', '-i', at, prc, '::/PALM/Launcher/TwGfxTest.prc'], check=True, env=env)
    return card


def read_png_rows(path, last_row):
    """Rows 0 to last_row of an 8-bit RGBA PNG (what benchmark-replay writes) as byte arrays, and the height."""
    with open(path, 'rb') as f:
        data = f.read()
    pos, idat = 8, b''
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            width, height, depth, colour = struct.unpack('>IIBB', body[:10])
            if depth != 8 or colour != 6:
                raise ValueError('%s: not an 8-bit RGBA PNG' % path)
        elif kind == b'IDAT':
            idat += body
        pos += 12 + length
    raw, stride, rows, prev = zlib.decompress(idat), width * 4, [], bytearray(width * 4)
    for y in range(min(last_row + 1, height)):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if kind == 2:
            line = bytearray((x + p) & 255 for x, p in zip(line, prev))
        elif kind:
            for i in range(stride):
                a = line[i - 4] if i >= 4 else 0
                b, c = prev[i], (prev[i - 4] if i >= 4 else 0)
                if kind == 1:
                    line[i] = (line[i] + a) & 255
                elif kind == 3:
                    line[i] = (line[i] + (a + b) // 2) & 255
                elif kind == 4:
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return rows, height


def read_result(png):
    """The program's result row as 10 words, or None. The snapshot is the portrait panel; landscape (x, y) is
    panel (y, height - 1 - x)."""
    if not os.path.exists(png):
        return None
    xs = [DISPLAY_W - MARGIN - (BLOCKS - b) * BLOCK + BLOCK // 2 for b in range(BLOCKS)]
    ly = DISPLAY_H - BLOCK // 2
    rows, height = read_png_rows(png, DISPLAY_W - 1 - min(xs))
    words = []
    for lx in xs:
        px, py = ly, height - 1 - lx
        r, g, b = rows[py][px * 4:px * 4 + 3]
        words.append((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3))
    if words[0] != 0xf81f or words[1] != 0x07e0 or words[9] != 0xf81f:
        return None
    return words


class Runner:
    def __init__(self, args, card):
        self.args, self.card, self.work = args, card, args.work

    def bench(self, name, events, end_ms, fast, load=None, save=None):
        """One benchmark-replay run: (stop text or None, its output folder)."""
        replay = os.path.join(self.work, name + '.replay')
        with open(replay, 'w') as f:
            f.write(''.join(e + '\n' for e in events))
        out = os.path.join(self.work, name + '.out')
        shutil.rmtree(out, ignore_errors=True)
        env = dict(os.environ, FROND_JIT='1' if fast else '0')
        for key in ('BENCH_ZODIAC_SAVE', 'BENCH_ZODIAC_LOAD', 'BENCH_ZODIAC_TRACE', 'BENCH_ZODIAC_SD2'):
            env.pop(key, None)
        if load:
            env['BENCH_ZODIAC_LOAD'] = load
        if save:
            env['BENCH_ZODIAC_SAVE'] = save
        result = subprocess.run([self.args.bench, '--device', 'tapwave-zodiac-2', '--sd', self.card, self.args.rom, replay,
                                 str(end_ms), 'original', 'on', out], env=env, capture_output=True, text=True)
        text = result.stdout + result.stderr
        with open(os.path.join(self.work, name + '.log'), 'w') as f:
            f.write(text)
        os.remove(replay)
        stop = re.search(r'^stopped: (.*)$', text, re.M)
        if result.returncode and not stop:
            sys.exit('run.py: %s failed:\n%s' % (name, text[-2000:]))
        return (stop.group(1) if stop else None), out

    def states(self):
        boot = os.path.join(self.work, 'boot91.state')
        launched = os.path.join(self.work, 'launched.state')
        # make relinks benchmark-replay every time, so the boot is redone only when the binary's bytes change.
        with open(self.args.bench, 'rb') as f:
            bench_hash = hashlib.sha256(f.read()).hexdigest()
        stamp = boot + '.bench'
        if not os.path.exists(boot) or not os.path.exists(stamp) or open(stamp).read() != bench_hash:
            log('cold boot to %d ms (Fast)' % BOOT_MS)
            events = []
            with open(self.args.fixture) as f:
                for line in f:
                    parts = line.split()
                    if parts and not line.startswith('#') and int(parts[0]) < BOOT_MS and parts[1] != 'snapshot':
                        events.append(line.strip())
            stop, out = self.bench('boot', events, BOOT_MS + 1000, True, save='%d:%s' % (BOOT_MS, boot))
            shutil.rmtree(out, ignore_errors=True)
            if stop or not os.path.exists(boot):
                sys.exit('run.py: the cold boot stopped: %s' % stop)
            with open(stamp, 'w') as f:
                f.write(bench_hash)
        log('launch to %d ms (Fast)' % LAUNCHED_MS)
        stop, out = self.bench('launch', LAUNCH + ['%d snapshot "s.png"' % (LAUNCHED_MS - 10)], LAUNCHED_MS + 1000, True,
                               load=boot, save='%d:%s' % (LAUNCHED_MS, launched))
        words = read_result(os.path.join(out, 'snapshot-1.png'))
        if stop or not words or words[2] != 1:
            sys.exit('run.py: the program is not waiting for the pen at %d ms (%s, see %s)' % (LAUNCHED_MS, stop, out))
        shutil.rmtree(out)
        return launched

    def one(self, engine, n, budget_ms, launched):
        """Test n on one engine: (status, surface hash or None, ms to the result or None)."""
        cell = (n % COLUMNS * CELL + CELL // 2, n // COLUMNS * CELL + CELL // 2)
        end = TAP_MS + budget_ms
        events = ['%d pen-down %d %d' % (TAP_MS, cell[0], cell[1]), '%d pen-up' % (TAP_MS + 40), '%d snapshot "s.png"' % (end - 10)]
        name = '%s-%03d' % (engine, n)
        stop, out = self.bench(name, events, end, engine == 'fast', load=launched)
        words = read_result(os.path.join(out, 'snapshot-1.png'))
        shutil.rmtree(out, ignore_errors=True)
        done = words is not None and words[2] == 3 and words[3] == n
        if stop:
            return ('refused: ' + stop if not done else 'refused after its result: ' + stop), None, None
        if not done:
            return 'crashed (no result in %d ms)' % budget_ms, None, None
        os.remove(os.path.join(self.work, name + '.log'))
        status = 'ok' if words[4] == 0 else 'error %04x' % words[4]
        return status, '%04x%04x%s' % (words[5], words[6], '' if words[7] else ' (unchanged)'), words[8]

    def engine(self, engine, names, launched, timings):
        def budget(n):
            # Fast runs get a fixed time; Exact twice what Fast took to its result, with room.
            if engine == 'fast' or timings.get(n) is None:
                return self.args.budget_ms
            return 2 * timings[n] + 500
        results = {}
        with concurrent.futures.ThreadPoolExecutor(self.args.jobs) as pool:
            jobs = {pool.submit(self.one, engine, n, budget(n), launched): n for n in range(len(names))}
            for job in concurrent.futures.as_completed(jobs):
                n = jobs[job]
                results[n] = job.result()
                if not results[n][0].startswith(('ok', 'error')):
                    log('%s: test %d %s: %s' % (engine, n, names[n], results[n][0]))
        return results


def short(status):
    """The status without the value written, which changes with unrelated work, for the golden list."""
    return re.sub(r', value [0-9a-f]+', '', status)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bench', required=True)
    parser.add_argument('--rom', required=True)
    parser.add_argument('--fixture', required=True, help='replay that boots through the setup wizard (tests/fixtures/zodiac-doom2.replay)')
    parser.add_argument('--prc', required=True)
    parser.add_argument('--work', required=True)
    parser.add_argument('--engines', default='fast,exact')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--budget-ms', type=int, default=1000, help='guest ms from the tap to the snapshot on Fast')
    parser.add_argument('--only', help='comma-separated test numbers, for a quick look (no golden check)')
    parser.add_argument('--golden')
    parser.add_argument('--update', action='store_true')
    args = parser.parse_args()
    os.makedirs(args.work, exist_ok=True)
    names = tests_from_source()
    card = make_card(args.work, args.prc)
    runner = Runner(args, card)
    launched = runner.states()
    engines = args.engines.split(',')
    if args.only:
        for engine in engines:
            for n in map(int, args.only.split(',')):
                log('%s %d %s: %s' % (engine, n, names[n], runner.one(engine, n, args.budget_ms * (1 if engine == 'fast' else 3), launched)))
        return
    columns, timings = {}, {}
    for engine in sorted(engines, key=lambda e: e != 'fast'):
        log('%s: %d tests' % (engine, len(names)))
        columns[engine] = runner.engine(engine, names, launched, timings)
        if engine == 'fast':
            timings = {n: r[2] for n, r in columns[engine].items()}
    table = ['| # | Test | ' + ' | '.join(e.capitalize() for e in engines) + ' | Surface hash |',
             '|---|---|' + '---|' * len(engines) + '---|']
    golden = []
    for n, name in enumerate(names):
        cells = [columns[e][n][0] for e in engines]
        sums = sorted(set(columns[e][n][1] for e in engines if columns[e][n][1]))
        table.append('| %d | %s | %s | %s |' % (n, name, ' | '.join(cells), ', '.join(sums) + (' (differ)' if len(sums) > 1 else '')))
        golden += ['%s %d %s: %s' % (e, n, name, short(c)) for e, c in zip(engines, cells)]
    with open(os.path.join(args.work, 'table.md'), 'w') as f:
        f.write('\n'.join(table) + '\n')
    bad = sum(1 for line in golden if ': refused' in line or ': crashed' in line)
    log('%d tests, %d results refused or crashed; table in %s' % (len(names), bad, os.path.join(args.work, 'table.md')))
    order = lambda l: (int(l.split(' ', 2)[1]), l.split(' ', 1)[0])
    golden.sort(key=order)
    if not args.golden:
        return
    # One line per engine and test, so a run on fewer engines compares its own lines only.
    old = []
    if os.path.exists(args.golden):
        with open(args.golden) as f:
            old = [l.rstrip('\n') for l in f if l.strip() and not l.startswith('#')]
    if args.update:
        kept = [l for l in old if l.split(' ', 1)[0] not in engines]
        with open(args.golden, 'w') as f:
            f.write("# tools/zodiac-twgfx/run.py: engine, test, name: ok, error (the ROM's code), refused (Frond's stop) or crashed\n")
            f.write('\n'.join(sorted(kept + golden, key=order)) + '\n')
        log('updated %s' % args.golden)
        return
    want = sorted((l for l in old if l.split(' ', 1)[0] in engines), key=order)
    if want != golden:
        sys.stdout.writelines(difflib.unified_diff([l + '\n' for l in want], [l + '\n' for l in golden], 'golden', 'now'))
        sys.exit('run.py: the table changed; if that is intended, run again with --update')
    log('matches %s (%d refused or crashed)' % (args.golden, bad))


if __name__ == '__main__':
    main()
