#!/usr/bin/env python3
"""Builds TwGfxTest.prc, the Zodiac TwGfx test program: a 68k launcher (code #0/#1, data #0), the ARM code as
'ARMZ' #1 (gzip, like the games) and the launcher name 'tAIN' #1000, which the card view needs to list it.

The ARM image is linked twice, at 0 and at 0x100000; every word that differs by exactly that much holds an
address and goes in the relocation table start.s applies at run time.

Usage: build.py OUTDIR (writes OUTDIR/TwGfxTest.prc)."""
import datetime
import gzip
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ARM_FLAGS = ['-O2', '-Wall', '-std=gnu99', '-marm', '-mcpu=arm920t', '-mthumb-interwork', '-ffixed-r9',
             '-fno-builtin', '-ffreestanding', '-nostdinc']
SHIFT = 0x100000


def run(*args):
    subprocess.run(args, check=True)


def link(out, base, objects):
    elf = os.path.join(out, 'twgfx-%x.elf' % base)
    libgcc = subprocess.run(['arm-palmos-gcc', '-print-libgcc-file-name'], check=True, capture_output=True, text=True).stdout.strip()
    script = os.path.join(out, 'link-%x.ld' % base)
    with open(os.path.join(HERE, 'link.ld')) as f, open(script, 'w') as g:
        g.write(f.read().replace('@BASE@', '%#x' % base))
    run('arm-palmos-ld', '-T', script, '-o', elf, *objects, libgcc)
    raw = elf + '.bin'
    run('arm-palmos-objcopy', '-O', 'binary', elf, raw)
    with open(raw, 'rb') as f:
        return f.read()


def main():
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    objects = []
    for source in ('start.s', 'twgfxtest.c'):
        obj = os.path.join(out, os.path.splitext(source)[0] + '.o')
        run('arm-palmos-gcc', *ARM_FLAGS, '-c', os.path.join(HERE, source), '-o', obj)
        objects.append(obj)
    # start.o must be first in the image: link.ld places it by name, so run ld from out.
    cwd = os.getcwd()
    os.chdir(out)
    try:
        low = link(out, 0, ['start.o', 'twgfxtest.o'])
        high = link(out, SHIFT, ['start.o', 'twgfxtest.o'])
    finally:
        os.chdir(cwd)
    if len(low) != len(high) or len(low) % 4:
        sys.exit('build.py: the two links differ in size')
    relocs = []
    for i in range(0, len(low), 4):
        a, = struct.unpack_from('<I', low, i)
        b, = struct.unpack_from('<I', high, i)
        if a != b:
            if b - a != SHIFT:
                sys.exit('build.py: word at %#x moved by %#x' % (i, b - a))
            relocs.append(i)
    image = bytearray(low)
    if image[28:32] != b'ANWT':
        sys.exit('build.py: no ANWT header at +28')
    offset_at = image.find(struct.pack('<I', 0x4f4c4552))
    count_at = image.find(struct.pack('<I', 0x544e4352))
    if offset_at < 0 or count_at != offset_at + 4:
        sys.exit('build.py: relocation placeholders not found')
    struct.pack_into('<II', image, offset_at, len(image), len(relocs))
    image += b''.join(struct.pack('<I', r) for r in relocs)
    armz = os.path.join(out, 'ARMZ0001.bin')
    with open(armz, 'wb') as f:
        f.write(gzip.compress(bytes(image), mtime=0))
    tain = os.path.join(out, 'tAIN03e8.bin')
    with open(tain, 'wb') as f:
        f.write(b'TwGfx\0')
    stub_o = os.path.join(out, 'stub.o')
    stub = os.path.join(out, 'stub')
    run('m68k-palmos-gcc', '-O2', '-Wall', '-c', os.path.join(HERE, 'stub.c'), '-o', stub_o)
    run('m68k-palmos-gcc', stub_o, '-o', stub)
    prc = os.path.join(out, 'TwGfxTest.prc')
    run('build-prc', '-o', prc, '-n', 'TwGfxTest', '-c', 'FTGx', stub, armz, tain)
    # Fixed dates (2026-01-01, Palm seconds from 1904) so the PRC is the same from build to build. The card
    # view leaves out a database dated 0.
    stamp = (datetime.date(2026, 1, 1) - datetime.date(1904, 1, 1)).days * 86400
    with open(prc, 'r+b') as f:
        f.seek(36)
        f.write(struct.pack('>III', stamp, stamp, 0))
    print('%s: %d bytes of ARM code, %d relocations' % (prc, len(image), len(relocs)))


if __name__ == '__main__':
    main()
