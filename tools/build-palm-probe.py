#!/usr/bin/env python3
"""Build an optional Palm validation application with a locally installed SDK."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdk', type=Path, default=Path('../palm-os-sdk/sdk-5r3'))
p.add_argument('--probe', choices=['sd', 'network', 'keyboard', 'host-network', 'compatibility', 'serial', 'serial-host', 'idle', 'debugger', 'audio', 'irda', 'irda-db', 'bluetooth'], default='sd')
p.add_argument('--output', type=Path)
a = p.parse_args()
sdk, output = a.sdk.resolve(), (a.output or Path('build/' + a.probe + '-probe')).resolve()
source = 'tests/palm/' + a.probe + '-probe'
name = {'sd':'FrondSDProbe','network':'FrondNetProbe','keyboard':'FrondKeyboardProbe',
        'host-network':'FrondHostProbe', 'compatibility':'FrondCompatProbe', 'serial':'FrondSerialProbe', 'serial-host':'FrondSerialHostProbe',
        'idle':'FrondIdleProbe', 'debugger':'FrondDebugProbe', 'audio':'FrondAudioProbe', 'irda':'FrondIRProbe', 'irda-db':'FrondDBBeam', 'bluetooth':'FrondBluetoothProbe'}[a.probe]
if not (sdk / 'include/PalmOS.h').is_file():
    p.error('Supply the Palm OS 5 SDK directory with --sdk')
output.mkdir(parents=True, exist_ok=True)
includes = ['-I' + str(sdk / 'include')] + ['-I' + str(d) for d in (sdk / 'include').rglob('*') if d.is_dir()]
subprocess.run(['m68k-palmos-gcc', '-O2', '-Wall', '-Wno-unknown-pragmas'] + includes +
               ['-c', source + '.c', '-o', str(output / 'probe.o')], check=True)
objects = [str(output / 'probe.o')]
if a.probe == 'debugger':
    semantics = output / 'semantics.o'
    subprocess.run(['m68k-palmos-gcc', '-c', source + '-semantics.S', '-o', str(semantics)], check=True)
    objects.append(str(semantics))
subprocess.run(['m68k-palmos-gcc', '-L' + str(sdk / 'lib/m68k-palmos-coff')] + objects + ['-o', str(output / 'probe')], check=True)
subprocess.run(['pilrc', '-q', '-ro', source + '.rcp', str(output / 'probe.ro')], check=True)
resources = []
if a.probe == 'compatibility':
    script = output / 'native.ld'
    script.write_text('SECTIONS { . = 0; .text : { *(.text.compatibilityEntry) '
                      '*(.text.compatibilityNative) *(.text*) *(.rodata*) } }\n')
    for mode, resource in [('arm', 1000), ('thumb', 1001)]:
        obj, elf = output / (mode + '.o'), output / (mode + '.elf')
        binary = output / ('armc%04x.bin' % resource)
        subprocess.run(['arm-palmos-gcc', '-O2', '-Wall', '-Werror', '-m' + mode,
                        '-mthumb-interwork', '-march=armv4t', '-ffunction-sections',
                        '-fno-pic', '-ffreestanding', '-nostdlib', '-fomit-frame-pointer'] + includes +
                       ['-c', 'tests/palm/compatibility-native.c', '-o', str(obj)], check=True)
        objects = [str(obj)]
        entry = 'compatibilityNative'
        if mode == 'thumb':
            veneer = output / 'thumb-entry.o'
            subprocess.run(['arm-palmos-gcc', '-march=armv4t', '-mthumb-interwork', '-c',
                            'tests/palm/compatibility-thumb.S', '-o', str(veneer)], check=True)
            objects.insert(0, str(veneer))
            entry = 'compatibilityEntry'
        for item in objects:
            relocations = subprocess.check_output(['arm-palmos-objdump', '-r', item], text=True)
            if 'R_ARM_ABS32' in relocations:
                raise SystemExit('Native probe must not contain absolute relocations')
        subprocess.run(['arm-palmos-ld', '-T', str(script), '-e', entry, '-nostdlib'] +
                       objects + ['-o', str(elf)], check=True)
        symbols = subprocess.check_output(['arm-palmos-nm', str(elf)], text=True).splitlines()
        if not any(line.split() == ['00000000', 'T', entry] for line in symbols):
            raise SystemExit('Native probe entry must be at resource offset zero')
        subprocess.run(['arm-palmos-objcopy', '-O', 'binary', '-j', '.text', str(elf), str(binary)], check=True)
        resources.append(str(binary))
if a.probe in ('host-network', 'audio'):
    arm_object, arm_elf, arm_binary = output / 'tone.o', output / 'tone.elf', output / 'armc03e8.bin'
    subprocess.run(['arm-palmos-gcc', '-O2', '-Wall', '-marm', '-march=armv4t',
                    '-fno-pic', '-fno-strict-aliasing', '-ffreestanding', '-nostdlib',
                    '-fomit-frame-pointer', '-c', 'tests/palm/' + ('audio-callback.c' if a.probe == 'audio' else 'host-network-tone.c'), '-o', str(arm_object)], check=True)
    relocations = subprocess.check_output(['arm-palmos-objdump', '-r', str(arm_object)], text=True)
    if 'R_ARM_ABS32' in relocations:
        raise SystemExit('ARM callback must not contain absolute relocations')
    subprocess.run(['arm-palmos-ld', '-Ttext=0', '-e', 'audioCallback' if a.probe == 'audio' else 'hostTone', '-nostdlib', str(arm_object), '-o', str(arm_elf)], check=True)
    subprocess.run(['arm-palmos-objcopy', '-O', 'binary', '-j', '.text', str(arm_elf), str(arm_binary)], check=True)
    resources.append(str(arm_binary))
subprocess.run(['build-prc', '-o', str(output / (name + '.prc')), source + '.def',
                str(output / 'probe'), str(output / 'probe.ro')] + resources, check=True)
print(output / (name + '.prc'))
