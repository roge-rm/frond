#!/usr/bin/env python3
"""Runs build/test-storage-ms: its self test, then blank classic sticks of every size formatted and
filled through Files (4, 8, 16, 32, 64 and 128 MB), read back with mtools from the logical image
LogicalMsMedia gives: the TG50's partition, FAT12 or FAT16, a hidden read-only MEMSTICK.IND and
every file. Needs a UTF-8 locale for mtools to show the non-ASCII name."""

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
ENV = dict(os.environ, LC_ALL='C.UTF-8')
# Partition start and FAT type the TG50 writes for each size.
FORMAT = {4: (27, 12), 8: (25, 12), 16: (25, 12), 32: (19, 12), 64: (39, 12), 128: (33, 16)}


def mtools(*args):
    return subprocess.run(list(args), check=True, capture_output=True, env=ENV).stdout


def listing(target):
    out = mtools('mdir', '-/', '-b', '-i', target, '::/').decode('utf-8')
    found = {}
    for line in out.splitlines():
        if not line.startswith('::/'):
            continue
        path = line[2:]
        if path.endswith('/'):
            path = path.rstrip('/')
            if path in ('', '/.', '/..') or path.endswith('/.') or path.endswith('/..'):
                continue
            found[path] = 'dir'
            continue
        data = mtools('mtype', '-i', target, '::' + path)
        found[path] = f'{len(data)}\t{zlib.crc32(data):08x}'
    return found


def main():
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'build/test-storage-ms')
    with tempfile.TemporaryDirectory(prefix='storage-ms-') as work:
        out = Path(work)
        subprocess.run([str(binary), 'selftest'], check=True, env=dict(ENV, TMPDIR=work))
        for mb, (start, bits) in FORMAT.items():
            image = out / f'stick-{mb}.img'
            subprocess.run([str(binary), 'make', str(image), str(mb)], check=True, env=ENV)
            mbr = image.read_bytes()[:512]
            kind, lba = struct.unpack_from('<4xB3xI', mbr, 446)
            assert lba == start and kind == (6 if bits == 16 else 1), (mb, kind, lba)
            target = f'{image}@@{512 * start}'
            lines = Path(str(image) + '.expect').read_text().splitlines()
            assert int(lines[0].split()[1]) == bits, (mb, lines[0])
            want = dict(line.split('\t', 1) for line in lines[1:])
            got = listing(target)
            assert got == want, (mb, sorted(set(got.items()) ^ set(want.items())))
            attrs = mtools('mattrib', '-i', target, '::/MEMSTICK.IND').decode()
            assert 'R' in attrs.split('::')[0] and 'H' in attrs.split('::')[0], (mb, attrs)
            print(f'{mb} MB: FAT{bits} at sector {start}, MEMSTICK.IND hidden and read-only, '
                  f'{len(want)} paths read back with mtools')
    print('storage ms: classic sticks read back with mtools')


if __name__ == '__main__':
    main()
