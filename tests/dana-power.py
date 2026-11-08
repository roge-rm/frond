#!/usr/bin/env python3
"""Focused Dana power and saved-protocol checks with a private ready session."""
import argparse
from pathlib import Path
import sys,time,struct,zlib,runpy
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import palm_boot
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--session',type=Path,required=True)
parser.add_argument('--core',type=Path)
parser.add_argument('--evidence',type=Path,required=True)
args=parser.parse_args()
if args.core:palm_boot.CORE_68K=args.core.resolve()
r=args.evidence.resolve();r.mkdir(mode=0o700,parents=True,exist_ok=False)
chunks=runpy.run_path(str(Path(__file__).with_name('dana-session.py')))['session_chunks']
def st(p):
 raw,cs=chunks(p.read_bytes());at,words=cs[0x1e0];return struct.unpack_from(f'<{words}I',raw,at)
def open(p,tag):return palm_boot.Core([p,'--device','alphasmart-dana'],r/(tag+'.log'),'--speed','original')
def save(c,name):
 p=r/(name+'.frond');c.send(f'save-session "{p}"','wrote session');return p
def tap(c,n):
 c.send('dana-down '+n,'input accepted');time.sleep(.2);c.send('dana-up '+n,'input accepted');time.sleep(.4)
c=open(args.session.resolve(),'initial')
try:
 assert c.frame(r/'initial.png') is not None
 before=c.send('backlight-state','backlight:').strip()
 for i in range(2):
  c.send('dana-down power','input accepted');time.sleep(3)
  c.send('dana-up power','input accepted');time.sleep(.3)
  after=c.send('backlight-state','backlight:').strip()
  assert (after!=before)==(i==0)
  assert c.frame(r/f'backlight-{i}.png') is not None
 print('PASS guest long-hold backlight on/off without sleeping',flush=True)
 tap(c,'power');assert c.frame(r/'initial-off.png') is None
 base=save(c,'initial-asleep')
finally:assert c.close()==0
c=open(base,'fresh-asleep')
try:
 assert c.frame(r/'fresh-asleep.png') is None
 saved=save(c,'asleep');assert st(saved)[300]==1
 for name,word,value in [('held-range',14,0xffffffff),('sleep-cursor',297,10),('mask0',298,256),('mask1',299,256),('sleep-state',300,3),('sleep-busy',297,1),('sleep-version-busy',3,1),('sleep-packet-busy',7,1),('eb-range',301,2),('eb-inconsistent',302,1),('group-range',303,8),('group-inconsistent',303,1),('pa3-range',304,2),('edge-range',305,2),('common-level',305,0)]:
  b=saved.read_bytes();raw,cs=chunks(b);raw=bytearray(raw);struct.pack_into('<I',raw,cs[0x1e0][0]+4*word,value);p=r/(name+'.frond');p.write_bytes(b[:92]+zlib.compress(raw))
  assert 'machine restored' in c.send(f'load-session "{p}"','session refused:')
  assert c.frame(r/(name+'.png')) is None
  assert st(save(c,name+'-restored'))[297:]==st(saved)[297:]
 print('PASS fresh-process asleep and malformed power-field production rollbacks',flush=True)
 # The halted guest does not clock SPI: production load/save retains every partial command.
 for cursor in range(1,10):
  b=saved.read_bytes();raw,cs=chunks(b);raw=bytearray(raw);at=cs[0x1e0][0];struct.pack_into('<4I',raw,at+297*4,cursor,9 if cursor==9 else 0,0,0);p=r/f'partial-{cursor}.frond';p.write_bytes(b[:92]+zlib.compress(raw))
  c.send(f'load-session "{p}"','loaded session');got=st(save(c,f'partial-{cursor}-saved'));assert got[297:]==(cursor,9 if cursor==9 else 0,0,0,0,0,0,1,1),got[297:]
 print('PASS all nine partial-command/mask production load/save boundaries',flush=True)
 c.send(f'load-session "{saved}"','loaded session');tap(c,'power');assert c.frame(r/'awake.png') is not None
 # Focus while asleep must release physical host ownership without asserting wake.
 tap(c,'power');assert c.frame(r/'asleep-again.png') is None
 c.send('dana-down a','input accepted');c.send('hidden on','hidden: on');c.send('hidden off','hidden: off');assert c.frame(r/'focus-asleep.png') is None
 c.send('reset','reset');time.sleep(2);assert c.frame(r/'reset-awake.png') is not None
 print('PASS allowed wake and soft reset while asleep',flush=True)
finally:assert c.close()==0
