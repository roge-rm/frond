#!/usr/bin/env python3
"""ACAL reference and normal ZXYU acquisition through the MMU and original-shaped helpers."""
from pathlib import Path
import os
import hashlib
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='zodiac-asp-pen-', dir=ROOT/'build') as work:
    rom = bytearray(0x7d0000)
    def word(at, value): struct.pack_into('<I', rom, at, value)
    def half(at, value): struct.pack_into('<H', rom, at, value)
    def branch(at, target): word(at, 0xea000000 | (((target-at-8)//4)&0xffffff))
    for at in (0, 0x20000):
        word(at+8, 0xfeedbeef); rom[at+0x10:at+0x19] = b'PalmCard\0'
        word(at+0x80, 0x54707776); word(at+0x84, 0x4d583161)
    word(0x2006c, len(rom)-0x20000)
    branch(0, 0xd0000)
    at = 0xd0000
    def arm(value):
        global at
        word(at, value); at += 4
    def load(reg, value):
        arm(0xe59f0000 | (reg<<12)); arm(0xea000000); arm(value)
    def store(address, value):
        load(0, address); load(1, value); arm(0xe5801000)
    for section, physical in [(2, 2), (3, 3), (0x800, 2)]+[(i, i) for i in range(0x200, 0x208)]:
        store(0x00304000+section*4, (physical<<20)|0xc02)
    load(0, 0x00304000); arm(0xee020f10)
    load(0, 1); arm(0xee030f10)
    load(0, 0x79); arm(0xee010f10)
    load(13, 0x0031d000); load(9, 0x00300000)
    store(0x00300000, 0x00300100); store(0x00300100, 0x00300200)
    store(0x00304c38, 0x00307000); store(0x00307004, 0x00308000)
    store(0x8001502c, 3); store(0x80015014, 0x3814)
    load(0, 0x00309000); load(1, 0x00309004); load(2, 0x00309008)
    load(12, 0x20096d51); arm(0xe1a0e00f); arm(0xe12fff1c)
    load(6, 0x00309000); arm(0xe5967000); arm(0xe5968004); arm(0xe596a008)
    store(0x80015010, 0x0200e001); store(0x80015010, 0x0200e003)
    for pointer, output_register in [(4, 11), (8, 5), (0, 4)]:
        load(0, 0x80015000)
        # Fill twelve actual words, then overrun one group before each read.
        arm(0xe590101c); arm(0xe3110002); arm(0x0afffffc)
        arm(0xe5901034); arm(0xe201100f); arm(0xe3510000 | pointer); arm(0x1afffffb)
        load(0, 0x0030900c); load(12, 0x20096cd1); arm(0xe1a0e00f); arm(0xe12fff1c)
        load(6, 0x0030900c); arm(0xe5960000 | (output_register<<12))
    branch(at, at)
    # Firmware-free ACAL helper: three references, then reset and restore.
    pc = 0x96d50
    def emit(value):
        global pc
        half(pc, value); pc += 2
    def literal(reg, value):
        global pc
        target = (pc+8)&~3
        half(pc, 0x4800 | (reg<<8) | ((target-((pc+4)&~3))//4))
        half(pc+2, 0xe000 | ((target+4-pc-6)//2))
        word(target, value); pc = target+4
    emit(0xb570); emit(0x1c04); emit(0x1c0d); emit(0x1c16)
    literal(3, 0x80015000)
    for offset, value in [(0x10, 0x00800000), (0x2c, 3), (0x14, 0x3814),
                          (0x10, 0x0600d201), (0x10, 0x0600d203)]:
        literal(0, value); emit(0x6018 | (offset//4<<6))
    loop = pc
    emit(0x6b58); emit(0x210f); emit(0x4008); emit(0x2803)
    emit(0xd300 | (((loop-pc-4)//2)&255))
    for reg in (4, 5, 6): emit(0x6818); emit(0x09c0); emit(0x6000 | (reg<<3))
    for offset, value in [(0x10, 0x00800000), (0x2c, 3), (0x14, 0x3814), (0x10, 0x02000001)]:
        literal(0, value); emit(0x6018 | (offset//4<<6))
    emit(0xbc70); emit(0xbc08); emit(0x4718)
    assert pc < 0x96e50
    # Firmware-free normal consumer reads the full untagged ring and subtracts Z.
    pc = 0x96cd0
    emit(0xb530); emit(0x1c04); literal(3, 0x80015000)
    emit(0x681d); emit(0x6818); emit(0x1b40); emit(0x0400); emit(0x0dc0); emit(0x8020)
    emit(0x6818); emit(0x1b40); emit(0x0400); emit(0x0dc0); emit(0x8060)
    for _ in range(9): emit(0x6818)
    emit(0x2000); emit(0xbc30); emit(0xbc08); emit(0x4718)
    assert pc < 0x96d50
    # Private qualification replaces complete original bodies and their literal pool.
    if original := os.environ.get('FROND_ZODIAC_ASP_ROM'):
        source = Path(original).read_bytes()
        assert hashlib.sha256(source).hexdigest() == '2207a735e812299b7d1ede9da01c5e867bd932a1f15ce46b81c8737e42e9111d'
        for lo, hi in [(0x96cd0, 0x96d50), (0x96d50, 0x96dd0), (0x96dd0, 0x96dec)]:
            rom[lo:hi] = source[lo:hi]
    path = Path(work)/'generated.rom'; path.write_bytes(rom)
    result = subprocess.run([str(ROOT/'build/zodiac-asp-pen-probe'), str(path)],
                            capture_output=True, text=True, timeout=30)
    output = result.stdout+result.stderr
    if evidence := os.environ.get('FROND_ZODIAC_ASP_EVIDENCE'): Path(evidence).write_text(output)
    assert result.returncode == 0, output[-8000:]
    values = re.findall(r'address=00215000 write=0 value=([0-9a-f]+) status=0', output)
    assert values == ['00000180', '00003280', '00007100'] + ['00001100', '00005500', '0000a900', '00001234']*9, values
    assert all('r'+str(r)+'=01300088' in output for r in (11, 5, 4)), output[-3000:]
    if original:
        for pointer in (4, 8, 0):
            assert f'PTR pc=20096cf8 write-pointer={pointer}' in output, output[-6000:]
print('PASS: timed ACAL Z/X/Y references, reset/restore and three overrun rings of normal Z/X/Y/U words through the MMU')
