#!/usr/bin/env python3
"""Natural firmware SPI checkpoints through production debugger and portable sessions."""
import argparse, fcntl, os, shlex, hashlib, runpy, struct, subprocess, sys, time, zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import palm_boot
p=argparse.ArgumentParser(description=__doc__)
for name in ('core','session','probe','make-card','evidence'): p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--production-core',type=Path,required=True)
p.add_argument('--power-peer',type=int,choices=(0,1))
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
os.environ['FROND_DANA_SPI_ARM']=str(r/'armed')
os.environ['FROND_DANA_SPI_TRACE']=str(r/'wire-original')
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
import socket,json
Rsp=runpy.run_path(str(Path(__file__).with_name("dana-rsp.py")))["Rsp"]
chunks=runpy.run_path(str(Path(__file__).with_name('dana-session.py')))['session_chunks']
def sdbytes(path):
 b=path.read_bytes();h=len(b'FRONDLINKSD1\n');n=struct.unpack_from('<I',b,h)[0]
 raw,cs=chunks(b[h+12+n:])
 return [raw[cs[k][0]:cs[k][0]+cs[k][1]*4] for k in (0x1d0,0x201)]
def states(path):
 b=path.read_bytes();h=len(b'FRONDLINKSD1\n');n=struct.unpack_from('<I',b,h)[0]
 raw,cs=chunks(b[h+12+n:])
 return [struct.unpack_from('<6I',raw,cs[k][0]) for k in (0x1d0,0x201)]
def equalclockmedia(left,right):
 for slot in range(2):
  pa=r/f'{left}-{slot}.frondsd';pb=r/f'{right}-{slot}.frondsd'
  a=pa.read_bytes();b=pb.read_bytes();size=struct.unpack_from('<Q',a,16)[0]
  assert struct.unpack_from('<Q',b,16)[0]==size
  a=bytearray(a[64:64+size]);b=bytearray(b[64:64+size])
  p=32256;bps=struct.unpack_from('<H',a,p+11)[0]
  reserved=struct.unpack_from('<H',a,p+14)[0];fats=a[p+16]
  sectors=struct.unpack_from('<H',a,p+22)[0]
  root=p+(reserved+fats*sectors)*bps;allowed=set()
  entries=struct.unpack_from('<H',a,p+17)[0]
  for off in range(root,root+entries*32,32):
   if a[off:off+11] in (b'GUEST   TXT',b'COPY    TXT'):
    assert a[off:off+11]==b[off:off+11]
    allowed.update((off+22,off+23))
  assert len(allowed)==4
  differences=[i for i,(x,y) in enumerate(zip(a,b)) if x!=y]
  assert set(differences)<=allowed,differences[:40]
  for off in allowed:a[off]=b[off]=0
  assert a==b
  print('full-file SHA256 with live FAT modification times',slot,fullhash(pa),fullhash(pb),
        'clock-field offsets',differences,'canonical payload SHA256',hashlib.sha256(a).hexdigest(),flush=True)
def completed(tag):
 dest=r/(tag+'-records.pdb');c.send(f'db-export-db FrondDanaCards "{dest}"','export written',timeout=120)
 b=dest.read_bytes();n=struct.unpack_from('>H',b,76)[0]
 off=[struct.unpack_from('>I',b,78+i*8)[0] for i in range(n)]+[len(b)]
 rows=[b[off[i]:off[i+1]].rstrip(b'\0').decode() for i in range(n)]
 rows=rows[max(i for i,row in enumerate(rows) if row.startswith('begin ')):]
 assert rows[-1]=='complete err=0000 value=2',rows
 for label in ('host-open','guest-match','copy-match'):
  assert sum(row.startswith(label+' err=0000') for row in rows)==2,rows
 print(tag,rows,flush=True)
 return [row for row in rows if not row.startswith(('pd-','int-'))]
