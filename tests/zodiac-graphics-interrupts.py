#!/usr/bin/env python3
"""Generated GPU status/enable/ack transactions and the explicit A13 connection."""
from pathlib import Path
import importlib.util
import re
import subprocess
spec = importlib.util.spec_from_file_location('graphics', Path(__file__).with_name('zodiac-virtual-graphics.py'))
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)

def check(rom, word, branch, path, bootstrap, profile, expected):
    saved = bytes(rom)
    connection = '--graphics-irq-connection=virtual-a13'
    masked = '--gpio-a-input-masked=0x80/0xffffdfff'
    def invoke(engine, *flags, budget='3500'):
        return subprocess.run([str(g.PROBE), str(path), 'linked', budget, engine, *flags],
                              capture_output=True, text=True, timeout=25)
    for flags in [(bootstrap, connection),
                  (bootstrap, profile, connection, '--gpio-a-input=0x80'),
                  (bootstrap, profile, connection, '--gpio-a-input-masked=0x80/0xffffffff'),
                  (bootstrap, profile, connection, masked, '--gpio-a-input=0x80')]:
        p = invoke('exact', *flags)
        assert p.returncode == 1 and 'GPIO-A13 is reserved' in p.stderr, p.stderr
    for bad in ['0x80/0', '0x2080/0xffffdfff', '0x80', '0x80/0x100000000', '-1/0xffffffff']:
        p = invoke('exact', bootstrap, profile, '--gpio-a-input-masked='+bad)
        assert p.returncode == 1, (bad, p.stdout, p.stderr)

    def emit(operations, scratch, final):
        for index, (pc, address, write, value) in enumerate(operations):
            at = scratch + index * 0x40
            word(at, 0xe59f0010); word(at+4, 0xe59f6010); word(at+8, 0xe59fe010)
            branch(at+12, pc)
            word(at+0x18, address); word(at+0x1c, value)
            word(at+0x20, 0x20000000+at+0x40 if index+1 < len(operations) else final)
            word(pc, 0xe5806000 if write else 0xe5906000); word(pc+4, 0xe12fff1e)

    for fiq in (False, True):
        for mode in (0x3e00, 0x80003f00):
            rom[:] = saved
            words = []
            def load(reg, value): words.extend([0xe59f0000 | reg << 12, 0xea000000, value])
            for address, value in [(0x00304000,0x10000c02),(0x00304008,0x00200c02),
                    (0x0030400c,0x00300c02),(0x00304800,0x10000c02),
                    (0x00304580,0x16000c02),(0x003045a0,0x16800c02)]:
                load(0,address); load(1,value); words.append(0xe5801000)
            load(0,0x00304000); words.append(0xee020f10)
            load(0,3); words.append(0xee030f10)
            load(0,0x79); words.append(0xee010f10)
            for index, instruction in enumerate(words): word(0xe4000+index*4,instruction)
            branch(0xe4000+len(words)*4,0xe2000)
            branch(0xdb000,0xe4000)
            operations = [(0xe1000,0x0021c020,True,0x2000),
                          (0xe1008,0x0021c038,True,0),
                          (0xe1010,0x0021c000,True,0),
                          (0xe1018,0x0021c028,True,3 << 26),
                          (0xe1020,0x0021c030,True,0x2000),
                          (0xe1028,0x0022301c,True,0x800 if fiq else 0),
                          (0xe1030,0x00223008,True,11),
                          (0xe1038,0x00220028,True,mode),
                          (0xe1040,0x0021c024,False,0),
                          (0x868cc,0x16010204,False,0),
                          (0x86e08,0x16010200,True,1)]
            # Enable IRQ and FIQ after GPIO/AITC setup, before the GPU enable.
            operations[8] = (0xe1050,0x0021c024,False,0)
            emit(operations,0xe2000,0x200e3000)
            word(0xe103c,0xe321f013); word(0xe1040,0xe12fff1e)
            word(0x18,0xe59ff000); word(0x20,0x200e7000)
            word(0x1c,0xe59ff000); word(0x24,0x200e7000)
            word(0xe7000,0xe1a0800e)  # Preserve the exception link across helper calls.
            branch(0xe7004,0xe7100)
            handler = [(0x86970,0x16010200,True,0),
                       (0x87824,0x16010204,True,1),
                       (0xe7060,0x0021c034,True,0x2000),
                       (0xe7068,0x0021c034,False,0)]
            emit(handler,0xe7100,0x200e7300)
            word(0xe7300,0xe1a0e008); word(0xe7304,0xe25ef004)
            branch(0xe3000,0xe3000)
            if not fiq and mode == 0x3e00:
                wait_base, wait_operations = bytes(rom), operations.copy()
            path.write_bytes(rom)
            outputs = []
            for engine in ('exact','fast'):
                p = invoke(engine,bootstrap,profile,connection,masked)
                output = p.stdout+p.stderr
                assert p.returncode == 2 and 'stop=instruction-budget' in output, output[-4500:]
                assert 'irq-entries=%d fiq-entries=%d' % (not fiq,fiq) in output, output[-4500:]
                assert re.search(r'interrupt .*interrupted-pc=20086e0c ',output), output[-4500:]
                assert re.search(r'pc=200e1050 .*value=00002080 .*status=ok',output), output[-4500:]
                rows = [x for x in output.splitlines() if x.startswith('bus ') and 'address=16010204 ' in x]
                assert len(rows) == 2 and 'value=00000001' in rows[0] and 'operation=read' in rows[0], rows
                assert 'operation=write' in rows[1] and 'status=ok' in rows[1] and 'cs5-hclk=' in rows[1],rows
                assert re.search(r'pc=200e7068 .*value=00000000 .*status=ok',output),output[-4500:]
                assert 'masked GPIO-A input=00000080 mask=ffffdfff' in output
                outputs.append(re.sub(r'^jit-blocks=.*\n','',re.sub(r'engine=(exact|fast)','engine=checked',output),flags=re.M))
            assert outputs[0] == outputs[1], 'GPU IRQ Exact/Fast complete traces differ'
    # Divider 1 and a fixed 400-cycle gap place the next scan event inside the 63-HCLK store.
    # At the reset divider 12 a transfer exceeds this small synthetic frame.
    rom[:] = wait_base
    setup_words = words + [0xe59f0000,0xea000000,0x0021b000,0xe59f1000,0xea000000,0x57000003,
                           0xe5801000,0xe3a01000,0xe5801020]
    for i, instruction in enumerate(setup_words): word(0xe4000+i*4,instruction)
    branch(0xe4000+len(setup_words)*4,0xe2000)
    operations = wait_operations[:-1] + [(0x87824,0x16010204,True,1),
        (0x86e08,0x16010200,True,1),(0xe1060,0x16800010,True,0x12345678)]
    emit(operations,0xe2000,0x200e3000)
    word(0xe103c,0xe321f013); word(0xe1040,0xe12fff1e)
    word(0xe2000+10*0x40+0x20,0x200e5000)
    for i in range(400): word(0xe5000+i*4,0xe1a00000)
    branch(0xe5000+400*4,0xe2000+11*0x40)
    path.write_bytes(rom)
    outputs = []
    for engine in ('exact','fast'):
        p = invoke(engine,bootstrap,profile,connection,masked)
        output = p.stdout+p.stderr
        assert p.returncode == 2 and 'stop=instruction-budget' in output,output[-4000:]
        assert 'irq-entries=1 fiq-entries=0' in output,output[-4000:]
        assert re.search(r'interrupt .*interrupted-pc=200e1064 ',output),output[-4000:]
        stores = [x for x in output.splitlines() if x.startswith('bus ') and
                  'pc=200e1060 ' in x and 'operation=write address=16800010 ' in x]
        assert len(stores) == 1 and 'cs5-hclk=63' in stores[0] and 'status=ok' in stores[0],stores
        assert re.search(r'pc=200e7068 .*value=00000000 .*status=ok',output),output[-4000:]
        outputs.append(re.sub(r'^jit-blocks=.*\n','',re.sub(r'engine=(exact|fast)','engine=checked',output),flags=re.M))
    assert outputs[0] == outputs[1], 'GPU event during CS5 wait Exact/Fast traces differ'
    rom[:] = saved
    print('PASS: explicit A13 ownership, masked status, timed enable/ack, GPIO/AITC IRQ and FIQ, Exact/Fast parity')

g.main(check)
