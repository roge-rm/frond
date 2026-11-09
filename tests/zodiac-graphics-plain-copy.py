#!/usr/bin/env python3
"""Submit a firmware-free plain RGB565 copy through the real probe bus."""
from pathlib import Path
import os
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'build/zodiac-boot-probe'
with tempfile.TemporaryDirectory(prefix='zodiac-plain-copy-', dir=ROOT / 'build') as work:
    rom = bytearray(0x7d0000)
    for base in (0, 0x20000):
        struct.pack_into('<I', rom, base+8, 0xfeedbeef)
        rom[base+0x10:base+0x19] = b'PalmCard\0'
        struct.pack_into('<II', rom, base+0x80, 0x54707776, 0x4d583161)
    struct.pack_into('<I', rom, 0x20000+0x6c, len(rom)-0x20000)
    path = Path(work) / 'synthetic.rom'
    # A short synthetic guest programmes the native bus and submits through the
    # original plain-copy producer PC, through the probe submission dispatch.
    def word(offset, value): struct.pack_into('<I', rom, offset, value)
    def branch(source, target): word(source, 0xea000000 | (((target-source-8)//4)&0xffffff))
    operations = [(0xdf000,0x00220028,True,0x3e00,4),
                  (0xdf008,0x0022002c,True,0x11111e01,4)]
    # Reuse the focused setup fixture's native transactions, without its guard tests.
    setup = (ROOT/'tests/zodiac-graphics-setup.h').read_text()
    interface = setup[setup.index('inline void configureGraphicsInterface'):setup.index('inline void configureGraphicsExternal')]
    for pc, address, size, value in re.findall(r'(?:Write)?\{(0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*(\d+),\s*(0x[0-9a-f]+|0)\}',interface):
        pc, address, size, value = int(pc,16), int(address,16), int(size), int(value,0)
        operations.append((pc-0x20000000,address,address != 0x16000010,value,size))
    assert len(operations) == 8
    operations += [(0xb31b0,0x160101a4,True,0x7800,4),(0xb31bc,0x16010180,True,0x100007,4)]
    external = setup[setup.index('inline void configureGraphicsExternal'):setup.index('inline void configureGraphicsController')]
    for pc, offset, value, write in re.findall(r'(?:Operation)?\{(0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*(true|false)\}',external):
        operations.append((int(pc,16)-0x20000000,0x16010000+int(offset,16),write == 'true',int(value,16),4))
    assert len(operations) == 39
    operations += [(0xc6888,0x1601019c,True,0x7bbb7444,4)]
    for pc, address, write, value in [
        (0xacb98,0x160113e8,False,0),(0xacba8,0x160113e8,True,0x400),
        (0xacbb4,0x16010144,True,0x2440),(0xacbc4,0x160110a8,True,0x1fff1fff),
        (0xacbd0,0x160104dc,True,0xc0000),(0xb8e80,0x16010214,True,0x10150000),
        (0xb8e88,0x16010220,True,0),(0xb8f04,0x16010008,True,0x04150000),
        (0xb8f10,0x16010230,True,0x80000000),(0xb8f20,0x16010210,True,0x0800000d)]:
        operations.append((pc,address,write,value,4))
    # Two rows use pitch5, source X1, destination X2/Y1 and pitch7.
    for index, value in enumerate([0x22221111,0x44443333,0x66665555,0x88887777,0xaaaa9999]):
        operations.append((0xed000+index*8,0x16800100+index*4,True,value,4))
    registers = {0x4a1:0,0x41b:0x5598b4df,0x41f:0xffff,0x472:0x3f,0x4b2:0x00cc0a00,
        0x4b1:0x40030d04,0x490:0xffffffff,0x491:0,0x46f:0,0x470:0x00080007,
        0x401:0x800000,0x402:7,0x46b:0x800100,0x46c:5,0x41e:0,0x488:4,
        0x484:0xf0f0f,0x471:0x1fff1fff,0x423:0x100000,0x41d:0,0x4b3:0xffffffff,
        0x30f:0x20,0x42e:0,0x42f:0,0x430:0,0x431:0,0x432:0,0x433:0,0x4a0:0,0x48f:0xffffffff}
    packet = [value for pair in registers.items() for value in pair]+[0xc0021b00,0x00010000,0x00020001,0x00030002]
    for index, value in enumerate(packet): operations.append((0xef000+index*8,0x16150000+index*4,True,value,4))
    operations += [(0xa9ed4,0x16010220,True,len(packet),4),
                   (0xb8c64,0x1601021c,False,0,4),(0xb8c70,0x16010220,False,0,4),
                   (0xb8cb0,0x16010140,False,0,4),(0xe2000,0x16800010,False,0,4),
                   (0xe2008,0x16800014,False,0,4),(0xe2010,0x16800020,False,0,4),
                   (0xe2018,0x16800024,False,0,4),(0xb8c64,0x1601021c,False,0,4)]
    branch(0,0xd0000)
    for index, (pc,address,write,value,size) in enumerate(operations):
        scratch = 0xd0000+index*0x40
        word(scratch,0xe59f0010); word(scratch+4,0xe59f6010); word(scratch+8,0xe59fe010)
        branch(scratch+12,pc)
        word(scratch+24,address); word(scratch+28,value)
        word(scratch+32,0x20000000+scratch+0x40 if index+1<len(operations) else 0x200e3000)
        word(pc,(0xe5c06000 if write else 0xe5d06000) if size == 1 else (0xe5806000 if write else 0xe5906000))
        word(pc+4,0xe12fff1e)
        if pc == 0xc4364:
            word(scratch+32,0x200e4000)
            for wait in range(64): word(0xe4000+wait*4,0xe1a00000)
            branch(0xe4100,scratch+0x40)
    branch(0xe3000,0xe3000)
    path.write_bytes(rom)
    result = subprocess.run([str(PROBE),str(path),'linked','2000','exact','--bootstrap-peripherals',
        '--graphics-profile=virtual5744-compatible-8mib','--trace=peripherals'],
        text=True,capture_output=True,timeout=15)
    output = result.stdout+result.stderr
    if evidence := os.environ.get('FROND_ZODIAC_PLAIN_PROBE_EVIDENCE'):
        (Path(evidence)/'probe-plain-copy.log').write_text(output)
    assert result.returncode == 2 and 'stop=instruction-budget' in output, output
    # All six copied halfwords, neighbouring untouched halfwords and completed RPTR/busy.
    for pc,address,value in [(0xa9ed4,0x16010220,len(packet)),(0xb8c64,0x1601021c,len(packet)),
                             (0xb8c70,0x16010220,len(packet)),(0xb8cb0,0x16010140,0),
                             (0xe2000,0x16800010,0x22220000),(0xe2008,0x16800014,0x44443333),
                             (0xe2010,0x16800020,0x88887777),(0xe2018,0x16800024,0x00009999)]:
        assert re.search(r'bus .*pc=20%06x .*operation=(?:write|read) address=%08x size=4 value=%08x .*status=ok' % (pc,address,value),output),output
print('PASS: plain copy real probe bus native producer/prepare/start, asymmetric geometry and completed RPTR/busy')
