#!/usr/bin/env python3
"""frond-68k ROM checks. Synthetic files only, unless --rom names private ROMs to accept."""
import argparse
import hashlib
import random
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'out/frond-68k'
# Private IIIx ROMs the profile takes: SHA-256, bytes and card header version.
KNOWN = {
    '6c49768abc2aeeb1926ce2efd48998464d0db7d17e83f447afa76e9c1ebeca80': (1212416, 3),  # Palm-IIIx-3.1-en
    '9ad306533815a814d683bd937269a5b76f1be49df8e0e68aa0d50c39d148e423': (1212416, 3),  # Palm-IIIx-3.1-dbg
    '6f5ea152a22e43e4c7b83c271bf7c91c557794aa0d1011be7898b91be65d1dde': (1540096, 4),  # Palm-OS-3.5-en-ez
    '29c696f48f50b6e2fcb37023aca24447d01c903c1b1bc8327a7debd7016d9fb1': (1540096, 4),  # Palm-OS-3.5-en-ez-dbg
    '78298aaf48f2247c013e54d1d05820edd41ca057a948ac8492939967c879d9a8': (1736704, 5),  # Palm-IIIx-V-4.1-en
}


def header(size, version=3, flags=0x20, reset=0x10c0822a, card=None):
    data = bytearray(size)
    if size >= 0x100:
        struct.pack_into('>IIIHH', data, 0, 0x3000, reset, 0xfeedbeef, version, flags)
        data[0x10:0x19] = b'PalmCard\0'
        struct.pack_into('>I', data, 0x6c, size if card is None else card)
    return bytes(data)


def run(path, *args):
    return subprocess.run([str(CORE), '--rom', str(path), *args], capture_output=True, text=True, timeout=60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path, action='append', default=[], help='private ROM that must be accepted')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='frond-68k-') as work:
        arm = bytearray(0x128000)
        struct.pack_into('<I', arm, 8, 0xfeedbeef)
        arm[0x10:0x19] = b'PalmCard\0'
        cases = {
            'empty': (b'', 'cannot read ROM'),
            'short': (header(0x80), 'not a 68k Palm ROM'),
            'arm': (bytes(arm), 'not a 68k Palm ROM'),
            'oversize': (header(0x201000), 'unsupported 68k ROM'),
            'truncated': (header(0x120000, card=0x128000), 'unsupported 68k ROM'),
            '68328': (header(0x128000, flags=0x10), 'unsupported 68k ROM'),
            # A header the quick check takes, with nothing behind it.
            'hollow': (header(0x128000), 'unreadable ROM'),
        }
        for name, (data, message) in cases.items():
            path = Path(work) / f'{name}.rom'
            path.write_bytes(data)
            result = run(path, '--check-rom')
            if result.returncode != 1 or message not in result.stderr:
                sys.exit(f'{name}: expected "{message}", got {result.returncode}: {result.stderr.strip()}')
        # Damaged ROMs behind a good header are refused, never crash the reader.
        for seed in range(8):
            rng = random.Random(seed)
            data = bytearray(header(0x128000))
            data[0x100:] = rng.randbytes(len(data) - 0x100)
            # Some point the big ROM header back at the small one or past the end.
            struct.pack_into('>I', data, 0x58, rng.choice([0, 0x10c08200, 0x10c00200]))
            struct.pack_into('>I', data, 0x68, rng.choice([0, 0x10c08000, 0x10cff000]))
            path = Path(work) / f'noise{seed}.rom'
            path.write_bytes(data)
            result = run(path, '--check-rom')
            if result.returncode != 1:
                sys.exit(f'noise {seed}: expected a refusal, got {result.returncode}: {result.stderr.strip()}')
        # Sessions refused before a machine starts: damaged, for another device, or with storage
        # a IIIx doesn't have. The ROM inside is a hollow one, which is never reached.
        def session(name='palm-iiix', nand=b'', sd=b'', metadata=b'frond-68k 1\ncloudpilot 064d5d5\n', cut=0):
            rom = header(0x128000)
            body = struct.pack('<II', 0x19800819, 6) + name.encode().ljust(32, b'\0')
            toc = struct.pack('<8I', 1, 4 << 20, len(metadata), len(rom), len(nand), 1, 1, len(sd))
            data = body + toc + zlib.compress(metadata + rom + nand + b'\0\0' + sd)
            return data[:len(data) - cut]
        sessions = {
            'damaged': (session(cut=3), 'invalid session image'),
            'tx': (session(name='palm-tx'), 'session is not for a 68k device'),
            'nand': (session(nand=bytes(16)), "storage a Palm IIIx doesn't have"),
        }
        for name, (data, message) in sessions.items():
            path = Path(work) / f'{name}.session'
            path.write_bytes(data)
            result = subprocess.run([str(CORE), str(path), '--run-for', '1'], capture_output=True, text=True, timeout=60)
            if result.returncode != 1 or message not in result.stderr:
                sys.exit(f'{name} session: expected "{message}", got {result.returncode}: {result.stderr.strip()}')
        result = run(Path(work) / 'hollow.rom', '--device', 'palm-tx', '--check-rom')
        if result.returncode != 1:
            sys.exit('palm-tx: an ARM device was taken by frond-68k')
    for rom in args.rom:
        data = rom.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest not in KNOWN:
            sys.exit(f'{rom.name}: unknown ROM {digest}')
        size, version = KNOWN[digest]
        result = run(rom, '--check-rom')
        expected = [f'device palm-iiix', f'card version {version}', f'rom bytes {size}']
        if result.returncode or any(line not in result.stdout.splitlines() for line in expected):
            sys.exit(f'{rom.name}: refused: {result.stdout}{result.stderr}')
        print(f'{rom.name}: accepted, card version {version}')
    print('PASS: frond-68k refuses synthetic non-IIIx ROMs' + (' and accepts the given ROMs' if args.rom else ''))


if __name__ == '__main__':
    main()
