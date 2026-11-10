#!/usr/bin/env python3
"""Dana IrDA scanning, active local checkpoints and malformed rollback with private inputs."""
import argparse
from pathlib import Path
import runpy,socket,struct,sys,time,zlib,shutil
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import palm_boot
p=argparse.ArgumentParser(description=__doc__)
for name in ('core','session','probe','evidence'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();palm_boot.CORE_68K=a.core.resolve()
r=a.evidence.resolve();r.mkdir(parents=True,mode=0o700,exist_ok=False)
chunks=runpy.run_path(str(Path(__file__).with_name('dana-session.py')))['session_chunks']
def open_core(image,tag):return palm_boot.Core([image,'--device','alphasmart-dana'],r/(tag+'.log'),'--speed','original')
def save(c,tag):
 path=r/(tag+'.frond');c.send(f'save-session "{path}"','wrote session');return path
def state(path):
 raw,cs=chunks(path.read_bytes());at,n=cs[0x200];assert n==132,n
 values=struct.unpack_from('<132I',raw,at);assert values[0]==1 and values[1]<=64 and values[2]<=64
 # VZ GPIO bytes occupy swapped pairs after the chunk version word.
 regs=cs[0x90][0]+4
 return raw,cs,values,raw[regs+0x430],struct.unpack_from('<H',raw,regs+0x918)[0]
def rows(c,tag):
 path=r/(tag+'.pdb');c.send(f'db-export-db FrondIRLog "{path}"','export written');b=path.read_bytes();n=struct.unpack_from('>H',b,76)[0];at=[struct.unpack_from('>I',b,78+i*8)[0] for i in range(n)]+[len(b)];return [b[at[i]:at[i+1]].rstrip(b'\0').decode() for i in range(n)]
source=r/'input.frond';shutil.copy2(a.session.resolve(),source)
c=open_core(source,'controls')
try:
 c.until(f'install "{a.probe.resolve()}"','installation ','successful');c.launch('Frond IR probe')
 c.tap(r,235,103);time.sleep(.3)
 assert any('scanning-off err=0000' in x for x in rows(c,'disabled-log'))
 disabled=save(c,'disabled');_,_,v,g,m=state(disabled);assert g&16,(g,m)
 with socket.socket() as listener:listener.bind(('127.0.0.1',0));port=listener.getsockname()[1]
 c.send(f'irda-mode tcp:{port}','infrared:')
 with socket.create_connection(('127.0.0.1',port)) as wire:
  wire.sendall(bytes(range(256)));time.sleep(.2)
  _,_,v,g,m=state(save(c,'disabled-after-wire'));assert v[1]==0 and g&16
 c.send('irda-mode off','infrared:')
 assert not any('receive-begin' in x for x in rows(c,'disabled-after-log'))
 c.tap(r,315,103);time.sleep(.3)
 assert any('scanning-on err=0000' in x for x in rows(c,'enabled-log'))
 _,_,v,g,m=state(save(c,'enabled'));assert not g&16 and m&32,(g,m)
 print('PASS guest scanning controls, PG4 shutdown and disabled incoming bytes',flush=True)
 c.tap(r,235,68);time.sleep(.25)
 active=save(c,'active');raw,cs,v,g,m=state(active);assert not g&16 and m&32,(g,m)
 baseline=rows(c,'active-log');assert 'send-begin' in baseline and 'send-complete' not in baseline
 for tag,word,value in [('version',0,2),('rx-count',1,65),('tx-count',2,65),('receiving',3,2),('rx-byte',4,256),('rx-padding',67,1),('tx-byte',68,256),('tx-padding',131,1)]:
  b=bytearray(raw);struct.pack_into('<I',b,cs[0x200][0]+word*4,value);bad=r/(tag+'.frond');bad.write_bytes(active.read_bytes()[:92]+zlib.compress(b))
  assert 'machine restored' in c.send(f'load-session "{bad}"','session refused:')
  assert rows(c,tag+'-rollback')==baseline,tag
 c.send(f'load-session "{active}"','loaded session');state(save(c,'active-reloaded'))
 print('PASS active local save/load and eight production UART malformed rollbacks',flush=True)
finally:
 status=c.close()
 if sys.exc_info()[0] is None:assert status==0,f'core exited {status}'
c=open_core(active,'fresh-active')
try:
 time.sleep(.3);state(save(c,'fresh-active'))
 c.tap(r,280,148);time.sleep(.5)
 log=rows(c,'cancelled');assert 'send-complete' not in log,log
 assert any(x.startswith('disconnect err=') and not x.startswith('disconnect err=0000') for x in log),log
 print('PASS fresh active local continuation with no external peer and guest cancellation',flush=True)
finally:
 status=c.close()
 if sys.exc_info()[0] is None:assert status==0,f'core exited {status}'
c=open_core(active,'reset-active')
try:
 c.send('reset','reset');time.sleep(2);c.launch('Frond IR probe');state(save(c,'after-reset'))
 assert 'send-complete' not in rows(c,'reset-log')
 print('PASS reset while guest beam is active',flush=True)
finally:
 status=c.close()
 if sys.exc_info()[0] is None:assert status==0,f'core exited {status}'
