#!/usr/bin/env python3
"""Synthetic simultaneous SD cursors, production pair load and failed-load rollback."""
import argparse, fcntl, os, shlex, hashlib, runpy, struct, subprocess, sys, time, zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import palm_boot
p=argparse.ArgumentParser(description=__doc__)
for name in ('core','session','probe','make-card','evidence'): p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--checkpoints',type=Path,required=True)
p.add_argument('--asleep',type=Path,required=True)
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
 command(slot,'attach',f'"{cards[index]}" {str(index+1)*32} {policy}','inserted')
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
chunks=runpy.run_path(str(Path(__file__).with_name('dana-session.py')))['session_chunks']
def split(path):
 b=path.read_bytes();h=len(b'FRONDLINKSD1\n');n=struct.unpack_from('<I',b,h)[0]
 return b[:h],b[h+12:h+12+n],b[h+12+n:]
def fnv(b):
 x=2166136261
 for v in b:x=((x^v)*16777619)&0xffffffff
 return x
def wrap(path,prefix,manifest,machine):
 path.write_bytes(prefix+struct.pack('<III',len(manifest),fnv(machine),fnv(manifest))+manifest+machine)
 return path
def protocol(path):
 _,_,b=split(path);raw,cs=chunks(b)
 return [raw[cs[k][0]:cs[k][0]+4*cs[k][1]] for k in (0x1d0,0x201)]
try:
 c.until(f'install "{a.probe.resolve()}"','installation ','successful')
 attach(0,0);attach(1,1);time.sleep(6);probe('initial',2)
 expected=exports('initial');awake=save('awake.frond')
 prefix,manifest,_=split(awake)
 _,_,sleepmachine=split(a.asleep)
 raw,cs=chunks(sleepmachine);raw=bytearray(raw)
 for slot,kind in ((0,0x1d0),(1,0x201)):
  natural=next(a.checkpoints.glob(f'natural-{slot}-5-*.frond'))
  payload=protocol(natural)[slot];at,words=cs[kind]
  assert len(payload)==4*words
  raw[at:at+len(payload)]=payload
 synthetic=wrap(r/'synthetic-both-write.frond',prefix,manifest,sleepmachine[:92]+zlib.compress(raw))
 pair=protocol(synthetic);load(synthetic)
 assert protocol(save('synthetic-same.frond'))==pair
 assert c.close()==0
 c=palm_boot.Core([synthetic,'--device','alphasmart-dana'],r/'synthetic-fresh.log','--speed','original');time.sleep(2)
 assert c.frame(r/'synthetic-asleep.png') is None
 assert protocol(save('synthetic-fresh.frond'))==pair
 assert exports('before-refusals')==expected
 roots=treehash(backingfiles())
 cases=[]
 for kind in (0x1d0,0x201):
  for name,word,value in [('spi',1,0x07010000),('card',1,0x01050000),('length',2,551),('index',3,0xffffffff),('block',4,0xffffffff),('application',5,2)]:
   bad=bytearray(raw);struct.pack_into('<I',bad,cs[kind][0]+4*word,value)
   cases.append((f'{kind:x}-{name}',manifest,sleepmachine[:92]+zlib.compress(bad)))
  table=sum(struct.unpack_from('<5Q',sleepmachine,48)[:4])
  for entry in range(struct.unpack_from('<I',raw,table)[0]):
   at=table+4+8*entry;identifier,words=struct.unpack_from('<II',raw,at)
   if identifier==kind:
    bad=bytearray(raw);struct.pack_into('<I',bad,at+4,words+1)
    cases.append((f'{kind:x}-chunk-length',manifest,sleepmachine[:92]+zlib.compress(bad)))
 bad=bytearray(raw);struct.pack_into('<I',bad,cs[0x1e0][0],99)
 cases.append(('late-machine',manifest,sleepmachine[:92]+zlib.compress(bad)))
 cases.append(('descriptor',manifest.replace(b'33554432',b'33554944',1),sleepmachine[:92]+zlib.compress(raw)))
 for tag,m,b in cases:
  path=wrap(r/f'bad-{tag}.frond',prefix,m,b);load(path,False)
  assert protocol(save('after-'+tag+'.frond'))==pair
  for card in cards[:2]:
   with card.open('rb') as held:
    try:fcntl.flock(held,fcntl.LOCK_EX|fcntl.LOCK_NB)
    except BlockingIOError:pass
    else:raise AssertionError('lost owned lock')
 assert treehash(backingfiles())==roots
 assert exports('after-refusals')==expected
 equalmedia('before-refusals','after-refusals')
 load(awake);probe('valid-after-refusals',2);assert exports('valid-after-refusals')==expected
 print('PASS synthetic simultaneous pending write buffers, fresh providers, malformed SD0/SD2 and late-machine rollback, no tentative media commit, valid guest retry',flush=True)
finally:
 status=c.close()
 if sys.exc_info()[0] is None:assert status==0,status
