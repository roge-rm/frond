#!/usr/bin/env python3
"""Firmware-free private I2C image parsing and endpoint attachment checks."""

from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'build/zodiac-boot-probe'


def main():
    with tempfile.TemporaryDirectory(prefix='zodiac-i2c-image-', dir=ROOT / 'build') as work:
        work = Path(work)
        rom = bytearray(0x7d0000)
        for base in (0, 0x20000):
            struct.pack_into('<I', rom, base + 8, 0xfeedbeef)
            rom[base + 0x10:base + 0x19] = b'PalmCard\0'
            struct.pack_into('<II', rom, base + 0x80, 0x54707776, 0x4d583161)
        struct.pack_into('<I', rom, 0x20000 + 0x6c, len(rom) - 0x20000)
        struct.pack_into('<I', rom, 0, 0xea00003e)
        # Read one byte through a repeated START, master NACK and STOP.
        words = [0, 0xe3a01000, 0xe5801000, 0,
                 0xe3a01080, 0xe5801008, 0xe3a010b0, 0xe5801008,
                 0xe3a010a0, 0xe5801010]
        poll = [0xe590200c, 0xe3120080, 0x0afffffc]
        words += poll
        words += [0xe3a01000, 0xe5801010]
        words += poll
        words += [0xe3a010b4, 0xe5801008, 0xe3a010a1, 0xe5801010]
        words += poll
        words += [0xe3a010a8, 0xe5801008, 0xe5901010]
        words += poll
        words += [0xe3a01088, 0xe5801008, 0xe5901010, 0xeafffffe]
        literals = 0x100 + 4 * len(words)
        words[0] = 0xe59f0000 | (literals - 0x108)
        words[3] = 0xe59f0000 | (literals + 4 - 0x114)
        struct.pack_into('<' + str(len(words)) + 'I', rom, 0x100, *words)
        struct.pack_into('<II', rom, literals, 0x0021c020, 0x00217000)
        rom_path = work / 'synthetic.rom'
        rom_path.write_bytes(rom)
        image = work / 'window.bin'
        contents = bytes(range(256))
        image.write_bytes(contents)
        scenario = ('--bootstrap-peripherals', '--assume-i2c-single-master')
        option = '--i2c-eeprom-image=' + str(image)

        def run(*options, code=1):
            result = subprocess.run([str(PROBE), str(rom_path), 'physical', '3000', 'exact', *options],
                                    capture_output=True, text=True, timeout=15)
            assert result.returncode == code, (options, result.returncode, result.stdout[-2000:], result.stderr)
            return result.stdout + result.stderr

        unknown = run(*scenario, code=2)
        assert 'phase=address address=50 byte=a0 direction=write master-ack=0 response=unknown' in unknown
        assert 'peripheral-status=external-input-unavailable' in unknown
        attached = run(*scenario, option, code=2)
        assert 'Explicit 256-byte I2C 0x50 read window attached; caller-supplied bytes;' in attached
        assert 'phase=address address=50 byte=a0 direction=write master-ack=0 response=ack' in attached
        assert 'stop=instruction-budget' in attached
        events = [dict(field.split('=', 1) for field in line.split()[1:])
                  for line in attached.splitlines() if line.startswith('i2c ')]
        phases = ['start', 'address', 'transmit', 'repeated-start', 'address', 'receive', 'stop']
        issuers = [0x11c, 0x124, 0x138, 0x14c, 0x154, 0x16c, 0x180]
        assert [event['phase'] for event in events] == phases, events
        for event, issuer in zip(events, issuers):
            assert int(event['issue-pc'], 16) == 0x10000000 + issuer, event
            if event['phase'] in ('start', 'repeated-start', 'stop'):
                assert event['issue-step'] == event['step'], event
            else:
                assert int(event['issue-step']) < int(event['step']), event
                assert event['status'] == 'a2', event
        assert events[-2]['byte'] == '00' and events[-2]['master-ack'] == '0'
        assert image.read_bytes() == contents
        assert 'I2C image requires --bootstrap-peripherals and --assume-i2c-single-master' in run(option)
        assert 'I2C image requires --bootstrap-peripherals and --assume-i2c-single-master' in run(scenario[0], option)
        for options in (('--i2c-eeprom-image=',), (option, option)):
            assert 'Supply one nonempty --i2c-eeprom-image path' in run(*scenario, *options)
        assert 'Expected an exact 256-byte I2C read-window image' in run(*scenario, option + '.missing')
        assert 'Expected an exact 256-byte I2C read-window image' in run(*scenario, '--i2c-eeprom-image=' + str(work))
        for size in (0, 1, 255, 257):
            image.write_bytes(b'\xff' * size)
            assert 'Expected an exact 256-byte I2C read-window image' in run(*scenario, option)
        assert 'Unknown diagnostic option:' in run(*scenario, '--i2c-eeprom-image')
    print('PASS: exact I2C image length, parsing, scenario guards, read-only input and explicit endpoint attachment')


if __name__ == '__main__':
    main()
