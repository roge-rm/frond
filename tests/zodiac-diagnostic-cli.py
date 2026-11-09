#!/usr/bin/env python3
"""Private end-stop capture, CLI errors and WFI capture without time advance."""
from pathlib import Path
import json
import struct
import subprocess
import tempfile

with tempfile.TemporaryDirectory(prefix='zodiac-diagnostic-', dir='build') as work:
    root = Path(work)
    rom = root / 'generated.rom'
    image = bytearray(0x7d0000)
    for offset in (0, 0x20000):
        struct.pack_into('<I', image, offset + 8, 0xfeedbeef)
        image[offset + 0x10:offset + 0x19] = b'PalmCard\0'
        struct.pack_into('<II', image, offset + 0x80, 0x54707776, 0x4d583161)
    struct.pack_into('<I', image, 0x2006c, len(image) - 0x20000)
    struct.pack_into('<I', image, 0, 0xea00003e)
    struct.pack_into('<II', image, 0x100, 0xee070f90, 0xe7f000f0)
    rom.write_bytes(image)
    base = ['build/zodiac-boot-probe', str(rom), 'physical', '4', 'exact', '--bootstrap-peripherals']
    def invoke(*flags):
        return subprocess.run(base + list(flags), capture_output=True, text=True, timeout=15)
    plain = invoke()
    prefix = root / 'state'
    flag = '--diagnostic-snapshot=' + str(prefix)
    captured = invoke(flag)
    assert plain.returncode == captured.returncode == 2 and plain.stdout == captured.stdout
    records = [json.loads(line) for line in prefix.with_suffix('.jsonl').read_text().splitlines()]
    assert records[0]['schema'] == 'frond-private-arm920-diagnostic-v1'
    rows = lambda kind: [r['words'] for r in records if r.get('kind') == kind]
    assert len(rows('recent')) == 2 and rows('recent')[-1][1:3] == [0x10000100, 0xee070f90]
    assert rows('cpu')[0][8] == 1 and rows('stop')[0][3:5] == [0, 0]
    assert prefix.with_suffix('.sram.bin').read_bytes() == bytes(0x20000)
    assert prefix.with_suffix('.sdram0.bin').stat().st_size == prefix.with_suffix('.sdram1.bin').stat().st_size == 0
    for flags in [('--diagnostic-snapshot=',), (flag, flag),
                  ('--diagnostic-snapshot=' + str(root / 'missing' / 'state'),)]:
        assert invoke(*flags).returncode == 1
    prefix.with_suffix('.sram.bin').unlink()
    prefix.with_suffix('.sram.bin').mkdir()
    failed = invoke(flag)
    assert failed.returncode == 1 and 'Could not write complete diagnostic output' in failed.stderr
    assert rom.read_bytes() == image
print('Zodiac private diagnostic CLI and unchanged WFI stop: PASS')
