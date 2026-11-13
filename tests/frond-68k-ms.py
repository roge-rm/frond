#!/usr/bin/env python3
"""The T415's Memory Stick in frond-68k, driven over the console with the VFS probe.

  tests/frond-68k-ms.py --session SESSION.frond --probe FrondMSProbe.prc --evidence DIR [--tg50-stick FILE]

A blank stick is made, attached, formatted by the guest (the probe's format step), written and read
back, ejected and inserted, write protected, saved with the stick and loaded again, then taken out
for Files: the frontends' Files edits the freed file host-side (--storage-tool, build/test-storage-ms
from make test-storage-ms, stands in for it) and put-in links it again. --tg50-stick is
a formatted stick a TG50 wrote: the T415 has to mount it. Build the probe with
tools/build-palm-probe.py ms. The core runs at original speed, so it takes a few minutes.
"""
import argparse, os, signal, struct, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--session', type=Path, required=True)
p.add_argument('--probe', type=Path, required=True)
p.add_argument('--evidence', type=Path, required=True)
p.add_argument('--core', type=Path, default=ROOT / 'out/frond-68k')
p.add_argument('--card-tool', type=Path, default=ROOT / 'out/frond')
p.add_argument('--storage-tool', type=Path, default=ROOT / 'build/test-storage-ms')
p.add_argument('--tg50-stick', type=Path)
p.add_argument('--size', type=int, default=8)
a = p.parse_args()
r = a.evidence.resolve()
r.mkdir(mode=0o700, parents=True, exist_ok=False)
palm_boot.CORE_68K = a.core.resolve()
KEY = '1' * 32
DEVICE = 'sony-clie-t415'
LOG = 'FrondMSLog'
counter = [0]


def read_log(path):
    b = path.read_bytes()
    n = struct.unpack_from('>H', b, 76)[0]
    off = [struct.unpack_from('>I', b, 78 + i * 8)[0] for i in range(n)] + [len(b)]
    return [b[off[i]:off[i + 1]].rstrip(b'\0').decode() for i in range(n)]


def format_pdb(path):
    header = b'FrondMSFormat'.ljust(32, b'\0') + struct.pack('>HHIIIIII4s4sI', 0, 1, 0xb6000000, 0xb6000000, 0, 0, 0, 0, b'DATA', b'FrUs', 0)
    path.write_bytes(header + struct.pack('>IH', 0, 0) + b'\0\0')
    return path


def start(session):
    counter[0] += 1
    return palm_boot.Core([session, '--device', DEVICE], r / f'core-{counter[0]}.log', '--speed', 'original')


