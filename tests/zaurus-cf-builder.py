#!/usr/bin/env python3
"""Checks the core's Zaurus CF builder (build/out/test-zaurus-cf-builder) against
tools/create-zaurus-cf.py on a synthetic HDD1 archive and CS0 font ROM made here (no Sharp files):
e2fsck -fn on both ext3 partitions and fsck.fat -n on the FAT come back clean, and the file tree,
modes, owners, times, links and contents match the script's image (debugfs and mtools listings,
file hashes). The C++ side checks bad archives, cancel and overflow itself. Needs e2fsprogs
(mke2fs with tar support, e2fsck, debugfs), dosfstools and mtools."""

import gzip
import hashlib
import io
import os
from pathlib import Path
import random
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
OBJ = Path(os.environ.get('FROND_OBJ') or ROOT / 'build/out')
SCRATCH = Path(os.environ.get('ZAURUS_CF_SCRATCH') or ROOT / 'build/zaurus-cf-builder')
SBIN = os.pathsep.join((os.environ.get('PATH', ''), '/usr/sbin', '/sbin'))
ENV = dict(os.environ, LC_ALL='C.UTF-8', TZ='UTC', PATH=SBIN)
SECTOR = 512
HDD1_START, HDD2_START, HDD3_START = 63, 205_632, 1_028_160
FONT_HEIGHTS = (10, 12, 16, 18, 20, 24, 36)
MTIME = 1_100_000_000


def tool(name):
    path = shutil.which(name, path=SBIN)
    if not path:
        sys.exit(f'{name} is not installed')
    return path


def run(*args, ok=(0,), **kwargs):
    result = subprocess.run([str(a) for a in args], capture_output=True, env=ENV, **kwargs)
    if result.returncode not in ok:
        sys.exit(f'{" ".join(map(str, args))} failed ({result.returncode}):\n'
                 f'{result.stdout.decode(errors="replace")}{result.stderr.decode(errors="replace")}')
    return result


# Synthetic archive

def pattern(seed, size):
    rng = random.Random(seed)
    return rng.randbytes(size)


def add(tar, name, kind='file', data=b'', mode=0o644, uid=0, gid=0, link='', mtime=None, pax=None):
    if name in getattr(tar, 'drop', ()):
        return
    info = tarfile.TarInfo(name)
    info.mtime = MTIME + len(tar.getmembers()) * 37 if mtime is None else mtime
    info.mode = mode
    info.uid, info.gid = uid, gid
    info.uname = info.gname = ''
    if pax:
        info.pax_headers = pax
    if kind in ('chr', 'fifo'):
        info.type = tarfile.CHRTYPE if kind == 'chr' else tarfile.FIFOTYPE
        info.devmajor, info.devminor = 4, 64
        tar.addfile(info)
    elif kind == 'dir':
        info.type = tarfile.DIRTYPE
        tar.addfile(info)
    elif kind == 'sym':
        info.type = tarfile.SYMTYPE
        info.linkname = link
        tar.addfile(info)
    elif kind == 'hard':
        info.type = tarfile.LNKTYPE
        info.linkname = link
        tar.addfile(info)
    else:
        info.size = len(data)
        tar.addfile(info, io.BytesIO(data))


def sparse_data(size, chunks):
    """`size` bytes of zeros with pattern blocks at the given offsets, for files with holes."""
    data = bytearray(size)
    for offset, length in chunks:
        data[offset:offset + length] = pattern(offset, length)
    return bytes(data)


