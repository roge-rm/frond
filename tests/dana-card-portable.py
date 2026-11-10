#!/usr/bin/env python3
"""Portable dual-media folder restore through production guest VFS and card loaders."""
import argparse, fcntl, os, shlex, hashlib, runpy, struct, subprocess, sys, time, zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import palm_boot
p=argparse.ArgumentParser(description=__doc__)
for name in ('core','session','probe','make-card','evidence'): p.add_argument('--'+name,type=Path,required=True)
p.add_argument("--protocols", choices=("sd,sd","sd,mmc","mmc,sd","mmc,mmc"), default="sd,sd")
a=p.parse_args();r=a.evidence.resolve();r.mkdir(mode=0o700,parents=True,exist_ok=False)
palm_boot.CORE_68K=a.core.resolve()
def fixture(i):
 raw=r/f'card{i}.img';subprocess.run([str(a.make_card.resolve()),str(raw)],check=True)
 host=r/f'host{i}.txt';host.write_text(f'Dana host card {i}\n')
 subprocess.run(['mlabel','-i',str(raw)+'@@32256',f'::DANA{i}'],check=True)
 subprocess.run(['mcopy','-i',str(raw)+'@@32256',str(host),'::HOST.TXT'],check=True)
 b=raw.read_bytes();h=bytearray(64);h[:8]=b'FRONDSD\0';struct.pack_into('<IIQI',h,8,1,512,len(b),zlib.crc32(b))
 dest=r/f'card{i}.frondsd';dest.write_bytes(h+b);return dest
