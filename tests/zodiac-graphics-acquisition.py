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
    # Mode/count/tag alone cannot distinguish alternate-board axes and the auxiliary domain.
    def mode2(selector):
        program = [(0x16010d08,0x2000),(0x16010d08,0x2800),(0x16010d08,0x2000),
                   (0x16010d10,0x1700a),(0x16010d08,0x2402),(0x16010d1c,8)]
        if selector is not None:
            direction, outputs, mux = selector
            program += [(0x0021c120,0x0f400000),(0x0021c100,direction),
                        (0x0021c108,mux),(0x0021c11c,outputs)]
        program += [(0x16010d0c,0x400)]
        branch(0xdb000,0xe2000)
        for i,(address,value) in enumerate(program):
            at=0xe2000+i*0x40
            for j,v in enumerate([0xe59f0010,0xe59f1010,0xe59fe010,0xe59f2010,0xe12fff12]): word(at+j*4,v)
            word(at+0x18,address);word(at+0x1c,value)
            word(at+0x20,0x20000000+at+0x40);word(at+0x24,0x200869b7)
        branch(0xe2000+len(program)*0x40,0xe3000)
        for i in range(22000):word(0xe3000+i*4,0xe1a00000)
        at=0xe3000+22000*4
        for j,v in enumerate([0xe59f0010,0xe59f1010,0xe59fe010,0xe59f2010,0xe12fff12]): word(at+j*4,v)
        word(at+0x18,0x16010d14);word(at+0x1c,0x16010000)
        word(at+0x20,0x20000000+at+0x40);word(at+0x24,0x200871db)
        branch(at+0x40,at+0x40);path.write_bytes(rom)
    first=(0x0a400000,0x08000000,0x00cc3000)
    second=(0x05400000,0x04000000,0x00ff3000)
    high=(0x0a400000,0x08400000,0x00cc3000)
    input22=(0x0a000000,0x08000000,0x00cc3000)
    unmatched=(0x00400000,0,0x3000)
    for selector,route,aux,expected in [
            (first,None,None,None),
            (first,'alternate-mux',None,128),(second,'alternate-mux',None,900),
            (input22,'alternate-mux',900,None),(unmatched,'alternate-mux',900,None),
            (high,'alternate-mux',None,None),(high,'alternate-mux',900,900),
            (None,'default-aux',None,None),(None,'default-aux',900,900),
            (first,'default-aux',512,512)]:
        mode2(selector)
        flags=[bootstrap,profile,'--graphics-raw-axes=128/900','--graphics-raw-mode1=712']
        if route: flags+=['--graphics-mode2-route='+route]
        if aux is not None: flags+=['--graphics-raw-aux='+str(aux)]
        for engine in ('exact','fast'):
            p=subprocess.run(common+[engine]+flags,capture_output=True,text=True,timeout=30)
            output=p.stdout+p.stderr
            assert p.returncode==2,output[-3000:]
            if expected is None:
                assert 'stop=unsupported-peripheral' in output and 'peripheral-status=external-input-unavailable' in output,output[-3000:]
                assert not any('address=16010d14 ' in x for x in output.splitlines() if x.startswith('bus ')),output[-3000:]
            else:
                assert 'stop=instruction-budget' in output,output[-3000:]
                rows=[x for x in output.splitlines() if x.startswith('bus ') and 'address=16010d14 ' in x]
                assert len(rows)==1 and ('value=%08x '%(0x0a000000|expected)) in rows[0],rows
    for option in ('--graphics-mode2-route=', '--graphics-mode2-route=unknown',
                   '--graphics-mode2-route=defaultAux', '--graphics-raw-aux=',
                   '--graphics-raw-aux=-1', '--graphics-raw-aux=1024',
                   '--graphics-raw-aux=4294967296', '--graphics-raw-aux=1x'):
        p=subprocess.run(common+['exact',bootstrap,profile,option],capture_output=True,text=True,timeout=30)
        assert p.returncode==1,(option,p.stdout,p.stderr)
    for new in ('--graphics-raw-aux=900','--graphics-mode2-route=default-aux'):
        for flags in ([new],[bootstrap,new],[profile,new],[bootstrap,profile,new,new]):
            p=subprocess.run(common+['exact']+flags,capture_output=True,text=True,timeout=30)
            assert p.returncode==1,(flags,p.stdout,p.stderr)
    print('PASS: explicit mode2 board routes, B22 output selector, independent auxiliary input and guarded missing sources')
    # Supply a generated guest to the machine API fixture. It updates axes only after
    # the first completed instruction which writes START, without guest execution in the API.
    branch(0xdb000,0xe2000)
    program=[]
    def write(address,value):program.append((address,value,None))
    def delay():program.append((None,None,None))
    def read(reg):program.append((0x16010d14,None,reg))
    for address,value in [(0x16010d08,0x2000),(0x16010d08,0x2800),(0x16010d08,0x2000),
                          (0x16010d10,0x2700a),(0x16010d08,0x2400),(0x16010d1c,8)]:write(address,value)
    for registers,mode in [((4,5),0),((6,7),0),((8,),1),((9,),2)]:
        write(0x16010d08,0x2400|mode);write(0x16010d10,0x2700a if mode==0 else 0x1700a)
        write(0x16010d0c,0x400);delay();write(0x16010d0c,0)
        for reg in registers:read(reg)
    at=0xe2000
    for address,value,reg in program:
        if address is None:
            for i in range(22000):word(at+i*4,0xe1a00000)
            at+=22000*4
        elif reg is None:
            for j,v in enumerate([0xe59f0010,0xe59f1010,0xe59fe010,0xe59f2010,0xe12fff12]):word(at+j*4,v)
            word(at+0x18,address);word(at+0x1c,value)
            word(at+0x20,0x20000000+at+0x40);word(at+0x24,0x200869b7);at+=0x40
        else:
            for j,v in enumerate([0xe59f0010,0xe59f1010,0xe59fe010,0xe59f2010,0xe12fff12]):word(at+j*4,v)
            word(at+0x18,address);word(at+0x1c,0x16010000)
            word(at+0x20,0x20000000+at+0x40);word(at+0x24,0x200871db)
            word(at+0x40,0xe1a00000 | reg<<12)
            at+=0x44
    word(at,0xe7f000f0);path.write_bytes(rom)
    for engine in ('exact','fast'):
        p=subprocess.run(['build/test-zodiac-live-controls',str(path),engine],capture_output=True,text=True,timeout=30)
        assert p.returncode==0,(engine,p.stdout,p.stderr)
        print(p.stdout.strip())
g.main(check)
