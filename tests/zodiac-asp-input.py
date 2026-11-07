#!/usr/bin/env python3
"""Firmware-free fixed ASP PX1 scenario parsing and propagation checks."""

from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'build/zodiac-boot-probe'


def main():
    with tempfile.TemporaryDirectory(prefix='zodiac-asp-input-', dir=ROOT / 'build') as work:
        rom = bytearray(0x7d0000)
        for base in (0, 0x20000):
            struct.pack_into('<I', rom, base + 8, 0xfeedbeef)
            rom[base + 0x10:base + 0x19] = b'PalmCard\0'
            struct.pack_into('<II', rom, base + 0x80, 0x54707776, 0x4d583161)
        struct.pack_into('<I', rom, 0x20000 + 0x6c, len(rom) - 0x20000)
        struct.pack_into('<I', rom, 0, 0xea00003e)
        # Software reset, enable active-low level detection, read PEN and ICNTLR.
        words = [0xe59f0018, 0xe59f1018, 0xe5801010, 0xe3a01010,
                 0xe5801018, 0xe590201c, 0xe5903018, 0xeafffffe,
                 0x00215000, 1 << 23]
        struct.pack_into('<10I', rom, 0x100, *words)
        path = Path(work) / 'synthetic.rom'
        path.write_bytes(rom)

        def run(*options, code=1):
            result = subprocess.run([str(PROBE), str(path), 'physical', '50', 'exact', *options],
                                    capture_output=True, text=True, timeout=15)
            assert result.returncode == code, (options, result.returncode, result.stdout, result.stderr)
            return result.stdout + result.stderr

        high = '--asp-px1-input=high'
        bootstrap = '--bootstrap-peripherals'
        assert 'External-input scenarios require --bootstrap-peripherals' in run(high)
        for option in ('--asp-px1-input=', '--asp-px1-input=low', '--asp-px1-input=HIGH',
                       '--asp-px1-input=high ', '--asp-px1-input=1'):
            assert 'Supply --asp-px1-input=high once, held from reset' in run(bootstrap, option)
        assert 'Supply --asp-px1-input=high once, held from reset' in run(bootstrap, high, high)
        assert 'Unknown diagnostic option:' in run(bootstrap, '--asp-px1-input')
        unknown = run(bootstrap, code=2)
        assert 'External ASP PX1 level and history are unknown.' in unknown
        assert 'address=00215018 size=4 value=00000010 register=ASP.ICNTLR status=external-input-unavailable' in unknown
        assert 'stop=unsupported-peripheral' in unknown
        known = run(bootstrap, high, code=2)
        assert 'Diagnostic ASP PX1 input held high from reset; no transitions;' in known
        assert 'address=00215018 size=4 value=00000010 register=ASP.ICNTLR status=ok' in known
        assert 'stop=instruction-budget' in known
        assert 'r2=00000000 r3=00000010' in known
    print('PASS: fixed ASP PX1 high input parsing, bootstrap requirement, reset retention and propagation')


if __name__ == '__main__':
    main()
