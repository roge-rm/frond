#!/usr/bin/env python3
"""Synthetic native graphics setup with an explicitly selected virtual profile."""
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'build/zodiac-boot-probe'

def main(bus_checks=None):
    with tempfile.TemporaryDirectory(prefix='zodiac-cs5-reset-', dir=ROOT / 'build') as work:
        rom = bytearray(0x7d0000)
        for base in (0, 0x20000):
            struct.pack_into('<I', rom, base + 8, 0xfeedbeef)
            rom[base + 0x10:base + 0x19] = b'PalmCard\0'
            struct.pack_into('<II', rom, base + 0x80, 0x54707776, 0x4d583161)
        struct.pack_into('<I', rom, 0x20000 + 0x6c, len(rom) - 0x20000)
        def word(offset, value): struct.pack_into('<I', rom, offset, value)
        def branch(source, target): word(source, 0xea000000 | (((target-source-8)//4) & 0xffffff))
        branch(0, 0x100)
        words = [0xe59f0038, 0xe59f1038, 0xe5801000, 0xe59f1034, 0xe5801004,
                 0xe59f1030]
        struct.pack_into('<6I', rom, 0x100, *words)
        branch(0x118, 0xbac84)
        struct.pack_into('<4I', rom, 0x140, 0x00220028, 0x3e00, 0x11111e01, 0x16000000)
        for offset in range(0xbac84, 0xbad08, 4): word(offset, 0xe1a00000)
        word(0xbac84, 0xe5d12018)
        word(0xbac88, 0xe3822008)
        word(0xbac9c, 0xe5c12018)
        word(0xbaca4, 0xe5d12018)
        word(0xbaca8, 0xe3c22008)
        branch(0xbacac, 0xbace8)
        word(0xbace8, 0xe5c12018)
        word(0xbacf0, 0xe5d12018)
        word(0xbacf4, 0xe5d13010)
        path = Path(work) / 'synthetic.rom'
        path.write_bytes(rom)
        def run(*options, code=1):
            p = subprocess.run([str(PROBE), str(path), 'linked', '100', 'exact', *options],
                               capture_output=True, text=True, timeout=15)
            assert p.returncode == code, (options, p.returncode, p.stdout, p.stderr)
            return p.stdout + p.stderr

        for budget in ('0','1000000001','4294967296'):
            p=subprocess.run([str(PROBE),str(path),'linked',budget,'exact'],capture_output=True,text=True,timeout=15)
            assert p.returncode==1 and 'Instruction budget must be 1..1000000000' in p.stderr,(budget,p.stdout,p.stderr)
        p=subprocess.run([str(PROBE),str(path),'linked','1000000000','exact'],capture_output=True,text=True,timeout=15)
        assert p.returncode==2 and 'budget=1000000000' in p.stdout and 'stop=unsupported-write step=3' in p.stdout,(p.stdout,p.stderr)
        print('PASS: 1B budget admission and early generated MMIO guard at step 3')
        profile = '--graphics-profile=virtual5744-compatible-8mib'
        bootstrap = '--bootstrap-peripherals'
        assert 'Graphics profile requires' in run(profile)
        assert 'Graphics profile requires' in run(bootstrap, profile, '--cs5-reset-status-input=0')
        assert 'Graphics profile requires' in run(bootstrap, '--cs5-reset-status-input=0', profile)
        for bad in ('', 'strict', '5744', 'virtual5744-compatible-8mibx'):
            assert 'Supply --graphics-profile=' in run(bootstrap, '--graphics-profile='+bad)
        assert 'Supply --graphics-profile=' in run(bootstrap, profile, profile)
        branch(0xbacf4, 0xbab4c)
        word(0xbab4c, 0xe5d12010)
        word(0xbab50, 0xe3a0200e)
        branch(0xbab54, 0xbabf8)
        word(0xbabf8, 0xe5c1201c)
        word(0xbabfc, 0xe2811801)
        word(0xbac00, 0xe5913000)
        branch(0xbac04, 0xbac28)
        word(0xbac28, 0xe5914004)
        branch(0xbac2c, 0xbac2c)
        path.write_bytes(rom)
        for engine in ('exact', 'fast'):
            p = subprocess.run([str(PROBE), str(path), 'linked', '100', engine, bootstrap, profile],
                               capture_output=True, text=True, timeout=15)
            assert p.returncode == 2, (engine, p.stdout, p.stderr)
            output = p.stdout + p.stderr
            assert 'r3=57441002' in output and 'r4=00000000' in output, output
            assert 'address=16010000 size=4 value=57441002' in output
            assert 'address=16010004 size=4 value=00000000' in output
            assert 'source=virtual5744-compatible-8mib' in output
            assert 'identity=57441002 revision=00000000 capacity=8388608 are model choices' in output
        # Native GPIO setup PCs use two banks, then the next controller stays guarded.
        word(0xbac2c, 0xe59f6004)
        branch(0xbac30, 0xb99c4)
        word(0xbac38, 0xffff1fff)
        word(0xb99c4, 0xe58164bc)
        word(0xb99c8, 0xe59f6000)
        branch(0xb99cc, 0xb99f0)
        word(0xb99d0, 0xffff0001)
        word(0xb99f0, 0xe581652c)
        word(0xb99f4, 0xe59f6000)
        branch(0xb99f8, 0xb9a14)
        word(0xb99fc, 0x01ff0008)
        word(0xb9a14, 0xe58164b8)
        word(0xb9a18, 0xe59f6000)
        branch(0xb9a1c, 0xb9a34)
        word(0xb9a20, 0x00010001)
        word(0xb9a34, 0xe5816528)
        branch(0xb9a38, 0xc45e4)
        word(0xc45e4, 0xe5816080)
        path.write_bytes(rom)
        for engine in ('exact', 'fast'):
            p = subprocess.run([str(PROBE), str(path), 'linked', '100', engine, bootstrap, profile],
                               capture_output=True, text=True, timeout=15)
            assert p.returncode == 2, (engine, p.stdout, p.stderr)
            output = p.stdout + p.stderr
            for address, value in (('160104bc', 'ffff1fff'), ('1601052c', 'ffff0001'),
                                   ('160104b8', '01ff0008'), ('16010528', '00010001')):
                assert re.search(r'cs5-configuration .*address=' + address + r' size=4 value=' + value +
                                 r' defaults-known=1 status=ok source=virtual5744-compatible-8mib', output), output
            assert 'address=16010080 size=4 value=00010001' in output, output
            assert 'peripheral-status=unsupported-sdram-sequence' in output, output
        # Exercise the probe's direct VRAM bus path after native window setup.
        word(0xb9a38, 0xe59f6000)
        branch(0xb9a3c, 0xc50bc)
        word(0xb9a40, 0x0c000000)
        word(0xc50bc, 0xe5816018)
        word(0xc50c0, 0xe59f0008)
        word(0xc50c4, 0xe3a06011)
        branch(0xc50c8, 0xc50dc)
        word(0xc50d0, 0x16000000)
        word(0xc50dc, 0xe5c06014)
        word(0xc50e0, 0xe59f6000)
        branch(0xc50e4, 0xc50f4)
        word(0xc50e8, 0x15ff1000)
        word(0xc50f4, 0xe5816188)
        branch(0xc50f8, 0xc50fc)
        word(0xc50fc, 0xe5917188)
        word(0xc5100, 0xe59f6000)
        branch(0xc5104, 0xc5130)
        word(0xc5108, 0xffff8000)
        word(0xc5130, 0xe5816190)
        branch(0xc5134, 0xc5138)
        word(0xc5138, 0xe5918190)
        word(0xc513c, 0xe59f6000)
        branch(0xc5140, 0xb31b0)
        word(0xc5144, 0x7800)
        word(0xb31b0, 0xe58161a4)
        word(0xb31b4, 0xe59f600c)
        branch(0xb31b8, 0xb31bc)
        word(0xb31bc, 0xe5816180)
        branch(0xb31c0, 0xc4114)
        word(0xb31c8, 0x100007)
        # Native external-memory RMW and mode commands, with guest cycles between writes.
        external_ops = [
            (0xc4114, 0x1b8, False, 0x0),
            (0xc4130, 0x1b0, False, 0x0),
            (0xc413c, 0x1b0, True, 0xff0000),
            (0xc4144, 0x194, False, 0x0),
            (0xc4150, 0x194, True, 0xff000000),
            (0xc4158, 0x18c, False, 0x0),
            (0xc4164, 0x18c, True, 0x80000),
            (0xc4178, 0x18c, False, 0x80000),
            (0xc4188, 0x18c, True, 0x70080000),
            (0xc4190, 0x18c, False, 0x70080000),
            (0xc4198, 0x18c, True, 0x70080001),
            (0xc41c4, 0x1b0, False, 0xff0000),
            (0xc41d4, 0x1b0, True, 0x0),
            (0xc41e8, 0x194, False, 0xff000000),
            (0xc4220, 0x194, True, 0x15001545),
            (0xc4234, 0x18c, False, 0x70080001),
            (0xc423c, 0x18c, True, 0x70000001),
            (0xc4250, 0x198, False, 0x0),
            (0xc4264, 0x18c, False, 0x70000001),
            (0xc42c0, 0x18c, True, 0x7000000d),
            (0xc42d4, 0x18c, False, 0x7000000d),
            (0xc42e4, 0x18c, True, 0x7964000d),
            (0xc42f8, 0x18c, False, 0x7964000d),
            (0xc4304, 0x18c, True, 0x964000d),
            (0xc4318, 0x18c, False, 0x964000d),
            (0xc4320, 0x18c, True, 0x964000d),
            (0xc4334, 0x198, True, 0x1600021),
            (0xc434c, 0x198, True, 0x81600021),
            (0xc4364, 0x198, True, 0x1600021),
        ]
        for index, (pc, offset, write, value) in enumerate(external_ops):
            word(pc, (0xe5816000 if write else 0xe5916000) | offset)
            scratch = 0xd1000 + index * 0x100
            branch(pc + 4, scratch)
            for offset in range(0, 0x80, 4): word(scratch + offset, 0xe1a00000)
            following = external_ops[index+1] if index+1 < len(external_ops) else (0xc6888, 0x19c, True, 0x7bbb7444)
            word(scratch + 0x80, 0xe59f6000)
            branch(scratch + 0x84, following[0])
            word(scratch + 0x88, following[3])
        word(0xc6888, 0xe581619c)
        branch(0xc688c, 0xd0000)
        for offset, instruction in enumerate((0xe59f001c, 0xe59f601c, 0xe5806000,
                0xe5d09001, 0xe1d0a0b2, 0xe590b000)):
            word(0xd0000 + offset * 4, instruction)
        branch(0xd0018, 0xc45e4)
        word(0xd0024, 0x16800000)
        word(0xd0028, 0x89abcdef)
        path.write_bytes(rom)
        for engine in ('exact', 'fast'):
            p = subprocess.run([str(PROBE), str(path), 'linked', '3000', engine, bootstrap, profile],
                               capture_output=True, text=True, timeout=15)
            output = p.stdout + p.stderr
            assert p.returncode == 2, (engine, output)
            assert 'r8=ffff8000 r9=000000cd r10=000089ab r11=89abcdef' in output, output
            assert re.search(r'cs5-configuration .*address=16800000 size=4 value=89abcdef .*status=ok', output), output
            assert 'address=16010080 size=4' in output and 'peripheral-status=unsupported-sdram-sequence' in output, output
        # Empty command-ring setup does not admit any producer or fetch.
        engine_ops = [
            (0xacb98, 0x160113e8, False, 0),
            (0xacba8, 0x160113e8, True, 0x400),
            (0xacbb4, 0x16010144, True, 0x2440),
            (0xacbc4, 0x160110a8, True, 0x1fff1fff),
            (0xacbd0, 0x160104dc, True, 0xc0000),
            (0xb8e80, 0x16010214, True, 0x100000),
            (0xb8e88, 0x16010220, True, 0),
            (0xb8f04, 0x16010008, True, 0x02100000),
            (0xb8f10, 0x16010230, True, 0x80000000),
            (0xb8f20, 0x16010210, True, 0x0800000b),
            (0xb02ac, 0x16011280, False, 0),
            (0xb02fc, 0x16011280, True, 0x10000),
        ]
        branch(0xd0018, 0xd4000)
        for index, (pc, address, write, value) in enumerate(engine_ops):
            scratch = 0xd4000 + index * 0x40
            word(scratch, 0xe59f0010)
            word(scratch + 4, 0xe59f6010)
            branch(scratch + 8, pc)
            word(scratch + 0x18, address)
            word(scratch + 0x1c, value)
            word(pc, 0xe5806000 if write else 0xe5906000)
            branch(pc + 4, scratch + 0x40 if index + 1 < len(engine_ops) else 0xd5000)
        display_ops = [
            (0xd8000,0x16800000,True,0x07e0f800),
            (0xd8010,0x16800004,True,0xdead001f),
            (0xd8020,0x16800008,True,0x0000ffff),
            (0xd8030,0x1680000c,True,0xbeef8410),
            (0xaeb68,0x160104d8,True,0x78),
            (0xad720,0x16010418,True,0x800000),
            (0xad72c,0x1601041c,True,8),
            (0xad738,0x16010414,True,6),
            (0xaeb68,0x160104d8,True,0x7b),
        ] + [(0xad74c,0x160104d8,False,0)] * 10 + [
            (0xaeb68,0x160104d8,True,0x78),
            (0xad65c,0x1601042c,True,0x40001),
            (0xad668,0x16010430,True,0x30001),
            (0xad2e0,0x16010410,True,3),
            (0xad404,0x16010540,True,0x80604020),
            (0xad410,0x16010544,True,0xffdfbf9f),
            (0xad418,0x16010548,True,0),
            (0xad420,0x16010578,True,0x80604020),
            (0xad428,0x1601058c,True,0xffdfbf9f),
            (0xad430,0x16010590,True,0),
            (0xad438,0x16010594,True,0x80604020),
            (0xad440,0x16010598,True,0xffdfbf9f),
            (0xad448,0x1601059c,True,0),
            (0xad528,0x16010538,True,0x800000),
            (0xad2ec,0x16010414,True,0x1800406),
            (0xad2f8,0x16010420,True,0x60008),
            (0xad304,0x16010424,True,0x40001),
            (0xad310,0x16010428,True,0x30001),
            (0xaeb68,0x160104d8,True,0x7b),
        ] + [(0xad80c,0x16010414,False,0)] * 10 + [
            (0xaeb68,0x160104d8,True,0x78),
            (0xad8b4,0x16010414,True,0x1801c06),
            (0xaeb68,0x160104d8,True,0x7b),
        ] + [(0xad74c,0x160104d8,False,0)] * 10 + [
            (0xb9cd4,0x160104c0,False,12),
            (0xb9cd4,0x160104c0,False,3),
        ]
        branch(0xd5000,0xd6000)
        for index, (pc,address,write,value) in enumerate(display_ops):
            scratch = 0xd6000 + index * 0x40
            word(scratch,0xe59f0010)
            word(scratch+4,0xe59f6010)
            word(scratch+8,0xe59fe010)
            branch(scratch+12,pc)
            word(scratch+0x18,address)
            word(scratch+0x1c,value)
            word(scratch+0x20,scratch+0x40 if index+1<len(display_ops) else 0x200d9000)
            # Linked entry addresses are 0x20000000 plus file offsets.
            if index+1<len(display_ops): word(scratch+0x20,0x20000000+scratch+0x40)
            word(pc,0xe5806000 if write else 0xe5906000)
            word(pc+4,0xe12fff1e)
            if pc == 0xb9cd4:
                word(scratch,0xe59f1018)
                word(scratch+4,0xe3a00000 | value)
                word(scratch+8,0xe3a07001)
                word(scratch+12,0xe59fc010)
                word(scratch+16,0xe59fe010)
                branch(scratch+20,pc)
                word(scratch+32,0x16010000)
                word(scratch+36,0x4c0)
                word(scratch+40,0x20000000+scratch+0x40 if index+1<len(display_ops) else 0x200d9000)
                word(pc,0xe791100c)
                word(pc+4,0xe1a00017)
                word(pc+8,0xe1110000)
                word(pc+12,0xe12fff1e)
        branch(0xd9000,0xd9000)
        path.write_bytes(rom)
        expected = 2166136261
        for pixel in (0xff0000ff,0xff00ff00,0xffff0000,0xffffffff,0xff000000,0xff848284):
            for lane in struct.pack('<I',pixel): expected = ((expected ^ lane) * 16777619) & 0xffffffff
        outputs = []
        for engine in ('exact', 'fast'):
            p = subprocess.run([str(PROBE), str(path), 'linked', '3000', engine, bootstrap, profile],
                               capture_output=True, text=True, timeout=15)
            output = p.stdout + p.stderr
            assert p.returncode == 2, (engine, output)
            for _, address, write, value in engine_ops:
                assert re.search(r'cs5-configuration .*operation=' + ('write' if write else 'read') +
                    r' address=%08x size=4 value=%08x defaults-known=1 status=ok' % (address, value), output), output
            assert 'stop=instruction-budget' in output, output
            assert 'graphics-frame lcd=1 width=3 height=2 fnv1a=%08x' % expected in output, output
            assert 'address=160104d8 size=4 value=0000007b' in output, output
            assert 'address=160104d8 size=4 value=00000079' in output, output
            assert 'address=16010414 size=4 value=07800406' in output, output
            assert re.search(r'address=160104c0 .*value=00000000 .*status=ok .*known-mask=00001000 single-pin-query',output),output
            assert re.search(r'address=160104c0 .*value=00000008 .*status=ok .*known-mask=00000008 single-pin-query',output),output
            normalised = re.sub(r'engine=(exact|fast)', 'engine=checked',output)
            outputs.append(re.sub(r'^jit-blocks=.*\n','',normalised,flags=re.M))
        assert outputs[0] == outputs[1], 'Exact/Fast synthetic display traces differ'
        # A second actual-bus scenario programs and samples the independent clock block.
        clock_ops = [(0xc45e4,0x16010080,True,0x3f),
            (0xc49ac,0x16010084,True,0x01100603),
            (0xc47b0,0x1601008c,True,0x300),
            (0xc4920,0x16010090,True,0x20),
            (0xc5930,0x16010088,True,0x4bd00240),
            (0xc5824,0x16010094,True,0x201),
            (0xc5850,0x16010094,True,1),
            (0xc587c,0x16010094,True,0x101),
            (0xc588c,0x16010094,False,0),
            (0xc58c4,0x16010094,True,1),
            (0xc5ca8,0x16010088,True,0x4b000200),
            (0xc55c0,0x1601008c,True,0x301)]
        branch(0xd9000,0xda000)
        for index,(pc,address,write,value) in enumerate(clock_ops):
            scratch=0xda000+index*0x40
            word(scratch,0xe59f0010);word(scratch+4,0xe59f6010);word(scratch+8,0xe59fe010)
            branch(scratch+12,pc);word(scratch+0x18,address);word(scratch+0x1c,value)
            word(scratch+0x20,0x20000000+scratch+0x40 if index+1<len(clock_ops) else 0x200db000)
            word(pc,0xe5806000 if write else 0xe5906000);word(pc+4,0xe12fff1e)
        branch(0xdb000,0xdb000)
        path.write_bytes(rom)
        clock_outputs=[]
        for engine in ('exact','fast'):
            p=subprocess.run([str(PROBE),str(path),'linked','3000',engine,bootstrap,profile],capture_output=True,text=True,timeout=15)
            output=p.stdout+p.stderr
            assert p.returncode==2 and 'stop=instruction-budget' in output,output
            assert any(int(v,16)>>16 for v in re.findall(r'pc=200c588c operation=read address=16010094 size=4 value=([0-9a-f]{8}) .*status=ok',output)),output
            assert 'graphics-frame lcd=1 width=3 height=2 fnv1a=%08x'%expected in output,output
            clock_outputs.append(re.sub(r'^jit-blocks=.*\n','',re.sub(r'engine=(exact|fast)','engine=checked',output),flags=re.M))
        assert clock_outputs[0]==clock_outputs[1], 'Exact/Fast clock traces differ'
        if bus_checks: bus_checks(rom,word,branch,path,bootstrap,profile,expected)
        branch(0xd9000,0xd9000)
        query = 0xd6000 + (len(display_ops)-2) * 0x40
        for instruction in (0xe3a07000,0xe3a07002):
            word(query+8,instruction)
            path.write_bytes(rom)
            for engine in ('exact','fast'):
                p=subprocess.run([str(PROBE),str(path),'linked','3000',engine,bootstrap,profile],capture_output=True,text=True,timeout=15)
                output=p.stdout+p.stderr
                assert p.returncode==2 and 'peripheral-status=external-input-unavailable' in output,(engine,output)
                assert re.search(r'pc=200b9cd4 .*address=160104c0 .*status=external-input-unavailable',output),output
        word(query+8,0xe3a07001)
        word(query+4,0xe3a0000d)
        path.write_bytes(rom)
        for engine in ('exact','fast'):
            p=subprocess.run([str(PROBE),str(path),'linked','3000',engine,bootstrap,profile],capture_output=True,text=True,timeout=15)
            output=p.stdout+p.stderr
            assert p.returncode==2 and 'peripheral-status=external-input-unavailable' in output,(engine,output)

    print('PASS: virtual profile CLI validation, native graphics/VRAM bus, RGB565 frame/hash, qualified per-pin context and full Exact/Fast trace parity')

if __name__ == '__main__': main()