def seed_archive(extra=(), drop=(), compress=True):
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.GNU_FORMAT) as tar:
        tar.drop = drop
        add(tar, 'hdd2', 'dir', mode=0o755)
        add(tar, 'hdd2/etc', 'dir', mode=0o755)
        add(tar, 'hdd2/etc/passwd', data=b'root:x:0:0::/home/root:/bin/sh\n')
        add(tar, 'hdd2/etc/shadow', data=b'root::0:0:99999:7:::\n', mode=0o600)
        add(tar, 'hdd2/etc/rc.local', data=b'#!/bin/sh\nexit 0\n', mode=0o755)
        add(tar, 'hdd2/etc/rc', 'hard', link='hdd2/etc/rc.local', mode=0o700, uid=9, gid=10, mtime=MTIME + 5)
        add(tar, 'hdd2/home', 'dir', mode=0o755)
        add(tar, 'hdd2/home/zaurus', 'dir', mode=0o700, uid=500, gid=500)
        add(tar, 'hdd2/home/zaurus/Settings', 'dir', mode=0o755, uid=500, gid=500)
        for i in range(260):
            add(tar, f'hdd2/home/zaurus/Settings/app-{i:03d}-with-a-longer-name.conf',
                data=f'[Main]\nIndex = {i}\n'.encode(), uid=500, gid=500)
        add(tar, 'hdd2/home/zaurus/Documents', 'dir', mode=0o775, uid=500, gid=500)
        add(tar, 'hdd2/home/zaurus/Documents/notes.txt', data=pattern(2, 5000), uid=500, gid=500)
        add(tar, 'hdd2/home/zaurus/Documents/big.bin', data=pattern(3, 1_300_000), uid=500, gid=500)
        add(tar, 'hdd2/home/zaurus/Documents/sparse.img',
            data=sparse_data(2_000_000, ((0, 3000), (700_000, 1024), (1_999_000, 1000))), uid=500, gid=500)
        add(tar, 'hdd2/home/zaurus/Documents/empty', uid=500, gid=500)
        add(tar, 'hdd2/home/zaurus/Documents/link', 'sym', link='notes.txt', uid=500, gid=500)
        add(tar, 'hdd2/home/zaurus/Documents/far', 'sym', link='/' + 'x' * 200, uid=500, gid=500)
        add(tar, 'hdd2/var', 'dir', mode=0o755)
        add(tar, 'hdd2/var/tmp', 'dir', mode=0o1777)
        add(tar, 'hdd2/var/log', 'dir', mode=0o755)
        add(tar, 'hdd2/var/log/messages', data=b'', mode=0o640, gid=4)
        for depth in range(1, 31):
            add(tar, 'hdd2' + '/deep' * depth, 'dir')
        add(tar, 'hdd2' + '/deep' * 30 + '/end', 'dir')

        add(tar, 'hdd3', 'dir', mode=0o755)
        add(tar, 'hdd3/Documents', 'dir')
        add(tar, 'hdd3/Documents/Text_Files', 'dir')
        add(tar, 'hdd3/Documents/Text_Files/readme.txt', data=b'hello\r\n')
        add(tar, 'hdd3/Documents/Text_Files/A Long File Name With Spaces.txt', data=pattern(4, 9000))
        add(tar, 'hdd3/Documents/Text_Files/café menu.txt'.encode().decode('utf-8'), data=b'cafe\n')
        add(tar, 'hdd3/Documents/Text_Files/x+y=z[1].dat', data=b'symbols\n')
        add(tar, 'hdd3/Documents/Image_Files', 'dir')
        for i in range(150):
            add(tar, f'hdd3/Documents/Image_Files/Picture number {i:03d}.jpg', data=pattern(100 + i, 700 + i))
        add(tar, 'hdd3/Documents/Image_Files/large.bin', data=pattern(5, 300_000))
        add(tar, 'hdd3/Documents/empty.txt')
        add(tar, 'hdd3/Empty Folder', 'dir')
        add(tar, 'hdd3/UPPER.TXT', data=b'upper\n')
        for entry in extra:
            add(tar, **entry)
    return gzip.compress(raw.getvalue(), mtime=0) if compress else raw.getvalue()


