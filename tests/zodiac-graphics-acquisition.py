#!/usr/bin/env python3
"""Real helper readback and identity transfers on the timed virtual CS5 bus."""
from pathlib import Path
import importlib.util
import re
import struct
import subprocess
spec = importlib.util.spec_from_file_location('graphics', Path(__file__).with_name('zodiac-virtual-graphics.py'))
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)
def check(rom, word, branch, path, bootstrap, profile, expected):
    branch(0xdb000,0xe2000)
    # Thumb helpers retain the native transfer instructions and return to ARM scratch code.
    for pc, ops in [(0x869b6,[0x6001,0x6800,0x4770]),(0x871da,[0x6800,0x6809,0x4770])]:
        for i, op in enumerate(ops): struct.pack_into('<H',rom,pc+i*2,op)
    operations = [(0xd08,0x2000),(0xd08,0x2800),(0xd08,0x2000),
                  (0xd10,0x2700a),(0xd08,0x2400),(0xd1c,8),(0xd0c,0x400)]
    for i,(offset,value) in enumerate(operations):
        at=0xe2000+i*0x40
        for j,v in enumerate([0xe59f0010,0xe59f1010,0xe59fe010,0xe59f2010,0xe12fff12]): word(at+j*4,v)
        word(at+0x18,0x16010000+offset);word(at+0x1c,value)
        word(at+0x20,0x20000000+at+0x40);word(at+0x24,0x200869b7)
    # Guest instructions, including real timed polls, are the only time producer.
    at=0xe2000+len(operations)*0x40
    branch(at,0xe3000)
    for i in range(22000): word(0xe3000+i*4,0xe1a00000)
    at=0xe3000+22000*4
    reads=[0xd10,0xd14,0xd14,0xd10]
    for i,offset in enumerate(reads):
        base=at+i*0x40
        for j,v in enumerate([0xe59f0010,0xe59f1010,0xe59fe010,0xe59f2010,0xe12fff12]): word(base+j*4,v)
        word(base+0x18,0x16010000+offset);word(base+0x1c,0x16010000)
        word(base+0x20,0x20000000+base+0x40);word(base+0x24,0x200871db)
    branch(at+len(reads)*0x40,at+len(reads)*0x40)
    path.write_bytes(rom)
    outputs=[]
    for engine in ('exact','fast'):
        p=subprocess.run([str(g.PROBE),str(path),'linked','26000',engine,bootstrap,profile,
                          '--graphics-raw-axes=128/900'],capture_output=True,text=True,timeout=30)
        output=p.stdout+p.stderr
        assert p.returncode==2 and 'stop=instruction-budget' in output,output[-5000:]
        for value in ['00002000','00002800','0002700a','00002400','00000008','00000400']:
            assert re.search(r'pc=200869b8 .*value='+value+r' .*status=ok .*cs5-hclk=',output),value
        rows=[x for x in output.splitlines() if x.startswith('bus ') and 'pc=200871da ' in x]
        assert len(rows)==4,rows
        for row,value in zip(rows,['0022700a','01000080','02000384','0002700a']): assert 'value='+value in row,row
        assert len([x for x in output.splitlines() if x.startswith('bus ') and 'pc=200871dc ' in x and 'value=57441002' in x])==4
        outputs.append(re.sub(r'^jit-blocks=.*\n','',re.sub(r'engine=(exact|fast)','engine=checked',output),flags=re.M))
    assert outputs[0]==outputs[1], 'acquisition Exact/Fast trace mismatch'
    print('PASS: acquisition real store/readback and read/identity helpers, timed CS5 and Exact/Fast parity')
g.main(check)
