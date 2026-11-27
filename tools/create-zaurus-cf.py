#!/usr/bin/env python3
"""Build an SL-C3000 Microdrive image from a complete factory HDD1 archive."""

import argparse
from contextlib import contextmanager, nullcontext
import copy
import gzip
import io
import os
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile


SECTOR_BYTES = 512
STOCK_SECTORS = 7_999_488
HDD1_START = 63
HDD2_START = 205_632
HDD3_START = 1_028_160
HDD1_FILES = PurePosixPath(".sys/hdlist1.dat")
FACTORY_SEED = PurePosixPath(".sys/hdimage2.tgz")
LC_FONT_FILES = {
    height: PurePosixPath(f"usr/QtPalmtop.rom/lib/fonts/lcfont_{height * 10}_50_t10.qpf")
    for height in (10, 12, 16, 18, 20, 24, 36)
}
MAX_PARTITION_ENTRIES = 32_768
MAX_ARCHIVE_READ = 256 * 1024
MAX_PATH_BYTES = 4096


class BoundedTarReader:
    def __init__(self, source, limit):
        self.source = source
        self.limit = limit

    def seekable(self):
        return True

    def tell(self):
        return self.source.tell()

    def seek(self, offset):
        self.check_offset(offset)
        return self.source.seek(offset)

    def read(self, size):
        # tarfile reads long-name and PAX headers in one call. Payload is streamed.
        if size < 0 or size > MAX_ARCHIVE_READ:
            raise ValueError("tar header is too large")
        self.check_offset(self.tell() + size)
        return self.source.read(size)

    def check_offset(self, offset):
        if not 0 <= offset <= self.limit:
            raise ValueError("uncompressed tar exceeds its partition budget")


class StockTarInfo(tarfile.TarInfo):
    # Reject sparse extent streams before tarfile reads their maps.
    def _proc_sparse(self, archive):
        raise ValueError("sparse GNU tar entries are unsupported")

    def _proc_gnusparse_10(self, member, headers, archive):
        raise ValueError("sparse PAX tar entries are unsupported")


@contextmanager
def open_archive(source, capacities, require_gzip=False):
    compressed = source.read(2) == b"\x1f\x8b"
    source.seek(0)
    if require_gzip and not compressed:
        raise ValueError("HDD1 .sys/hdimage2.tgz must be a gzip-compressed factory seed")
    stream = gzip.GzipFile(fileobj=source) if compressed else nullcontext(source)
    # Allow headers, padding and long filenames above the user-file payload.
    limit = sum(capacities.values()) + len(capacities) * MAX_PARTITION_ENTRIES * (MAX_PATH_BYTES + 1024)
    with stream as decoded:
        with tarfile.open(fileobj=BoundedTarReader(decoded, limit), mode="r:", tarinfo=StockTarInfo) as archive:
            yield archive


def required_tool(name):
    path = shutil.which(name, path=os.pathsep.join((os.environ.get("PATH", ""), "/usr/sbin", "/sbin")))
    if not path:
        raise ValueError(f"{name} is required to create a Zaurus CF image")
    return os.path.abspath(path)


def archive_path(name):
    path = PurePosixPath(name)
    if (path.is_absolute() or not path.parts or ".." in path.parts or "\0" in name or
        len(os.fsencode(name)) > MAX_PATH_BYTES or any(len(os.fsencode(part)) > 255 for part in path.parts)):
        raise ValueError(f"unexpected archive path {name!r}")
    return path


