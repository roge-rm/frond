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

        def run(*options, code=1, budget=50):
            result = subprocess.run([str(PROBE), str(path), 'physical', str(budget), 'exact', *options],
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
        stimulus = '--asp-u-sample=0x8000'
        assert 'External-input scenarios require --bootstrap-peripherals' in run(stimulus)
        for value in ('', '-1', '+1', '65536', '0x10000', ' 1', '1 ', 'wat', '0x'):
            assert 'Supply one 16-bit --asp-u-sample value' in run(bootstrap, '--asp-u-sample=' + value)
        assert 'Supply one 16-bit --asp-u-sample value' in run(bootstrap, stimulus, stimulus)
        for flags in ((stimulus, '--cs5-reset-status-input=0'), ('--cs5-reset-status-input=0', stimulus)):
            combined = run(bootstrap, high, *flags, code=2)
            assert 'stop=instruction-budget' in combined
            assert 'manual U post-filter sample=8000' in combined
            assert 'opaque status input=00' in combined
        # Reset, manual U setup, poll PDR, consume once, preserve the stimulus through software reset.
        words = [0xe59f0044, 0xe3a01502, 0xe5801010, 0xe3a01003, 0xe580102c,
                 0xe59f1034, 0xe5801014, 0xe59f1030, 0xe5801010,
                 0xe3811002, 0xe5801010, 0xe590201c, 0xe3120001, 0x0afffffc,
                 0xe5903000, 0xe590201c, 0xeafffffe,
                 0xe1a00000, 0xe1a00000, 0x00215000, 0x1814, 0x02003001]
        struct.pack_into('<22I', rom, 0x100, *words)
        path.write_bytes(rom)
        unknown = run(bootstrap, code=2, budget=100000)
        assert 'address=00215010 size=4 value=02003003 register=ASP.ACNTLCR status=external-input-unavailable' in unknown
        for value, expected in (('0', '00000000'), ('0x8000', '00008000'), ('65535', '0000ffff')):
            known = run(bootstrap, '--asp-u-sample=' + value, code=2, budget=100000)
            assert 'stop=instruction-budget' in known, known[-3000:]
            assert 'address=00215000 size=4 value=' + expected + ' register=ASP.PADFIFO status=ok' in known
            assert 'r2=00000000 r3=' + expected in known
    print('PASS: fixed PX1 and manual U stimulus parsing, reset retention, timed polling and FIFO consumption')


if __name__ == '__main__':
    main()
