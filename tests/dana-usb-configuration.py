#!/usr/bin/env python3
"""Dana disconnected DC startup and local register restore with private inputs."""
import argparse, runpy, shutil, struct, sys, time, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import palm_boot
p = argparse.ArgumentParser(description=__doc__)
for name in ('core', 'session', 'evidence'):
    p.add_argument('--' + name, type=Path, required=True)
a = p.parse_args()
palm_boot.CORE_68K = a.core.resolve()
r = a.evidence.resolve()
r.mkdir(mode=0o700, parents=True, exist_ok=False)
chunks = runpy.run_path(str(Path(__file__).with_name('dana-session.py')))['session_chunks']
def open_core(image, tag):
    return palm_boot.Core([image, '--device', 'alphasmart-dana'], r / (tag + '.log'), '--speed', 'uncapped')
def save(c, tag):
    path = r / (tag + '.frond')
    c.send(f'save-session "{path}"', 'wrote session')
    raw, cs = chunks(path.read_bytes())
    at, n = cs[0x1f0]
    assert n == 85, n
    v = struct.unpack_from('<85I', raw, at)
    assert v[0] == 3 and v[69:71] == (0x83, 0xc3), v
    return path, raw, cs, v
source = r / 'input.frond'
shutil.copy2(a.session.resolve(), source)
c = open_core(source, 'startup')
try:
    palm_boot.dana_reset(c, r, False)
    c.send('reset-hard', 'done')
    c.wait_frame(r / 'erase.png', palm_boot.dana_erase, 120)
    # Deliberately hold across reboot to enter the ROM's USB startup path.
    c.key(r, 'up', 5000)
    time.sleep(1)
    active, raw, cs, v = save(c, 'usb-startup')
    expected = (0x83, 0xc3, 0x01, 0x41, 0x23, 0x63,
                0x23, 0x63, 0x23, 0x63, 0x23, 0x63, 0x23, 0x63, 0x23, 0x63)
    assert v[69:] == expected, v[69:]
    print('PASS guest programmed all16 ECRs:', v[69:], flush=True)
    for tag, word, value in [('version', 0, 4), ('address', 68, 256),
                             ('control-out', 69, 0), ('control-in', 70, 0), ('ecr', 71, 256)]:
        bad = bytearray(raw)
        struct.pack_into('<I', bad, cs[0x1f0][0] + word * 4, value)
        path = r / (tag + '.frond')
        path.write_bytes(active.read_bytes()[:92] + zlib.compress(bad))
        assert 'machine restored' in c.send(f'load-session "{path}"', 'session refused:')
        assert save(c, tag + '-after')[3][68:] == v[68:]
    print('PASS five production malformed DC register rollback cases', flush=True)
finally:
    status = c.close()
    if sys.exc_info()[0] is None:
        assert status == 0, status
c = open_core(active, 'fresh')
try:
    assert save(c, 'fresh-restored')[3][68:] == v[68:]
    print('PASS fresh-process DC register restore; enumeration unimplemented', flush=True)
finally:
    status = c.close()
    if sys.exc_info()[0] is None:
        assert status == 0, status