def partition_members(archive, capacities):
    members = {partition: {} for partition in capacities}
    payload = {partition: 0 for partition in capacities}
    for member in archive:
        path = archive_path(member.name)
        if path.parts[0] not in capacities:
            raise ValueError(f"unexpected archive partition in {member.name!r}")
        partition = path.parts[0]
        path = path.relative_to(partition)
        entries = members[partition]
        if path in entries:
            raise ValueError(f"duplicate archive path {member.name!r}")
        if len(entries) >= MAX_PARTITION_ENTRIES:
            raise ValueError(f"{partition} archive has too many entries")
        if member.issparse():
            raise ValueError(f"sparse archive entry is unsupported: {member.name!r}")
        if not (member.isdir() or member.isreg() or member.issym() or member.islnk()):
            raise ValueError(f"unsupported archive entry {member.name!r}")
        if member.size < 0 or (not member.isreg() and member.size):
            raise ValueError(f"invalid archive entry size for {member.name!r}")
        if partition == "hdd3" and member.size > 0xffffffff:
            raise ValueError(f"{member.name!r} exceeds the FAT32 file size limit")
        if len(os.fsencode(member.linkname)) > MAX_PATH_BYTES or "\0" in member.linkname:
            raise ValueError(f"invalid archive link for {member.name!r}")
        payload[partition] += member.size
        if payload[partition] > capacities[partition]:
            raise ValueError(f"{partition} archive payload exceeds its partition capacity")
        if path == PurePosixPath(".") and not member.isdir():
            raise ValueError(f"archive partition root must be a directory: {member.name!r}")
        entries[path] = member

    for partition, entries in members.items():
        if not entries:
            raise ValueError(f"archive does not contain {partition}")
        for path, member in entries.items():
            for parent in path.parents:
                if parent in entries and not entries[parent].isdir():
                    raise ValueError(f"archive path is inside a non-directory: {member.name!r}")
            if member.islnk():
                target = archive_path(member.linkname.lstrip("/"))
                if target.parts[0] != partition:
                    raise ValueError(f"archive hard link crosses a partition: {member.name!r}")
                linked = entries.get(target.relative_to(partition))
                if linked is None or not linked.isreg():
                    raise ValueError(f"archive hard link needs a regular file: {member.name!r}")
    return members


def open_regular(archive, member):
    if member is None or not member.isreg():
        raise ValueError("a complete HDD1 archive needs its .sys/hdlist1.dat and .sys/hdimage2.tgz")
    return archive.extractfile(member)


def lcfont_files(font_rom, entries):
    rom_size = 8 * 1024 * 1024
    with font_rom.open("rb") as source:
        rom = source.read(rom_size + 1)
    directory = 0x80000
    if len(rom) != rom_size or rom[directory:directory + 16] != b"ROM_DATA_BLOCK\0\0":
        raise ValueError("expected the 8 MiB SL-C3000 CS0 dump with its ROM resource directory")

    def records(start, end, count):
        if start + 20 + count * 4 > end or struct.unpack_from("<I", rom, start + 16)[0] != count:
            raise ValueError("unexpected SL-C3000 ROM resource count")
        table = start + 20
        data = table + count * 4
        offsets = struct.unpack_from(f"<{count}I", rom, table)
        starts = [data + offset for offset in offsets]
        if offsets[0] != 0 or not all(a < b for a, b in zip(starts, starts[1:] + [end])):
            raise ValueError("invalid offsets in the SL-C3000 ROM resources")
        return zip(starts, starts[1:] + [end])

    # The CS0 directory and QPF table use offsets relative to the end of their offset lists.
    for start, end in records(directory, rom_size, 4):
        if rom[start + 4:start + 20] == b"C700_QPF_DATA\0\0\0":
            length, = struct.unpack_from("<I", rom, start)
            if not 20 <= length <= end - start - 4:
                raise ValueError("invalid LC Font resource block size")
            start += 4
            end = start + length
            break
    else:
        raise ValueError("the CS0 dump does not contain the stock LC Font resources")

    fonts = {}
    for (height, path), (start, end) in zip(LC_FONT_FILES.items(), records(start, end, len(LC_FONT_FILES))):
        if end - start < 14:
            raise ValueError("truncated LC Font resource")
        length, metrics = struct.unpack_from("<II", rom, start)
        if not 10 <= length <= end - start - 4 or metrics != height:
            raise ValueError("invalid LC Font resource size or metrics")
        member = entries.get(path)
        if member is not None and member.isreg() and member.size == 0:
            fonts[path] = rom[start + 4:start + 4 + length]
    return fonts


