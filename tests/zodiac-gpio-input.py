#!/usr/bin/env python3
"""Firmware-free GPIO scenario parsing and probe propagation checks."""

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
    struct.pack_into('<I', rom, 0x20000 + 0x6c, len(rom) - 0x20000)
    # Branch past the card header, read each port sample, then loop.
    struct.pack_into('<I', rom, 0, 0xea00003e)
    words = (0xe59f0010, 0xe5901000, 0xe59f000c, 0xe5902000,
             0xeafffffe, 0xe1a00000, 0x0021c024, 0x0021c324)
    struct.pack_into('<8I', rom, 0x100, *words)
    with tempfile.NamedTemporaryFile(prefix='zodiac-gpio-', suffix='.rom', dir=ROOT / 'build') as image:
        image.write(rom)
        image.flush()

        def run(*options, code=2):
            result = subprocess.run([str(PROBE), image.name, 'physical', '12', 'exact', *options],
                                    capture_output=True, text=True, timeout=15)
            assert result.returncode == code, (options, result.returncode, result.stderr)
            return result.stdout + result.stderr

        baseline = run()
        assert 'stop=unsupported-read' in baseline and 'address=0021c024' in baseline
        unknown = run('--bootstrap-peripherals')
        assert 'stop=unsupported-peripheral' in unknown and 'address=0021c024' in unknown
        assert 'peripheral-status=external-input-unavailable' in unknown
        assert 'External GPIO-A levels are unknown.' in unknown
        assert 'External GPIO-D levels are unknown.' in unknown
        d_only = run('--bootstrap-peripherals', '--gpio-d-input=0x600')
        assert 'stop=unsupported-peripheral' in d_only and 'address=0021c024' in d_only
        assert 'Hypothetical static full-port GPIO-D input=00000600' in d_only
        a_only = run('--bootstrap-peripherals', '--gpio-a-input=0')
        assert 'stop=unsupported-peripheral' in a_only and 'address=0021c324' in a_only
        assert 'Hypothetical static full-port GPIO-A input=00000000' in a_only
        assert 'External GPIO-D levels are unknown.' in a_only
        for sample in ('0', '0x80', '4294967295', '0377'):
            known = run('--bootstrap-peripherals', '--gpio-a-input=' + sample, '--gpio-d-input=0x600')
            value = 255 if sample == '0377' else int(sample, 0)
            assert 'stop=instruction-budget' in known
            assert f'r1={value:08x} r2=00000600' in known
            assert f'Hypothetical static full-port GPIO-A input={value:08x}' in known
            assert 'Hypothetical static full-port GPIO-D input=00000600' in known
            assert 'jit-blocks=0 jit-instructions=0' in known
        settled = run('--bootstrap-peripherals', '--assume-mpll-settled')
        assert 'MPLL settled assumption=enabled; nominal FCLK only after completed restart; no lock claim.' in settled
        assert 'MPLL settled assumption=disabled;' in unknown
        assert 'MPLL scenario requires --bootstrap-peripherals' in run('--assume-mpll-settled', code=1)
        assert 'Unknown diagnostic option:' in run('--bootstrap-peripherals', '--assume-mpll-settled=yes', code=1)
        spll = run('--bootstrap-peripherals', '--assume-spll-settled')
        assert 'SPLL settled assumption=enabled; initial nominal output/LF=1; restart holds output high for 2 CLK32 then activates target/LF=1 with phase zero; bus waits for that event without retiring instructions.' in spll
        assert 'SPLL settled assumption=disabled;' in unknown
        assert 'SPLL scenario requires --bootstrap-peripherals' in run('--assume-spll-settled', code=1)
        assert 'Unknown diagnostic option:' in run('--bootstrap-peripherals', '--assume-spll-settled=yes', code=1)
        for port in ('a', 'd'):
            for sample in ('', '-1', '-0', '+1', ' 1', '0x', '1junk', '4294967296', '0x100000000', '999999999999999999999999'):
                invalid = run('--bootstrap-peripherals', f'--gpio-{port}-input={sample}', code=1)
                assert f'GPIO-{port.upper()} input must be a 32-bit value' in invalid
            missing_bootstrap = run(f'--gpio-{port}-input=0', code=1)
            assert 'External-input scenarios require --bootstrap-peripherals' in missing_bootstrap
        for option in ('--gpio-b-input=0', '--gpio-c-input=0', '--gpio-a-input', '--gpio-A-input=0'):
            assert 'Unknown diagnostic option:' in run('--bootstrap-peripherals', option, code=1)
    print('PASS: explicit GPIO-A/D whole-port samples, unknown defaults, parsing and Exact propagation')


if __name__ == '__main__':
    main()
