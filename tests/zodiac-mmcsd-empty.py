#!/usr/bin/env python3
"""Run command discovery and IRQ35 through a generated guest and the real probe."""
from pathlib import Path
import os
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='zodiac-mmcsd-', dir=ROOT/'build') as work:
    rom = bytearray(0x7d0000)
    def word(at, value): struct.pack_into('<I', rom, at, value)
    def half(at, value): struct.pack_into('<H', rom, at, value)
    def branch(at, target): word(at, 0xea000000 | (((target-at-8)//4)&0xffffff))
    for at in (0,0x20000):
        word(at+8,0xfeedbeef); rom[at+0x10:at+0x19] = b'PalmCard\0'
        word(at+0x80,0x54707776); word(at+0x84,0x4d583161)
    word(0x2006c,len(rom)-0x20000)
    branch(0,0xd0000); branch(0x100018,0x10e000)
    at = 0xd0000
    def arm(value):
        global at
        word(at,value); at += 4
    def load(reg,value):
        arm(0xe59f0000 | (reg<<12)); arm(0xea000000); arm(value)
    def store(address,value):
        load(0,address); load(1,value); arm(0xe5801000)
    # Both the native MMIO alias and low exception vectors use section mappings.
    for section,physical in [(0,0x201),(2,2),(3,3),(0x800,2)]+[(i,i) for i in range(0x200,0x208)]:
        store(0x00304000+section*4,(physical<<20)|0xc02)
    load(0,0x00304000); arm(0xee020f10)
    load(0,1); arm(0xee030f10)
    load(0,0x79); arm(0xee010f10)
    arm(0xe321f0d2); load(13,0x0031e000)
    arm(0xe321f0d3); load(13,0x0031d000)
    load(9,0x00300000)
    store(0x0030000c,0x00300100); store(0x00300148,0x00300200)
    store(0x00300250,1)
    store(0x00210000,0xffffc400); store(0x00210004,0xfffffbff)
    for value in [8,13]+[5]*8: store(0x00214000,value)
    for offset,value in [(8,0x36),(0x10,0xff),(0x24,0x78)]: store(0x00214000+offset,value)
    store(0x00223008,35)
    arm(0xe321f013)
    load(12,0x204c635d); arm(0xe1a0e00f); arm(0xe12fff1c)
    load(1,0x00300222); arm(0xe5d17000)
    load(1,0x00300300); arm(0xe5918000)
    load(1,0x00300223); arm(0xe5d1a000)
    branch(at,at)
    # Preserve interrupted registers across the native-shaped Thumb callback.
    at = 0x10e000
    arm(0xe92d500f); load(12,0x204c5c55); arm(0xe1a0e00f); arm(0xe12fff1c)
    load(0,0x00300300); arm(0xe5901000); arm(0xe2811001); arm(0xe5801000)
    arm(0xe8bd500f); arm(0xe25ef004)
    def thumb(start, instructions):
        for index,value in enumerate(instructions): half(start+index*2,value)
    def literal(pc,reg,target): half(pc,0x4800 | (reg<<8) | ((target-((pc+4)&~3))//4))
    def bl(pc,target):
        delta=target-pc-4; half(pc,0xf000|((delta>>12)&0x7ff)); half(pc+2,0xf800|((delta>>1)&0x7ff))
    # Firmware-free command-only clock helper and IRQ callback.
    thumb(0x4c5dc4,[0x4900,0x6008,0x2805,0xd003,0x684b,0x05db,0xd5fc,0x4770,
                     0x684b,0x05db,0xd4fc,0x4770])
    literal(0x4c5dc4,1,0x4c5f30)
    thumb(0x4c5c54,[0x4900,0x2078,0x6248,0x4900,0x2000,0x7008,0x4770])
    literal(0x4c5c54,1,0x4c5d90); literal(0x4c5c5a,1,0x4c5d88)
    word(0x4c5d88,0x00300250); word(0x4c5d90,0x80014000)
    # The wait stub uses the callback's guest flag, then reads controller status.
    thumb(0x4c5df6,[0x4900,0x7808,0x2801,0xd0fc,0x2001,0x7008,0x4900,0x6848,
                     0x2202,0x4010,0x4770])
    literal(0x4c5df6,1,0x4c5f34); literal(0x4c5e02,1,0x4c5f30)
    word(0x4c5f34,0x00300250); word(0x4c5f30,0x80014000)
    pc = 0x4c635c
    def emit(value):
        global pc
        half(pc,value); pc += 2
    def call(target):
        global pc
        bl(pc,target); pc += 4
    emit(0xb500)
    literal(pc,6,0x4c6660); pc += 2
    for command,format_value in [(0,0x80),(55,1),(1,3),(1,3)]:
        emit(0x2005); call(0x4c5dc4)
        emit(0x2000|command); emit(0x62b0)
        emit(0x2000); emit(0x62f0); emit(0x6330)
        emit(0x2000|format_value); emit(0x60f0)
        emit(0x2006); call(0x4c5dc4); call(0x4c5df6)
    literal(pc,1,0x4c6668); pc += 2
    emit(0x2004); emit(0x7008); emit(0xbc08); emit(0x4718)
    word(0x4c6660,0x80014000); word(0x4c6668,0x00300222)
    # Private verification can replace complete helpers and their original literals.
    original = os.environ.get('FROND_ZODIAC_MMCSD_ROM')
    if original:
        source = Path(original).read_bytes()
        for lo,hi in [(0x4c635c,0x4c645c),(0x4c6272,0x4c62d6),
                      (0x4c5dc4,0x4c5df6),(0x4c5c54,0x4c5c86),
                      (0x4c5d8c,0x4c5d94),(0x4c665c,0x4c6668),(0x4c6170,0x4c6174)]:
            rom[lo:hi] = source[lo:hi]
    path=Path(work)/'generated.rom'; path.write_bytes(rom)
    result=subprocess.run([str(ROOT/'build/zodiac-boot-probe'),str(path),'linked','500000','exact',
        '--bootstrap-peripherals','--imx1-mmcsd-empty-slot','--trace=peripherals'],
        capture_output=True,text=True,timeout=30)
    output=result.stdout+result.stderr
    if evidence := os.environ.get('FROND_ZODIAC_MMCSD_EVIDENCE'): Path(evidence).write_text(output)
    assert result.returncode == 2 and 'stop=instruction-budget' in output,output[-8000:]
    commands=re.findall(r'operation=write address=00214028 size=4 value=([0-9a-f]+)',output)
    assert commands == ['00000000','00000037','00000001','00000001'],commands
    assert re.search(r'irq-entries=4 fiq-entries=0',output),output[-5000:]
    assert 'r7=00000004' in output and 'r8=00000004' in output,output[-5000:]
    if original: assert 'r10=00000001' in output,output[-5000:]
    assert 'value=00002100 register=MMCSD.STATUS status=ok' in output
    assert output.count('value=00002102 register=MMCSD.STATUS status=ok') == 3
    assert not re.search(r'register=MMCSD.(RES_FIFO|BUFFER_ACCESS)',output)
    assert len(re.findall(r'pc=204c5c58 operation=write address=00214024',output)) == 4
print('PASS: timed empty-slot CMD0, CMD55 and two CMD1 timeouts with guest IRQ35 acknowledgement')