def hdd1_archive(seed=None, fonts_filled=(), extra=(), drop=(), compress=True):
    seed = seed_archive() if seed is None else seed
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.PAX_FORMAT) as tar:
        tar.drop = drop
        add(tar, 'hdd1', 'dir', mode=0o755)
        add(tar, 'hdd1/.sys', 'dir', mode=0o755)
        add(tar, 'hdd1/.sys/hdlist1.dat', data=b'./usr\n./usr/bin\n')
        add(tar, 'hdd1/.sys/hdimage2.tgz', data=seed)
        add(tar, 'hdd1/usr', 'dir', mode=0o755)
        add(tar, 'hdd1/usr/bin', 'dir', mode=0o755)
        add(tar, 'hdd1/usr/bin/busybox', data=pattern(6, 400_000), mode=0o4755)
        for name in ('ls', 'cat', 'sh'):
            add(tar, f'hdd1/usr/bin/{name}', 'sym', link='busybox')
        add(tar, 'hdd1/usr/bin/sh-hard', 'hard', link='hdd1/usr/bin/busybox', mode=0o4755, uid=7, gid=8)
        add(tar, 'hdd1/usr/bin/setgid', data=b'#!/bin/sh\n', mode=0o2711, uid=0, gid=50)
        add(tar, 'hdd1/usr/QtPalmtop.rom', 'dir', mode=0o755)
        add(tar, 'hdd1/usr/QtPalmtop.rom/lib', 'dir', mode=0o755)
        add(tar, 'hdd1/usr/QtPalmtop.rom/lib/fonts', 'dir', mode=0o755)
        for height in FONT_HEIGHTS:
            data = pattern(height, 3000) if height in fonts_filled else b''
            add(tar, f'hdd1/usr/QtPalmtop.rom/lib/fonts/lcfont_{height * 10}_50_t10.qpf', data=data)
        add(tar, 'hdd1/usr/lib', 'dir')
        long_dir = 'hdd1/usr/lib/' + 'a-very-long-directory-name-' * 3
        add(tar, long_dir, 'dir', mode=0o750, uid=1, gid=2)
        add(tar, long_dir + '/' + 'f' * 200, data=pattern(7, 2048), uid=3, gid=4, mtime=MTIME + 999)
        add(tar, 'hdd1/usr/lib/holes',
            data=sparse_data(67_500_000, ((0, 10), (300_000, 2048), (67_490_000, 10_000))))
        add(tar, 'hdd1/usr/lib/libfrond.so.1.0', data=pattern(8, 70_000), mode=0o755)
        add(tar, 'hdd1/usr/lib/libfrond.so.1', 'sym', link='libfrond.so.1.0')
        add(tar, 'hdd1/usr/lib/edge', 'sym', link='e' * 59)
        add(tar, 'hdd1/usr/lib/edge60', 'sym', link='e' * 60)
        add(tar, 'hdd1/usr/lib/unicode-éè', data='é'.encode())
        add(tar, 'hdd1/home', 'dir', mode=0o755)
        for entry in extra:
            add(tar, **entry)
    return gzip.compress(raw.getvalue(), mtime=0) if compress else raw.getvalue()


def font_rom():
    """An 8 MiB CS0 dump shaped like the SL-C3000's: the resource directory at 0x80000 with four
    records, one of them the C700_QPF_DATA block holding the seven LC Fonts."""
    rom = bytearray(b'\xff' * (8 * 1024 * 1024))
    fonts = [pattern(1000 + h, 500 + h * 7) for h in FONT_HEIGHTS]
    table = b''.join(struct.pack('<II', len(f) + 4, h) + f for h, f in zip(FONT_HEIGHTS, fonts))
    offsets, at = [], 0
    for h, f in zip(FONT_HEIGHTS, fonts):
        offsets.append(at)
        at += 8 + len(f)
    qpf = b'C700_QPF_DATA\0\0\0' + struct.pack('<I', len(FONT_HEIGHTS)) + \
        struct.pack(f'<{len(offsets)}I', *offsets) + table
    block = struct.pack('<I', len(qpf)) + qpf
    records = [b'\0' * 64, b'\1' * 64, block, b'\2' * 64]
    record_offsets, at = [], 0
    for record in records:
        record_offsets.append(at)
        at += len(record)
    directory = b'ROM_DATA_BLOCK\0\0' + struct.pack('<I', 4) + \
        struct.pack('<4I', *record_offsets) + b''.join(records)
    rom[0x80000:0x80000 + len(directory)] = directory
    return bytes(rom), dict(zip(FONT_HEIGHTS, (struct.pack('<I', h) + f for h, f in zip(FONT_HEIGHTS, fonts))))


