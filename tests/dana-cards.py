#!/usr/bin/env python3
"""Private Dana guest gate through the public runtime card interface."""
import argparse, hashlib, struct, subprocess, sys, time, zlib
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
try:
 c.until(f'install "{a.probe.resolve()}"','installation ','successful')
 attach(0,0);attach(1,1);time.sleep(6)
 probe('dual',2)
 for slot in range(2):
  command(slot,'export',f'"{r}/live{slot}.frondsd"','exported')
  assert extract(r/f'live{slot}.frondsd','HOST.TXT')==f'Dana host card {slot}\n'.encode()
  assert extract(r/f'live{slot}.frondsd','GUEST.TXT')==f'Dana guest slot {slot+1}\n'.encode()
 detach(0);detach(1)
 for slot in range(2):
  assert extract(cards[slot],'GUEST.TXT')==extract(r/f'live{slot}.frondsd','GUEST.TXT')
  print('persisted guest SHA256',slot,hashlib.sha256(extract(cards[slot],'GUEST.TXT')).hexdigest(),flush=True)
 before=hashlib.sha256(cards[0].read_bytes()).hexdigest()
 attach(0,0,'protected');attach(1,2);time.sleep(6);probe('readonly',2,True)
 command(1,'export',f'"{r}/slot1-write.frondsd"','exported')
 assert extract(r/'slot1-write.frondsd','HOST.TXT')==b'Dana host card 2\n'
 assert extract(r/'slot1-write.frondsd','GUEST.TXT')==b'Dana guest slot 2\n'
 assert hashlib.sha256(cards[0].read_bytes()).hexdigest()==before
 detach(0);time.sleep(3);probe('removed-first',1)
 attach(0,3);time.sleep(6);probe('swapped-first',2)
 detach(0);attach(0,0,'protected');time.sleep(6);probe('reinserted-first',2,True)
 detach(0);detach(1)
 assert hashlib.sha256(cards[0].read_bytes()).hexdigest()==before
 print(f'PASS dual card {a.protocols} guest read/write, persisted host equality, readonly, slot1 isolation and independent remove/swap/reinsert',flush=True)
finally:c.close()
