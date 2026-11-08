#!/usr/bin/env python3
"""Focused validation of the bounded diagnostic idle option."""
from pathlib import Path
import struct
import subprocess

rom = Path('build/zodiac-idle-wake-cli.rom')
image = bytearray(0x7d0000)
for offset in (0, 0x20000):
    struct.pack_into('<I', image, offset + 8, 0xfeedbeef)
    image[offset + 0x10:offset + 0x19] = b'PalmCard\0'
    struct.pack_into('<II', image, offset + 0x80, 0x54707776, 0x4d583161)
struct.pack_into('<I', image, 0x2006c, len(image) - 0x20000)
struct.pack_into('<I', image, 0, 0xea00003e)
struct.pack_into('<II', image, 0x100, 0xee070f90, 0xe7f000f0)
rom.write_bytes(image)
base = ['build/zodiac-boot-probe', str(rom), 'physical', '4', 'exact']
try:
    for value in ('', '0', '-1', '+1', '64000001', '4294967296', '999999999999999999999999999', '1x', '0x10', ' 1'):
        p = subprocess.run(base + ['--bootstrap-peripherals', '--idle-hclk-budget=' + value], capture_output=True, text=True)
        assert p.returncode == 1 and 'Supply one --idle-hclk-budget' in p.stderr, (value, p.stdout, p.stderr)
    p = subprocess.run(base + ['--idle-hclk-budget=1'], capture_output=True, text=True)
    assert p.returncode == 1 and 'requires --bootstrap-peripherals' in p.stderr
    p = subprocess.run(base + ['--bootstrap-peripherals', '--idle-hclk-budget=1', '--idle-hclk-budget=2'], capture_output=True, text=True)
    assert p.returncode == 1 and 'Supply one' in p.stderr
    for value in (None, 1, 64000000):
        args = base + ['--bootstrap-peripherals']
        if value is not None:
            args.append('--idle-hclk-budget=' + str(value))
        p = subprocess.run(args, capture_output=True, text=True)
        assert p.returncode == 2 and 'stop=idle-no-known-wake' in p.stdout, (value, p.stdout, p.stderr)
        assert f'idle-hclk-budget={16000000 if value is None else value} cumulative-per-run' in p.stdout
        assert 'idle-hclk-total=0' in p.stdout
finally:
    rom.unlink()
print('Zodiac cumulative idle budget CLI: PASS')
