#!/usr/bin/env python3
"""Stage a Frond AppDir from built binaries, with the libraries and Qt plugins they need.

Runs inside the frond-appimage image (tools/appimage/build.sh), where the target's libraries are.
The files are laid out like the Debian package's, under usr/.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from package_policy import DOCS, LICENCE_FILES  # noqa: E402

# The oldest glibc the AppImages run on: Debian 12's.
GLIBC_MAX = (2, 36)
# Libraries every desktop has, taken from the system: glibc, the C++ runtime (Qt needs the system's),
# graphics drivers, X and Wayland client libraries, sound servers, fonts, zlib and D-Bus.
EXCLUDE = re.compile(r'^(ld-linux.*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libanl|libnsl|libmvec'
                     r'|libBrokenLocale|libgcc_s|libstdc\+\+|libGL|libGLX|libGLdispatch|libOpenGL|libEGL|libGLESv1_CM'
                     r'|libGLESv2|libgbm|libdrm|libglapi|libX11|libX11-xcb|libxcb|libwayland-client|libwayland-cursor'
                     r'|libwayland-egl|libwayland-server|libasound|libpulse|libpulse-simple|libjack|libpipewire-0\.3'
                     r'|libfontconfig|libfreetype|libharfbuzz|libz|libexpat|libdbus-1)\.so(\.[0-9.]+)?$')
# Qt plugin folders bundled whole, and single platform plugins.
PLUGIN_DIRS = ('imageformats', 'iconengines', 'printsupport', 'platforminputcontexts', 'xcbglintegrations',
               'wayland-shell-integration', 'wayland-decoration-client', 'wayland-graphics-integration-client')
PLATFORMS = ('libqxcb.so', 'libqwayland-generic.so', 'libqwayland-egl.so', 'libqoffscreen.so')


def is_elf(path):
    with open(path, 'rb') as f:
        return f.read(4) == b'\x7fELF'


def needed(path):
    out = subprocess.check_output(['readelf', '-d', '--wide', str(path)], text=True)
    return re.findall(r'\(NEEDED\)\s+Shared library: \[([^\]]+)\]', out)


def glibc_versions(path):
    out = subprocess.check_output(['readelf', '-V', '--wide', str(path)], text=True)
    return {tuple(int(n) for n in v.split('.')) for v in re.findall(r'\bGLIBC_([0-9]+(?:\.[0-9]+)+)\b', out)}


def stage(arch, binaries, appdir, strip):
    triple = f'{arch}-linux-gnu'
    search = [Path(f'/usr/lib/{triple}'), Path(f'/lib/{triple}')]
    qt_plugins = Path(f'/usr/lib/{triple}/qt6/plugins')
    if appdir.exists():
        shutil.rmtree(appdir)
    usr = appdir / 'usr'
    lib = usr / 'lib'

    def copy(source, destination, mode=0o644):
        target = appdir / destination
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        target.chmod(mode)
        return target

    # The Debian package's layout: the desktop app finds the cores and the Bluetooth helpers in ../lib/frond.
    programs = [copy(binaries / 'frond-desktop', 'usr/bin/frond-desktop', 0o755),
                copy(binaries / 'frond', 'usr/lib/frond/frond-core', 0o755),
                copy(binaries / 'frond-68k', 'usr/lib/frond/frond-68k-core', 0o755),
                copy(binaries / 'btvirt', 'usr/lib/frond/btvirt', 0o755),
                copy(binaries / 'bluetooth-phone', 'usr/lib/frond/bluetooth-phone', 0o755)]
    # mtools dispatches on argv[0], so mdir and mcopy are links to it.
    programs.append(copy(Path('/usr/bin/mtools'), 'usr/lib/frond/mtools', 0o755))
    for tool in ('mdir', 'mcopy'):
        os.symlink('mtools', appdir / 'usr/lib/frond' / tool)
    for program in programs:
        subprocess.run([strip, '--strip-unneeded', str(program)], check=True)
    copy(ROOT / 'packaging/org.frond.Frond.desktop', 'usr/share/applications/org.frond.Frond.desktop')
    copy(ROOT / 'packaging/frond.svg', 'usr/share/icons/hicolor/scalable/apps/frond.svg')
    copy(ROOT / 'packaging/frond-mime.xml', 'usr/share/mime/packages/frond.xml')
    copy(ROOT / 'packaging/copyright', 'usr/share/doc/frond/copyright')
    copy(ROOT / 'LICENSE', 'usr/share/doc/frond/LICENSE')
    for name in LICENCE_FILES:
        copy(ROOT / 'licences' / name, f'usr/share/doc/frond/{name}')
    copy(ROOT / 'doc/using-frond.md', 'usr/share/doc/frond/README')
    for name in DOCS:
        copy(ROOT / 'doc' / name, f'usr/share/doc/frond/{name}')
    # appimagetool wants the desktop file and its icon at the top.
    copy(ROOT / 'packaging/org.frond.Frond.desktop', 'org.frond.Frond.desktop')
    copy(ROOT / 'packaging/frond.svg', 'frond.svg')
    copy(ROOT / 'tools/appimage/AppRun', 'AppRun', 0o755)
    # Qt finds its plugins from qt.conf beside the program, so nothing leaks into the programs it starts.
    (usr / 'bin/qt.conf').write_text('[Paths]\nPrefix = ..\nPlugins = plugins\n')

    plugins = []
    for name in PLATFORMS:
        plugins.append(copy(qt_plugins / 'platforms' / name, f'usr/plugins/platforms/{name}'))
    for folder in PLUGIN_DIRS:
        for source in sorted((qt_plugins / folder).glob('*.so')):
            plugins.append(copy(source, f'usr/plugins/{folder}/{source.name}'))

    # Every library the programs and plugins need, by DT_NEEDED, except the system ones.
    bundled = {}
    pending = [*programs, *plugins]
    while pending:
        for name in needed(pending.pop()):
            if name in bundled or EXCLUDE.match(name):
                continue
            source = next((d / name for d in search if (d / name).exists()), None)
            if source is None:
                raise SystemExit(f'Library not found: {name}')
            bundled[name] = source
            pending.append(copy(source.resolve(), f'usr/lib/{name}'))
    # Each ELF looks for its libraries in usr/lib, relative to where it is.
    for path in sorted(p for p in appdir.rglob('*') if p.is_file() and is_elf(p)):
        rpath = '$ORIGIN/' + os.path.relpath(lib, path.parent) if path.parent != lib else '$ORIGIN'
        subprocess.run(['patchelf', '--set-rpath', rpath, str(path)], check=True)

    # The bundled libraries' Debian copyright files.
    packages = {'mtools'}
    for name, source in bundled.items():
        # Merged /usr: a library may be listed under /lib or /usr/lib.
        for path in (source, Path(str(source).removeprefix('/usr'))):
            result = subprocess.run(['dpkg', '-S', str(path)], capture_output=True, text=True)
            if result.returncode == 0:
                packages.add(result.stdout.split(':')[0])
                break
        else:
            print(f'No package found for {source}', file=sys.stderr)
    for package in sorted(packages):
        notice = Path('/usr/share/doc') / package / 'copyright'
        if notice.is_file():
            copy(notice, f'usr/share/doc/bundled/{package}/copyright')

    highest = max(v for p in appdir.rglob('*') if p.is_file() and is_elf(p) for v in glibc_versions(p))
    print(f'{len(bundled)} libraries, {len(plugins)} Qt plugins, highest glibc symbol version {".".join(map(str, highest))}')
    if highest[:2] > GLIBC_MAX:
        raise SystemExit(f'Needs glibc {".".join(map(str, highest))}, above {".".join(map(str, GLIBC_MAX))}')
    for path in [appdir, *appdir.rglob('*')]:
        if path.is_dir():
            path.chmod(0o755)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', required=True, choices=('x86_64', 'aarch64'))
    parser.add_argument('--binaries', required=True, type=Path)
    parser.add_argument('--appdir', required=True, type=Path)
    parser.add_argument('--strip', default='strip')
    args = parser.parse_args()
    stage(args.arch, args.binaries, args.appdir, args.strip)
