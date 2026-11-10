#!/usr/bin/env python3
"""Exercise Dana runtime bank replacement through the native console with private sessions."""
import argparse
from pathlib import Path
import os, queue, re, socket, struct, subprocess, threading, time, zlib, runpy

p=argparse.ArgumentParser(description=__doc__)
for name in ('core','session','evidence'): p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--probe',action='store_true',help='Also run private queue and failed-rollback hooks')
p.add_argument('--require-recovery',action='store_true',help='Require normal guest reset recovery without a ready-checkpoint fallback')
a=p.parse_args();r=a.evidence.resolve();r.mkdir(mode=0o700,parents=True,exist_ok=False)
chunks=runpy.run_path(str(Path(__file__).with_name('dana-session.py')))['session_chunks']
data=a.session.read_bytes();raw,cs=chunks(data)
bad=bytearray(raw);struct.pack_into('<I',bad,cs[0x1e0][0],99)
late=r/'late.frond';late.write_bytes(data[:92]+zlib.compress(bad))
pre=r/'preflight.frond';pre.write_bytes(b'bad header')

def free_port():
 with socket.socket() as s:
  s.bind(('127.0.0.1',0));return s.getsockname()[1]
class Runner:
 def __init__(self,tag):
  self.q=queue.Queue();self.log=(r/(tag+'.log')).open('w')
  self.p=subprocess.Popen([str(a.core.resolve()),str(a.session.resolve()),'--device','alphasmart-dana','--no-sound','--speed','original'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy'})
  def read():
   for line in self.p.stdout:self.log.write(line);self.log.flush();self.q.put(line)
  self.t=threading.Thread(target=read);self.t.start()
 def wait(self,expected,timeout=45):
  end=time.monotonic()+timeout
  while time.monotonic()<end:
   try:line=self.q.get(timeout=max(.01,end-time.monotonic()))
   except queue.Empty:break
   if expected in line:return line
  raise AssertionError(f'missing {expected}, process {self.p.poll()}')
 def wait_phase(self,timeout=45):
  end=time.monotonic()+timeout
  while time.monotonic()<end:
   line=self.q.get(timeout=max(.01,end-time.monotonic()))
   if 'usb: running' in line:return 'running'
   if 'usb: failed' in line:return 'failed'
  raise AssertionError('no final USB phase')
 def send(self,cmd,expected):
  self.p.stdin.write(cmd+'\n');self.p.stdin.flush();return self.wait(expected)
 def close(self):
  if self.p.poll() is None:self.p.stdin.write('quit\n');self.p.stdin.flush()
  try:assert self.p.wait(timeout=20)==0
  finally:
   if self.p.poll() is None:self.p.terminate();self.p.wait(timeout=10)
   self.t.join();self.log.close()
def disconnected(peer):
 peer.settimeout(5)
 try:assert peer.recv(4096)==b''
 except ConnectionResetError:pass
 peer.close()
# A refused bind leaves USB off and must not be retried during later loads.
c=Runner('failed-bind')
try:
 with socket.socket() as occupied:
  occupied.bind(('127.0.0.1',0));occupied.listen()
  port=occupied.getsockname()[1]
  c.send(f'usb-debug-mode tcp:{port}','usb-debug: error')
  c.send(f'load-session "{a.session.resolve()}"','loaded session')
  c.send('usb-debug-mode off','usb-debug: off')
finally:c.close()
assert (r/'failed-bind.log').read_text().count('usb-debug: error')==1
print('PASS failed bind stays off across a runtime load',flush=True)
for mode in ('off','tcp','tcp-zero'):
 c=Runner(mode)
 try:
  port=0 if mode=='tcp-zero' else free_port()
  setting='off' if mode=='off' else f'tcp:{port}'
  line=c.send('usb-debug-mode '+setting,'usb-debug:')
  if mode=='tcp-zero':
   port=int(re.search(r'127\.0\.0\.1:(\d+)',line)[1]);assert port>0
  peer=None
  if mode!='off':
   peer=socket.create_connection(('127.0.0.1',port),timeout=5)
   c.wait('usb: running')
   if a.require_recovery:
    configured=r/(mode+'-configured.frond')
    c.send(f'save-session "{configured}"','wrote session')
    c.send('usb-debug-mode off','usb-debug: off');disconnected(peer)
    c.send(f'usb-debug-mode tcp:{port}','usb-debug:')
    peer=socket.create_connection(('127.0.0.1',port),timeout=5)
    c.wait('usb: resetting');assert c.wait_phase()=='running','live second reset failed'
    c.send(f'load-session "{configured}"','loaded session');disconnected(peer)
    peer=socket.create_connection(('127.0.0.1',port),timeout=5)
    c.wait('usb: resetting');assert c.wait_phase()=='running','configured session reset failed'
    print('PASS live second reset and committed configured-session recovery:',mode,flush=True)
  for repeat in range(2):
   if a.probe and peer:
    c.send('usb-test-pause','usb-test: paused')
    peer.sendall(b'queued-lifecycle-bytes')
    end=time.monotonic()+3
    before=c.send('usb-test-state','usb-test: state')
    while 'pending=22' not in before and time.monotonic()<end:
     time.sleep(.02);before=c.send('usb-test-state','usb-test: state')
    assert 'pending=22' in before,before
   line=c.send(f'load-session "{pre}"','session refused:');assert 'machine unchanged' in line
   if peer:
    peer.settimeout(.2)
    try:assert peer.recv(1)!=b''
    except socket.timeout:pass
   if a.probe and peer:
    assert c.send('usb-test-state','usb-test: state')==before
   # A successful committed load closes the external peer and starts a new host.
   c.send(f'load-session "{a.session.resolve()}"','loaded session')
   if a.probe and peer:
    assert 'pending=0' in c.send('usb-test-state','usb-test: state')
    c.send('usb-test-resume','usb-test: resumed')
   if peer:
    disconnected(peer);peer=socket.create_connection(('127.0.0.1',port),timeout=5)
    c.wait('usb: resetting');c.wait('usb: running')
   line=c.send(f'load-session "{late}"','session refused:')
   assert 'machine restored' in line,line
   if peer:assert 'USB peer must reconnect and reset' in line,line
   if peer:
    disconnected(peer);peer=socket.create_connection(('127.0.0.1',port),timeout=5)
    c.wait('usb: resetting')
    phase=c.wait_phase()
    if a.require_recovery:assert phase=='running','post-enumeration rollback reset failed'
    if phase=='failed':
     print('OPEN guest gate: restored post-enumeration debug guest failed descriptor after fresh reset; recovering with ready checkpoint',flush=True)
     c.send(f'load-session "{a.session.resolve()}"','loaded session')
     disconnected(peer);peer=socket.create_connection(('127.0.0.1',port),timeout=5)
     c.wait('usb: resetting');c.wait('usb: running')
  if a.probe and peer:c.send('usb-test-directions','usb-test: reversed ECR directions timed out')
  if peer:peer.close()
  c.send('usb-debug-mode off','usb-debug: off')
 finally:c.close()
 print('PASS runtime repeated ready load, preflight refusal, late rollback, reconnect/reset/enumeration, off and quit:',mode,flush=True)
c=Runner('active-quit')
try:
 port=free_port();c.send(f'usb-debug-mode tcp:{port}','usb-debug:')
 peer=socket.create_connection(('127.0.0.1',port),timeout=5);c.wait('usb: running')
finally:c.close()
disconnected(peer)
print('PASS quit with active enumerated TCP bridge',flush=True)
if a.probe:
 for failmode in ('off','tcp'):
  c=Runner('rollback-failure-'+failmode)
  try:
   peer=None
   port=free_port();setting='off' if failmode=='off' else f'tcp:{port}'
   c.send('usb-debug-mode '+setting,'usb-debug:')
   if failmode=='tcp':
    peer=socket.create_connection(('127.0.0.1',port),timeout=5);c.wait('usb: running')
   c.send('usb-test-fail-rollback','usb-test: rollback armed')
   c.send(f'load-session "{late}"',"the running machine couldn't be restored")
   if peer:disconnected(peer)
  finally:c.close()
  print('PASS private reported rollback failure after actual rollback apply, shutdown:',failmode,flush=True)