# Images

def extract(image, start, end, dest):
    """Copies sectors start..end of `image` to `dest`, keeping holes."""
    run('dd', f'if={image}', f'of={dest}', 'bs=1M', f'skip={start * SECTOR}', f'count={(end - start) * SECTOR}',
        'iflag=skip_bytes,count_bytes', 'conv=sparse', 'status=none')


def debugfs(image, commands):
    """Runs debugfs commands; returns each command's output lines in order."""
    script = SCRATCH / 'debugfs.cmd'
    script.write_text(''.join(c + '\n' for c in commands))
    out = run(tool('debugfs'), '-f', script, image).stdout.decode('utf-8', 'surrogateescape')
    sections, current = [], None
    for line in out.splitlines():
        if line.startswith('debugfs: '):
            current = []
            sections.append(current)
        elif current is not None:
            current.append(line)
    if len(sections) != len(commands):
        sys.exit(f'debugfs gave {len(sections)} answers to {len(commands)} commands')
    return sections


def quote(path):
    return '"' + path + '"'


def ext_tree(image, work):
    """Every path in an ext3 image with its type, mode, owner, size, links, times and contents."""
    entries, dirs = {}, ['/']
    while dirs:
        listings = debugfs(image, [f'ls -p {quote(d)}' for d in dirs])
        found = []
        for d, lines in zip(dirs, listings):
            for line in lines:
                m = re.match(r'^/(\d+)/(\d+)/(\d+)/(\d+)/(.*)/(\d*)/$', line)
                if not m:
                    continue
                ino, mode, uid, gid, name, size = m.groups()
                if name in ('.', '..', '') or ino == '0':
                    continue
                path = d.rstrip('/') + '/' + name
                entries[path] = {'ino': int(ino), 'mode': int(mode, 8), 'uid': int(uid), 'gid': int(gid),
                                 'size': int(size or 0)}
                if stat.S_ISDIR(int(mode, 8)):
                    found.append(path)
        dirs = found
    paths = ['/'] + sorted(entries)
    entries['/'] = {'ino': 2}
    stats = debugfs(image, [f'stat {quote(p)}' for p in paths])
    dumps = []
    for path, lines in zip(paths, stats):
        text = '\n'.join(lines)
        e = entries[path]
        e['links'] = int(re.search(r'Links: (\d+)', text).group(1))
        mode = re.search(r'Mode:\s+(\d+)', text).group(1)
        e['perm'] = int(mode, 8)
        for field in ('atime', 'ctime', 'mtime'):
            e[field] = int(re.search(field + r': (0x[0-9a-f]+)', text).group(1), 16)
        fast = re.search(r'Fast link dest: "(.*)"', text)
        if fast:
            e['target'] = fast.group(1)
        elif 'Type: regular' in text or 'Type: symlink' in text:
            dumps.append(path)
    hosts = [work / f'dump-{i}' for i in range(len(dumps))]
    for host in hosts:
        host.unlink(missing_ok=True)
    debugfs(image, [f'dump {quote(p)} {h}' for p, h in zip(dumps, hosts)])
    for path, host in zip(dumps, hosts):
        data = host.read_bytes()
        host.unlink()
        if stat.S_ISLNK(entries[path]['mode']):
            entries[path]['target'] = data.decode('utf-8', 'surrogateescape')
        else:
            entries[path]['sha256'] = hashlib.sha256(data).hexdigest()
    # Hard links: paths sharing an inode, by path rather than inode number.
    by_ino = {}
    for path, e in entries.items():
        by_ino.setdefault(e['ino'], []).append(path)
    for path, e in entries.items():
        e['names'] = tuple(sorted(by_ino[e.pop('ino')]))
    # lost+found is stamped when the file system is made.
    for field in ('atime', 'ctime', 'mtime'):
        entries['/lost+found'].pop(field)
    return entries


