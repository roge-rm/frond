#!/usr/bin/env python3
"""Generated asymmetric display through persistent machine frame boundaries."""
from pathlib import Path
import hashlib
import os
import re
import struct
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
PROBE=ROOT/os.environ.get('FROND_ZODIAC_PROBE','build/test-zodiac-machine-cli')
with tempfile.TemporaryDirectory(prefix='zodiac-machine-frame-',dir=ROOT/'build') as work:
    rom=bytearray(0x7d0000)
    def word(at,value):struct.pack_into('<I',rom,at,value)
    def branch(at,target):word(at,0xea000000|(((target-at-8)//4)&0xffffff))
    for at in (0,0x20000):
        word(at+8,0xfeedbeef);rom[at+0x10:at+0x19]=b'PalmCard\0'
        word(at+0x80,0x54707776);word(at+0x84,0x4d583161)
    word(0x2006c,len(rom)-0x20000)
    ops=[(0xdf000,0x00220028,True,0x3e00,4),(0xdf008,0x0022002c,True,0x11111e01,4)]
    setup=(ROOT/'tests/zodiac-graphics-setup.h').read_text()
    interface=setup[setup.index('inline void configureGraphicsInterface'):setup.index('inline void configureGraphicsExternal')]
    for pc,address,size,value in re.findall(r'(?:Write)?\{(0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*(\d+),\s*(0x[0-9a-f]+|0)\}',interface):
        pc,address,size,value=int(pc,16),int(address,16),int(size),int(value,0)
        ops.append((pc-0x20000000,address,address!=0x16000010,value,size))
    ops += [(0xb31b0,0x160101a4,True,0x7800,4),(0xb31bc,0x16010180,True,0x100007,4)]
    external=setup[setup.index('inline void configureGraphicsExternal'):setup.index('inline void configureGraphicsController')]
    for pc,offset,value,write in re.findall(r'(?:Operation)?\{(0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*(true|false)\}',external):
        ops.append((int(pc,16)-0x20000000,0x16010000+int(offset,16),write=='true',int(value,16),4))
    ops.append((0xc6888,0x1601019c,True,0x7bbb7444,4))
    for pc,address,write,value in [(0xacb98,0x160113e8,False,0),(0xacba8,0x160113e8,True,0x400),
        (0xacbb4,0x16010144,True,0x2440),(0xacbc4,0x160110a8,True,0x1fff1fff),
        (0xacbd0,0x160104dc,True,0xc0000),(0xb8e80,0x16010214,True,0x10150000),
        (0xb8e88,0x16010220,True,0),(0xb8f04,0x16010008,True,0x04150000),
        (0xb8f10,0x16010230,True,0x80000000),(0xb8f20,0x16010210,True,0x0800000d)]:
        ops.append((pc,address,write,value,4))
    for pc,address,value in [(0xe6000,0x16800000,0x07e0f800),(0xe6008,0x16800004,0x0000001f),
                             (0xe6010,0x16800008,0x0000ffff),(0xe6018,0x1680000c,0x0000ffe0)]:
        ops.append((pc,address,True,value,4))
    # Same admitted 3x2 display mode as the peripheral fixture, real guest stores.
    def display(pc,offset,value):ops.append((pc,0x16010000+offset,True,value,4))
    display(0xaeb68,0x4d8,0x78)
    for pc,offset,value in [(0xad720,0x418,0x800000),(0xad72c,0x41c,8),(0xad738,0x414,6)]:display(pc,offset,value)
    display(0xaeb68,0x4d8,0x7b)
    display(0xaeb68,0x4d8,0x78)
    for pc,offset,value in [(0xad65c,0x42c,0x40001),(0xad668,0x430,0x30001),
        (0xad2e0,0x410,3),(0xad404,0x540,0x80604020),(0xad410,0x544,0xffdfbf9f),(0xad418,0x548,0),
        (0xad420,0x578,0x80604020),(0xad428,0x58c,0xffdfbf9f),(0xad430,0x590,0),
        (0xad438,0x594,0x80604020),(0xad440,0x598,0xffdfbf9f),(0xad448,0x59c,0),
        (0xad528,0x538,0x800000),(0xad3b8,0x524,0x102aa),(0xad2ec,0x414,0x1800406),
        (0xad2f8,0x420,0x60008),(0xad304,0x424,0x40001),(0xad310,0x428,0x30001)]:display(pc,offset,value)
    display(0xaeb68,0x4d8,0x7b)
    display(0xaeb68,0x4d8,0x78);display(0xad8b4,0x414,0x1801c06);display(0xaeb68,0x4d8,0x7b)
    branch(0,0xd0000)
    for i,(pc,address,write,value,size) in enumerate(ops):
        scratch=0xd0000+i*0x200
        word(scratch,0xe59f0010);word(scratch+4,0xe59f6010);word(scratch+8,0xe59fe010)
        branch(scratch+12,pc)
        word(scratch+24,address);word(scratch+28,value);word(scratch+32,0x20000000+scratch+0x40)
        word(pc,(0xe5c06000 if write else 0xe5d06000) if size==1 else (0xe5806000 if write else 0xe5906000))
        word(pc+4,0xe12fff1e)
        for wait in range(64):word(scratch+0x40+wait*4,0xe1a00000)
        branch(scratch+0x140,0xd0000+(i+1)*0x200 if i+1<len(ops) else 0xf0000)
    branch(0xf0000,0xf0000)
    path=Path(work)/'display.rom';path.write_bytes(rom)
    for engine in ('exact','fast'):
        frame=Path(work)/(engine+'.ppm')
        command=[str(PROBE),str(path),'linked','15000',engine,'--bootstrap-peripherals',
            '--graphics-profile=virtual5744-compatible-8mib','--graphics-raw-mode1=712',
            '--asp-u-sample=0x9b00','--asp-px1-input=high','--trace=peripherals','--frame-output='+str(frame),
            '--diagnostic-snapshot='+str(Path(work)/engine)]
        result=subprocess.run(command,capture_output=True,text=True,timeout=30)
        assert result.returncode==2 and 'stop=instruction-budget' in result.stdout,result.stdout+result.stderr
        assert 'graphics-frame lcd=1 width=3 height=2' in result.stdout,result.stdout
        if baseline := os.environ.get('FROND_ZODIAC_BASELINE_PROBE'):
            frozen={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in Path(work).glob(engine+'.*')}
            old=subprocess.run([baseline]+command[1:],capture_output=True,text=True,timeout=30)
            assert (old.returncode,old.stdout,old.stderr)==(result.returncode,result.stdout,result.stderr)
            assert frozen=={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in Path(work).glob(engine+'.*')}
        assert frame.read_bytes()==b'P6\n3 2\n255\n'+bytes([255,0,0,0,255,0,0,0,255,255,255,255,0,0,0,255,255,0]),frame.read_bytes()
print('PASS: persistent Exact/Fast rendered frame acquire/ack/peek and asymmetric pixels')
