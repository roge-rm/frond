#!/usr/bin/env python3
"""Build the Android router's static dependencies with the NDK."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

SOURCES = [
    ('libiconv-1.18', 'https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.18.tar.gz',
     '3b08f5f4f9b4eb82f151a7040bfd6fe6c6fb922efe4b1659c66ea933276965e8'),
    ('proxy-libintl-33934de09af6a6627eb44e310a8079df009abdbb',
     'https://codeload.github.com/frida/proxy-libintl/tar.gz/33934de09af6a6627eb44e310a8079df009abdbb',
     '4ab5af85ec6124cb526a15c58bbc0877d241807e30855779808dbf9292d861aa'),
    ('glib-2.84.4', 'https://download.gnome.org/sources/glib/2.84/glib-2.84.4.tar.xz',
     '8a9ea10943c36fc117e253f80c91e477b673525ae45762942858aef57631bb90'),
    ('libslirp-v4.9.1', 'https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v4.9.1/libslirp-v4.9.1.tar.bz2',
     '3caff6e2de445f4995629d4929c55419f661b2b1d14f12481e155a71c1e8f811'),
]
FFI_WRAP = '''[wrap-file]
directory = libffi-3.4.6
source_url = https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz
source_filename = libffi-3.4.6.tar.gz
source_hash = b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e
patch_filename = libffi_3.4.6-1_patch.zip
patch_url = https://wrapdb.mesonbuild.com/v2/libffi_3.4.6-1/get_patch
patch_hash = 22914080b188ba71df6e43f057f3b96ab9045c3593a0a62d237929b1907ae99c
[provide]
dependency_names = libffi
'''


def run(*args, env=None, cwd=None):
    subprocess.run([str(a) for a in args], check=True, env=env, cwd=cwd)


def fetch(root, name, url, digest):
    source = root / name
    stamp = source / '.frond-sha256'
    if stamp.exists() and stamp.read_text().strip() == digest:
        return source
    archive = root / url.rsplit('/', 1)[1]
    if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        partial = archive.with_suffix('.part')
        urllib.request.urlretrieve(url, partial)
        if hashlib.sha256(partial.read_bytes()).hexdigest() != digest:
            partial.unlink()
            raise RuntimeError(f'SHA-256 mismatch: {url}')
        partial.replace(archive)
    shutil.rmtree(source, ignore_errors=True)
    with tarfile.open(archive) as tar:
        tar.extractall(root, filter='data')
    stamp.write_text(digest + '\n')
    return source


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ndk', type=Path, required=True)
    parser.add_argument('--abi', choices=['arm64-v8a', 'x86_64'], required=True)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    build = root / args.abi
    stamp = build / '.built'
    identity = hashlib.sha256(Path(__file__).read_bytes() + str(args.ndk.resolve()).encode()
                              + (args.ndk / 'source.properties').read_bytes()).hexdigest()
    outputs = ['lib/libslirp.a', 'lib/libglib-2.0.a', 'lib/libpcre2-8.a', 'lib/libiconv.a', 'lib/libintl.a',
               'include/slirp/libslirp.h', 'include/slirp/libslirp-version.h', 'include/glib-2.0/glib.h',
               'include/glib-2.0/glibconfig.h', 'lib/pkgconfig/glib-2.0.pc',
               'lib/pkgconfig/slirp.pc']
    if (stamp.exists() and stamp.read_text() == identity
            and all((build / 'install' / output).is_file() for output in outputs)):
        print(f'Android network {args.abi}: cached')
        return
    sources = root / 'sources'
    sources.mkdir(exist_ok=True)
    iconv, intl, glib, slirp = [fetch(sources, *spec) for spec in SOURCES]
    (glib / 'subprojects/libffi.wrap').write_text(FFI_WRAP)
    shutil.copytree(intl, glib / 'subprojects/proxy-libintl', dirs_exist_ok=True)
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir()
    prefix = build / 'install'
    toolchain = args.ndk.resolve() / 'toolchains/llvm/prebuilt/linux-x86_64'
    binary = toolchain / 'bin'
    triple, cpu = ('aarch64-linux-android', 'aarch64') if args.abi == 'arm64-v8a' else ('x86_64-linux-android', 'x86_64')
    ib = build / 'iconv'
    ib.mkdir()
    env = os.environ.copy()
    env.update(CC=str(binary / f'{triple}26-clang'), AR=str(binary / 'llvm-ar'),
               RANLIB=str(binary / 'llvm-ranlib'), CFLAGS='-O2 -fPIC')
    run(iconv / 'configure', f'--host={triple}', f'--prefix={prefix}',
        '--disable-shared', '--enable-static', '--disable-nls', env=env, cwd=ib)
    run('make', '-C', ib, 'lib/localcharset.h', env=env)
    run('make', '-C', ib / 'lib', '-j4', 'install', env=env)
    (prefix / 'include').mkdir(exist_ok=True)
    shutil.copy(ib / 'include/iconv.h', prefix / 'include/iconv.h')
    cross = build / 'android.ini'
    cross.write_text(f'''[binaries]
c = '{binary}/{triple}26-clang'
cpp = '{binary}/{triple}26-clang++'
ar = '{binary}/llvm-ar'
strip = '{binary}/llvm-strip'
pkg-config = '/usr/bin/pkg-config'
[host_machine]
system = 'android'
cpu_family = '{cpu}'
cpu = '{cpu}'
endian = 'little'
[properties]
needs_exe_wrapper = true
pkg_config_libdir = ['{prefix}/lib/pkgconfig']
[built-in options]
c_args = ['-ffunction-sections', '-fdata-sections', '-I{prefix}/include']
c_link_args = ['-L{prefix}/lib']
cpp_args = ['-I{prefix}/include']
cpp_link_args = ['-L{prefix}/lib']
''')
    common = ['--cross-file', cross, '--prefix', prefix, '--libdir', 'lib',
              '--default-library', 'static', '--buildtype', 'release', '-Db_staticpic=true']
    gb = build / 'glib'
    run('meson', 'setup', gb, glib, *common, '--force-fallback-for=libpcre2-8,libffi',
        '-Dtests=false', '-Dinstalled_tests=false', '-Dintrospection=disabled',
        '-Dnls=disabled', '-Dman-pages=disabled', '-Ddocumentation=false',
        '-Dlibmount=disabled', '-Dselinux=disabled', '-Dxattr=false',
        '-Dlibelf=disabled', '-Ddtrace=disabled', '-Dsystemtap=disabled', '-Dsysprof=disabled')
    # Only libglib is needed by slirp. Do not build GIO, GObject or their tools.
    run('ninja', '-C', gb, '-j4', 'glib/libglib-2.0.a',
        'subprojects/pcre2-10.44/libpcre2-8.a', 'subprojects/proxy-libintl/libintl.a')
    include = prefix / 'include/glib-2.0'
    shutil.copytree(glib / 'glib', include / 'glib', dirs_exist_ok=True)
    shutil.copytree(gb / 'glib', include / 'glib', dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('*.o', '*.a', '*.p'))
    shutil.copy(glib / 'glib/glib.h', include / 'glib.h')
    shutil.copy(gb / 'glib/glibconfig.h', include / 'glibconfig.h')
    lib = prefix / 'lib'
    lib.mkdir(exist_ok=True)
    shutil.copy(gb / 'glib/libglib-2.0.a', lib)
    pcre = next(gb.glob('subprojects/pcre2-*/libpcre2-8.a'))
    shutil.copy(pcre, lib)
    shutil.copy(gb / 'subprojects/proxy-libintl/libintl.a', lib)
    pc = lib / 'pkgconfig'
    pc.mkdir(exist_ok=True)
    (pc / 'glib-2.0.pc').write_text(f'''Name: GLib
Description: Android GLib for libslirp
Version: 2.84.4
Libs: -L{lib} -lglib-2.0 -lpcre2-8 -liconv -lintl -lm
Cflags: -I{include}
''')
    sb = build / 'slirp'
    run('meson', 'setup', sb, slirp, *common, '--wrap-mode=nodownload')
    run('ninja', '-C', sb, '-j4', 'libslirp.a')
    run('meson', 'install', '-C', sb, '--no-rebuild')
    stamp.write_text(identity)


if __name__ == '__main__':
    main()
