#!/usr/bin/env python3
"""Native Dana inventory guards retain the live mixed pair on invalid attachment."""
import argparse, shlex, shutil, subprocess, sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import palm_boot
p=argparse.ArgumentParser(description=__doc__)
for name in ('core','session','sd','mmc','make-geometry-card','evidence'): p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();a.evidence.mkdir(mode=0o700,parents=True,exist_ok=False)
# Start pristine independent namespaces; never attach the earlier qualification sources.
for name in ('sd','mmc'):
 src=getattr(a,name); dest=a.evidence/(name+'.frondsd');shutil.copyfile(src,dest);setattr(a,name,dest)
bad=a.evidence/'unrepresentable.frondsd'
control=a.evidence/'representable.frondsd'
for path,size in ((bad,(32<<20)+512),(control,(32<<20)+16384)):
 subprocess.run([str(a.make_geometry_card.resolve()),str(path.resolve()),str(size)],check=True)
palm_boot.CORE_68K=a.core.resolve()
c=palm_boot.Core([a.session.resolve(),'--device','alphasmart-dana'],a.evidence/'core.log','--speed','original')
def cmd(slot,op,arg=''):
 return c.send(f'{slot}-{op} {arg}'.rstrip(),'card:').strip()
try:
 for slot,kind,src,key in [('sd','sd',a.sd,'a'*32),('sd2','mmc',a.mmc,'b'*32)]:
  assert 'inserted' in cmd(slot,'attach',f'{shlex.quote(str(src.resolve()))} {key} writable {kind}')
 states=[cmd(s,'state') for s in ('sd','sd2')]
 assert 'mmc' not in states[0] and 'mmc' in states[1]
 for slot in ('sd','sd2'):
  assert 'ejected' in cmd(slot,'eject')
  assert 'taken-out' in cmd(slot,'take-out')
  for kind in ('sdhc','sdio','mmcplus','emmc','unknown'):
   assert 'error:' in cmd(slot,'attach',f'{shlex.quote(str(a.sd.resolve()))} '+ 'c'*32 + f' writable {kind}')
  assert 'error:' in cmd(slot,'attach',f'{shlex.quote(str(bad.resolve()))} '+ 'd'*32 +' writable sd')
  assert 'none' in cmd(slot,'state')
  assert not list(a.evidence.glob('.gen-'+bad.name+'-*'))
  assert 'inserted' in cmd(slot,'attach',f'{shlex.quote(str(control.resolve()))} '+ 'e'*32 +' writable sd')
  assert 'ejected' in cmd(slot,'eject')
  assert 'taken-out' in cmd(slot,'take-out')
  peer='sd2' if slot=='sd' else 'sd'
  assert cmd(peer,'state')==states[1 if peer=='sd2' else 0]
  src=a.sd if slot=='sd' else a.mmc;kind='sd' if slot=='sd' else 'mmc';key='a'*32 if slot=='sd' else 'b'*32
  assert 'inserted' in cmd(slot,'attach',f'{shlex.quote(str(src.resolve()))} {key} writable {kind}')
 assert [cmd(s,'state') for s in ('sd','sd2')]==states
 print('PASS: native unsupported families/CSD refusal and independent mixed pair retention')
finally:
 c.close()
