#!/usr/bin/env python3
"""Exercise the native GPIO14 read/modify/write PCs through the probe bus."""
from pathlib import Path
import os
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'build/zodiac-boot-probe'
with tempfile.TemporaryDirectory(prefix='zodiac-slot-gpio-', dir=ROOT / 'build') as work:
    rom = bytearray(0x7d0000)
    for base in (0, 0x20000):
        struct.pack_into('<I', rom, base+8, 0xfeedbeef)
        rom[base+0x10:base+0x19] = b'PalmCard\0'
        struct.pack_into('<II', rom, base+0x80, 0x54707776, 0x4d583161)
    struct.pack_into('<I', rom, 0x20000+0x6c, len(rom)-0x20000)
    def word(offset, value): struct.pack_into('<I', rom, offset, value)
    def half(offset, value): struct.pack_into('<H', rom, offset, value)
    def branch(source, target): word(source, 0xea000000 | (((target-source-8)//4)&0xffffff))
    operations = [(0xdf000,0x00220028,True,0x3e00,4),
                  (0xdf008,0x0022002c,True,0x11111e01,4),
                  (0xbab4c,0x16000010,False,0,1),
                  (0xbabf8,0x1600001c,True,0xe,1),
                  (0xb99c4,0x160104bc,True,0xffff1fff,4),
                  (0xb99f0,0x1601052c,True,0xffff8001,4),
                  (0xb9a14,0x160104b8,True,0x01ff0008,4),
                  (0xb9a34,0x16010528,True,0x80010001,4)]
    branch(0,0xd0000)
    for index,(pc,address,write,value,size) in enumerate(operations):
        scratch = 0xd0000+index*0x40
        word(scratch,0xe59f0010); word(scratch+4,0xe59f6010); word(scratch+8,0xe59fe010)
        branch(scratch+12,pc)
        word(scratch+24,address); word(scratch+28,value)
        word(scratch+32,0x20000000+scratch+0x40 if index+1<len(operations) else 0x200e0000)
        word(pc,(0xe5c06000 if write else 0xe5d06000) if size == 1 else (0xe5806000 if write else 0xe5906000))
        word(pc+4,0xe12fff1e)
    word(0xe0000,0xe59f2008); word(0xe0004,0xe59f0008); word(0xe0008,0xe12fff10)
    word(0xe0010,0x16010480); word(0xe0014,0x204d3211)
    # Firmware-free Thumb arithmetic preserves every bit except the GPIO14 pair.
    for pc in range(0x4d3210,0x4d3228,2): half(pc,0x46c0)
    for pc,instruction in [(0x4d3210,0x6b90),(0x4d3212,0x2101),(0x4d3214,0x0389),
                           (0x4d3216,0x4308),(0x4d3218,0x0409),(0x4d321a,0x4308),
                           (0x4d3226,0x6390)]: half(pc,instruction)
    if original := os.environ.get('FROND_ZODIAC_SLOT_ROM'):
        source = Path(original).read_bytes()
        rom[0x4d3210:0x4d3228] = source[0x4d3210:0x4d3228]
    half(0x4d3228,0xe7fe)
    path = Path(work)/'synthetic.rom'
    path.write_bytes(rom)
    result = subprocess.run([str(PROBE),str(path),'linked','200','exact','--bootstrap-peripherals',
        '--graphics-profile=virtual5744-compatible-8mib','--trace=peripherals'],
        text=True,capture_output=True,timeout=15)
    output = result.stdout+result.stderr
    if evidence := os.environ.get('FROND_ZODIAC_SLOT_EVIDENCE'):
        Path(evidence).write_text(output)
    assert result.returncode == 2 and 'stop=instruction-budget' in output, output
    for pc,value,operation in [(0x204d3210,0x01ff0008,'read'),(0x204d3226,0x41ff4008,'write')]:
        assert re.search(r'bus .*pc=%08x .*operation=%s address=160104b8 size=4 value=%08x .*status=ok' %
                         (pc,operation,value),output), output
print('PASS: native GPIO14 read and paired direction/latch store through exact probe bus')
