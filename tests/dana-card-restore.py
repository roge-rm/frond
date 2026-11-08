#!/usr/bin/env python3
"""Native linked-session dual restore through production card and chunk loaders."""
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
try:
 c.until(f'install "{a.probe.resolve()}"','installation ','successful')
 attach(0,0);attach(1,1);time.sleep(6)
 prewrite=save('before-write.frond');probe('before',2)
 expected=exports('before')
 load(prewrite);assert exports('nonrewind')==expected
 saved=save('dual.frond');load(saved);assert exports('same-process')==expected
 assert c.close()==0
 c=palm_boot.Core([saved,'--device','alphasmart-dana'],r/'fresh.log','--speed','original')
 time.sleep(3)
 for slot in range(2):command(slot,'state',expected='inserted '+str(slot+1)*32)
 assert exports('fresh')==expected
 rows=probe('fresh-guest',2)
 assert sum(x.startswith('persisted-match err=0000') for x in rows)==2,rows
 # A damaged second descriptor cannot disturb either live backing.
 bad=bytearray(saved.read_bytes());bad[40]^=1;(r/'bad.frond').write_bytes(bad)
 load(r/'bad.frond',False);assert exports('after-bad')==expected
 # Recompute checksums so failures reach descriptor/backing/protocol validation.
 original=saved.read_bytes();prefix=b'FRONDLINKSD1\n';h=len(prefix)
 length=struct.unpack_from('<I',original,h)[0]
 manifest=original[h+12:h+12+length];machine=original[h+12+length:]
 def fnv(b):
  x=2166136261
  for v in b:x=((x^v)*16777619)&0xffffffff
  return x
 def edited(tag,m=manifest,b=machine):
  path=r/(tag+'.frond');path.write_bytes(prefix+struct.pack('<III',len(m),fnv(b),fnv(m))+m+b);return path
 def rejected(tag,path):
  states=[command(n,'state') for n in range(2)]
  load(path,False);assert exports(tag)==expected
  assert [command(n,'state') for n in range(2)]==states
  for card in cards[:2]:
   with card.open('rb') as held:
    try:fcntl.flock(held,fcntl.LOCK_EX|fcntl.LOCK_NB)
    except BlockingIOError:pass
    else:raise AssertionError('live lock released')
 legacy=('frond-linked-sd 1\n'+'\n'.join(x.rsplit(' ',2)[0] for x in manifest.decode().splitlines()[1:])+'\n').encode()
 rejected('legacy-v1-without-history-proof',edited('legacy-v1',legacy));load(saved)
 legacy=('frond-linked-sd 2\n'+'\n'.join(x.rsplit(' ',1)[0] for x in manifest.decode().splitlines()[1:])+'\n').encode()
 rejected('legacy-v2-without-history-proof',edited('legacy-v2',legacy));load(saved)
 lines=manifest.decode().splitlines()
 rejected('duplicate-slot',edited('duplicate-slot',('\n'.join([lines[0],lines[1],lines[1]])+'\n').encode()))
 rejected('wrong-protocol',edited('wrong-protocol',manifest.replace(b' mmc ',b' sd ',1) if b' mmc ' in manifest else manifest.replace(b' sd ',b' mmc ',1)))
 rejected('invalid-protocol',edited('invalid-protocol',manifest.replace(b' mmc ',b' invalid ',1) if b' mmc ' in manifest else manifest.replace(b' sd ',b' invalid ',1)))
 rejected('serial',edited('serial',manifest.replace(b'572662306',b'572662307')))
 badlines=[shlex.split(x) for x in lines[1:]]
 badlines[1][10]=str(int(badlines[1][10])+1)
 for fields in badlines:fields[2]='"'+fields[2]+'"'
 rejected('replaced-source',edited('replaced-source',(lines[0]+'\n'+'\n'.join(' '.join(x) for x in badlines)+'\n').encode()))
 # Owned hardlinks in another directory must not redirect the generation namespace.
 aliasdir=r/'aliases';aliasdir.mkdir();alias=aliasdir/cards[0].name;os.link(cards[0],alias)
 rejected('owned-hardlink-namespace',edited('owned-hardlink-namespace',manifest.replace(str(cards[0]).encode(),str(alias).encode())))
 normal=str(cards[0].parent)+'/./'+cards[0].name
 load(edited('normalised-path',manifest.replace(str(cards[0]).encode(),normal.encode())));assert exports('normalised-path')==expected;load(saved)
 root=cards[0].with_name('.gen-'+cards[0].name+'-root');rootbytes=root.read_bytes()
 root.unlink();rejected('owned-missing-root',saved);assert not root.exists();root.write_bytes(rootbytes)
 gens={p:p.read_bytes() for p in r.glob('.gen-'+cards[0].name+'-*')}
 for path in gens:path.unlink()
 rejected('owned-missing-entire-history',saved)
 for path,data in gens.items():path.write_bytes(data)
 load(saved)
 alternate=manifest.replace(str(cards[0]).encode(),str(cards[2]).encode()).replace(b'1'*32,b'3'*32).replace(b'286331153',b'858993459')
 alternate=alternate.replace(str(cards[1]).encode(),str(cards[3]).encode()).replace(b'2'*32,b'4'*32).replace(b'572662306',b'1145324612')
 altlines=alternate.decode().splitlines()
 for index in range(2):
  fields=shlex.split(altlines[index+1]);st=os.stat(cards[index+2])
  fields[2]='"'+fields[2]+'"';fields[8:11]=map(str,(st.st_dev,st.st_ino,st.st_mtime_ns));fields[12]='0'
  # A deliberately pristine alternate fixture has its own committed base marker.
  base=cards[index+2];key=fields[3];binding=1469598103934665603
  for byte in key.encode()+b'\0'+base.read_bytes()[:64]:binding=((binding^byte)*1099511628211)&0xffffffffffffffff
  marker=bytearray(64);marker[:8]=b'FRONDRT\0';struct.pack_into('<IQQQQQQI',marker,8,2,0,33554432,st.st_dev,st.st_ino,st.st_mtime_ns,binding,4096)
  base.with_name('.gen-'+base.name+'-root').write_bytes(marker)
  altlines[index+1]=' '.join(fields)
 alternate=('\n'.join(altlines)+'\n').encode()
 with cards[3].open('rb') as held:
  fcntl.flock(held,fcntl.LOCK_EX|fcntl.LOCK_NB)
  rejected('locked-second',edited('locked-second',alternate))
 for path in (cards[2],cards[3]):
  with path.open('rb') as f:fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)
 root=cards[3].with_name('.gen-'+cards[3].name+'-root');root.write_bytes(b'invalid committed root')
 rejected('root-second',edited('root-second',alternate));root.unlink()
 for path in (cards[2],cards[3]):
  with path.open('rb') as f:fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)
 chunks=runpy.run_path(str(Path(__file__).with_name('dana-session.py')))['session_chunks']
 raw,cs=chunks(machine)
 for tag,kind,word,value in [('sd2-cursor',0x201,3,0xffffffff),('st-rollback',0x1e0,0,99)]:
  bad=bytearray(raw);struct.pack_into('<I',bad,cs[kind][0]+4*word,value)
  rejected(tag,edited(tag,b=machine[:92]+zlib.compress(bad)))
 # Restore a changed immutable protection policy through the same owned lock.
 protected_manifest=manifest.replace(b'33554432 0 1',b'33554432 1 1',1)
 load(edited('policy-reuse',protected_manifest));command(0,'state',expected='protected')
 load(saved);command(0,'state',expected='writable')
 detach(0);attach(0,0,'protected');time.sleep(4)
 command(1,'eject',expected='ejected');time.sleep(2)
 protected=save('protected-ejected.frond');load(protected)
 assert c.close()==0
 c=palm_boot.Core([protected,'--device','alphasmart-dana'],r/'protected-fresh.log','--speed','original');time.sleep(3)
 command(0,'state',expected='inserted '+str(1)*32+' protected')
 command(1,'state',expected='ejected '+str(2)*32+' writable')
 assert exports('protected-fresh')==expected
 rows=probe('protected-guest',1,True)
 assert sum(x.startswith('persisted-match err=0000') for x in rows)==1,rows
 command(1,'insert',expected='inserted');time.sleep(5);probe('both-again',2,True)
 assert exports('final')==expected
 assert hashlib.sha256((r/'protected-fresh-0.frondsd').read_bytes()).digest()==hashlib.sha256((r/'final-0.frondsd').read_bytes()).digest()
 print('PASS native save, same-process handle reuse, fresh dual guest restore, protected/ejected restore and damaged-load retention',flush=True)
finally:
 status=c.close()
 if sys.exc_info()[0] is None:assert status==0,status
