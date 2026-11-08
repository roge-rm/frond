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
    # Native mode-1 bus closure, including the bounded count poll and one FIFO read.
    setup = operations[:-1]
    at=0xe2000+len(setup)*0x40
    for j,v in enumerate([0xe59fc010,0xe59f0010,0xe12fff10]): word(at+j*4,v)
    word(at+0x18,0x200e2500); word(at+0x1c,0x20087403)
    branch(0xe2500,0xe2500)
    native = [0x48de,0x6801,0x08c9,0x00c9,0x3101,0x6001,0x6800,
              0x48da,0x3008,0x6800,0x210f,0x0409,0x4388,0x2101,0x0409,
              0x1841,0x48d6,0x3008,0x6001,0x6800,0x49d4,0x3104,0x6808,
              0x2201,0x0292,0x4390,0x6008,0x6808,0x6808,0x4310,0x6008,
              0x6808,0x2200,0x27d1,0x013f,0x48cd,0x4282,0xd219,0x1c38,
              0xf7ff,0xfec1,0x3201,0x01c0,0x0ec0,0xd0f5,0x49c7,0x3104,
              0x6808,0x2201,0x0292,0x4390,0x6008,0x6808,0x1d08,0x6800,
              0x48c2,0x300c,0x6800,0x0081,0x0e89,0x2909,0xd101,0x0585,0x0dad]
    for i,op in enumerate(native): struct.pack_into('<H',rom,0x87402+i*2,op)
    struct.pack_into('<H',rom,0x87482,0x4760) # Return to the synthetic caller after bus closure.
    for i,op in enumerate([0x4975,0x1840,0x6800,0x6809,0x4770]):
        struct.pack_into('<H',rom,0x871d6+i*2,op)
    word(0x873ac,0x16010000); word(0x8777c,0x16010d08); word(0x87780,10000)
    path.write_bytes(rom)
    common=[str(g.PROBE),str(path),'linked','26000']
    flags=[bootstrap,profile,'--graphics-raw-axes=128/900']
    for bad in ('','-1','1024','4294967296','1x'):
        p=subprocess.run(common+['exact']+flags+['--graphics-raw-mode1='+bad],capture_output=True,text=True,timeout=30)
        assert p.returncode==1,(bad,p.stdout,p.stderr)
    for extra in ([bootstrap,'--graphics-raw-mode1=1'],[profile,'--graphics-raw-mode1=1'],
                  flags+['--graphics-raw-mode1=1','--graphics-raw-mode1=2']):
        p=subprocess.run(common+['exact']+extra,capture_output=True,text=True,timeout=30)
        assert p.returncode==1,(extra,p.stdout,p.stderr)
    p=subprocess.run(common+['exact']+flags,capture_output=True,text=True,timeout=30)
    assert 'stop=unsupported-peripheral' in p.stdout and 'pc=2008743e' in p.stdout and 'peripheral-status=external-input-unavailable' in p.stdout,p.stdout[-3000:]
    outputs=[]
    for engine in ('exact','fast'):
        p=subprocess.run(common+[engine]+flags+['--graphics-raw-mode1=512'],capture_output=True,text=True,timeout=30)
        output=p.stdout+p.stderr
        assert p.returncode==2 and 'stop=instruction-budget' in output and 'r5=00000200' in output,output[-3000:]
        reads=[x for x in output.splitlines() if x.startswith('bus ') and 'address=16010d14 ' in x]
        assert len(reads)==1 and 'pc=20087474 ' in reads[0] and 'value=09000200 ' in reads[0],reads
        polls=[x for x in output.splitlines() if x.startswith('bus ') and 'pc=200871da ' in x]
        assert 1<len(polls)<10000 and 'value=0011700a ' in polls[-1],polls[-3:]
        assert 'pc=2008746e operation=read address=16010d10 size=4 value=0011700a ' in output
        outputs.append(re.sub(r'^jit-blocks=.*\n','',re.sub(r'engine=(exact|fast)','engine=checked',output),flags=re.M))
    assert outputs[0]==outputs[1],'mode1 Exact/Fast trace mismatch'
    print('PASS: independent mode1 input, native bounded poll/count/stop/tag9 single-read closure and Exact/Fast parity')
g.main(check)
