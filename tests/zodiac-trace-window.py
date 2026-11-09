#!/usr/bin/env python3
"""Generated-ROM checks for a passive instruction-entry window."""
from pathlib import Path
import re
import struct
import subprocess
import tempfile

with tempfile.TemporaryDirectory(prefix='zodiac-trace-window-', dir='build') as work:
    rom = Path(work) / 'generated.rom'
    image = bytearray(0x7d0000)
    for offset in (0, 0x20000):
        struct.pack_into('<I', image, offset + 8, 0xfeedbeef)
        image[offset + 0x10:offset + 0x19] = b'PalmCard\0'
        struct.pack_into('<II', image, offset + 0x80, 0x54707776, 0x4d583161)
    struct.pack_into('<I', image, 0x2006c, len(image) - 0x20000)
    struct.pack_into('<I', image, 0, 0xea00003e)
    struct.pack_into('<3I', image, 0x100, 0xe3a00000, 0xe2800001, 0xeafffffd)
    rom.write_bytes(image)

    def run(*flags, budget=5000, code=2):
        result = subprocess.run(['build/zodiac-boot-probe', str(rom), 'physical', str(budget),
                                 'exact', '--trace=peripherals', *flags],
                                capture_output=True, text=True, timeout=15)
        assert result.returncode == code, (flags, result.stdout, result.stderr)
        return result.stdout

    def entries(output):
        return [int(value) for value in re.findall(r'^step=(\d+) ', output, re.M)]

    def ordinary(output):
        return '\n'.join(line for line in output.splitlines()
                         if not line.startswith(('trace-window', 'step=')))

    plain = run()
    assert 'trace-window' not in plain and not entries(plain)
    first = run('--trace-window=0x10000104/1')
    assert entries(first) == [0, 1, 2]
    assert 'preceding=2 step=2' in first
    assert 'trace-window-emitted=1 matched-entries=1' in first
    wrapped = run('--trace-window=0x10000104/2200')
    assert entries(wrapped) == list(range(304, 4401))
    assert 'preceding=4096 step=4400' in wrapped
    assert wrapped.count('trace-window begin') == 1
    absent = run('--trace-window=0x10000110/1')
    assert not entries(absent) and 'trace-window-emitted=0 matched-entries=0' in absent
    assert ordinary(plain) == ordinary(first) == ordinary(wrapped) == ordinary(absent)
    for value in ['', '1/1', '-2/1', '+2/1', '0x100000000/1', '0x10000104/0',
                  '0x10000104/-1', '0x10000104/+1', '0x10000104/5001',
                  '0x10000104/4294967296', '0x10000104/1/', '0x10000104/1x']:
        run('--trace-window=' + value, code=1)
    run('--trace-window=0/1', '--trace-window=0/1', code=1)
    struct.pack_into('<4I', image, 0x100, 0xe3a00000, 0xe59f1000, 0xe12fff11, 0x10000121)
    struct.pack_into('<2H', image, 0x120, 0x3001, 0xe7fd)
    rom.write_bytes(image)
    thumb = run('--trace-window=0x10000120/4', budget=20)
    assert entries(thumb) == list(range(11))
    assert 'pc=10000120 instruction=3001' in thumb and 'width=2' in thumb
print('Zodiac passive trace window checks passed')
