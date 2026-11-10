# Sourced by native GDB around the unmodified production runner.
import gdb,json,os
from pathlib import Path
seen=set()
active=[]
replay=json.loads(os.environ.get('FROND_DANA_SPI_REPLAY','null'))
if replay:active.append(dict(slot=replay['slot'],phase=replay['phase'],file=replay['file'],rows=[],tail=None))
class Result(gdb.FinishBreakpoint):
 def __init__(self,obj,track,data,index):
  super().__init__(gdb.newest_frame(),internal=True)
  self.silent=True;self.obj=obj;self.track=track
  self.row=[index,data]
 def stop(self):
  track=self.track;spi=int(self.obj['spiState'])
  self.row.extend([int(self.return_value),int(self.obj['bufferIndex']),spi])
  track['rows'].append(self.row)
  if track['tail'] is None and spi!=track['phase']:track['tail']=2
  elif track['tail'] is not None:track['tail']-=1
  if track['tail']==0:
   Path(track['file']).write_text(json.dumps(track['rows'])+'\n')
   active.remove(track)
   if not active and not Path(os.environ['FROND_DANA_SPI_ARM']).exists():cursor.enabled=False
  return False
class Cursor(gdb.Breakpoint):
 def stop(self):
  armed=Path(os.environ['FROND_DANA_SPI_ARM']).exists()
  if not armed and not active:return False
  obj=gdb.parse_and_eval('this').dereference()
  slot=int(obj['slot']);spi=int(obj['spiState']);index=int(obj['bufferIndex'])
  size=int(obj['bufferSize']);cmd=int(obj['lastCmd'])
  for track in list(active):
   if track['slot']==slot:
    Result(obj,track,int(gdb.parse_and_eval('data')),index)
  wanted=(spi==2 and index==1) or (spi==3 and size==518 and 16<=index<64) or (spi==5 and 64<=index<128)
  if not armed or not wanted or (slot,spi) in seen:return False
  nesting=int(gdb.parse_and_eval('gSession->nestLevel'))
  if nesting:return False
  seen.add((slot,spi))
  active.append(dict(slot=slot,phase=spi,file=os.environ['FROND_DANA_SPI_TRACE']+f'-{slot}-{spi}.json',rows=[],tail=None))
  # Only the normal debugger is stopped. SD objects and guest memory are untouched.
  gdb.execute('set variable gDebugger.enabled = true')
  gdb.execute('set variable gDebuggerActive = 1')
  gdb.execute('set variable gDebugger.breakState = Debugger::BreakState::externalInterrupt')
  print('NATIVE CAPTURE',slot,spi,index,size,cmd,'guest-nesting',nesting,flush=True)
  return False
cursor=Cursor('EmSPISlaveSD::DoExchange')
cursor.enabled=bool(replay)
class Arm(gdb.Breakpoint):
 def stop(self):
  cursor.enabled=True
  return False
Arm('GdbStub::Listen')