try:
 c.until(f'install "{a.probe.resolve()}"','installation ','successful')
 attach(0,0);attach(1,1);time.sleep(6);probe('initial',2)
 # Choose a free localhost port for this short-lived guest debugger.
 with socket.socket() as free:
  free.bind(('127.0.0.1',0));port=free.getsockname()[1]
 c.send(f'gdb-start {port}','gdb:')
 d=Rsp(port);print('debugger',d.cmd('?'),flush=True)

 (r/'armed').touch()
 d.put('c');time.sleep(.3);c.send('dana-down a','input accepted');c.send('dana-up a','input accepted')
 found={}
 for attempt in range(8):
  reason=d.get();assert reason.startswith(('T05','S02')),reason
  sample=r/'sample.frond';c.send(f'save-session "{sample}"','wrote session')
  ss=states(sample)
  if a.power_peer is not None:
   peer=a.power_peer;v=ss[peer]
   if v[1]>>24 == 5 and 0<v[3]<v[2]:
    original=sdbytes(sample);own=1-peer
    exports("power-before");roots=treehash(backingfiles())
    # Synthetic host GPIO stimulus around a naturally pending firmware cursor.
    # No clocks are sent, so the selected peer's complete protocol bytes must stay.
    for addr,mask in ((0xfffff441 if own else 0xfffff409,0x20),
                      (0xfffff429 if own else 0xfffff439,4 if own else 8),
                      (0xfffff43b,7)):
     old=int(d.cmd(f'm{addr:x},1'),16)
     for value in (old^mask,old):
      assert d.cmd(f'M{addr:x},1:{value:02x}')=='OK'
      out=save(f'power-{peer}-{addr:x}-{value:02x}.frond')
      assert sdbytes(out)[peer]==original[peer],(peer,addr,states(out),ss)
    assert treehash(backingfiles())==roots
    exports('power-after');equalmedia('power-before','power-after')
    print('PASS synthetic production GPIO power/select/mux stimulus preserves natural peer cursor and buffer',peer,v,flush=True)
    raise SystemExit(0)
  for slot,v in enumerate(ss):
   version,packed,size,index,block,acmd=v;spi=packed>>24
   if spi in (2,3,5) and index>0 and (slot,spi) not in found:
    tag=f'natural-{slot}-{spi}-{index}'
    target=r/(tag+'.frond');target.write_bytes(sample.read_bytes())
    folder=portable(tag)
    found[slot,spi]=(tag,folder)
    print('CAPTURE',tag,v,ss,flush=True)
  if len(found)==6:break
  d.put('c')
 else:raise AssertionError(('missing natural boundaries',list(found)))
 # Refuse malformed whole-session state while a genuine transfer is pending.
 original=sample.read_bytes();h=len(b'FRONDLINKSD1\n')
 n=struct.unpack_from('<I',original,h)[0];manifest=original[h+12:h+12+n];machine=original[h+12+n:]
 raw,cs=chunks(machine)
 def fnv(b):
  x=2166136261
  for v in b:x=((x^v)*16777619)&0xffffffff
  return x
 before=exports('rollback-before')
 for kind in (0x1d0,0x201):
  for name,word,value in [('spi-enum',1,0x07010000),('card-enum',1,0x01050000),('length',2,551),('index',3,0xffffffff),('block',4,0xffffffff),('application',5,2)]:
   b=bytearray(raw);struct.pack_into('<I',b,cs[kind][0]+4*word,value)
   image=machine[:92]+zlib.compress(b)
   path=r/f'bad-{kind:x}-{name}.frond'
   path.write_bytes(original[:h]+struct.pack('<III',len(manifest),fnv(image),fnv(manifest))+manifest+image)
   load(path,False)
   out=save(f'after-{kind:x}-{name}.frond');assert sdbytes(out)==sdbytes(sample)
   for card in cards[:2]:
    with card.open('rb') as held:
     try:fcntl.flock(held,fcntl.LOCK_EX|fcntl.LOCK_NB)
     except BlockingIOError:pass
     else:raise AssertionError('lost live backing lock')
 assert exports('rollback-after')==before
 equalmedia('rollback-before','rollback-after')

 (r/'armed').unlink()
 assert d.cmd('D')=='OK';d.s.close();time.sleep(5)
 expectedrows=completed('uninterrupted');expected=exports('uninterrupted')
 assert c.close()==0
 palm_boot.CORE_68K=a.production_core.resolve()
 c=palm_boot.Core([r/(next(iter(found.values()))[0]+'.frond'),'--device','alphasmart-dana'],r/'same-process.log','--speed','original')
 time.sleep(5)
 for (slot,spi),(tag,folder) in found.items():
  target=r/(tag+'.frond');out=r/(tag+'-roundtrip.frond')
  # Keep load and save in one console drain, before another guest slice runs.
  c.send(f'load-session "{target}"\nsave-session "{out}"','wrote session')
  assert sdbytes(out)==sdbytes(target)
  time.sleep(5);completed(tag+'-same-process');assert exports(tag+'-same-process')==expected
 palm_boot.CORE_68K=a.core.resolve()
 for (slot,spi),(tag,folder) in found.items():
  assert c.close()==0
  os.environ['FROND_DANA_SPI_REPLAY']=json.dumps(dict(slot=2 if slot else 0,phase=spi,file=str(r/(tag+'-wire.json'))))
  c=palm_boot.Core([folder,'--device','alphasmart-dana'],r/(tag+'-fresh.log'),'--speed','original')
  time.sleep(5)
  assert completed(tag+'-completed')==expectedrows
  assert json.loads((r/(tag+'-wire.json')).read_text())==json.loads((r/f'wire-original-{2 if slot else 0}-{spi}.json').read_text())
  assert exports(tag+'-completed')==expected
  # Host RTC time can change the two files' FAT modification-time words.
  equalclockmedia('uninterrupted',tag+'-completed')
 os.environ.pop('FROND_DANA_SPI_REPLAY',None)
 palm_boot.CORE_68K=a.production_core.resolve()
 # Fresh linked restores reuse current committed media history, never older bytes.
 for (slot,spi),(tag,folder) in found.items():
  assert c.close()==0
  c=palm_boot.Core([r/(tag+'.frond'),'--device','alphasmart-dana'],r/(tag+'-linked-fresh.log'),'--speed','original')
  time.sleep(5);completed(tag+'-linked-fresh');assert exports(tag+'-linked-fresh')==expected
 print('PASS six natural firmware partial SD checkpoints: exact same-process protocol, fresh portable wire/payload equality apart from FAT modification times, fresh linked guest data',flush=True)
finally:
 (r/'armed').unlink(missing_ok=True)
 status=c.close()
 if sys.exc_info()[0] in (None,SystemExit):assert status==0,status
