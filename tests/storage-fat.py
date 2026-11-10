#!/usr/bin/env python3
"""Runs build/test-storage-fat: its self test, then FAT12 cards it formats and fills (4, 8, 16
and 64 MiB, raw and as card files) and images mtools made (a 16 MiB SD card with a partition, a 4 MiB
superfloppy), read back with mtools; files mtools adds are read back by the library; FAT32 is
refused. Needs a UTF-8 locale for mtools to show the non-ASCII name."""

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
ENV = dict(os.environ, LC_ALL='C.UTF-8')


def mtools(*args):
    return subprocess.run(list(args), check=True, capture_output=True, env=ENV).stdout


def partition(image, base=0):
    """The mtools target for the FAT in a raw image at `base`: its partition, or the whole image."""
    with open(image, 'rb') as f:
        f.seek(base)
        sector = f.read(512)
    if sector[54:62] in (b'FAT12   ', b'FAT16   '):
        return f'{image}@@{base}'
    start = struct.unpack_from('<I', sector, 446 + 8)[0]
    return f'{image}@@{base + 512 * start}'


def listing(target):
    """'bits N' and each path with 'dir' or 'SIZE\\tCRC', as the .expect files have them."""
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


def expected(text):
    lines = text.splitlines()
    bits = int(lines[0].split()[1])
    return bits, dict(line.split('\t', 1) for line in lines[1:])


def fat_bits(target):
    """FAT12 or FAT16 from the boot sector's own numbers, as mtools and the Palm decide it."""
    image, base = target.split('@@')
    with open(image, 'rb') as f:
        f.seek(int(base))
        b = f.read(512)
    per, reserved, fats, root, total16, fat_sectors = struct.unpack_from('<BHBHHxH', b, 13)
    total = total16 or struct.unpack_from('<I', b, 32)[0]
    clusters = (total - reserved - fats * fat_sectors - (root * 32 + 511) // 512) // per
    return 12 if clusters < 4085 else 16


def check(binary, image, base, label):
    want_bits, want = expected(Path(str(image) + '.expect').read_text())
    target = partition(image, base)
    assert fat_bits(target) == want_bits == 12, (label, fat_bits(target), want_bits)
    got = listing(target)
    assert got == want, (label, sorted(set(got.items()) ^ set(want.items())))
    print(f'{label}: FAT12, {len(want)} paths read back with mtools')
    return target


def main():
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'build/test-storage-fat')
    with tempfile.TemporaryDirectory(prefix='storage-fat-') as work:
        out = Path(work)
        subprocess.run([str(binary), 'selftest'], check=True, env=dict(ENV, TMPDIR=work))

        for mib in (4, 8, 16, 64):
            raw = out / f'fresh-{mib}.img'
            subprocess.run([str(binary), 'make', str(raw), str(mib), 'raw'], check=True, env=ENV)
            target = check(binary, raw, 0, f'{mib} MiB raw')
            # mtools adds a file and a folder; the library reads them back.
            host = out / 'host.bin'
            host.write_bytes(os.urandom(50000))
            mtools('mmd', '-i', target, '::/From mtools')
            mtools('mcopy', '-i', target, str(host), '::/From mtools/Written by mtools.bin')
            listed = subprocess.run([str(binary), 'list', str(raw)], check=True, capture_output=True, env=ENV).stdout
            bits, mine = expected(listed.decode('utf-8'))
            assert bits == 12 and mine == listing(target), f'{mib} MiB: the library reads what mtools wrote'

            card = out / f'card-{mib}.frondsd'
            subprocess.run([str(binary), 'make', str(card), str(mib), 'container'], check=True, env=ENV)
            check(binary, card, 64, f'{mib} MiB card file')
            generation = struct.unpack_from('<Q', card.read_bytes()[:64], 28)[0]
            assert generation == 7, generation

        # A 16 MiB SD card as a camera leaves it: a partition at sector 57, FAT12, a few photos,
        # then filled by the library.
        sd = out / 'camera-16.img'
        sd.write_bytes(bytes(16 << 20))
        start, sectors = 57, (16 << 11) - 57
        with open(sd, 'r+b') as f:
            entry = struct.pack('<B3sB3sII', 0x80, b'\0\0\0', 0x01, b'\0\0\0', start, sectors)
            f.seek(446)
            f.write(entry)
            f.seek(510)
            f.write(b'\x55\xaa')
        target = f'{sd}@@{512 * start}'
        mtools('mformat', '-i', target, '-T', str(sectors), '-h', '2', '-s', '32', '-H', str(start), '-c', '32',
               '-v', 'CAMERA', '::')
        mtools('mmd', '-i', target, '::/DCIM', '::/DCIM/100CANON')
        for n in range(3):
            photo = out / f'IMG_{n:04}.JPG'
            photo.write_bytes(os.urandom(200000 + n))
            mtools('mcopy', '-i', target, str(photo), f'::/DCIM/100CANON/IMG_{n:04}.JPG')
        subprocess.run([str(binary), 'fill', str(sd)], check=True, env=ENV)
        check(binary, sd, 0, '16 MiB SD image from mtools')

        # A 4 MiB superfloppy.
        floppy = out / 'floppy-4.img'
        mtools('mformat', '-C', '-i', str(floppy), '-T', '8192', '-h', '2', '-s', '16', '-c', '16', '::')
        subprocess.run([str(binary), 'fill', str(floppy)], check=True, env=ENV)
        check(binary, floppy, 0, '4 MiB superfloppy from mtools')

        # FAT32 from mtools is refused.
        fat32 = out / 'fat32.img'
        mtools('mformat', '-C', '-F', '-i', str(fat32), '-T', '81920', '-h', '2', '-s', '32', '-c', '1', '::')
        said = subprocess.run([str(binary), 'refuse', str(fat32)], check=True, capture_output=True, env=ENV).stdout
        assert said.decode().strip() == 'This card uses FAT32.', said
    print('storage fat: FAT12 cards read back with mtools')


if __name__ == '__main__':
    main()
