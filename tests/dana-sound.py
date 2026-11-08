#!/usr/bin/env python3
"""Focused Dana sound checks; needs a private ready session and dana-sound probe."""
from pathlib import Path
import sys,time,struct,zlib,runpy,array,wave,argparse
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import palm_boot
parser=argparse.ArgumentParser(description="Dana guest sound and saved-tone checks")
parser.add_argument('--core',type=Path,required=True)
parser.add_argument('--session',type=Path,required=True)
parser.add_argument('--probe',type=Path,required=True)
parser.add_argument('--evidence',type=Path,required=True)
args=parser.parse_args()
palm_boot.CORE_68K=args.core.resolve()
r=args.evidence.resolve();r.mkdir(mode=0o700,parents=True,exist_ok=False)
session=args.session.resolve();probe=args.probe.resolve()
def open(p,tag):return palm_boot.Core([p,'--device','alphasmart-dana'],r/(tag+'.log'),'--speed','original','--audio-output',r/(tag+'.wav'),'--tone-log')
def save(c,name):
 p=r/(name+'.frond');c.send(f'save-session "{p}"','wrote session');return p
def records(c):
 p=r/'log.pdb';c.send(f'db-export-db FrondDanaSoundLog "{p}"','export written');b=p.read_bytes();n=struct.unpack_from('>H',b,76)[0];a=[struct.unpack_from('>I',b,78+i*8)[0] for i in range(n)]+[len(b)];return [b[a[i]:a[i+1]].rstrip(b'\0').decode() for i in range(n)]
def key(c,n):
 c.send('dana-down '+n,'input accepted');time.sleep(.15);c.send('dana-up '+n,'input accepted');time.sleep(.2)
c=open(session,'calls')
try:
 c.until(f'install "{probe}"','installation ','successful');c.launch('Frond Dana sound')
 end=time.monotonic()+15
 while True:
  rows=records(c)
  if any('pending1200' in x for x in rows):break
  assert time.monotonic()<end;time.sleep(.1)
 print('\n'.join(rows),flush=True)
 assert all('err=0' in row for row in rows)
 pending=save(c,'pending');time.sleep(.3);key(c,'q');time.sleep(.5)
 c.send(f'load-session "{pending}"','loaded session');time.sleep(.4)
 chunks=runpy.run_path(str(Path(__file__).with_name('dana-session.py')))['session_chunks']
 b=pending.read_bytes();raw,cs=chunks(b);raw=bytearray(raw);struct.pack_into('<I',raw,cs[0x1e0][0]+297*4,10);bad=r/'bad.frond';bad.write_bytes(b[:92]+zlib.compress(raw))
 assert 'machine restored' in c.send(f'load-session "{bad}"','session refused:');time.sleep(.4)
 key(c,'q');time.sleep(.3)
 key(c,'t');time.sleep(.2);c.send('reset','reset');time.sleep(1)
 print('PASS calls, pending save/load, malformed rollback and reset',flush=True)
finally:assert c.close()==0
c=open(pending,'fresh')
try:
 time.sleep(.5);key(c,'q');time.sleep(.3);key(c,'t');time.sleep(.2);key(c,'power');time.sleep(1);assert c.frame(r/'off.png') is None
 asleep=save(c,'asleep');time.sleep(.5);key(c,'a');assert c.frame(r/'denied.png') is None;key(c,'power');assert c.frame(r/'awake.png') is not None
 print('PASS fresh pending tone and ordinary sleep/wake',flush=True)
finally:assert c.close()==0
c=open(asleep,'asleep')
try:time.sleep(.5);assert c.frame(r/'fresh-off.png') is None
finally:assert c.close()==0
print('PASS fresh asleep silence candidate',flush=True)

c=open(pending,'expiry')
try:time.sleep(9)
finally:assert c.close()==0
measure=runpy.run_path(str(Path(__file__).with_name('frond-68k-peripherals.py')))['tones']
calls=measure(r/'calls.wav');fresh=measure(r/'fresh.wav');expiry=measure(r/'expiry.wav')
assert len(calls)==8,calls
for (_,duration,hz),target in zip(calls[:5],[440,1000,440,200,500]):
    assert abs(hz-target)<max(6,target*.02),(hz,target)
for _,duration,_ in calls[:3]:assert abs(duration-.3)<.04,duration
assert abs(calls[3][1]-.02)<.01 and abs(calls[4][1]-.08)<.02,calls
for _,duration,hz in calls[5:]:assert abs(hz-1200)<24 and duration>.2
assert len(fresh)==2 and all(abs(hz-1200)<24 for _,_,hz in fresh),fresh
assert len(expiry)==1 and abs(expiry[0][2]-1200)<24 and 7<expiry[0][1]<8.1,expiry
for tag in ('calls','fresh','asleep','expiry'):
    with wave.open(str(r/(tag+'.wav'))) as w:
        rate=w.getframerate();pcm=array.array('h',w.readframes(w.getnframes()))
    assert rate==44100
    if tag=='calls':
        muteStart=calls[4][0]+calls[4][1]+.1;muteEnd=calls[5][0]-.1
        assert muteEnd-muteStart>.4 and not any(pcm[int(muteStart*rate):int(muteEnd*rate)]),'guest mute'
        assert not any(pcm[int((calls[-1][0]+calls[-1][1]+.1)*rate):]),'reset silence'
    if tag=='asleep':assert not any(pcm),'fresh asleep'
    if tag=='fresh':assert not any(pcm[int((fresh[-1][0]+fresh[-1][1]+.1)*rate):]),'ordinary sleep and wake stay quiet'
    if tag=='expiry':assert not any(pcm[int(8.3*rate):]),'guest duration expires'
print('PASS measured PCM: calls, mute, quiet, state continuation, reset, sleep and expiry',flush=True)
print('calls',calls,'fresh',fresh,'expiry',expiry,flush=True)
