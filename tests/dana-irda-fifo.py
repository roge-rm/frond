#!/usr/bin/env python3
"""Dana raw UART2 FIFO continuation diagnostic; real IrLAP/OBEX needs separate transfer checks."""
import argparse
from pathlib import Path
import runpy,socket,struct,sys,time,zlib,shutil
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import palm_boot
p=argparse.ArgumentParser(description=__doc__)
for name in ('core','session','probe','evidence'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();palm_boot.CORE_68K=a.core.resolve();r=a.evidence.resolve();r.mkdir(parents=True,mode=0o700,exist_ok=False)
chunks=runpy.run_path(str(Path(__file__).with_name('dana-session.py')))['session_chunks']
expected=bytes((i*73+17)&255 for i in range(32))
def open_core(image,tag):return palm_boot.Core([image,'--device','alphasmart-dana'],r/(tag+'.log'),'--speed','original')
def save(c,tag):
 path=r/(tag+'.frond');c.send(f'save-session "{path}"','wrote session');return path
def pending(path):
 raw,cs=chunks(path.read_bytes());at,n=cs[0x200];assert n==132;v=struct.unpack_from('<132I',raw,at);return raw,cs,v

def read(c,tag):
 time.sleep(.3)
 c.frame(r/(tag+'-before.png'))
 assert pending(save(c,tag+'-before'))[2][1]==32
 c.send('dana-down r','input accepted');time.sleep(.15);c.send('dana-up r','input accepted');time.sleep(.2)
 path=r/(tag+'.pdb');c.send(f'db-export-db FrondDanaIRFifo "{path}"','export written');b=path.read_bytes();assert struct.unpack_from('>H',b,76)[0]==1;offset=struct.unpack_from('>I',b,78)[0];assert b[offset:]==expected,b[offset:]
source=r/'input.frond';shutil.copy2(a.session.resolve(),source)
c=open_core(source,'capture')
try:
 c.until(f'install "{a.probe.resolve()}"','installation ','successful');c.launch('Frond Dana IR FIFO');c.tap(r,235,68)
 with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
 c.send(f'irda-mode tcp:{port}','infrared:')
 with socket.create_connection(('127.0.0.1',port)) as wire:
  wire.sendall(expected);time.sleep(.2);checkpoint=save(c,'pending')
  raw,cs,v=pending(checkpoint);assert v[1:3]==(32,0),v[:4];assert bytes(v[4:36])==expected
 c.send('irda-mode off','infrared:')
 b=bytearray(raw);struct.pack_into('<I',b,cs[0x200][0]+4,65);bad=r/'malformed.frond';bad.write_bytes(checkpoint.read_bytes()[:92]+zlib.compress(b))
 assert 'machine restored' in c.send(f'load-session "{bad}"','session refused:')
 read(c,'rollback-bytes')
 c.send(f'load-session "{checkpoint}"','loaded session');read(c,'same-process-bytes')
 print('PASS 32 real incoming bytes held in UART2, malformed rollback and same-process continuation',flush=True)
finally:
 status=c.close()
 if sys.exc_info()[0] is None:assert status==0,f'core exited {status}'
c=open_core(checkpoint,'fresh')
try:
 read(c,'fresh-bytes')
 print('PASS fresh-process local FIFO continuation without an external peer',flush=True)
 c.send(f'load-session "{checkpoint}"','loaded session');c.send('reset','reset');time.sleep(1)
 assert pending(save(c,'reset'))[2][1:3]==(0,0)
 print('PASS reset clears pending UART2 FIFO',flush=True)
finally:
 status=c.close()
 if sys.exc_info()[0] is None:assert status==0,f'core exited {status}'
