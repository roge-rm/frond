#!/usr/bin/env python3
"""Firmware-free retained RTC scenario parsing and propagation checks."""
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'build/zodiac-boot-probe'


def main():
    rom = bytearray(0x7d0000)
    for base in (0, 0x20000):
        struct.pack_into('<I', rom, base + 8, 0xfeedbeef)
        rom[base + 0x10:base + 0x19] = b'PalmCard\0'
        struct.pack_into('<II', rom, base + 0x80, 0x54707776, 0x4d583161)
    struct.pack_into('<I', rom, 0x2006c, len(rom) - 0x20000)
    struct.pack_into('<I', rom, 0, 0xea00003e)
    words = (0xe59f0010, 0xe5901000, 0xe5902004, 0xe5903020, 0xeafffffe, 0xe1a00000, 0x00204000)
    struct.pack_into('<7I', rom, 0x100, *words)
    with tempfile.NamedTemporaryFile(prefix='zodiac-rtc-', suffix='.rom', dir=ROOT / 'build') as image:
        image.write(rom)
        image.flush()

        def run(*options, code=2):
            result = subprocess.run([str(PROBE), image.name, 'physical', '10', 'exact', *options],
                                    capture_output=True, text=True, timeout=15)
            assert result.returncode == code, (options, result.returncode, result.stderr)
            return result.stdout + result.stderr

        unknown = run('--bootstrap-peripherals')
        assert 'address=00204000' in unknown and 'peripheral-status=external-input-unavailable' in unknown
        assert 'Retained RTC day/time and subsecond phase are unknown.' in unknown
        for supplied, hm, sec, day in (('1:12:34:56', '00000c22', '00000038', '00000001'),
                                      ('511:23:59:59', '0000173b', '0000003b', '000001ff'),
                                      ('0:00:00:00', '00000000', '00000000', '00000000')):
            result = run('--bootstrap-peripherals', '--rtc-initial-time=' + supplied)
            assert 'stop=instruction-budget' in result
            assert f'r1={hm} r2={sec} r3={day}' in result
            assert 'CLK32 subsecond phase zero at entry' in result
            assert 'no civil epoch or host time' in result
        for value in ('', '1:12:34', '1:12:34:56:', '-1:0:0:0', '512:0:0:0', '1:24:0:0',
                      '1:0:60:0', '1:0:0:60', '1:0:0:0garbage', '1::0:0', ' 1:0:0:0',
                      '999999999999999999999:0:0:0', '1:0:0:0:0'):
            assert 'Supply one --rtc-initial-time' in run('--bootstrap-peripherals', '--rtc-initial-time=' + value, code=1)
        assert 'require --bootstrap-peripherals' in run('--rtc-initial-time=1:12:34:56', code=1)
        assert 'Supply one --rtc-initial-time' in run('--bootstrap-peripherals', '--rtc-initial-time=1:0:0:0',
                                                    '--rtc-initial-time=2:0:0:0', code=1)
    print('PASS: explicit retained RTC seed parsing, default unknown time and probe propagation')


if __name__ == '__main__':
    main()
