#!/usr/bin/env python3
"""Build the Android btvirt executable with the NDK."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

REVISION = '82667d713949447d7c94725432c61b33689ae68a'
URL = f'https://codeload.github.com/bluez/bluez/tar.gz/{REVISION}'
SHA256 = 'e19bad8f61645fe8d26502fd724ff69cb0f1d039f8f72f6bb185d819de6fc008'
FILES = [
    'emulator/main.c', 'emulator/serial.c', 'emulator/server.c',
    'emulator/vhci.c', 'emulator/btdev.c', 'emulator/bthost.c',
    'emulator/smp.c', 'emulator/phy.c', 'emulator/le.c',
    'lib/bluetooth/bluetooth.c', 'src/shared/queue.c', 'src/shared/util.c',
    'src/shared/crypto.c', 'src/shared/ecc.c', 'src/shared/io-mainloop.c',
    'src/shared/timeout-mainloop.c', 'src/shared/mainloop.c',
    'src/shared/mainloop-notify.c',
]


def run(*args, cwd=None):
    subprocess.run([str(a) for a in args], cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, required=True)
    parser.add_argument('--abi', choices=['arm64-v8a', 'x86_64'], required=True)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    patch = Path(__file__).resolve().parents[1] / 'tools/bluez-bdaddr.patch'
    identity = hashlib.sha256(Path(__file__).read_bytes() + patch.read_bytes()
                              + str(args.ndk.resolve()).encode()
                              + (args.ndk / 'source.properties').read_bytes()).hexdigest()
    build = root / args.abi
    stamp = build / '.built'
    output = root / 'jniLibs' / args.abi / 'libbtvirt.so'
    if stamp.is_file() and stamp.read_text() == identity and output.is_file():
        print(f'Android btvirt {args.abi}: cached')
        return
    archive = root / 'sources/bluez.tar.gz'
    archive.parent.mkdir(exist_ok=True)
    if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        partial = archive.with_suffix('.part')
        urllib.request.urlretrieve(URL, partial)
        if hashlib.sha256(partial.read_bytes()).hexdigest() != SHA256:
            partial.unlink()
            raise RuntimeError(f'SHA-256 mismatch: {URL}')
        partial.replace(archive)
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir()
    with tarfile.open(archive) as tar:
        tar.extractall(build, filter='data')
    source = build / f'bluez-{REVISION}'
    run('patch', '-p1', '--batch', '--fuzz=0', '-i', patch, cwd=source)
    # Bionic exposes getrandom at API 28; the syscall is available at API 26.
    for name in ['emulator/server.c', 'src/shared/util.c']:
        path = source / name
        text = path.read_text()
        old = '#include <sys/random.h>'
        if text.count(old) != 1:
            raise RuntimeError(f'{name}: random header not found')
        path.write_text(text.replace(old, old + '\n#include <sys/syscall.h>\n'
            '#define getrandom(buf, len, flags) syscall(__NR_getrandom, buf, len, flags)'))
    (build / 'config.h').write_text('''#define VERSION "5.87-frond"
#define HAVE_LINUX_TYPES_H 1
#define HAVE_LINUX_IF_ALG_H 1
#define HAVE_SYS_RANDOM_H 1
#define HAVE_GETRANDOM 1
''')
    (build / 'bluetooth').symlink_to(source / 'lib/bluetooth', target_is_directory=True)
    binary = args.ndk.resolve() / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
    triple = 'aarch64-linux-android' if args.abi == 'arm64-v8a' else 'x86_64-linux-android'
    cc = binary / f'{triple}26-clang'
    objects = []
    for name in FILES:
        obj = build / (name.replace('/', '_') + '.o')
        run(cc, '-O2', '-fPIE', '-ffunction-sections', '-fdata-sections',
            '-Wno-macro-redefined', '-DHAVE_CONFIG_H', '-I', build, '-I', source,
            '-c', source / name, '-o', obj)
        objects.append(obj)
    output.parent.mkdir(parents=True, exist_ok=True)
    run(cc, '-pie', '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384',
        *objects, '-o', output)
    run(binary / 'llvm-strip', '--strip-unneeded', output)
    stamp.write_text(identity)
    print(f'Android btvirt {args.abi}: built {output}')


if __name__ == '__main__':
    main()
