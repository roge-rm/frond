#!/usr/bin/env python3
"""Runs build/test-storage-settle, then reads each leftover card it settled with mtools and
checks it holds what the app that made it says (tests/fixtures/card-leftovers/*/*.expect).
Needs a UTF-8 locale for mtools to show the non-ASCII name."""

from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tests/fixtures/card-leftovers'


def listing(image):
    """Path -> 'dir' or 'SIZE\\tCRC', as the .expect files have them."""
    # The card's bytes start at 64; its FAT partition at the first MBR entry's start sector.
    with open(image, 'rb') as f:
        f.seek(64 + 446 + 8)
        start = struct.unpack('<I', f.read(4))[0]
    target = f'{image}@@{64 + 512 * start}'
    out = subprocess.run(['mdir', '-/', '-b', '-i', target, '::/'], check=True, capture_output=True).stdout
    found = {}
    for line in out.decode('utf-8').splitlines():
        if not line.startswith('::/'):
            continue
        path = line[2:]
        if path.endswith('/'):
            path = path.rstrip('/')
            if path in ('', '/.', '/..') or path.endswith('/.') or path.endswith('/..'):
                continue
            found[path] = 'dir'
            continue
        data = subprocess.run(['mtype', '-i', target, '::' + path], check=True, capture_output=True).stdout
        found[path] = f'{len(data)}\t{zlib.crc32(data):08x}'
    return found


def main():
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'build/test-storage-settle')
    with tempfile.TemporaryDirectory(prefix='storage-settle-') as work:
        out = Path(work)
        subprocess.run([str(binary), str(FIXTURES), str(out)], check=True, cwd=ROOT)
        cards = sorted(out.glob('*.frondsd'))
        assert len(cards) == 4, cards
        for card in cards:
            side, name = card.stem.split('-')
            lines = (FIXTURES / side / f'{name}.expect').read_text().splitlines()
            generation = struct.unpack_from('<Q', card.read_bytes()[:64], 28)[0]
            assert lines[0] == f'generation {generation}', (card, lines[0], generation)
            want = dict(line.split('\t', 1) for line in lines[1:])
            got = listing(card)
            assert got == want, (card.name, got, want)
    print('storage settle: leftover cards read back with mtools')


if __name__ == '__main__':
    main()
