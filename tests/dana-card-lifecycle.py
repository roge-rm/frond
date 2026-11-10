#!/usr/bin/env python3
"""Dual-card guest reset and sleep lifecycle through production session loaders."""
import argparse, fcntl, os, shlex, hashlib, runpy, struct, subprocess, sys, time, zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import palm_boot
p=argparse.ArgumentParser(description=__doc__)
for name in ('core','session','probe','make-card','evidence'): p.add_argument('--'+name,type=Path,required=True)
p.add_argument("--protocols",default="sd,sd",choices=("sd,sd","mmc,mmc","sd,mmc","mmc,sd"))
a=p.parse_args();r=a.evidence.resolve();r.mkdir(mode=0o700,parents=True,exist_ok=False)
palm_boot.CORE_68K=a.core.resolve()
def fixture(i):
 raw=r/f'card{i}.img';subprocess.run([str(a.make_card.resolve()),str(raw)],check=True)
 host=r/f'host{i}.txt';host.write_text(f'Dana host card {i}\n')
 subprocess.run(['mlabel','-i',str(raw)+'@@32256',f'::DANA{i}'],check=True)
 subprocess.run(['mcopy','-i',str(raw)+'@@32256',str(host),'::HOST.TXT'],check=True)
 b=raw.read_bytes();h=bytearray(64);h[:8]=b'FRONDSD\0';struct.pack_into('<IIQI',h,8,1,512,len(b),zlib.crc32(b))
 dest=r/f'card{i}.frondsd';dest.write_bytes(h+b);return dest
cards=[fixture(i) for i in range(2)]
c=palm_boot.Core([a.session.resolve(),'--device','alphasmart-dana'],r/'core.log','--speed','original')
def command(slot,op,args='',expected=None):
 reply=c.send(f'{"sd2" if slot else "sd"}-{op} {args}'.rstrip(),'card:').strip()
 print(reply,flush=True)
 if expected: assert expected in reply,reply
 else: assert 'error:' not in reply,reply
 return reply
def attach(slot,index,policy='writable'):
 command(slot,'attach',f'"{cards[index]}" {str(index+1)*32} {policy} {a.protocols.split(chr(44))[slot]}','inserted')
def probe(tag,count,protected=False):
 c.launch('Frond Dana cards');time.sleep(3)
 dest=r/f'{tag}.pdb';c.send(f'db-export-db FrondDanaCards "{dest}"','export written')
 b=dest.read_bytes();n=struct.unpack_from('>H',b,76)[0];off=[struct.unpack_from('>I',b,78+i*8)[0] for i in range(n)]+[len(b)]
 rows=[b[off[i]:off[i+1]].rstrip(b'\0').decode() for i in range(n)]
 start=max(i for i,row in enumerate(rows) if row.startswith('begin '));rows=rows[start:]
 print(tag,rows,flush=True)
 assert rows[-1]==f'complete err=0000 value={count}',rows
 assert sum(x.startswith('volume-info err=0000') for x in rows)==count
 assert sum(x.startswith('host-open err=0000') for x in rows)==count
 assert sum(x.startswith('guest-match err=0000') for x in rows)==count-int(protected),rows
 if protected: assert any(x.startswith('guest-open err=2907') for x in rows),rows
 c.frame(r/f'{tag}.png')
 return rows
def extract(path,name):
 return subprocess.check_output(['mtype','-i',str(path)+'@@32320','::'+name])
def detach(slot):
 command(slot,'eject',expected='ejected');time.sleep(1);command(slot,'take-out',expected='taken-out')
def save(name):
 dest=r/name
 print(c.send(f'save-session "{dest}"','wrote session'),flush=True)
 assert dest.read_bytes().startswith(b'FRONDLINKSD1\n')
 return dest
def load(path,ok=True):
 line=c.send(f'load-session "{path}"',('loaded session','session refused:'))
 print(line,flush=True);assert ('loaded session' in line)==ok,line
def exports(tag):
 hashes=[]
 for slot in range(2):
  dest=r/f'{tag}-{slot}.frondsd';command(slot,'export',f'"{dest}"','exported')
  for name in ('HOST.TXT','GUEST.TXT'):
   payload=extract(dest,name);hashes.append(hashlib.sha256(payload).hexdigest())
   print(tag,slot,name,hashes[-1],flush=True)
 return hashes
