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
    words = (0xe59f0018, 0xe5901000, 0xe59f0014, 0xe5902000,
             0xe59f0010, 0xe5903000, 0xeafffffe, 0xe1a00000,
             0x0021c024, 0x0021c124, 0x0021c324)
    struct.pack_into('<11I', rom, 0x100, *words)
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
        assert 'External GPIO-B levels are unknown.' in unknown
        assert 'External GPIO-D levels are unknown.' in unknown
        d_only = run('--bootstrap-peripherals', '--gpio-d-input=0x600')
        assert 'stop=unsupported-peripheral' in d_only and 'address=0021c024' in d_only
        assert 'Hypothetical static from-reset full-port GPIO-D input=00000600' in d_only
        a_only = run('--bootstrap-peripherals', '--gpio-a-input=0')
        assert 'stop=unsupported-peripheral' in a_only and 'address=0021c124' in a_only
        assert 'Hypothetical static from-reset full-port GPIO-A input=00000000' in a_only
        assert 'External GPIO-B levels are unknown.' in a_only
        assert 'External GPIO-D levels are unknown.' in a_only
        b_only = run('--bootstrap-peripherals', '--gpio-b-input=0x08000000')
        assert 'stop=unsupported-peripheral' in b_only and 'address=0021c024' in b_only
        assert 'Hypothetical static from-reset full-port GPIO-B input=08000000' in b_only
        ad_only = run('--bootstrap-peripherals', '--gpio-a-input=0x80', '--gpio-d-input=0x600')
        assert 'stop=unsupported-peripheral' in ad_only and 'address=0021c124' in ad_only
        assert 'r1=00000080' in ad_only
        ab_only = run('--bootstrap-peripherals', '--gpio-a-input=0x80', '--gpio-b-input=0x08000000')
        assert 'stop=unsupported-peripheral' in ab_only and 'address=0021c324' in ab_only
        assert 'r1=00000080 r2=08000000' in ab_only
        for sample in ('0', '0x80', '4294967295', '0377'):
            known = run('--bootstrap-peripherals', '--gpio-a-input=' + sample,
                        '--gpio-b-input=0x08000000', '--gpio-d-input=0x600')
            value = 255 if sample == '0377' else int(sample, 0)
            assert 'stop=instruction-budget' in known
            assert f'r1={value:08x} r2=08000000 r3=00000600' in known
            assert f'Hypothetical static from-reset full-port GPIO-A input={value:08x}' in known
            assert 'Hypothetical static from-reset full-port GPIO-D input=00000600' in known
            assert 'jit-blocks=0 jit-instructions=0' in known
        for sample in ('0', '0x08000000', '4294967295', '0377'):
            known = run('--bootstrap-peripherals', '--gpio-a-input=0x80',
                        '--gpio-b-input=' + sample, '--gpio-d-input=0x600')
            value = 255 if sample == '0377' else int(sample, 0)
            assert 'stop=instruction-budget' in known
            assert f'r1=00000080 r2={value:08x} r3=00000600' in known
            assert f'Hypothetical static from-reset full-port GPIO-B input={value:08x}' in known
            assert 'jit-blocks=0 jit-instructions=0' in known
        # Configure B14, clear only its ISR bit, enable it, read IMR then try ISR.
        # The admitted path must still reject a full ISR read with other pins unqualified.
        def interrupt_rom(mode):
            words = [0, 0, 0xe5801028, 0, 0xe5801034, 0xe5801030,
                     0xe5902030, 0xe5903034, 0xeafffffe]
            constants = [(0, 0, 0x0021c100), (1, 1, mode << 28), (3, 1, 0x4000)]
            for at, register, value in constants:
                literal = len(words)
                words[at] = 0xe59f0000 | register << 12 | ((literal - at - 2) * 4)
                words.append(value)
            struct.pack_into('<' + str(len(words)) + 'I', rom, 0x100, *words)
            image.seek(0)
            image.write(rom)
            image.flush()

        for mode, sample, idle in ((2, '0x08000000', True), (2, None, False),
                                   (2, '0x08004000', False), (3, '0x08004000', True),
                                   (3, '0x08000000', False), (0, '0x08000000', True),
                                   (0, '0x08004000', False), (1, '0x08004000', True),
                                   (1, '0x08000000', False)):
            interrupt_rom(mode)
            options = ['--bootstrap-peripherals']
            if sample is not None:
                options.append('--gpio-b-input=' + sample)
            result = run(*options)
            assert 'stop=unsupported-peripheral' in result
            if idle:
                assert 'address=0021c130 size=4 value=00004000 register=GPIO.IMR_B status=ok' in result
                assert 'address=0021c134 size=4 value=00000000 register=GPIO.ISR_B status=external-input-unavailable' in result
                assert 'r2=00004000' in result
            else:
                assert 'address=0021c130 size=4 value=00004000 register=GPIO.IMR_B status=unsupported-side-effect' in result
                assert 'r2=00000000' in result
        # Restore the sample-reader image used by the remaining parser checks.
        words = (0xe59f0018, 0xe5901000, 0xe59f0014, 0xe5902000,
                 0xe59f0010, 0xe5903000, 0xeafffffe, 0xe1a00000,
                 0x0021c024, 0x0021c124, 0x0021c324)
        struct.pack_into('<11I', rom, 0x100, *words)
        image.seek(0)
        image.write(rom)
        image.flush()
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
        for port in ('a', 'b', 'd'):
            for sample in ('', '-1', '-0', '+1', ' 1', '\t1', '1 ', '0x', '1junk', '4294967296', '0x100000000', '999999999999999999999999'):
                invalid = run('--bootstrap-peripherals', f'--gpio-{port}-input={sample}', code=1)
                assert f'GPIO-{port.upper()} input must be a 32-bit value' in invalid
            missing_bootstrap = run(f'--gpio-{port}-input=0', code=1)
            assert 'External-input scenarios require --bootstrap-peripherals' in missing_bootstrap
        for option in ('--gpio-c-input=0', '--gpio-a-input', '--gpio-b-input', '--gpio-A-input=0', '--gpio-B-input=0'):
            assert 'Unknown diagnostic option:' in run('--bootstrap-peripherals', option, code=1)
    print('PASS: explicit GPIO-A/B/D whole-port samples, idle interrupt/ISR guards, parsing and Exact propagation')


if __name__ == '__main__':
    main()
