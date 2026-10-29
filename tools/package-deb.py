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

from package_policy import check_deb, check_dependencies, check_stage

ROOT = Path(__file__).resolve().parents[1]


def package(root=ROOT):
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
    for executable in ('out/frond', 'out/frond-desktop'):
        binary = root / executable
        if binary.is_symlink() or not binary.is_file():
            raise ValueError(f'Expected regular built binary: {executable}')
        reported = subprocess.check_output([str(binary), '--version'],
            env=dict(env, QT_QPA_PLATFORM='offscreen'), text=True).strip()
        if not reported or reported.split()[-1] != version:
            raise ValueError(f'{executable} reports {reported!r}, expected {version}')
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

        core = copy('out/frond', 'usr/lib/frond/frond-core', 0o755)
        gui = copy('out/frond-desktop', 'usr/bin/frond-desktop', 0o755)
        for binary in (core, gui):
            subprocess.run(['strip', '--strip-unneeded', str(binary)], check=True)
        copy('packaging/org.frond.Frond.desktop', 'usr/share/applications/org.frond.Frond.desktop')
        copy('packaging/frond.svg', 'usr/share/icons/hicolor/scalable/apps/frond.svg')
        copy('packaging/frond-mime.xml', 'usr/share/mime/packages/frond.xml')
        copy('packaging/copyright', 'usr/share/doc/frond/copyright')
        copy('LICENSE', 'usr/share/doc/frond/LICENSE')
        copy('vendor/cloudpilot/src/argparse/LICENSE', 'usr/share/doc/frond/argparse-LICENSE')
        copy('vendor/cloudpilot/src/common/zip/UNLICENSE', 'usr/share/doc/frond/zip-UNLICENSE')
        copy('doc/desktop.md', 'usr/share/doc/frond/README')
        copy('doc/peripherals.md', 'usr/share/doc/frond/peripherals.md')
        copy('doc/infrared.md', 'usr/share/doc/frond/infrared.md')
        copy('doc/bluetooth-provider.md', 'usr/share/doc/frond/bluetooth-provider.md')
        copy('doc/peripheral-qualification.md', 'usr/share/doc/frond/peripheral-qualification.md')
        (work / 'debian/control').write_text('Source: frond\n\nPackage: frond\nArchitecture: any\nDescription: Palm TX emulator\n')
        deps = subprocess.check_output(['dpkg-shlibdeps', '--warnings=1', '-O', '-e' + str(core),
                                        '-e' + str(gui)], cwd=work, text=True)
        deps = next(line.removeprefix('shlibs:Depends=') for line in deps.splitlines()
                    if line.startswith('shlibs:Depends='))
        deps += ', mtools, qt6-qpa-plugins, qt6-wayland, libqt6svg6, hicolor-icon-theme'
        # libslirp0 must come from the actual ELF linkage, not a hand-written fallback.
        check_dependencies(deps)
        changelog = stage / 'usr/share/doc/frond/changelog.gz'
        with changelog.open('wb') as raw, gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=0) as output:
            output.write(f'Frond {version}\n\n * Native Palm TX emulator and desktop launcher.\n'.encode())
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
Description: Native Palm TX and Tungsten E2 emulator
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
        temporary_output.replace(output)
    print(output)
    return output


if __name__ == '__main__':
    argparse.ArgumentParser(description=__doc__).parse_args()
    try:
        package()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