def stop(c):
    try:
        os.killpg(c.process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    time.sleep(1)
    c.log.flush()


def card(c, command, expected=None):
    reply = c.send(command, ('card:', 'error:')).strip()
    print(reply, flush=True)
    assert (expected in reply) if expected else 'error:' not in reply, reply
    return reply


def probe_run(c, tag, wait=240):
    """Launches the probe and returns the records of its latest run."""
    c.launch('Frond MS probe')
    dest = r / f'{tag}.pdb'
    end = time.monotonic() + wait
    while time.monotonic() < end:
        time.sleep(5)
        dest.unlink(missing_ok=True)
        try:
            if 'written' not in c.send(f'db-export-db {LOG} "{dest}"', ('export written', 'export failed')):
                continue
            rows = read_log(dest)
        except Exception:
            continue
        starts = [i for i, row in enumerate(rows) if row.startswith('begin ')]
        if starts and rows[-1].startswith('complete '):
            rows = rows[starts[-1]:]
            print(tag, rows, flush=True)
            c.frame(r / f'{tag}.png')
            return {row.split(' ')[0]: row for row in rows}
    c.frame(r / f'{tag}-stuck.png')
    raise AssertionError(f'{tag}: probe did not complete')


def ok(rows, label, value=None):
    assert label in rows and ' err=0000' in rows[label], (label, rows.get(label))
    if value is not None:
        assert rows[label].endswith(f'value={value}'), rows[label]


def mounts(rows):
    ok(rows, 'volume-enumerate'); ok(rows, 'volume-size'); ok(rows, 'guest-open'); ok(rows, 'guest-write'); ok(rows, 'guest-match')


stick = r / 'stick.frondms'
res = subprocess.run([str(a.card_tool), '--card', 'make', 'ms', str(a.size), str(stick)], capture_output=True, text=True)
assert res.returncode == 0, res
c = start(a.session.resolve())
try:
    time.sleep(8)
    card(c, f'ms-attach "{stick}" {KEY} writable', 'inserted')
    card(c, 'ms-state', f'inserted {KEY} writable')
    time.sleep(10)
    c.frame(r / 'unrecognized-format.png')
    c.tap(r, 112, 296)   # Cancel the launcher's own prompt; the probe formats.
    time.sleep(2)
    c.send(f'install "{a.probe.resolve()}"', 'install')
    c.send(f'install "{format_pdb(r / "format.pdb")}"', 'install')
    time.sleep(3)
    c.launch('Frond MS probe')
    time.sleep(15)
    c.frame(r / 'format-prompt.png')
    c.tap(r, 45, 293)    # OK on the probe's format confirmation
    rows = None
    end = time.monotonic() + 600
    dest = r / 'format.pdb.out'
    while time.monotonic() < end and rows is None:
        time.sleep(10)
        dest.unlink(missing_ok=True)
        try:
            if 'written' not in c.send(f'db-export-db {LOG} "{dest}"', ('export written', 'export failed')):
                continue
            got = read_log(dest)
        except Exception:
            continue
        if got and got[-1].startswith('complete '):
            rows = {row.split(' ')[0]: row for row in got}
            print('format', got, flush=True)
    assert rows, 'format did not finish'
    ok(rows, 'format'); mounts(rows)
    c.frame(r / 'formatted.png')

    card(c, 'ms-eject', 'ejected'); time.sleep(2)
    card(c, 'ms-insert', 'inserted'); time.sleep(8)
    rows = probe_run(c, 'reinsert')
    mounts(rows)

    card(c, 'ms-eject', 'ejected')
    card(c, 'ms-protect on', 'protected')
    card(c, 'ms-insert', 'inserted'); time.sleep(8)
    rows = probe_run(c, 'protected')
    ok(rows, 'volume-enumerate')
    assert ' err=0000' not in rows.get('guest-open', ' err=0000') or ' err=0000' not in rows.get('guest-write', ' err=0000'), rows
    card(c, 'ms-eject', 'ejected')
    card(c, 'ms-protect off', 'writable')
    card(c, 'ms-insert', 'inserted'); time.sleep(8)

    saved = r / 'with-stick.frond'
    print(c.send(f'save-session "{saved}"', 'wrote session').strip(), flush=True)
    assert saved.read_bytes().startswith(b'FRONDLINKSD1\n')
finally:
    stop(c)

c = start(saved)
try:
    time.sleep(10)
    card(c, 'ms-state', f'inserted {KEY} writable')
    rows = probe_run(c, 'reloaded')
    mounts(rows)

    # Files: only an ejected stick comes out, and put-in takes back only the stick taken out.
    card(c, 'ms-take-out', 'eject first')
    card(c, 'ms-eject', 'ejected'); time.sleep(2)
    card(c, f'ms-put-in {KEY}', 'take out the current stick first')
    card(c, 'ms-take-out', 'taken-out')
    card(c, 'ms-state', 'none')
    tool = str(a.storage_tool.resolve())
    listing = subprocess.run([tool, 'ls', str(stick)], capture_output=True, text=True)
    print(listing.stdout, listing.stderr, flush=True)
    assert listing.returncode == 0 and 'GUEST.TXT' in listing.stdout.upper(), listing
    added = subprocess.run([tool, 'put', str(stick), 'HOST.TXT', 'from the host'], capture_output=True, text=True)
    assert added.returncode == 0, added
    card(c, f'ms-put-in {"2" * 32} writable', 'identity')
    card(c, f'ms-put-in {KEY} writable', 'inserted')
    card(c, 'ms-state', f'inserted {KEY} writable')
    time.sleep(8)
    rows = probe_run(c, 'after-files')
    mounts(rows)
    card(c, 'ms-eject', 'ejected'); time.sleep(2)
    card(c, 'ms-take-out', 'taken-out')
    listing = subprocess.run([tool, 'ls', str(stick)], capture_output=True, text=True)
    print(listing.stdout, listing.stderr, flush=True)
    assert listing.returncode == 0 and 'HOST.TXT 13' in listing.stdout and 'GUEST.TXT' in listing.stdout.upper(), listing
    card(c, f'ms-put-in {KEY}', 'inserted')
finally:
    stop(c)

if a.tg50_stick:
    foreign = r / 'tg50.frondms'
    foreign.write_bytes(a.tg50_stick.read_bytes())
    c = start(a.session.resolve())
    try:
        time.sleep(8)
        card(c, f'ms-attach "{foreign}" {"2" * 32} writable', 'inserted')
        time.sleep(10)
        rows = probe_run(c, 'tg50-stick')
        ok(rows, 'volume-enumerate'); ok(rows, 'volume-size')
    finally:
        stop(c)
print('frond-68k-ms: ok')
