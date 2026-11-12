#!/usr/bin/env python3
"""Synthetic native reset-helper addresses and explicit diagnostic CLI input."""
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'build/zodiac-boot-probe'

def main():
    with tempfile.TemporaryDirectory(prefix='zodiac-cs5-reset-', dir=ROOT / 'build') as work:
        rom = bytearray(0x7d0000)
        for base in (0, 0x20000):
            struct.pack_into('<I', rom, base + 8, 0xfeedbeef)
            rom[base + 0x10:base + 0x19] = b'PalmCard\0'
            struct.pack_into('<II', rom, base + 0x80, 0x54707776, 0x4d583161)
        struct.pack_into('<I', rom, 0x20000 + 0x6c, len(rom) - 0x20000)
        def word(offset, value): struct.pack_into('<I', rom, offset, value)
        def branch(source, target): word(source, 0xea000000 | (((target-source-8)//4) & 0xffffff))
        branch(0, 0x100)
        words = [0xe59f0038, 0xe59f1038, 0xe5801000, 0xe59f1034, 0xe5801004,
                 0xe59f1030]
        struct.pack_into('<6I', rom, 0x100, *words)
        branch(0x118, 0xbac84)
        struct.pack_into('<4I', rom, 0x140, 0x00220028, 0x3e00, 0x11111e01, 0x16000000)
        for offset in range(0xbac84, 0xbad08, 4): word(offset, 0xe1a00000)
        word(0xbac84, 0xe5d12018)
        word(0xbac88, 0xe3822008)
        word(0xbac9c, 0xe5c12018)
        word(0xbaca4, 0xe5d12018)
        word(0xbaca8, 0xe3c22008)
        branch(0xbacac, 0xbace8)
        word(0xbace8, 0xe5c12018)
        word(0xbacf0, 0xe5d12018)
        word(0xbacf4, 0xe5d13010)
        path = Path(work) / 'synthetic.rom'
        path.write_bytes(rom)
        def run(*options, code=1):
            p = subprocess.run([str(PROBE), str(path), 'linked', '100', 'exact', *options],
                               capture_output=True, text=True, timeout=15)
            assert p.returncode == code, (options, p.returncode, p.stdout, p.stderr)
            return p.stdout + p.stderr
        flag = '--cs5-reset-status-input='
        bootstrap = '--bootstrap-peripherals'
        assert 'External-input scenarios require --bootstrap-peripherals' in run(flag+'0')
        for bad in ('', '-1', '+1', '256', '0x100', '0x', '1 ', ' 1', '1.5', '0xffjunk', '18446744073709551616'):
            assert 'Supply one 8-bit --cs5-reset-status-input value' in run(bootstrap, flag+bad)
        assert 'Supply one 8-bit --cs5-reset-status-input value' in run(bootstrap, flag+'0', flag+'1')
        default = run(bootstrap, code=2)
        assert 'cs5-reset ' not in default and 'DIAGNOSTIC CS5 RESET' not in default
        assert 'address=16000018 size=1 value=00000000 register=unknown status=unmapped-register' in default
        for sample in (0, 0xf7):
            output = run(bootstrap, flag+str(sample), code=2)
            events = [line for line in output.splitlines() if line.startswith('cs5-reset ')]
            assert len(events) == 5, output
            assert [int(re.search(r'value=(\w+)', e)[1],16) for e in events[:5]] == [sample,sample|8,sample|8,sample,sample]
            assert all('source=diagnostic-reset-echo' in e for e in events)
            config = [line for line in output.splitlines() if line.startswith('cs5-configuration ')]
            assert len(config) == 1 and 'address=16000010 size=1' in config[0]
            assert 'status=external-input-unavailable' in config[0], config
            assert 'Configured virtual REG_BASE=01 selects +10000' in output
            assert 'r2='+f'{sample:08x}' in output and 'source=diagnostic-cs5-reset' in output
        branch(0xbacf4, 0xbab4c)
        word(0xbab4c, 0xe5d12010)
        word(0xbab50, 0xe3520001)
        word(0xbab54, 0x03a0200e)
        branch(0xbab58, 0xbabf8)
        word(0xbabf8, 0xe5c1201c)
        word(0xbabfc, 0xe2811801)
        word(0xbac00, 0xe5913000)
        path.write_bytes(rom)
        for engine in ('exact', 'fast'):
            p = subprocess.run([str(PROBE), str(path), 'linked', '100', engine, bootstrap, flag+'0'],
                               capture_output=True, text=True, timeout=15)
            assert p.returncode == 2, (engine, p.stdout, p.stderr)
            output = p.stdout + p.stderr
            config = [line for line in output.splitlines() if line.startswith('cs5-configuration ')]
            assert len(config) == 3, output
            assert 'pc=200bab4c operation=read address=16000010 size=1 value=00000001' in config[0]
            assert 'pc=200babf8 operation=write address=1600001c size=1 value=0000000e defaults-known=1 status=ok' in config[1]
            assert 'pc=200bac00 operation=read address=16010000 size=4' in config[2]
            assert 'defaults-known=1 status=external-input-unavailable' in config[2]
            assert 'stop=unsupported-peripheral' in output and 'pc=200bac00 instruction=e5913000' in output
            assert 'source=diagnostic-cs5-configuration' in output
    print('PASS: explicit CS5 parsing, default guard, native reset-helper PCs, opaque sample preservation configured aperture/defaults and full identity guard in both engines')

if __name__ == '__main__': main()
