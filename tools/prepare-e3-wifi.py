#!/usr/bin/env python3
"""Prepare the Tungsten E3 driver files from Palm's Wi-Fi Installer 1.2."""

import argparse
import hashlib
from pathlib import Path
import struct
import zlib

INSTALLER_SHA256 = 'adb714f4e8a9f979340991cbd9dc71d03dcfa1e30b4d96236cb938c4bedde43c'
E2_PACKAGE_OFFSET = 625709
E2_PACKAGE_SIZE = 562849

# The E3 ROM keeps its own SDIO, Net Library and status bar.
DATABASES = {
    6: ('ArmLog', 'libr', 0x0001),
    20: ('NetServLib', 'libr', 0x0001),
    21: ('PmNet Library', 'libr', 0x0001),
    22: ('WL155_PRIMARY_FIRMWARE', 'PRIF', 0),
    23: ('Prism3SlotDriver', 'libr', 0x0001),
    25: ('SCardIntf', 'libr', 0x0001),
    26: ('WL155_SECONDARY_FIRMWARE', 'SECF', 0),
    27: ('SP3WLAN', 'libr', 0x0001),
    44: ('WiFiLib', 'neti', 0x0001),
    46: ('WiFiLib_enUS', 'ovly', 0x0801),
    51: ('WiFiNPPI', 'nppi', 0x0001),
    53: ('WiFiNPPI_enUS', 'ovly', 0x0801),
    58: ('WiFiPanel', 'panl', 0x0001),
    60: ('WiFiPanel_enUS', 'ovly', 0x0801),
    65: ('WiFiSetup', 'appl', 0x0001),
    67: ('WiFiSetup_enUS', 'ovly', 0x0801),
}


def prepare(installer):
    if hashlib.sha256(installer).hexdigest() != INSTALLER_SHA256:
        raise ValueError('Expected the original Palm Wi-Fi Installer 1.2 PRC')
    package = installer[E2_PACKAGE_OFFSET:E2_PACKAGE_OFFSET + E2_PACKAGE_SIZE]
    entries = {}
    for i in range(struct.unpack_from('>H', package, 76)[0]):
        kind, part, offset = struct.unpack_from('>4sHI', package, 78 + 10 * i)
        if kind.startswith(b'i') and kind[1:].isdigit() and int(kind[1:]) in DATABASES:
            entries.setdefault(int(kind[1:]), {})[part] = zlib.decompress(package[offset:])
    for index, (name, kind, attributes) in DATABASES.items():
        parts = entries[index]
        database = bytearray(b''.join(parts[part] for part in range(len(parts))))
        database[:32] = name.encode().ljust(32, b'\0')
        database[60:64] = kind.encode()
        struct.pack_into('>H', database, 32, attributes)
        yield f'{index:03d}-{name}.prc', database


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('installer', type=Path)
    parser.add_argument('output', type=Path, help='new private output folder')
    args = parser.parse_args()
    try:
        files = list(prepare(args.installer.read_bytes()))
        args.output.mkdir(mode=0o700)
        for name, data in files:
            with open(args.output / name, 'xb', opener=private_file) as output:
                output.write(data)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')


def private_file(path, flags):
    import os
    return os.open(path, flags, 0o600)


if __name__ == '__main__':
    main()
