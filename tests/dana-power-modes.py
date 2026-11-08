#!/usr/bin/env python3
"""Dana 1.5 two-button power checks with the real Keyboard preference."""
import argparse,runpy,struct,time,zlib,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import palm_boot
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--session',type=Path,required=True);p.add_argument('--core',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True)
a=p.parse_args();palm_boot.CORE_68K=a.core.resolve();r=a.evidence.resolve();r.mkdir(mode=0o700,parents=True,exist_ok=False)
chunks=runpy.run_path(str(Path(__file__).with_name('dana-session.py')))['session_chunks']
def st(p):
 raw,cs=chunks(p.read_bytes());at,n=cs[0x1e0];return struct.unpack_from(f'<{n}I',raw,at)
def open(p,name):return palm_boot.Core([p,'--device','alphasmart-dana'],r/(name+'.log'),'--speed','original')
def save(c,name):
 p=r/(name+'.frond');c.send(f'save-session "{p}"','wrote session');return p
def key(c,n,down):c.send(('dana-down ' if down else 'dana-up ')+n,'input accepted')
def tap(c,n):key(c,n,True);time.sleep(.2);key(c,n,False);time.sleep(.5)
c=open(a.session.resolve(),'setting')
try:
 c.launch('Keyboard');time.sleep(.3)
 for x,y in [(330,6),(320,62),(246,40),(270,50)]:c.tap(r,x,y);time.sleep(.3)
 assert c.frame(r/'setting.png') is not None
 tap(c,'power');assert c.frame(r/'off.png') is None
 asleep=save(c,'asleep');assert st(asleep)[297:]==(0,0,0,1,1,0,0,1,1)
finally:assert c.close()==0
c=open(asleep,'reverse')
try:
 key(c,'power',True);time.sleep(.5);key(c,'enter',True);time.sleep(.3)
 held=save(c,'reverse-held');v=st(held)
 assert v[301:303]==(1,1),v[297:]
 # The first completed wake scan contains only Power, never the late Enter.
 assert v[289]==0x0f and v[290]==0,v[281:]
 key(c,'power',False);key(c,'enter',False);time.sleep(8)
 assert c.frame(r/'reverse-rejected.png') is None
 tap(c,'a');assert c.frame(r/'letter-denied.png') is None
 key(c,'enter',True);time.sleep(.2)
 pending=r/'pending-wake.frond'
 c.send(f'dana-down power\nsave-session "{pending}"','wrote session')
 v=st(pending);assert v[16] or v[7],v[:20]
 key(c,'power',False);key(c,'enter',False);time.sleep(.5)
 assert c.frame(r/'retry-awake.png') is not None
 print('PASS real setting, fresh EB sleep, reverse-order rejection, denied letter, release and retry; natural pending wake saved',flush=True)
finally:assert c.close()==0
c=open(pending,'pending-fresh')
try:
 time.sleep(.7);assert c.frame(r/'pending-fresh.png') is not None
 base=save(c,'pending-resumed')
 # These continuation fixtures exercise the production loader with the CPU
 # asleep. Their partial SPI boundaries are synthetic, not captured transfers.
 c.send(f'load-session "{asleep}"','loaded session')
 for mode,pa3 in [(1,1),(1,0),(2,1)]:
  for cursor in [0,1,2]:
   b=asleep.read_bytes();raw,cs=chunks(b);raw=bytearray(raw);at=cs[0x1e0][0]
   struct.pack_into('<5I',raw,at+300*4,mode,0,0,0,pa3)
   if cursor:
    # A two-key immutable packet with a header or first key already clocked.
    for word,value in [(7,cursor),(8,1),(14,0),(15,0),(16,1),(25,0x6e),(26,0x0f)]:struct.pack_into('<I',raw,at+word*4,value)
   path=r/f'synthetic-{mode}-{pa3}-{cursor}.frond';path.write_bytes(b[:92]+zlib.compress(raw))
   out=r/(path.stem+'-saved.frond');neutral=r/(path.stem+'-neutral.frond')
   c.send(f'load-session "{path}"\nsave-session "{out}"\nload-session "{asleep}"\nsave-session "{neutral}"',f'wrote session to {neutral}');got=st(out)
   assert got[300:]==(mode,0,0,0,pa3,1),got[297:]
 print('PASS natural pending wake in fresh process and synthetic production mode/header/key continuation loads',flush=True)
finally:assert c.close()==0
