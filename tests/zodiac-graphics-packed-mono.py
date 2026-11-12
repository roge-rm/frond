#!/usr/bin/env python3
"""Private virtual packed 1bpp policy CLI, using only a short generated ROM."""
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'build/zodiac-boot-probe'
FLAG = '--graphics-virtual-packed-mono'
PROFILE = '--graphics-profile=virtual5744-compatible-8mib'
BOOT = '--bootstrap-peripherals'
ON = 'virtual-msb-first-set-bit-foreground-opaque-copy-exclusive-scissor-zero-phase'
with tempfile.TemporaryDirectory(prefix='zodiac-packed-mono-', dir=ROOT / 'build') as work:
    rom = bytearray(0x7d0000)
    for base in (0, 0x20000):
        struct.pack_into('<I', rom, base+8, 0xfeedbeef)
        rom[base+0x10:base+0x19] = b'PalmCard\0'
        struct.pack_into('<II', rom, base+0x80, 0x54707776, 0x4d583161)
    struct.pack_into('<I', rom, 0x20000+0x6c, len(rom)-0x20000)
    struct.pack_into('<I', rom, 0, 0xeafffffe)
    path = Path(work) / 'synthetic.rom'
    path.write_bytes(rom)
    def run(*args, code=1):
        result = subprocess.run([str(PROBE),str(path),'linked','8','exact',*args],
                                text=True,capture_output=True,timeout=15)
        assert result.returncode == code, (args,result.returncode,result.stdout,result.stderr)
        return result.stdout+result.stderr
    for args in [(FLAG,), (BOOT,FLAG), (PROFILE,FLAG)]:
        assert 'Virtual packed mono requires' in run(*args)
    for args in [(BOOT,PROFILE,FLAG,FLAG), (BOOT,PROFILE,FLAG+'=true'), (BOOT,PROFILE,FLAG+'x')]:
        run(*args)
    assert 'graphics-packed-mono-policy=guarded physical-w4200-unqualified' in run(BOOT,PROFILE,code=2)
    for args in [(BOOT,PROFILE,FLAG),(FLAG,PROFILE,BOOT)]:
        output = run(*args,code=2)
        assert f'graphics-packed-mono-policy={ON} physical-w4200-unqualified' in output
        assert 'graphics-a4-policy=guarded' in output and 'stop=instruction-budget' in output, output
print('PASS: virtual packed 1bpp CLI needs the profile, is guarded by default and reported when on')
