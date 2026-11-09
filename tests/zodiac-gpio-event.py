#!/usr/bin/env python3
"""Generated-ROM checks for explicit GPIO instruction-boundary events."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile

with tempfile.TemporaryDirectory(prefix='zodiac-gpio-event-', dir='build') as work:
    rom = Path(work) / 'generated.rom'
    image = bytearray(0x7d0000)
    for offset in (0, 0x20000):
        struct.pack_into('<I', image, offset + 8, 0xfeedbeef)
        image[offset + 0x10:offset + 0x19] = b'PalmCard\0'
        struct.pack_into('<II', image, offset + 0x80, 0x54707776, 0x4d583161)
    struct.pack_into('<I', image, 0x2006c, len(image) - 0x20000)
    struct.pack_into('<I', image, 0, 0xea00003e)
    words = (0xe59f0010, 0xe5901000, 0xe5902000, 0xe59f0008, 0xe5903000,
             0xeafffffe, 0x0021c024, 0x0021c324)
    struct.pack_into('<8I', image, 0x100, *words)
    rom.write_bytes(image)
    base = [os.environ.get('FROND_ZODIAC_PROBE', 'build/zodiac-boot-probe'), str(rom), 'physical', '10', 'exact']
    baseline = ['--bootstrap-peripherals', '--gpio-a-input=0x80', '--gpio-d-input=0x600']
    def run(*flags, initial=baseline, code=2):
        result = subprocess.run(base + initial + list(flags), capture_output=True, text=True, timeout=15)
        assert result.returncode == code, (flags, result.stdout, result.stderr)
        return result.stdout
    plain = run()
    assert 'r1=00000080 r2=00000080 r3=00000600' in plain and 'gpio-input-event' not in plain
    event = '--gpio-input-event=3/0/0x400/0x400'
    changed = run(event, '--gpio-input-event=3/3/0x1000/0x1000')
    assert 'r1=00000080 r2=00000480 r3=00001600' in changed
    assert 'gpio-input-event-applied=1 before-instruction=3' in changed
    assert 'r1=00000480' in run('--gpio-input-event=1/0/0x400/0x400')
    for value in ['', '0/0/0/1', '10/0/0/1', '100000000/0/0/1', '4294967296/0/0/1',
                  '-1/0/0/1', '+1/0/0/1', '1/4/0/1', '1/2/0/1', '1/0/0/0',
                  '1/0/2/1', '1/0/0/4294967296', '1/0/0/1/',
                  '1/0/0', '1//0/1', '1/0/-1/1', '1/0/0/ 1', ' 1/0/0/1']:
        run('--gpio-input-event=' + value, code=1)
    run('--gpio-input-event=1/0/0/0x2000', initial=['--bootstrap-peripherals', '--gpio-a-input-masked=0x80/0xffffdfff'], code=1)
    run(event, event, code=1)
    run(event, '--gpio-input-event=4/3/0/1', code=1)
    run(event, initial=[], code=1)
    run(event, initial=['--bootstrap-peripherals'], code=1)
    # A halted guest does not acquire retired instructions from idle time.
    struct.pack_into('<I', image, 0x100, 0xee070f90)
    rom.write_bytes(image)
    halted = run(event)
    assert 'gpio-input-event-applied=0' in halted and 'idle-hclk-total=0' in halted
    assert rom.read_bytes() == image
print('Zodiac GPIO scheduled input CLI: PASS')
