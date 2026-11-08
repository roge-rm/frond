#!/usr/bin/env python3
"""Package locally built Frond binaries using a checked, explicit file inventory."""

import argparse
import gzip
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from package_policy import DOCS, LICENCE_FILES, check_deb, check_dependencies, check_stage

ROOT = Path(__file__).resolve().parents[1]


def package(root=ROOT, core='out/frond', desktop='out/frond-desktop',
            core68k='out/frond-68k', btvirt='build/bluetooth-provider/emulator/btvirt', architecture=None,
            strip='strip', runner=()):
    # A cross build passes its own binaries, architecture, strip tool and an emulator runner.
    version = (root / 'VERSION').read_text().strip()
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise ValueError('VERSION must contain a release version such as 0.2.0')
    revision = (root / 'packaging/debian-revision').read_text().strip()
    if not re.fullmatch(r'[1-9][0-9]*', revision):
        raise ValueError('packaging/debian-revision must be a positive integer')
    package_version = version + '-' + revision
    # Use the source commit time by default, never the wall clock.
    epoch = os.environ.get('SOURCE_DATE_EPOCH')
    if epoch is None:
        epoch = subprocess.check_output(['git', '-C', str(root), 'log', '-1', '--format=%ct'], text=True).strip()
    if not re.fullmatch(r'[0-9]+', epoch):
        raise ValueError('SOURCE_DATE_EPOCH must be a non-negative integer')
    env = dict(os.environ, SOURCE_DATE_EPOCH=epoch, LC_ALL='C', TZ='UTC')
    for executable in (core, desktop, core68k):
        binary = root / executable
        if binary.is_symlink() or not binary.is_file():
            raise ValueError(f'Expected regular built binary: {executable}')
        reported = subprocess.check_output([*runner, str(binary), '--version'],
            env=dict(env, QT_QPA_PLATFORM='offscreen'), text=True).strip()
        if not reported or reported.split()[-1] != version:
            raise ValueError(f'{executable} reports {reported!r}, expected {version}')
    # This Frond's Bluetooth radio, BlueZ btvirt (tools/build-bluetooth-provider.py). It has no --version.
    radio = root / btvirt
    if radio.is_symlink() or not radio.is_file():
        raise ValueError(f'Expected regular built binary: {btvirt} (build it with tools/build-bluetooth-provider.py)')
    if architecture is None:
        architecture = subprocess.check_output(['dpkg', '--print-architecture'], text=True).strip()
    work_parent = root / 'build'
    work_parent.mkdir(exist_ok=True)
    # Isolated staging preserves existing package trees and user files.
    with tempfile.TemporaryDirectory(prefix='deb-', dir=work_parent) as directory:
        work = Path(directory)
        stage = work / 'debian/frond'
        control = stage / 'DEBIAN'
        control.mkdir(parents=True)

        def copy(source, destination, mode=0o644):
            source = root / source
            if source.is_symlink() or not source.is_file():
                raise ValueError(f'Expected regular package input: {source}')
            target = stage / destination
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
            target.chmod(mode)
            return target

        core = copy(core, 'usr/lib/frond/frond-core', 0o755)
        core68k = copy(core68k, 'usr/lib/frond/frond-68k-core', 0o755)
        gui = copy(desktop, 'usr/bin/frond-desktop', 0o755)
        radio = copy(btvirt, 'usr/lib/frond/btvirt', 0o755)
        for binary in (core, core68k, gui, radio):
            subprocess.run([strip, '--strip-unneeded', str(binary)], check=True)
        copy('packaging/org.frond.Frond.desktop', 'usr/share/applications/org.frond.Frond.desktop')
        copy('packaging/frond.svg', 'usr/share/icons/hicolor/scalable/apps/frond.svg')
        copy('packaging/frond-mime.xml', 'usr/share/mime/packages/frond.xml')
        copy('packaging/copyright', 'usr/share/doc/frond/copyright')
        copy('LICENSE', 'usr/share/doc/frond/LICENSE')
        for name in LICENCE_FILES:
            copy(f'licences/{name}', f'usr/share/doc/frond/{name}')
        copy('doc/using-frond.md', 'usr/share/doc/frond/README')
        for name in DOCS:
            copy(f'doc/{name}', f'usr/share/doc/frond/{name}')
        (work / 'debian/control').write_text('Source: frond\n\nPackage: frond\nArchitecture: any\nDescription: Palm OS emulator\n')
        deps = subprocess.check_output(['dpkg-shlibdeps', '--warnings=1', '-O', '-e' + str(core),
                                        '-e' + str(core68k), '-e' + str(gui), '-e' + str(radio)], cwd=work, text=True,
                                       env=dict(os.environ, DEB_HOST_ARCH=architecture))
        deps = next(line.removeprefix('shlibs:Depends=') for line in deps.splitlines()
                    if line.startswith('shlibs:Depends='))
        deps += ', mtools, qt6-qpa-plugins, qt6-wayland, libqt6svg6, hicolor-icon-theme'
        # libslirp0 must come from the actual ELF linkage, not a hand-written fallback.
        check_dependencies(deps)
        changelog = stage / 'usr/share/doc/frond/changelog.gz'
        with changelog.open('wb') as raw, gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=0) as output:
            output.write(f'Frond {version}\n\n * Native Palm OS emulator and desktop launcher.\n'.encode())
        changelog.chmod(0o644)
        installed = (sum(p.stat().st_size for p in stage.rglob('*') if p.is_file()) + 1023) // 1024
        (control / 'control').write_text(f'''Package: frond
Version: {package_version}
Architecture: {architecture}
Maintainer: Frond contributors <frond@users.noreply.github.com>
Section: otherosfs
Priority: optional
Depends: {deps}
Installed-Size: {installed}
Homepage: https://github.com/roge-rm/frond
Description: Native Palm OS emulator
 Qt desktop launcher and SDL Palm display with persistent sessions,
 application installation and database export. Firmware is supplied separately.
''')
        (control / 'control').chmod(0o644)
        for directory in [stage, *stage.rglob('*')]:
            if directory.is_dir():
                directory.chmod(0o755)
            os.utime(directory, (int(epoch), int(epoch)))
        check_stage(stage)
        temporary_output = work / 'frond.deb'
        subprocess.run(['dpkg-deb', '--root-owner-group', '--uniform-compression', '-Zxz', '-z6',
                        '--build', str(stage), str(temporary_output)], env=env, check=True)
        check_deb(temporary_output, package_version, architecture)
        output = root / 'out' / f'frond_{package_version}_{architecture}.deb'
        output.parent.mkdir(exist_ok=True)
        temporary_output.replace(output)
    print(output)
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', default='out/frond', help='core binary, relative to the checkout')
    parser.add_argument('--core-68k', default='out/frond-68k', help='68k core binary, relative to the checkout')
    parser.add_argument('--desktop', default='out/frond-desktop', help='desktop binary, relative to the checkout')
    parser.add_argument('--btvirt', default='build/bluetooth-provider/emulator/btvirt',
                        help='btvirt binary (Bluetooth radio), relative to the checkout')
    parser.add_argument('--architecture', help='Debian architecture; defaults to the host')
    parser.add_argument('--strip', default='strip', help='strip tool for the binaries')
    parser.add_argument('--runner', default='', help='emulator that runs the binaries for their version check')
    args = parser.parse_args()
    try:
        package(core=args.core, core68k=args.core_68k, desktop=args.desktop, btvirt=args.btvirt, architecture=args.architecture,
                strip=args.strip, runner=args.runner.split())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
