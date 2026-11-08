#!/usr/bin/env python3
"""Dana/T|X guest payload or database beams with explicit private sessions and binaries."""
from pathlib import Path
import sys,time,socket,threading,select,struct,argparse
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import palm_boot
sys.path.insert(0,str(ROOT/'tests'))
import smoke,shlex,zipfile,shutil
p=argparse.ArgumentParser(description=__doc__)
for name in ('core','peer-core','session','peer-session','probe','evidence'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--database',action='store_true')
p.add_argument('--sender',type=int,choices=(0,1),default=0)
a=p.parse_args();args_database=a.database;smoke.CORE=a.peer_core.resolve()

class Arm:
 def __init__(self):
  work=r/'arm';work.mkdir();(work/'machine').mkdir();self.run=smoke.Run('palm-tx',work,session=a.peer_session.resolve(),extra=['--work-folder',str(work/'machine')]);self.run.ready()
 def until(self,cmd,*args):
  self.run.install_and_launch(str(a.probe.resolve()),('Frond database beam' if a.database else 'Frond IR probe'))
 def launch(self,*args):pass
 def send(self,cmd,*args):
  v=shlex.split(cmd)
  if v[0]=='irda-mode':return str(self.run.require('irda-mode',v[1]))
  if v[0]=='db-export-db':
   z=Path(v[2]+'.zip');reply=self.run.require('export-db',str(z)+'\n'+v[1])
   with zipfile.ZipFile(z) as archive:
    names=archive.namelist();assert len(names)==1,names;Path(v[2]).write_bytes(archive.read(names[0]))
   return str(reply)
  raise AssertionError(v)
 def tap(self,root,x,y):return self.run.tap(root,x,y)
 def frame(self,path):return self.run.frame(path)
 def close(self):
  self.run.close();return self.run.process.returncode

r=a.evidence.resolve();r.mkdir(parents=True,mode=0o700,exist_ok=False)
palm_boot.CORE_68K=a.core.resolve()
base=r/'input-dana.frond';shutil.copy2(a.session.resolve(),base)
rows_index=0
cores=[];peers=[];stop=threading.Event();cut=threading.Event();dropped=threading.Event();phase=0
def rows(c,tag):
 global rows_index
 rows_index+=1
 p=r/(tag+'-'+str(rows_index)+'.pdb');name='FrondDBBeamLog' if args_database else 'FrondIRLog';c.send(f'db-export-db {name} "{p}"','export written');b=p.read_bytes();n=struct.unpack_from('>H',b,76)[0];a=[struct.unpack_from('>I',b,78+i*8)[0] for i in range(n)]+[len(b)];return [b[a[i]:a[i+1]].rstrip(b'\0').decode() for i in range(n)]
def relay():
 amount=0
 with (r/f'wire{phase}-0.bin').open('wb') as f0,(r/f'wire{phase}-1.bin').open('wb') as f1:
  while not stop.is_set():
   for s in select.select(peers,[],[],.1)[0]:
    b=s.recv(65536)
    if not b:return
    i=peers.index(s)
    if cut.is_set() and i==0:
     amount+=len(b)
     if amount>=2048:
      for peer in peers:peer.shutdown(socket.SHUT_RDWR)
      dropped.set();return
    [f0,f1][i].write(b);[f0,f1][i].flush();peers[1-i].sendall(b)
try:
 for i in range(2):
  c=palm_boot.Core([base,'--device','alphasmart-dana'],r/'core0.log','--speed','original') if i==0 else Arm();cores.append(c)
  c.until(f'install "{a.probe.resolve()}"','installation ','successful');c.launch(('Frond database beam' if a.database else 'Frond IR probe'))
  with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
  print(c.send(f'irda-mode tcp:{port}','infrared:'),flush=True)
  peers.append(socket.create_connection(('127.0.0.1',port)))
 thread=threading.Thread(target=relay);thread.start()
 senders=[a.sender] if args_database else range(2)
 for sender in senders:
  cores[sender].tap(r,235 if sender==0 else 70,68 if sender==0 else 136)
  end=time.monotonic()+90
  while time.monotonic()<end:
   time.sleep(2)
   tx=rows(cores[sender],f'send{sender}');b=rows(cores[1-sender],f'recv{sender}')
   if any(('complete err=0000' if args_database else 'send-complete') in x for x in tx) and any('receive-complete' in x for x in b):break
  else:
   for i,c in enumerate(cores):c.frame(r/f'timeout{i}.png')
   raise AssertionError((tx,b))
  p=r/f'payload{sender}.pdb';name='Frond5FRecord' if args_database else 'FrondIRReceived';cores[1-sender].send(f'db-export-db {name} "{p}"','export written')
  data=p.read_bytes();assert struct.unpack_from('>H',data,76)[0]==(2 if args_database else 1);n=struct.unpack_from('>I',data,86 if args_database else 78)[0];assert data[n:]==bytes(((k*73+17)^((k>>8)*31))&255 for k in range(4096))
  if args_database:
   first=struct.unpack_from('>I',data,78)[0]
   assert data[first:n]==b'Frond 5f original record database beam\nTX and E2 native Exchange Manager\n\0'
  print('PASS guest database' if args_database else 'PASS guest payload',sender,flush=True)
 if args_database:sys.exit(0)
 before=rows(cores[0],'before-disconnect');count=before.count('send-complete')
 cut.set();cores[0].tap(r,235,68)
 assert dropped.wait(20),'did not reach data transfer cut'
 time.sleep(.5);cores[0].frame(r/'disconnected-sender.png');cores[0].tap(r,280,148);time.sleep(.5)
 after=rows(cores[0],'after-disconnect');assert after.count('send-complete')==count,after
 cores[0].send('reset','reset');time.sleep(1);cores[0].launch(('Frond database beam' if a.database else 'Frond IR probe'))
 assert rows(cores[0],'after-reset').count('send-complete')==count
 print('PASS disconnect in real guest data transfer, no false completion, active reset',flush=True)
 thread.join(3);assert not thread.is_alive()
 for peer in peers:peer.close()
 peers.clear();cut.clear();dropped.clear();phase=1
 cores[1].run.require('reset');cores[1].run.ready();cores[1].run.control.retry_ready('launch-app','Frond IR probe',60)
 receiver_before=rows(cores[1],'receiver-before-retry').count('receive-complete')
 for c in cores:
  c.send('irda-mode off','infrared:')
  with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
  c.send(f'irda-mode tcp:{port}','infrared:');peers.append(socket.create_connection(('127.0.0.1',port)))
 thread=threading.Thread(target=relay);thread.start();cores[0].tap(r,235,68)
 end=time.monotonic()+90
 while time.monotonic()<end:
  time.sleep(2)
  if rows(cores[0],'retry-sender').count('send-complete')==count+1 and rows(cores[1],'retry-receiver').count('receive-complete')==receiver_before+1:break
 else:raise AssertionError('guest retry after reconnect/reset failed')
 path=r/'retry-payload.pdb';cores[1].send(f'db-export-db FrondIRReceived "{path}"','export written');data=path.read_bytes();offset=struct.unpack_from('>I',data,78)[0]
 assert data[offset:]==bytes(((i*73+17)^((i>>8)*31))&255 for i in range(4096))
 print('PASS new guest transfer after disconnect, reset and reconnect',flush=True)
finally:
 stop.set()
 if 'thread' in globals():thread.join(3)
 for s in peers:s.close()
 statuses=[c.close() for c in cores]
 if sys.exc_info()[0] is None:assert all(status==0 for status in statuses),statuses