def write_partition_tar(archive, entries, dest, partition, replacements=None):
    with tarfile.open(dest, "w") as out:
        # The formatter resolves hard links against files it has already added.
        for path, member in sorted(entries.items(), key=lambda entry: entry[1].islnk()):
            stripped = copy.copy(member)
            stripped.name = str(path)
            stripped.pax_headers = {}
            if member.islnk():
                stripped.linkname = str(archive_path(member.linkname.lstrip("/")).relative_to(partition))
            if replacements and path in replacements:
                payload = replacements[path]
                stripped.size = len(payload)
                out.addfile(stripped, io.BytesIO(payload))
            elif member.isreg():
                with archive.extractfile(member) as data:
                    out.addfile(stripped, data)
            else:
                out.addfile(stripped)


def validate_fat_entries(entries):
    seen = {}
    for path, member in entries.items():
        if not (member.isdir() or member.isreg()):
            raise ValueError(f"FAT32 does not support archive link {member.name!r}")
        if any(part.endswith((".", " ")) or any(c in '"*/:<>?\\|' or ord(c) < 32 for c in part)
               for part in path.parts):
            raise ValueError(f"invalid FAT32 name {member.name!r}")
        for part in (path, *path.parents):
            key = str(part).casefold()
            if seen.setdefault(key, part) != part:
                raise ValueError(f"case-insensitive FAT32 path conflict in {member.name!r}")


def extract_fat_files(archive, entries, root):
    root.mkdir()
    for path, member in entries.items():
        target = root / path
        if member.isdir():
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.extractfile(member) as data, target.open("xb") as out:
                shutil.copyfileobj(data, out, length=MAX_ARCHIVE_READ)
    for path, member in entries.items():
        os.utime(root / path, (member.mtime, member.mtime))


def write_mbr(image, total_sectors):
    def chs(lba):
        cylinder, offset = divmod(lba, 16 * 63)
        head, sector = divmod(offset, 63)
        if cylinder > 1023:
            cylinder, head, sector = 1023, 15, 62
        return bytes((head, (sector + 1) | ((cylinder >> 2) & 0xc0), cylinder & 0xff))

    mbr = bytearray(SECTOR_BYTES)
    partitions = (
        (HDD1_START, HDD2_START, 0x83),
        (HDD2_START, HDD3_START, 0x83),
        (HDD3_START, total_sectors, 0x0c),
    )
    for index, (start, end, kind) in enumerate(partitions):
        struct.pack_into("<B3sB3sII", mbr, 446 + index * 16,
                         0, chs(start), kind, chs(end - 1), start, end - start)
    mbr[510:] = b"\x55\xaa"
    image.seek(0)
    image.write(mbr)