cards=[fixture(i) for i in range(4)]
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
 if count==2:
  for slot,kind in enumerate(a.protocols.split(",")):
   label="MultiMedia Card" if kind=="mmc" else "Secure Digital Card"
   assert any(x.startswith(label+" err=0000 value="+str(slot+1)) for x in rows),rows
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
try:
 import shutil
 c.until(f'install "{a.probe.resolve()}"','installation ','successful')
 attach(0,0);attach(1,1);time.sleep(6);probe('source',2)
 expected=exports('source')
 folder=portable('portable');peek(folder)
 for alias in ('copy-session','duplicate-session'):
  other=portable(alias,alias);peek(other)
 # Existing output must remain byte-identical and failed staging must disappear.
 before=treehash(folder.iterdir())
 reply=c.send(f'export-portable "{folder}"','portable export refused:');print(reply,flush=True)
 assert treehash(folder.iterdir())==before
 assert not list(r.glob('portable.stage-*'))
 source=treehash(backingfiles());assert c.close()==0
 moved=r/'away';moved.mkdir();folder.rename(moved/'copy');folder=moved/'copy'
 c=palm_boot.Core([folder,'--device','alphasmart-dana'],r/'fresh.log','--speed','original');time.sleep(3)
 assert exports('fresh')==expected
 equalmedia('source','fresh')
 rows=probe('fresh-guest',2);assert sum(x.startswith('persisted-match err=0000') for x in rows)==2
 assert treehash(backingfiles())==source
 assert sum(x=='persisted-copy err=0000 value=1' for x in rows)==2,rows
 assert sum(x.startswith('copy-match err=0000') for x in rows)==2,rows
 exports('written-copy')
 for slot in range(2):
  assert extract(r/f'source-{slot}.frondsd','COPY.TXT')==b'1\n'
  assert extract(r/f'written-copy-{slot}.frondsd','COPY.TXT')==b'2\n'
 # Guest writes commit to the copy, and survive another process.
 assert list(folder.glob('.gen-*'))
 again=portable('again','duplicate-session');assert c.close()==0
 c=palm_boot.Core([again,'--device','alphasmart-dana'],r/'again.log','--speed','original');time.sleep(3)
 assert exports('again')==expected
 equalmedia('written-copy','again')
 for slot in range(2):assert extract(r/f'again-{slot}.frondsd','COPY.TXT')==b'2\n'
 peek(again)
 # Every rejected import must retain both current cards and release the first staged lock.
 bad=r/'malformed';shutil.copytree(r/'copy-session',bad)
 for name in ('sd0.frondsd','sd2.frondsd'):
  path=bad/name;st=path.stat();root=bad/('.gen-'+name+'-root');b=bytearray(root.read_bytes())
  struct.pack_into('<QQQ',b,28,st.st_dev,st.st_ino,st.st_mtime_ns);root.write_bytes(b)
 original=(bad/'machine.frond').read_bytes();prefix=b'FRONDPORTSD1\n';h=len(prefix)
 count=struct.unpack_from('<I',original,h)[0];manifest=original[h+12:h+12+count];machine=original[h+12+count:]
 def fnv(b):
  x=2166136261
  for v in b:x=((x^v)*16777619)&0xffffffff
  return x
 def edit(m=manifest,b=machine):
  (bad/'machine.frond').write_bytes(prefix+struct.pack('<III',len(m),fnv(b),fnv(m))+m+b)
 def reject(tag):
  states=[command(n,'state') for n in range(2)]
  print(tag,flush=True);load(bad,False);assert exports(tag)==expected
  assert [command(n,'state') for n in range(2)]==states
  for name in ('sd0.frondsd','sd2.frondsd'):
   with (again/name).open('rb') as held:
    try:fcntl.flock(held,fcntl.LOCK_EX|fcntl.LOCK_NB)
    except BlockingIOError:pass
    else:raise AssertionError('live lock released')
  for name in ('sd0.frondsd','sd2.frondsd'):
   with (bad/name).open('rb') as f:fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)
 lines=manifest.splitlines()
 edit(b'\n'.join([lines[0],lines[1],lines[1]])+b'\n');reject('duplicate-slot')
 edit(manifest.replace(b' mmc\n',b' sd\n',1) if b' mmc\n' in manifest else manifest.replace(b' sd\n',b' mmc\n',1));reject('wrong-protocol')
 edit(manifest.replace(b' mmc\n',b' invalid\n',1) if b' mmc\n' in manifest else manifest.replace(b' sd\n',b' invalid\n',1));reject('invalid-protocol')
 edit(manifest.replace(b'572662306',b'572662307'));reject('serial')
 edit(manifest.replace(b'sd2.frondsd',b'../sd2.frondsd'));reject('path')
 edit(manifest.replace(b'2 sd ',b'1 sd '));reject('slot-kind')
 edit(manifest.replace(b'2'*32,b'1'*32).replace(b'572662306',b'286331153'));reject('identity')
 edit(manifest.replace(b'33554432',b'33554944',1));reject('geometry')
 edit(manifest.replace(b'2 sd ',b'2 nand '));reject('nand-kind')
 edit(manifest.replace(b'33554432 0 1',b'33554432 2 1',1));reject('readonly')
 edit(manifest.replace(b'33554432 0 1',b'33554432 0 0',1));reject('insertion')
 broken=bytearray(original);broken[35]^=1;(bad/'machine.frond').write_bytes(broken);reject('checksum')
 edit()
 payload=bad/'sd2.frondsd';payloadstat=payload.stat()
 with payload.open('r+b') as f:f.seek(100);byte=f.read(1);f.seek(100);f.write(bytes([byte[0]^1]))
 reject('payload')
 with payload.open('r+b') as f:f.seek(100);f.write(byte)
 os.utime(payload,ns=(payloadstat.st_atime_ns,payloadstat.st_mtime_ns))
 root=bad/'.gen-sd2.frondsd-root';rootbytes=root.read_bytes();root.write_bytes(b'invalid committed root');reject('root');root.unlink();reject('missing-root');root.write_bytes(rootbytes)
 # Validate the complete staged generation before replacing either live attachment.
 rootid=struct.unpack_from('<Q',rootbytes,12)[0]
 # This standalone copied fixture is pristine; add a labelled synthetic overlay
 # with identical bytes to exercise the real generation staging path.
 if rootid==0:
  rootid=1;b=bytearray(rootbytes);struct.pack_into('<Q',b,12,rootid);root.write_bytes(b)
  header=bytearray(64);header[:8]=b'FRONDGEN';struct.pack_into('<IIQQQQQQ',header,8,1,4096,33554432,1,0,1,4160,64)
  (bad/'.gen-sd2.frondsd-1').write_bytes(header+payload.read_bytes()[64:4160]+struct.pack('<QQ',0,64))
 generation=bad/('.gen-sd2.frondsd-'+str(rootid));goodgen=generation.read_bytes()
 index=struct.unpack_from('<Q',goodgen,48)[0]
 for tag,cut in [('generation-header',12),('generation-index',len(goodgen)-1),('generation-payload',index-1)]:
  generation.write_bytes(goodgen[:cut]);before=generation.read_bytes()
  reject(tag);assert generation.read_bytes()==before
 for tag,offset,value in [('generation-cycle',32,rootid),('generation-page',index,0xffffffffffffffff),('generation-offset',index+8,index),('generation-count',40,0xffffffffffffffff)]:
  b=bytearray(goodgen);struct.pack_into('<Q',b,offset,value);generation.write_bytes(b)
  reject(tag);assert generation.read_bytes()==b
 generation.write_bytes(goodgen)
 with payload.open('rb') as held:
  fcntl.flock(held,fcntl.LOCK_EX|fcntl.LOCK_NB)
  load(bad,False)
 with (bad/'sd0.frondsd').open('rb') as f:fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)
 # Fully valid import after failures, including rollback for a late machine error.
 chunks=runpy.run_path(str(Path(__file__).with_name('dana-session.py')))['session_chunks']
 raw,cs=chunks(machine)
 broken=bytearray(raw);struct.pack_into('<I',broken,cs[0x201][0]+12,0xffffffff)
 edit(b=machine[:92]+zlib.compress(broken));reject('sd2-protocol')
 broken=bytearray(raw);struct.pack_into('<I',broken,cs[0x1e0][0],99)
 edit(b=machine[:92]+zlib.compress(broken));reject('machine-rollback');edit()
 load(bad);assert exports('imported')==expected
 command(1,'eject',expected='ejected');time.sleep(2)
 # Reattach through the normal physical path so the guest learns the protection policy.
 detach(0)
 command(0,'attach',f'"{bad}/sd0.frondsd" '+str(1)*32+' protected '+a.protocols.split(',')[0],'inserted')
 time.sleep(4)
 protected=portable('protected');assert c.close()==0
 c=palm_boot.Core([protected,'--device','alphasmart-dana'],r/'protected.log','--speed','original');time.sleep(3)
 command(0,'state',expected='protected');command(1,'state',expected='ejected')
 before=hashlib.sha256((protected/'sd0.frondsd').read_bytes()).hexdigest()
 probe('protected',1,True);command(1,'insert',expected='inserted');time.sleep(5);probe('reinserted',2,True)
 assert exports('protected-final')==expected
 assert hashlib.sha256((protected/'sd0.frondsd').read_bytes()).hexdigest()==before
 assert treehash(backingfiles())==source
 print('PASS portable fresh dual guest reads/writes, moved folder, source isolation, duplicate/import/peek, protected/ejected, malformed rollback and lock cleanup',flush=True)
finally:
 status=c.close()
 if sys.exc_info()[0] is None:assert status==0,status
