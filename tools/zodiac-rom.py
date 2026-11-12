#!/usr/bin/env python3
"""Makes the Zodiac ROM from Tapwave's Zodiac 1.1 update.

  zodiac-rom.py Zodiac_1.1_ROM_Update.zip zodiac.rom

The input is the update zip or the zodiac_1.1_update.prc inside it. The ROM is the PRC's
ROMZ resource with id 2, unpacked. Same checks as zodiac-rom/ZodiacRom.cpp.
"""
import hashlib
import struct
import sys
import zipfile
import zlib

LENGTH = 8192000
SHA256 = "2207a735e812299b7d1ede9da01c5e867bd932a1f15ce46b81c8737e42e9111d"
NOT_UPDATE = "This isn't the Zodiac 1.1 update."
DAMAGED = "This update is damaged or cut short."


def from_prc(prc):
    if len(prc) < 0x4e or not struct.unpack_from(">H", prc, 0x20)[0] & 1 or struct.unpack_from(">I", prc, 0x48)[0] != 0:
        raise ValueError(NOT_UPDATE)
    count = struct.unpack_from(">H", prc, 0x4c)[0]
    if len(prc) < 0x4e + count * 10:
        raise ValueError(DAMAGED)
    entries = [struct.unpack_from(">4sHI", prc, 0x4e + i * 10) for i in range(count)]
    for kind, number, start in entries:
        if kind != b"ROMZ" or number != 2:
            continue
        end = min([o for _, _, o in entries if o > start] + [len(prc)])
        if start >= len(prc) or end <= start:
            raise ValueError(DAMAGED)
        gunzip = zlib.decompressobj(31)
        rom = b""
        pending = prc[start:end]
        try:
            while pending and not gunzip.eof and len(rom) <= LENGTH:
                rom += gunzip.decompress(pending, 1 << 20)
                pending = gunzip.unconsumed_tail
                if not pending:
                    break
        except zlib.error:
            raise ValueError(DAMAGED)
        if len(rom) > LENGTH:
            raise ValueError(DAMAGED)
        if not gunzip.eof or gunzip.unused_data:
            raise ValueError(DAMAGED)
        if len(rom) != LENGTH or hashlib.sha256(rom).hexdigest() != SHA256:
            raise ValueError(NOT_UPDATE)
        return rom
    raise ValueError(NOT_UPDATE)


def make_rom(data):
    if len(data) == LENGTH and hashlib.sha256(data).hexdigest() == SHA256:
        return data
    if data[:4] == b"PK\x03\x04":
        import io
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                names = [n for n in z.namelist() if n.lower().endswith(".prc")]
                if not names:
                    raise ValueError(NOT_UPDATE)
                data = z.read(names[0])
        except (zipfile.BadZipFile, zlib.error, EOFError):
            raise ValueError("This zip is damaged or cut short.")
    return from_prc(data)


def main(args):
    if len(args) != 2:
        print(__doc__.strip().split("\n\n")[0] + "\nusage: zodiac-rom.py UPDATE.zip|UPDATE.prc OUT.rom", file=sys.stderr)
        return 2
    try:
        with open(args[0], "rb") as f:
            rom = make_rom(f.read())
    except OSError as e:
        print("Couldn't read the file: %s" % e, file=sys.stderr)
        return 1
    except ValueError as e:
        print(e, file=sys.stderr)
        return 1
    with open(args[1], "wb") as f:
        f.write(rom)
    print("Wrote %s (%d bytes, sha256 %s)" % (args[1], len(rom), SHA256))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