def format_ext3(mke2fs, image, archive, start, end):
    # Linux 2.4 needs 128-byte inodes and ext3-compatible feature flags.
    subprocess.run(
        [mke2fs, "-q", "-F", "-t", "ext3", "-I", "128", "-m", "0", "-b", "1024",
         "-O", "none,has_journal,filetype,sparse_super,large_file",
         "-E", f"offset={start * SECTOR_BYTES}", "-d", str(archive),
         str(image), str((end - start) // 2)],
        check=True,
    )


def create_image(hdd1, size_gb, output, font_rom=None, require_fonts=False):
    if size_gb not in range(4, 33):
        raise ValueError("Zaurus CF size must be a whole number from 4 to 32 GB")
    if os.path.lexists(output):
        raise FileExistsError(f"output already exists: {output}")
    mke2fs = required_tool("mke2fs")
    mformat = required_tool("mformat")
    mcopy = required_tool("mcopy")
    sectors = STOCK_SECTORS if size_gb == 4 else size_gb * 1_000_000_000 // SECTOR_BYTES
    hdd1_capacity = {"hdd1": (HDD2_START - HDD1_START) * SECTOR_BYTES}
    seed_capacities = {"hdd2": (HDD3_START - HDD2_START) * SECTOR_BYTES,
                       "hdd3": (sectors - HDD3_START) * SECTOR_BYTES}
    with tempfile.TemporaryDirectory(prefix=".frond-zaurus-cf-", dir=output.parent) as scratch:
        scratch = Path(scratch).absolute()
        hdd1_tar, hdd2_tar = scratch / "hdd1.tar", scratch / "hdd2.tar"
        with hdd1.open("rb") as raw, open_archive(raw, hdd1_capacity) as source:
            entries = partition_members(source, hdd1_capacity)["hdd1"]
            with open_regular(source, entries.get(HDD1_FILES)) as file_list:
                if not file_list.read(1):
                    raise ValueError("HDD1 .sys/hdlist1.dat is empty")
            with open_regular(source, entries.get(FACTORY_SEED)) as seed_file:
                with open_archive(seed_file, seed_capacities, require_gzip=True) as seed:
                    seeded = partition_members(seed, seed_capacities)
                    validate_fat_entries(seeded["hdd3"])
                    write_partition_tar(seed, seeded["hdd2"], hdd2_tar, "hdd2")
                    fat_root = scratch / "hdd3"
                    extract_fat_files(seed, seeded["hdd3"], fat_root)
            replacements = lcfont_files(font_rom, entries) if font_rom else {}
            if require_fonts:
                for path in LC_FONT_FILES.values():
                    member = entries.get(path)
                    if not member or not member.isreg() or (not member.size and path not in replacements):
                        raise ValueError("Direct boot needs the stock LC Fonts. Choose a CS0 dump or a complete HDD1 with LC Fonts.")
            if sum(member.size for member in entries.values()) + sum(map(len, replacements.values())) > hdd1_capacity["hdd1"]:
                raise ValueError("HDD1 archive and LC Fonts exceed the first partition capacity")
            write_partition_tar(source, entries, hdd1_tar, "hdd1", replacements)

        fd, name = tempfile.mkstemp(prefix="image-", dir=scratch)
        image = Path(name)
        with os.fdopen(fd, "r+b") as stream:
            stream.truncate(sectors * SECTOR_BYTES)
            write_mbr(stream, sectors)
        format_ext3(mke2fs, image, hdd1_tar, HDD1_START, HDD2_START)
        format_ext3(mke2fs, image, hdd2_tar, HDD2_START, HDD3_START)

        fat = f"{image.name}@@{HDD3_START * SECTOR_BYTES}"
        subprocess.run([mformat, "-i", fat, "-F", "-T", str(sectors - HDD3_START),
                        "-h", "16", "-s", "63", "-H", str(HDD3_START), "::"],
                       cwd=scratch, check=True)
        fat_sources = sorted(str(path) for path in fat_root.iterdir())
        if fat_sources:
            subprocess.run([mcopy, "-s", "-m", "-i", fat, *fat_sources, "::/"],
                           cwd=scratch, check=True)
        with image.open("r+b") as stream:
            if os.fstat(stream.fileno()).st_size != sectors * SECTOR_BYTES:
                raise ValueError("formatter changed the CF image size")
            os.fsync(stream.fileno())
        # Publish the complete image without replacing an existing file.
        os.link(image, output)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="Use xlfag HDD1 or Sharp hdimage1.tgz with its embedded factory seed. "
               "Use firmware from the same release. Requires mke2fs with tar support and mtools.",
    )
    parser.add_argument("--hdd1", type=Path, required=True, help="complete stock HDD1 tar archive")
    parser.add_argument("--size-gb", type=int, choices=range(4, 33), default=4, metavar="4–32",
                        help="4 uses the original 4,095,737,856-byte capacity, larger sizes are decimal GB")
    parser.add_argument("--font-rom", type=Path,
                        help="fill empty stock LC Font files from an 8 MiB SL-C3000 CS0 dump for direct boot")
    parser.add_argument("--require-fonts", action="store_true",
                        help="require the stock LC Font files needed for direct boot")
    parser.add_argument("--output", type=Path, required=True, help="new sparse CF disk image")
    args = parser.parse_args()
    try:
        create_image(args.hdd1, args.size_gb, args.output, args.font_rom, args.require_fonts)
    except (OSError, EOFError, RecursionError, tarfile.TarError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Could not create Zaurus CF image: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
