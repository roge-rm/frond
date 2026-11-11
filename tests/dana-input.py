#!/usr/bin/env python3
"""Focused Dana 1.5 guest keyboard check. Requires a private ready session and keyboard probe."""
import argparse
import json
from pathlib import Path
import re
import runpy
import struct
import sys
import time
import zlib
import zipfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import palm_boot

def run(args):
    root=args.evidence.resolve()
    root.mkdir(mode=0o700,parents=True,exist_ok=False)
    if args.core: palm_boot.CORE_68K=args.core.resolve()
    serial=0
    floor=0
    def key(name,down):
        assert 'accepted' in c.send(('dana-down ' if down else 'dana-up ')+json.dumps(name),'input ')
    def tap(name):
        key(name,True);key(name,False)
    def records():
        nonlocal serial
        serial+=1
        path=root/f'keys-{serial}.pdb'
        c.send(f'db-export-db FrondKeyboardLog "{path}"','export written')
        b=path.read_bytes();n=struct.unpack_from('>H',b,76)[0]
        offsets=[struct.unpack_from('>I',b,78+i*8)[0] for i in range(n)]+[len(b)]
        rows=[b[offsets[i]:offsets[i+1]].decode('ascii').rstrip('\0') for i in range(n)]
        return [tuple(int(v,16 if j else 10) for j,v in enumerate(m.groups())) for row in rows if (m:=re.match(r'event=(\d+) chr=([0-9A-Fa-f]+) code=([0-9A-Fa-f]+) err=([0-9A-Fa-f]+)',row))]
    def launch():
        nonlocal floor
        c.launch('Frond keyboard probe')
        floor=len(records())
    def text(value): assert 'accepted' in c.send('dana-text '+json.dumps(value),'input ')
    def has(chr,mod=0,code=0):
        time.sleep(.15)
        assert (4,chr,code,mod) in records()[floor:],(hex(chr),hex(mod),hex(code))
    def fresh(pending,pendingBase):
        # A new process resumes the same saved, neutral pending text.
        nonlocal c,floor
        c=palm_boot.Core([pending,'--device','alphasmart-dana'],root/'fresh.log','--speed','original')
        try:
            time.sleep(.5)
            typed=[e[1] for e in records()[pendingBase or 0:] if e[0]==4]
            # Without a base, the pending text is the last text the session's log gets.
            assert (typed if pendingBase is not None else typed[-3:])==list(map(ord,'abc')),typed[-6:]
            floor=len(records());text('d');has(ord('d'))
            print('PASS fresh-process pending input',flush=True)
        finally: assert c.close()==0
    def startup_replay(session):
        # A replay given on the command line survives the window's first hidden events.
        nonlocal c,floor
        replay=root/'startup.replay';replay.write_text('400 key-down dana.x\n450 key-up dana.x\n')
        c=palm_boot.Core([session,'--device','alphasmart-dana'],root/'startup.log','--speed','original','--input-replay',replay)
        try:
            end=time.monotonic()+20
            while not any('replay event 2 ' in line for line in c.lines) and time.monotonic()<end: time.sleep(.1)
            assert any('replay event 2 ' in line for line in c.lines),'startup replay dropped'
            time.sleep(.3)
            assert ord('x') in [e[1] for e in records() if e[0]==4][-4:]
            print('PASS startup input replay',flush=True)
        finally: assert c.close()==0
    if args.fresh_only:
        c=None;fresh(args.session,None);return
    if args.replay_only:
        c=None;startup_replay(args.session);return
    c=palm_boot.Core([args.session,'--device','alphasmart-dana'],root/'guest.log','--speed','original')
    try:
        c.until(f'install "{args.probe.resolve()}"','installation ','successful');launch()
        if not args.skip_basics:
            for value in ['abcdefghijkl','mnopqrstuvwx','yz0123456789',"`-=[]\\;',./",'ABCXYZ','~!@#$%','^&*()_','+{}|:"','<>?']:
                launch();before=len(records());text(value);time.sleep(1.5)
                got=[e[1] for e in records()[before:] if e[0]==4]
                assert got==list(map(ord,value)),(value,got)
            print('PASS alphabet, numbers, shifted text and punctuation',flush=True)
            for name,ch,mod in [('ctrl',ord('a'),0x20),('command',ord('a'),8),('function',0xe6,0),('lshift',ord('A'),1),('rshift',ord('A'),1)]:
                floor=len(records());key(name,True);tap('a');key(name,False);has(ch,mod)
            floor=len(records());tap('caps');tap('a');tap('caps');has(ord('A'),2)
            # Sticky shift is consumed by the next letter, without a host-held modifier.
            floor=len(records());tap('lshift');tap('a');tap('b');has(ord('a'));has(ord('b'))
            print('PASS Ctrl, command, function, caps, both shifts and default unlatched modifiers',flush=True)
            for name,ch,mod,code in [('left',0x1c,0,0),('right',0x1d,0,0),('up',0x1e,0,0),('down',0x1f,0,0),('backspace',8,0,0),('delete',127,0,0),('escape',27,0,0),('tab',0x103,8,0),('enter',10,0,0),('calendar',0x204,8,0),('address',0x205,8,0),('todo',0x206,8,0),('memo',0x207,8,0),('find',0x10a,8,0),('menu',0x105,8,0),('apps',0x108,8,0),('send',0x116,8,0)]+[(f'f{i+1}',0x2000,8,code) for i,code in enumerate([0x2d,0x2c,4,15,14,10,1,0x27])]:
                launch();tap(name);has(ch,mod,code)
            print('PASS navigation, editing, app keys and F1-F8',flush=True)
            launch();before=len(records());key('a',True);time.sleep(4);key('a',False);time.sleep(.2)
            repeated=records()[before:];assert (4,ord('a'),0,0) in repeated
            assert len([e for e in repeated if e[0]==4 and e[1]==ord('a')])>=2
            before=records();time.sleep(.5);assert records()==before
            print('PASS guest repeat and release',flush=True)
        # A letter typed twice in a row reaches the guest as two presses.
        # More than eight different keys released just before a repeat still hold it back.
        for chunks in [['hello'],['line0011'],['aaaaaaaaaaaa'],['abcdefghijll'],['the bookkeep','er will see ','three apples']]:
            launch();before=len(records())
            for chunk in chunks: text(chunk);time.sleep(2.5)
            value=''.join(chunks);got=[e[1] for e in records()[before:] if e[0]==4]
            assert got==list(map(ord,value)),(value,bytes(got))
        print('PASS repeated letters',flush=True)
        launch();before=len(records())
        chord='abcdefgh'
        c.send('\n'.join(['dana-down '+k for k in chord]+['dana-up '+k for k in chord]+['cycles']),'cycles:')
        time.sleep(1)
        assert [e[1] for e in records()[before:] if e[0]==4]==list(map(ord,'abcgh'))
        # Text past the 32-report key FIFO waits in the ST backlog and arrives whole and in order.
        def burst(value,chunk,wait):
            launch();before=len(records());mark=len(c.lines)
            parts=[value[i:i+chunk] for i in range(0,len(value),chunk)]
            c.send('\n'.join(['dana-text '+json.dumps(part) for part in parts]+['cycles']),'cycles:')
            replies=c.lines[mark:]
            assert sum('input accepted' in x for x in replies)==len(parts) and not any('input refused' in x for x in replies),replies
            # Exporting the log is a host call, so wait without one while the keys go in.
            time.sleep(wait)
            got=[e[1] for e in records()[before:] if e[0]==4]
            assert got==list(map(ord,value)),(value,bytes(got))
        burst('abcdefghijkl'*4,12,4)
        # The probe logs its first 100 key events, so 96 keys.
        burst('hello bookkeeper letter '*4,12,12)
        print('PASS guest rollover guard and queued bursts of 48 and 96 keys',flush=True)

        launch()
        replay=root/'ownership.replay';replay.write_text('0 key-down dana.lshift\n1500 key-up dana.lshift\n')
        c.send(f'input-replay "{replay}"','input replay loaded');time.sleep(.2)
        key('lshift',True);time.sleep(1.7);floor=len(records());tap('a');has(ord('A'),1);key('lshift',False);floor=len(records());tap('b');has(ord('b'))
        key('lshift',True)
        empty=root/'empty.replay';empty.write_text('')
        c.send(f'input-replay "{empty}"','input replay loaded')
        floor=len(records());tap('a');has(ord('A'),1);key('lshift',False)
        print('PASS replay/console ownership',flush=True)
        assert 'refused' in c.send('dana-down 256','input ')
        assert 'refused' in c.send('dana-text "abcdefghijklm"','input ')
        assert 'refused' in c.send('dana-text "é"','input ')
        # Save before the CPU consumes a complete neutral text batch.
        pendingBase=len(records())
        pending=root/'pending.frond'
        c.send(f'dana-text "abc"\nsave-session "{pending}"','wrote session')
        data=pending.read_bytes();raw=zlib.decompress(data[92:]);start=sum(struct.unpack_from('<5Q',data,48)[:4]);n=struct.unpack_from('<I',raw,start)[0];at=start+4+8*n
        for i in range(n):
            kind,words=struct.unpack_from('<II',raw,start+4+8*i)
            if kind==0x1e0: board=at
            at+=4*words
        assert struct.unpack_from('<I',raw,board+16*4)[0]>0
        c.send(f'load-session "{pending}"','loaded session');time.sleep(.5)
        assert [e[1] for e in records()[pendingBase:] if e[0]==4]==list(map(ord,'abc'))
        for label,word,value in [('held',14,9),('pacing-clock',287,1),('pacing-delay',285,0xffffffff),('pacing-unclocked',286,0),('poll-cursor',288,9),('presented-scan',289,255)]:
            bad=bytearray(raw);struct.pack_into('<I',bad,board+word*4,value)
            if label=='pacing-unclocked': struct.pack_into('<I',bad,board+284*4,1)
            malformed=root/(label+'.frond');malformed.write_bytes(data[:92]+zlib.compress(bad))
            before=records();assert 'restored' in c.send(f'load-session "{malformed}"','session refused:');assert records()==before
        key('lshift',True)
        assert 'restored' in c.send(f'load-session "{malformed}"','session refused:')
        floor=len(records());tap('a');has(ord('A'),1);key('lshift',False)
        key('lshift',True);c.send('hidden on','hidden: on');c.send('hidden off','hidden: off');floor=len(records());tap('a');tap('b');has(ord('b'))
        c.send('reset','reset done');time.sleep(2);launch();text('b');has(ord('b'))
        print('PASS refusals, pending save/load, malformed rollback, focus release and reset',flush=True)
        # Use the guest Keyboard application to enable Sticky Keys.
        c.launch('Keyboard');c.tap(root,330,6);time.sleep(.3)
        c.tap(root,320,73);time.sleep(.3);c.tap(root,235,25);time.sleep(.3)
        launch()
        for name,ch,mod in [('ctrl',ord('a'),0x20),('command',ord('a'),8),('function',0xe6,0),('lshift',ord('A'),1),('rshift',ord('A'),1)]:
            floor=len(records());tap(name);tap('a');tap('b');has(ch,mod);has(ord('b'))
        tap('ctrl');time.sleep(.2)
        latched=root/'sticky-ctrl.frond'
        c.send(f'save-session "{latched}"','wrote session')
        c.send(f'load-session "{latched}"','loaded session')
        floor=len(records());tap('a');tap('b');has(ord('a'),0x20);has(ord('b'))
        print('PASS guest Sticky Keys for every modifier and saved one-shot Ctrl',flush=True)

    finally:
        assert c.close()==0
    fresh(pending,pendingBase)
    client_type=runpy.run_path(str(Path(__file__).with_name('core-control.py')))['Client']
    channel=client_type(palm_boot.CORE_68K,[pending,'--device','alphasmart-dana'],root,'socket')
    try:
        channel.ok('guest-ready',code='guest-ready')
        channel.fails('dana-down','256',code='unsupported-key')
        channel.fails('dana-text','é',code='unsupported-text')
        channel.ok('dana-down','lshift',code='accepted')
        channel.fails('dana-text','a',code='busy')
        channel.ok('dana-up','lshift',code='accepted')
        channel.ok('dana-down','a',code='accepted');channel.ok('dana-up','a',code='accepted')
        channel.ok('dana-text','b',code='accepted')
        time.sleep(.4)
        archive=root/'socket-keys.zip'
        channel.until_ok('export-db',str(archive)+'\nFrondKeyboardLog')
        with zipfile.ZipFile(archive) as files:
            assert any(files.read(name).endswith(b'event=4 chr=0062 code=0000 err=0000 value=0\0') for name in files.namelist())
        channel.quit()
        print('PASS socket typed input and distinct unsupported/busy refusals',flush=True)
    finally: channel.close()

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('session','evidence'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--probe',type=Path)
    parser.add_argument('--core',type=Path)
    parser.add_argument('--skip-basics',action='store_true',help='Run only chord and input lifecycle checks')
    parser.add_argument('--fresh-only',action='store_true',help='Start a new process on --session, a pending.frond from an earlier run, and check its pending text')
    parser.add_argument('--replay-only',action='store_true',help='Start a new process on --session, a pending.frond from an earlier run, with an --input-replay and check its key arrives')
    args=parser.parse_args()
    if not args.fresh_only and not args.replay_only and not args.probe: parser.error('--probe is required')
    run(args)