def mdir_listing(target):
    """Each FAT entry: folder, name as a reader shows it, size or <DIR>, date and time."""
    out = run('mdir', '-/', '-a', '-i', target, '::/').stdout.decode('utf-8')
    found, folder = set(), None
    for line in out.splitlines():
        if line.startswith('Directory for ::'):
            folder = line[len('Directory for ::'):]
            continue
        m = re.match(r'^(.{8}) (.{3}) +(<DIR>|\d+) +(\d{4}-\d\d-\d\d) +(\d+:\d\d) ?(.*)$', line)
        if not m or folder is None:
            continue
        base, ext, size, date, time, long_name = (g.strip() for g in m.groups())
        name = long_name or (base + ('.' + ext if ext else ''))
        if name in ('.', '..'):
            continue
        found.add((folder, name, size, date, time))
    return found


def fat_files(target, dest):
    """Paths, sizes and hashes of the files in a FAT, copied out with mcopy."""
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir()
    run('mcopy', '-s', '-n', '-m', '-i', target, '::/*', dest)
    found = {}
    for path in sorted(dest.rglob('*')):
        rel = str(path.relative_to(dest))
        if path.is_dir():
            found[rel] = 'dir'
        else:
            found[rel] = (path.stat().st_size, int(path.stat().st_mtime), hashlib.sha256(path.read_bytes()).hexdigest())
    shutil.rmtree(dest)
    return found


def check_image(image, work, label):
    """e2fsck and fsck.fat on each partition; returns the trees for comparing."""
    trees = {}
    with open(image, 'rb') as f:
        mbr = f.read(512)
    if mbr[510:] != b'\x55\xaa':
        sys.exit(f'{label}: no MBR signature')
    size = image.stat().st_size // SECTOR
    for name, (start, end) in (('hdd1', (HDD1_START, HDD2_START)), ('hdd2', (HDD2_START, HDD3_START))):
        part = work / f'{label}-{name}.img'
        extract(image, start, end, part)
        result = run(tool('e2fsck'), '-fn', part, ok=(0, 1, 2, 4, 8))
        if result.returncode:
            sys.exit(f'{label} {name}: e2fsck -fn found problems:\n{result.stdout.decode()}{result.stderr.decode()}')
        trees[name] = ext_tree(part, work)
        header = run(tool('dumpe2fs'), '-h', part).stdout.decode()
        skip = ('Filesystem UUID', 'Directory Hash Seed', 'Filesystem created', 'Last write time', 'Last checked')
        trees[name + ' superblock'] = dict(line.split(':', 1) for line in header.splitlines()
                                           if ':' in line and not line.startswith(skip))
        part.unlink()
    part = work / f'{label}-hdd3.img'
    extract(image, HDD3_START, size, part)
    result = run(tool('fsck.fat'), '-n', part, ok=(0, 1, 2))
    if result.returncode:
        sys.exit(f'{label} hdd3: fsck.fat -n found problems:\n{result.stdout.decode()}{result.stderr.decode()}')
    part.unlink()
    target = f'{image}@@{HDD3_START * SECTOR}'
    trees['hdd3'] = (mdir_listing(target), fat_files(target, work / f'{label}-files'))
    return trees, mbr


def compare(ours, theirs, label):
    if ours == theirs:
        return
    if isinstance(ours, dict):
        for key in sorted(set(ours) | set(theirs), key=str):
            if ours.get(key) != theirs.get(key):
                print(f'{label} {key}: ours {ours.get(key)!r}\n{" " * len(label)} script {theirs.get(key)!r}')
    else:
        for item in sorted(ours ^ theirs):
            print(f'{label}: {"ours" if item in ours else "script"} only {item!r}')
    sys.exit(f'{label} differs from the script\'s image')


# Runs

