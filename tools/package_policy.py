"""The complete installed-file inventory for a Frond Debian package."""

import subprocess
import tarfile
import tempfile

FILES = {
    'usr/lib/frond/frond-core': 0o755,
    'usr/bin/frond-desktop': 0o755,
    'usr/share/applications/org.frond.Frond.desktop': 0o644,
    'usr/share/icons/hicolor/scalable/apps/frond.svg': 0o644,
    'usr/share/mime/packages/frond.xml': 0o644,
    **{f'usr/share/doc/frond/{name}': 0o644 for name in (
        'copyright', 'LICENSE', 'argparse-LICENSE', 'zip-UNLICENSE', 'README',
        'peripherals.md', 'infrared.md', 'bluetooth-provider.md', 'peripheral-qualification.md', 'changelog.gz')},
}
RUNTIME_PACKAGES = ('mtools', 'libslirp0', 'qt6-qpa-plugins', 'qt6-wayland',
                    'libqt6svg6', 'hicolor-icon-theme')


def check_dependencies(value):
    # Each required package must be unconditional, not just one alternative.
    packages = {part.strip().split()[0] for part in value.split(',') if part.strip() and '|' not in part}
    missing = set(RUNTIME_PACKAGES) - packages
    if missing:
        raise ValueError('Missing runtime dependencies: ' + ', '.join(sorted(missing)))


def check_stage(stage):
    found = {}
    for path in stage.rglob('*'):
        name = path.relative_to(stage).as_posix()
        if path.is_symlink():
            raise ValueError(f'Unexpected symlink in package: {name}')
        if name == 'DEBIAN' or name.startswith('DEBIAN/'):
            if name not in ('DEBIAN', 'DEBIAN/control'):
                raise ValueError(f'Unexpected package metadata: {name}')
            continue
        if path.is_file():
            found[name] = path.stat().st_mode & 0o7777
        elif not path.is_dir():
            raise ValueError(f'Unexpected special file: {name}')
    if found != FILES:
        raise ValueError(f'Package file inventory/modes differ: {sorted(set(found) ^ set(FILES)) or "file modes"}')


def check_deb(package, version, architecture):
    for field, expected in (('Package', 'frond'), ('Version', version), ('Architecture', architecture)):
        actual = subprocess.check_output(['dpkg-deb', '--field', str(package), field], text=True).strip()
        if actual != expected:
            raise ValueError(f'{field}: expected {expected!r}, got {actual!r}')
    check_dependencies(subprocess.check_output(['dpkg-deb', '--field', str(package), 'Depends'], text=True))
    with tempfile.TemporaryFile() as data:
        subprocess.run(['dpkg-deb', '--fsys-tarfile', str(package)], stdout=data, check=True)
        data.seek(0)
        with tarfile.open(fileobj=data) as archive:
            files = {}
            for member in archive:
                name = member.name.removeprefix('./').rstrip('/')
                if name in ('', '.'):
                    if not member.isdir() or member.mode != 0o755 or member.uid or member.gid:
                        raise ValueError('Unexpected package root metadata')
                    continue
                if member.uid or member.gid:
                    raise ValueError(f'Package member is not root-owned: {name}')
                if member.isfile():
                    if name in files:
                        raise ValueError(f'Duplicate package member: {name}')
                    files[name] = member.mode
                elif not member.isdir():
                    raise ValueError(f'Unexpected package link/special file: {name}')
                elif member.mode != 0o755:
                    raise ValueError(f'Unexpected package directory mode: {name}')
                elif not any(path.startswith(name + '/') for path in FILES):
                    raise ValueError(f'Unexpected package directory: {name}')
            if files != FILES:
                raise ValueError('Debian artifact file inventory/modes differ')

    with tempfile.TemporaryFile() as data:
        subprocess.run(['dpkg-deb', '--ctrl-tarfile', str(package)], stdout=data, check=True)
        data.seek(0)
        with tarfile.open(fileobj=data) as archive:
            members = [member for member in archive if member.name not in ('.', './')]
            if len(members) != 1 or members[0].name.removeprefix('./') != 'control':
                raise ValueError('Unexpected Debian control files')
            member = members[0]
            if not member.isfile() or member.mode != 0o644 or member.uid or member.gid:
                raise ValueError('Unexpected Debian control file metadata')
