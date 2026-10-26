#!/usr/bin/env python3
"""Build the optional Palm peripheral validation application with a locally installed SDK."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdk', type=Path, default=Path('../palm-os-sdk/sdk-5r3'))
p.add_argument('--probe', choices=['sd', 'network', 'keyboard'], default='sd')
p.add_argument('--output', type=Path)
a = p.parse_args()
sdk, output = a.sdk.resolve(), (a.output or Path('build/' + a.probe + '-probe')).resolve()
source = 'tests/palm/' + a.probe + '-probe'
name = {'sd':'FrondSDProbe','network':'FrondNetProbe','keyboard':'FrondKeyboardProbe'}[a.probe]
if not (sdk / 'include/PalmOS.h').is_file():
    p.error('Supply the Palm OS 5 SDK directory with --sdk')
output.mkdir(parents=True, exist_ok=True)
includes = ['-I' + str(sdk / 'include')] + ['-I' + str(d) for d in (sdk / 'include').rglob('*') if d.is_dir()]
subprocess.run(['m68k-palmos-gcc', '-O2', '-Wall', '-Wno-unknown-pragmas'] + includes +
               ['-c', source + '.c', '-o', str(output / 'probe.o')], check=True)
subprocess.run(['m68k-palmos-gcc', '-L' + str(sdk / 'lib/m68k-palmos-coff'), str(output / 'probe.o'), '-o', str(output / 'probe')], check=True)
subprocess.run(['pilrc', '-q', '-ro', source + '.rcp', str(output / 'probe.ro')], check=True)
subprocess.run(['build-prc', '-o', str(output / (name + '.prc')), source + '.def',
                str(output / 'probe'), str(output / 'probe.ro')], check=True)
print(output / (name + '.prc'))