def fullhash(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  while chunk:=f.read(1<<20):h.update(chunk)
 return h.hexdigest()
def equalmedia(left,right):
 for slot in range(2):
  a=fullhash(r/f'{left}-{slot}.frondsd');b=fullhash(r/f'{right}-{slot}.frondsd')
  print('whole-media SHA256',left,right,slot,a,b,flush=True);assert a==b

def portable(name,op='export-portable'):
 dest=r/name
 reply=c.send(f'{op} "{dest}"',('wrote portable folder','portable export refused:'))
 print(reply,flush=True);assert 'wrote portable folder' in reply,reply
 assert (dest/'machine.frond').read_bytes().startswith(b'FRONDPORTSD1\n')
 return dest
def treehash(paths):
 return {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
def backingfiles():
 return [p for p in r.iterdir() if p.name.startswith('card') and p.suffix=='.frondsd' or p.name.startswith('.gen-card')]
def peek(path,ok=True):
 reply=c.send(f'peek-session "{path}"',('session media:','peek refused:'))
 print(reply,flush=True)
 assert ('session media: 2 SD cards; slots SD0=inserted SD2=inserted' in reply)==ok,reply
original_launcher=palm_boot.dana_launcher
def launcher_ready(rows):
 if original_launcher(rows):return True
 # Palm OS can select an empty external-card category after reset.
 return (palm_boot.dana_geometry(rows) and
         sum(palm_boot.iiix_dark(rows,x,13) for x in range(560))>550 and
         any(palm_boot.iiix_dark(rows,x,6) for x in range(510,520)) and
         sum(palm_boot.iiix_dark(rows,x,y) for x in range(560) for y in range(20,150))<10)
palm_boot.dana_launcher=launcher_ready
try:
 c.until(f'install "{a.probe.resolve()}"','installation ','successful')
 attach(0,0);attach(1,1);time.sleep(6);probe('baseline',2)
 expected=exports('baseline')
 def checked(tag,count=2,protected=False):
  rows=probe(tag,count,protected)
  assert sum(x.startswith('persisted-match err=0000') for x in rows)==count,rows
  assert exports(tag)==expected
 def reopen(path,tag):
  global c
  assert c.close()==0
  c=palm_boot.Core([path,'--device','alphasmart-dana'],r/(tag+'.log'),'--speed','original');time.sleep(3)
 def erased(tag):
  reply=c.send('launch "Frond Dana cards"','launch: ');assert 'started' not in reply,reply
  dest=r/(tag+'-erased.pdb')
  reply=c.send(f'db-export-db FrondDanaCards "{dest}"','export ')
  assert 'export failed: no database named FrondDanaCards' in reply,reply
  assert not dest.exists()
 def power():
  c.send('dana-down power','input accepted');time.sleep(.2)
  c.send('dana-up power','input accepted');time.sleep(1)
 power();assert c.frame(r/'asleep.png') is None
 asleep=save('asleep.frond');reopen(asleep,'sleep-fresh')
 assert c.frame(r/'fresh-asleep.png') is None
 power();assert c.frame(r/'awake.png') is not None
 checked('wake-fresh')
 palm_boot.dana_reset(c,r,False);checked('soft-reset')
 palm_boot.dana_reset(c,r,True);erased('hard')
 c.until(f'install "{a.probe.resolve()}"','installation ','successful')
 checked('hard-reset')
 for slot in range(2):
  command(slot,'eject',expected='ejected');time.sleep(3)
  checked(f'eject-{slot}',1)
  command(slot,'insert',expected='inserted');time.sleep(4)
  checked(f'reinsert-{slot}')
 detach(0);attach(0,0,'protected');time.sleep(4)
 command(1,'eject',expected='ejected');time.sleep(3)
 checked('protected-ejected',1,True)
 exports('readonly-before')
 power();assert c.frame(r/'protected-asleep.png') is None
 protected_linked=save('protected-asleep.frond');reopen(protected_linked,'protected-linked-fresh')
 assert c.frame(r/'protected-linked-asleep.png') is None
 power();checked('protected-linked-wake',1,True)
 power();assert c.frame(r/'protected-portable-asleep.png') is None
 protected=portable('protected-asleep');reopen(protected,'protected-sleep-fresh')
 power();checked('protected-wake',1,True)
 palm_boot.dana_reset(c,r,False);checked('protected-soft',1,True)
 palm_boot.dana_reset(c,r,True);erased('protected-hard')
 c.until(f'install "{a.probe.resolve()}"','installation ','successful')
 checked('protected-hard',1,True)
 command(1,'insert',expected='inserted');time.sleep(4)
 checked('protected-both',2,True)
 exports('readonly-after');assert fullhash(r/'readonly-before-0.frondsd')==fullhash(r/'readonly-after-0.frondsd')
 print('PASS guest dual-card sleep/fresh wake, soft/hard reset, independent eject/reinsert and protected/ejected portable restore',flush=True)
finally:
 status=c.close()
 if sys.exc_info()[0] is None:assert status==0,status
