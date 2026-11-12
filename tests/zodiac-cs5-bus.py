#!/usr/bin/env python3
"""Exercise the composed virtual GPU bus through generated ARM instructions."""
from pathlib import Path
import importlib.util
import re
import subprocess
spec=importlib.util.spec_from_file_location('graphics',Path(__file__).with_name('zodiac-virtual-graphics.py'))
g=importlib.util.module_from_spec(spec);spec.loader.exec_module(g)
def check(rom,word,branch,path,bootstrap,profile,expected):
    saved=bytes(rom)
    def program(mode,stop=False,bad_route=False,unknown=False,irq=False):
        rom[:]=saved
        # Program a slow, running reference SCLK so the wait exceeds 3T.
        operations=[(0xc56c0,0x1601008c,True,0x78f0),
            (0xc573c,0x16010098,True,4),
            (0xe1000,0x0021c020,True,(1<<17) if bad_route else 0),
            (0xe1008,0x0021c038,True,0),
            (0xe1010,0x0021c000,True,0),
            (0xe1018,0x00220028,True,mode),
            (0xe1020,0x16800010,True,0x12345678),
            (0xe1028,0x16800010,False,0)]
        if unknown: operations.append((0xe1030,0x160104c0,False,0))
        if stop:
            operations.extend([(0xc573c,0x16010098,True,12),
                               (0xe1038,0x16800010,True,0xabcdef01)])
        if irq:
            operations[6:6]=[(0xe1100,0x00223008,True,59),(0xe1108,0x00202004,True,0),
                (0xe1110,0x00202008,True,40),(0xe1118,0x00202000,True,0x13)]
            instructions=[]
            def load(reg,value): instructions.extend([0xe59f0000|(reg<<12),0xea000000,value])
            for address,value in [(0x00304000,0x10000c02),(0x00304008,0x00200c02),
                    (0x0030400c,0x00300c02),(0x00304800,0x10000c02),
                    (0x00304580,0x16000c02),(0x003045a0,0x16800c02)]:
                load(0,address);load(1,value);instructions.append(0xe5801000)
            load(0,0x00304000);instructions.append(0xee020f10)
            load(0,3);instructions.append(0xee030f10)
            load(0,0x79);instructions.append(0xee010f10)
            load(0,0x0021b000);load(1,0x57000003);instructions.append(0xe5801000)
            load(1,0);instructions.append(0xe5801020)
            for index,ins in enumerate(instructions):word(0xe4000+index*4,ins)
            branch(0xe4000+len(instructions)*4,0xe2000)
            branch(0xdb000,0xe4000)
            word(0x18,0xe59ff000);word(0x20,0x200e7000)
            handler=[]
            handler.extend([0xe59f9000,0xea000000,0x00202000,0xe3a0a000,0xe589a000,
                            0xe599b014,0xe589a014,0xe25ef004])
            for index,ins in enumerate(handler):word(0xe7000+4*index,ins)
        else: branch(0xdb000,0xe2000)
        for index,(pc,address,write,value) in enumerate(operations):
            scratch=0xe2000+index*0x40
            word(scratch,0xe59f0010);word(scratch+4,0xe59f6010);word(scratch+8,0xe59fe010)
            branch(scratch+12,pc)
            word(scratch+0x18,address);word(scratch+0x1c,value)
            word(scratch+0x20,0x20000000+scratch+0x40 if index+1<len(operations) else 0x200e3000)
            word(pc,0xe5806000 if write else 0xe5906000);word(pc+4,0xe12fff1e)
        if irq:
            word(0xe111c,0xe321f053);word(0xe1120,0xe12fff1e)
        branch(0xe3000,0xe3000)
        path.write_bytes(rom)
    def pair(status):
        outputs=[]
        for engine in ('exact','fast'):
            p=subprocess.run([str(g.PROBE),str(path),'linked','3500',engine,bootstrap,profile,'--gpio-a-input=0x80'],capture_output=True,text=True,timeout=20)
            output=p.stdout+p.stderr
            assert p.returncode==2 and status in output,(engine,output[-3000:])
            outputs.append(re.sub(r'^jit-blocks=.*\n','',re.sub(r'engine=(exact|fast)','engine=checked',output),flags=re.M))
        assert outputs[0]==outputs[1], 'CS5 Exact/Fast composed traces differ'
        return outputs[0]
    for mode in (0x3e00,0x3f00,0x80003f00):
        program(mode)
        output=pair('stop=instruction-budget')
        rows=re.findall(r'bus .*pc=200e102[08].*address=16800010.*status=ok.*cs5-hclk=(\d+)',output)
        assert len(rows)==2 and all(int(n)==(63 if mode==0x3e00 else 9) for n in rows),output[-3000:]
        assert re.search(r'pc=200e1028 .*value=12345678 .*status=ok',output),output[-3000:]
        assert 'graphics-frame lcd=1 width=3 height=2' in output
        program(mode,unknown=True)
        output=pair('peripheral-status=external-input-unavailable')
        row=[x for x in output.splitlines() if x.startswith('bus ') and 'pc=200e1030 ' in x]
        assert len(row)==1 and 'cs5-hclk=' not in row[0],row
        if mode!=0x3e00:
            program(mode,bad_route=True)
            output=pair('peripheral-status=unsupported-sdram-sequence')
            assert re.search(r'pc=200e1020 .*status=unsupported-sdram-sequence',output),output[-3000:]
            program(mode,stop=True)
            output=pair('peripheral-status=external-bus-timeout')
            rows=[x for x in output.splitlines() if x.startswith('bus ') and 'status=external-bus-timeout' in x]
            assert len(rows)==1 and 'cs5-hclk=1022' in rows[0],rows
            assert 'pc=200e1038' in rows[0],rows
    program(0x80003f00,irq=True)
    output=pair('stop=instruction-budget')
    assert 'irq-entries=1 fiq-entries=0' in output,output[-5000:]
    assert re.search(r'interrupt .*interrupted-pc=200e1024 ',output),[x for x in output.splitlines() if x.startswith('interrupt ') or ('bus ' in x and ('002020' in x or '200e1020 ' in x))]
    store=[x for x in output.splitlines() if x.startswith('bus ') and 'pc=200e1020 ' in x and 'operation=write address=16800010 ' in x]
    assert len(store)==1 and 'status=ok' in store[0] and 'cs5-hclk=99' in store[0],store
    rom[:]=saved
    print('PASS: fixed/DTACK/WAIT Exact/Fast transactions, GPU-derived waits, mux/unknown guards and one 1022-HCLK timeout')
g.main(check)