def builder(binary, *args, ok=(0,)):
    return run(binary, 'cli', *args, ok=ok)


def script(*args, ok=(0,)):
    return run(sys.executable, '-I', ROOT / 'tools/create-zaurus-cf.py', *args, ok=ok)


def write(path, data):
    path.write_bytes(data)
    return path


def error_cases(work, rom):
    """(name, archive bytes, extra arguments, expected message, whether the script refuses it too)."""
    good_seed = seed_archive()
    raw_outer = hdd1_archive(good_seed, compress=False)
    good = gzip.compress(raw_outer, mtime=0)
    bad_crc = bytearray(good)
    bad_crc[-6] ^= 0xff
    damaged = bytearray(good)
    for i in range(len(damaged) // 2, len(damaged) // 2 + 64):
        damaged[i] ^= 0x55
    bad_header = bytearray(raw_outer)
    with tarfile.open(fileobj=io.BytesIO(raw_outer)) as tar:
        bad_header[tar.getmembers()[5].offset + 10] ^= 0x01
    ones = b'\x01' * 100_000_000
    fonts = {f'usr/QtPalmtop.rom/lib/fonts/lcfont_{h * 10}_50_t10.qpf' for h in FONT_HEIGHTS}
    long_link = '/' + 'l' * 1100
    cases = [
        ('gzip cut short', good[:len(good) // 2], (), 'cut off', True),
        ('tar cut short', raw_outer[:len(raw_outer) * 2 // 3 + 100], (), 'cut off', True),
        ('gzip CRC', bytes(bad_crc), (), 'CRC', False),
        ('damaged deflate', bytes(damaged), (), 'gzip', None),
        ('not an archive', pattern(9, 5000), (), 'not a tar archive', True),
        ('empty file', b'', (), 'empty', True),
        ('damaged tar header', bytes(bad_header), (), 'damaged tar header', None),
        ('dot-dot path', hdd1_archive(good_seed, extra=[dict(name='hdd1/../x')]), (), 'unexpected path', True),
        ('unexpected partition', hdd1_archive(good_seed, extra=[dict(name='hdd2/x')]), (), 'unexpected partition', True),
        ('duplicate path', hdd1_archive(good_seed, extra=[dict(name='hdd1/usr/lib/edge')]), (), 'twice', True),
        ('device node', hdd1_archive(seed_archive(extra=[dict(name='hdd2/console', kind='chr')])), (),
         'unsupported entry', True),
        ('fifo', hdd1_archive(good_seed, extra=[dict(name='hdd1/home/pipe', kind='fifo')]), (),
         'unsupported entry', True),
        ('sparse pax', hdd1_archive(good_seed, extra=[dict(name='hdd1/home/sp', data=b'x',
                                                           pax={'GNU.sparse.major': '1', 'GNU.sparse.minor': '0'})]),
         (), 'sparse', True),
        ('FAT symlink', hdd1_archive(seed_archive(extra=[dict(name='hdd3/link', kind='sym', link='UPPER.TXT')])), (),
         "doesn't support", True),
        ('FAT case', hdd1_archive(seed_archive(extra=[dict(name='hdd3/upper.txt')])), (), 'by case', True),
        ('FAT name', hdd1_archive(seed_archive(extra=[dict(name='hdd3/a:b')])), (), 'invalid FAT32 name', True),
        ('no seed', hdd1_archive(good_seed, drop={'hdd1/.sys/hdimage2.tgz'}), (), 'complete HDD1', True),
        ('seed not gzip', hdd1_archive(seed_archive(compress=False)), (), 'gzip-compressed', True),
        ('empty file list', hdd1_archive(good_seed, drop={'hdd1/.sys/hdlist1.dat'},
                                         extra=[dict(name='./hdd1/.sys/hdlist1.dat')]), (), 'is empty', True),
        ('no fonts', hdd1_archive(good_seed), ('--require-fonts',), 'Direct boot', True),
        ('missing font', hdd1_archive(good_seed, drop={'hdd1/' + next(iter(sorted(fonts)))}),
         ('--require-fonts', '--font-rom', rom), 'Direct boot', True),
        ('bad font ROM', hdd1_archive(good_seed), ('--font-rom', write(work / 'short.rom', b'\0' * 1000)),
         '8 MiB', True),
        ('broken hard link', hdd1_archive(good_seed, extra=[dict(name='hdd1/home/l', kind='hard', link='hdd1/nope')]),
         (), 'needs a regular file', True),
        ('cross hard link', hdd1_archive(seed_archive(extra=[dict(name='hdd2/l', kind='hard', link='hdd3/UPPER.TXT')])),
         (), 'crosses a partition', True),
        ('inside a file', hdd1_archive(seed_archive(extra=[dict(name='hdd2/etc/passwd/x')])), (),
         "isn't a folder", True),
        ('missing parent', hdd1_archive(seed_archive(extra=[dict(name='hdd2/nowhere/x')])), (), 'no parent folder', True),
        ('long symlink', hdd1_archive(good_seed, extra=[dict(name='hdd1/home/long', kind='sym', link=long_link)]), (),
         "can't be stored", True),
        ('empty symlink', hdd1_archive(good_seed, extra=[dict(name='hdd1/home/empty', kind='sym', link='')]), (),
         "can't be stored", None),
        ('over capacity', hdd1_archive(good_seed, extra=[dict(name='hdd1/home/big', data=ones + ones[:10_000_000])]),
         (), 'larger than its partition', True),
        ('partition full', hdd1_archive(good_seed, drop={'hdd1/usr/lib/holes'},
                                        extra=[dict(name='hdd1/home/big', data=ones)]), (),
         "doesn't fit in the hdd1", True),
        ('inodes full', hdd1_archive(good_seed, extra=[dict(name=f'hdd1/home/f{i}') for i in range(26_000)]), (),
         'too many files', True),
        ('size 3 GB', hdd1_archive(good_seed), ('--size-gb', '3'), None, True),
    ]
    return cases


def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else OBJ / 'test-zaurus-cf-builder'
    for name in ('e2fsck', 'debugfs', 'dumpe2fs', 'fsck.fat', 'mke2fs', 'mformat', 'mcopy', 'mdir'):
        tool(name)
    shutil.rmtree(SCRATCH, ignore_errors=True)
    SCRATCH.mkdir(parents=True)
    work = SCRATCH

    # FAT32 layout against mformat at the sizes where its cluster size changes.
    for gb in (4, 5, 9, 10, 17, 18, 32):
        sectors = (7_999_488 if gb == 4 else gb * 1_000_000_000 // SECTOR) - HDD3_START
        probe = work / 'mformat.img'
        probe.unlink(missing_ok=True)
        with open(probe, 'wb') as f:
            f.truncate(sectors * SECTOR)
        run('mformat', '-i', probe, '-F', '-T', sectors, '-h', '16', '-s', '63', '-H', HDD3_START, '::')
        with open(probe, 'rb') as f:
            boot = f.read(512)
        theirs = (boot[13], struct.unpack_from('<I', boot, 36)[0])
        ours = tuple(map(int, run(binary, 'geometry', sectors).stdout.split()[:2]))
        probe.unlink()
        if ours != theirs:
            sys.exit(f'{gb} GB FAT32 layout: ours {ours}, mformat {theirs}')
    print('FAT32 layout matches mformat from 4 to 32 GB')

    rom, fonts = font_rom()
    rom_file = write(work / 'cs0.bin', rom)
    archive = write(work / 'hdd1.tgz', hdd1_archive(fonts_filled=(10,)))
    ref, img = work / 'script.img', work / 'frond.img'
    script('--hdd1', archive, '--size-gb', '4', '--font-rom', rom_file, '--require-fonts', '--output', ref)
    builder(binary, '--hdd1', archive, '--size-gb', '4', '--font-rom', rom_file, '--require-fonts', '--output', img)
    if img.stat().st_size != ref.stat().st_size:
        sys.exit(f'image size {img.stat().st_size}, the script made {ref.stat().st_size}')
    used = img.stat().st_blocks * 512
    if used > 64 << 20:
        sys.exit(f'the image is not sparse: {used} bytes allocated')
    print(f'built a 4 GB image, {used >> 20} MiB allocated')
    theirs, their_mbr = check_image(ref, work, 'script')
    ours, our_mbr = check_image(img, work, 'frond')
    print('e2fsck -fn and fsck.fat -n are clean on both images')
    if our_mbr != their_mbr:
        sys.exit('the MBR differs from the script\'s')
    for name in ('hdd1', 'hdd2', 'hdd1 superblock', 'hdd2 superblock'):
        compare(ours[name], theirs[name], name)
    compare(ours['hdd3'][0], theirs['hdd3'][0], 'hdd3 listing')
    compare(ours['hdd3'][1], theirs['hdd3'][1], 'hdd3 files')
    fonts_seen = {k: v for k, v in ours['hdd1'].items() if 'lcfont' in k}
    for height, data in fonts.items():
        e = fonts_seen[f'/usr/QtPalmtop.rom/lib/fonts/lcfont_{height * 10}_50_t10.qpf']
        expected = pattern(height, 3000) if height == 10 else data
        if e['sha256'] != hashlib.sha256(expected).hexdigest():
            sys.exit(f'LC Font {height} has the wrong contents')
    print(f'the trees match the script\'s: {len(ours["hdd1"])} hdd1, {len(ours["hdd2"])} hdd2, '
          f'{len(ours["hdd3"][1])} hdd3 paths')
    img.unlink()
    ref.unlink()

    # An uncompressed HDD1 and a bigger card with 16-sector clusters.
    plain = write(work / 'hdd1.tar', gzip.decompress(archive.read_bytes()))
    builder(binary, '--hdd1', plain, '--size-gb', '10', '--font-rom', rom_file, '--output', img)
    plain_trees, _ = check_image(img, work, 'plain')
    for name in ('hdd1', 'hdd2'):
        compare(plain_trees[name], ours[name], f'plain {name}')
    compare(plain_trees['hdd3'][1], ours['hdd3'][1], 'plain hdd3 files')
    img.unlink()
    plain.unlink()
    print('an uncompressed archive on a 10 GB card gives the same trees, fsck clean')

    run(binary, 'stream', archive, rom_file)
    cancelled = work / 'cancelled.img'
    run(binary, 'cancel', archive, cancelled)
    cancelled.unlink()
    print('builds through a callback without zero writes, cancel stops it')

    exists = write(work / 'exists.img', b'keep')
    result = builder(binary, '--hdd1', archive, '--output', exists, ok=(1,))
    if exists.read_bytes() != b'keep' or b'already exists' not in result.stderr:
        sys.exit('an existing output was not refused')
    exists.unlink()

    for name, data, args, message, script_refuses in error_cases(work, rom_file):
        bad = write(work / 'bad.tgz', data)
        out = work / 'bad.img'
        result = builder(binary, '--hdd1', bad, *map(str, args), '--output', out, ok=(1, 2))
        err = result.stderr.decode(errors='replace')
        if message and message not in err:
            sys.exit(f'{name}: expected "{message}", got: {err.strip()}')
        if out.exists() or any(p.name.startswith('.frond-zaurus-cf-') for p in work.iterdir()):
            sys.exit(f'{name}: left an output behind')
        if script_refuses is not None:
            theirs = script('--hdd1', bad, *map(str, args), '--output', out, ok=(0, 1, 2))
            refused = theirs.returncode != 0
            out.unlink(missing_ok=True)
            if refused != script_refuses:
                sys.exit(f'{name}: the script {"refuses" if refused else "accepts"} it')
        print(f'refused: {name}: {err.strip().removeprefix("Could not create Zaurus CF image: ")}')
        bad.unlink()

    shutil.rmtree(SCRATCH)
    print('zaurus-cf-builder: all checks passed')


if __name__ == '__main__':
    main()
