#!/usr/bin/env python3
"""Firmware-free explicit CS4 transport recording and CLI guards."""

from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'build/zodiac-boot-probe'


def main():
    with tempfile.TemporaryDirectory(prefix='zodiac-cs4-capture-', dir=ROOT / 'build') as work:
        rom = bytearray(0x7d0000)
        for base in (0, 0x20000):
            struct.pack_into('<I', rom, base + 8, 0xfeedbeef)
            rom[base + 0x10:base + 0x19] = b'PalmCard\0'
            struct.pack_into('<II', rom, base + 0x80, 0x54707776, 0x4d583161)
        struct.pack_into('<I', rom, 0x20000 + 0x6c, len(rom) - 0x20000)
        struct.pack_into('<I', rom, 0, 0xea00003e)
        # EIM CS4U/L configuration followed by the native startup's four pairs.
        words = [0xe59f0100, 0xe59f1100, 0xe5801000, 0xe59f10fc, 0xe5801004,
                 0xe59f00f8]
        for index, byte in ((0x0f, 1), (0x0d, 0x0d), (0x49, 3), (0x48, 0)):
            words.extend((0xe3a01000 | index, 0xe5c01000, 0xe5d02000,
                          0xe3a01000 | byte, 0xe5c01001, 0xe5d02000))
        # A consumed data read must stop, even after a captured write to that index.
        words.extend((0xe5d03001, 0xeafffffe))
        struct.pack_into(f'<{len(words)}I', rom, 0x100, *words)
        struct.pack_into('<4I', rom, 0x208, 0x00220020, 0x3e70, 0xffff4301, 0x15000000)
        path = Path(work) / 'synthetic.rom'
        path.write_bytes(rom)

        def run(*options, code=1, engine="exact", budget="100"):
            result = subprocess.run([str(PROBE), str(path), 'physical', budget, engine, *options],
                                    capture_output=True, text=True, timeout=15)
            assert result.returncode == code, (options, result.returncode, result.stdout, result.stderr)
            return result.stdout + result.stderr

        bootstrap = '--bootstrap-peripherals'
        supplied = '--cs4-port0-input='
        assert 'External-input scenarios require --bootstrap-peripherals' in run(supplied + '0')
        for bad in ('', '-1', '+1', '256', '0x100', '0x', '0xffjunk', '1 ', ' 1', '1.5',
                    '18446744073709551616'):
            assert 'Supply one 8-bit --cs4-port0-input value' in run(bootstrap, supplied + bad)
        assert 'Supply one 8-bit --cs4-port0-input value' in run(bootstrap, supplied + '0', supplied + '255')
        assert 'Unknown diagnostic option:' in run(bootstrap, '--cs4-port0-input')
        for budget in ('0', '100000001', '4294967296'):
            assert 'Instruction budget must be 1..100000000' in run(bootstrap, budget=budget)
        # The maximum parses, but the unsupplied data read stops this fixture promptly.
        capped = run(bootstrap, supplied + '0', code=2, budget='100000000')
        assert 'budget=100000000' in capped and 'stop=unsupported-peripheral' in capped
        normal = run(bootstrap, code=2)
        assert 'cs4-capture ' not in normal and 'DIAGNOSTIC CS4 RECORDER' not in normal
        assert 'stop=unsupported-write' in normal
        assert 'address=15000000 size=1 value=0000000f register=unknown status=unmapped-register' in normal
        commands = []
        for byte in ('0x00', '0xff'):
            output = run(bootstrap, supplied + byte, code=2)
            assert f'DIAGNOSTIC CS4 RECORDER: raw port0 input={int(byte, 0):02x}' in output
            assert 'physical read side effects, audio, FIFO and timing are unmodelled; data reads stop.' in output
            events = [line for line in output.splitlines() if line.startswith('cs4-capture ')]
            data = [line for line in events if 'operation=data-command ' in line]
            assert len(data) == 4, output
            commands.append([re.search(r'value=(\w+) index=(\w+) bank=(\w+) status=(\S+)', line).groups()
                             for line in data])
            for line in events:
                assert 'source=diagnostic-recorder' in line
                pc = int(re.search(r'pc=(\w+)', line)[1], 16)
                step = int(re.search(r'step=(\d+)', line)[1])
                assert pc == 0x10000100 + (step - 1) * 4, line
            samples = [line for line in events if 'operation=raw-port0-input ' in line]
            assert len(samples) == 8 and all(f'value={int(byte, 0):02x} ' in line for line in samples)
            assert 'operation=index-write address=15000000 size=1 value=0f index=0f bank=unknown status=ok' in output
            assert 'operation=data-read address=15000001 size=1 value=00 index=48 bank=1 status=external-input-unavailable' in output
            assert 'source=diagnostic-cs4-capture' in output
            assert 'stop=unsupported-peripheral' in output and 'r3=00000000' in output
            assert 'r2=' + ('000000ff' if byte == '0xff' else '00000000') in output
        indexed = '--cs4-indexed-input='
        error = 'Supply unique --cs4-indexed-input=BANK/INDEX/BYTE'
        for bad in ('', '1', '1/14', '1/14/', '/14/0', '2/14/0', '1/256/0', '1/14/256',
                    '-1/14/0', '+1/14/0', '1/-1/0', '1/14/+1', ' 1/14/0', '1/14/0 ',
                    '1/14/0/1', '1//0', '1/0x/0', '1/14/1.5', '1/14/18446744073709551616'):
            assert error in run(bootstrap, supplied + '0', indexed + bad)
        assert error in run(bootstrap, supplied + '0', indexed + '1/14/0', indexed + '1/0x0e/3')
        for options in ((indexed + '1/14/0',), (bootstrap, indexed + '1/14/0')):
            assert 'CS4 indexed inputs require' in run(*options)
        assert 'Unknown diagnostic option:' in run(bootstrap, '--cs4-indexed-input')

        # Native init compares index 0e with 3; other bytes take its ordinary -2 failure path.
        words = [0xe59f0100, 0xe59f1100, 0xe5801000, 0xe59f10fc, 0xe5801004, 0xe59f00f8,
                 0xe3a0100f, 0xe5c01000, 0xe3a01001, 0xe5c01001,
                 0xe3a0100e, 0xe5c01000, 0xe5d04001, 0xe3540003, 0x13e05001, 0x03a05000,
                 0xe3a06000, 0xe3550000, 0x03a06001, 0xeafffffe]
        struct.pack_into(f'<{len(words)}I', rom, 0x100, *words)
        path.write_bytes(rom)
        for engine in ('exact', 'fast'):
            guarded = run(bootstrap, supplied + '0', code=2, engine=engine)
            assert 'status=external-input-unavailable' in guarded and 'stop=unsupported-peripheral' in guarded
            for byte in (0, 3, 255):
                output = run(bootstrap, supplied + '0', indexed + f'1/0x0e/{byte}',
                             indexed + '0/14/42', code=2, engine=engine)
                assert 'stop=instruction-budget' in output, output
                assert f'r4={byte:08x} r5=' + ('00000000' if byte == 3 else 'fffffffe') in output
                assert 'r6=' + ('00000001' if byte == 3 else '00000000') in output
                assert f'operation=raw-indexed-input address=15000001 size=1 value={byte:02x} index=0e bank=1 status=ok source=diagnostic-indexed-input' in output
                assert f'DIAGNOSTIC CS4 INDEXED INPUT: bank=1 index=0e byte={byte:02x}' in output
            unknown = run(bootstrap, supplied + '0', indexed + '0/14/3', code=2, engine=engine)
            assert 'stop=unsupported-peripheral' in unknown and 'status=external-input-unavailable' in unknown
        expected = [('01', '0f', '1', 'ok'), ('0d', '0d', '1', 'ok'),
                    ('03', '49', '1', 'ok'), ('00', '48', '1', 'ok')]
        assert commands[0] == commands[1] == expected, commands
    print('PASS: default CS4 stop, explicit byte parsing, four startup pairs, raw sample independence, trace PCs, guarded data reads and explicit banked input branches')


if __name__ == '__main__':
    main()
