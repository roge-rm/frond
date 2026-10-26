#!/usr/bin/env python3
"""Build the optional Palm peripheral validation application with a locally installed SDK."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdk', type=Path, default=Path('../palm-os-sdk/sdk-5r3'))
p.add_argument('--probe', choices=['sd', 'network', 'keyboard', 'host-network'], default='sd')
p.add_argument('--output', type=Path)
a = p.parse_args()
sdk, output = a.sdk.resolve(), (a.output or Path('build/' + a.probe + '-probe')).resolve()
source = 'tests/palm/' + a.probe + '-probe'
name = {'sd':'FrondSDProbe','network':'FrondNetProbe','keyboard':'FrondKeyboardProbe','host-network':'FrondHostProbe'}[a.probe]
if not (sdk / 'include/PalmOS.h').is_file():
    p.error('Supply the Palm OS 5 SDK directory with --sdk')
output.mkdir(parents=True, exist_ok=True)
includes = ['-I' + str(sdk / 'include')] + ['-I' + str(d) for d in (sdk / 'include').rglob('*') if d.is_dir()]
subprocess.run(['m68k-palmos-gcc', '-O2', '-Wall', '-Wno-unknown-pragmas'] + includes +
               ['-c', source + '.c', '-o', str(output / 'probe.o')], check=True)
subprocess.run(['m68k-palmos-gcc', '-L' + str(sdk / 'lib/m68k-palmos-coff'), str(output / 'probe.o'), '-o', str(output / 'probe')], check=True)
subprocess.run(['pilrc', '-q', '-ro', source + '.rcp', str(output / 'probe.ro')], check=True)
resources = []
if a.probe == 'host-network':
    arm_object, arm_elf, arm_binary = output / 'tone.o', output / 'tone.elf', output / 'armc03e8.bin'
    subprocess.run(['arm-palmos-gcc', '-O2', '-Wall', '-marm', '-march=armv4t',
                    '-fno-pic', '-fno-strict-aliasing', '-ffreestanding', '-nostdlib',
                    '-fomit-frame-pointer', '-c', 'tests/palm/host-network-tone.c', '-o', str(arm_object)], check=True)
    relocations = subprocess.check_output(['arm-palmos-objdump', '-r', str(arm_object)], text=True)
    if 'R_ARM_ABS32' in relocations:
        raise SystemExit('ARM callback must not contain absolute relocations')
    subprocess.run(['arm-palmos-ld', '-Ttext=0', '-e', 'hostTone', '-nostdlib', str(arm_object), '-o', str(arm_elf)], check=True)
    subprocess.run(['arm-palmos-objcopy', '-O', 'binary', '-j', '.text', str(arm_elf), str(arm_binary)], check=True)
    resources.append(str(arm_binary))
subprocess.run(['build-prc', '-o', str(output / (name + '.prc')), source + '.def',
                str(output / 'probe'), str(output / 'probe.ro')] + resources, check=True)
print(output / (name + '.prc'))
