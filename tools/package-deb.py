#!/usr/bin/env python3
"""Package locally built Frond binaries; firmware and user sessions are never included."""
from pathlib import Path
import gzip
import os
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
version = (ROOT / 'VERSION').read_text().strip()
if not re.fullmatch(r'\d+\.\d+\.\d+', version):
    raise SystemExit('VERSION must contain a release version such as 0.1.0')
for executable in ('out/frond', 'out/frond-desktop'):
    reported = subprocess.check_output([str(ROOT / executable), '--version'],
        env=dict(os.environ, QT_QPA_PLATFORM='offscreen'), text=True).strip()
    if reported.split()[-1] != version:
        raise SystemExit(f'{executable} reports {reported!r}, expected {version}')
revision = (ROOT / 'packaging/debian-revision').read_text().strip()
if not re.fullmatch(r'[1-9][0-9]*', revision):
    raise SystemExit('packaging/debian-revision must be a positive integer')
package_version = version + '-' + revision
architecture = subprocess.check_output(['dpkg', '--print-architecture'], text=True).strip()
work = ROOT / 'build' / 'deb'
stage = work / 'debian' / 'frond'
if stage.exists():
    shutil.rmtree(stage)
stage.mkdir(parents=True)
control = stage / 'DEBIAN'
control.mkdir()

def copy(source, destination, mode=0o644):
    target = stage / destination
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / source, target)
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
# dpkg-shlibdeps needs package metadata in its working directory.
metadata = work / 'debian'
metadata.mkdir(exist_ok=True)
(metadata / 'control').write_text('Source: frond\n\nPackage: frond\nArchitecture: any\nDescription: Palm TX emulator\n')
deps = subprocess.check_output(['dpkg-shlibdeps', '--warnings=1', '-O', '-e' + str(core), '-e' + str(gui)], cwd=work, text=True)
deps = next(line.removeprefix('shlibs:Depends=') for line in deps.splitlines() if line.startswith('shlibs:Depends='))
installed = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file()) // 1024 + 1
(control / 'control').write_text(f'''Package: frond
Version: {package_version}
Architecture: {architecture}
Maintainer: Frond contributors <frond@users.noreply.github.com>
Section: otherosfs
Priority: optional
Depends: {deps}, qt6-qpa-plugins, qt6-wayland, libqt6svg6, hicolor-icon-theme
Installed-Size: {installed}
Homepage: https://github.com/roge-rm/frond
Description: Native Palm TX emulator
 Qt desktop launcher and SDL Palm display with persistent sessions,
 application installation and database export. Firmware is supplied separately.
''')
changelog = stage / 'usr/share/doc/frond/changelog.gz'
with gzip.GzipFile(filename=str(changelog), mode='wb', mtime=0) as output:
    output.write(f'Frond {version}\n\n * Initial Debian desktop package.\n'.encode())
for directory in stage.rglob('*'):
    if directory.is_dir():
        directory.chmod(0o755)
changelog.chmod(0o644)
stage.chmod(0o755)
output = ROOT / 'out' / f'frond_{package_version}_{architecture}.deb'
subprocess.run(['dpkg-deb', '--root-owner-group', '--build', str(stage), str(output)], check=True)
print(output)
