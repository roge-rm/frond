#!/usr/bin/env python3
"""Firmware-free core copy and packaging regressions; packages contain dummy ELF only."""

import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import package_policy as policy

copy_spec = importlib.util.spec_from_file_location('copy_core', ROOT / 'tools/copy-core.py')
copy_core = importlib.util.module_from_spec(copy_spec)
copy_spec.loader.exec_module(copy_core)

spec = importlib.util.spec_from_file_location('package_deb', ROOT / 'tools/package-deb.py')
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


class PackageCliTests(unittest.TestCase):
    def test_help_and_unknown_options_do_not_build(self):
        with tempfile.TemporaryDirectory(prefix='frond-package-cli-') as directory:
            tools = Path(directory) / 'tools'
            tools.mkdir()
            for name in ('package-deb.py', 'package_policy.py'):
                shutil.copyfile(ROOT / 'tools' / name, tools / name)
            script = tools / 'package-deb.py'
            help_result = subprocess.run([sys.executable, str(script), '--help'], capture_output=True)
            self.assertEqual(help_result.returncode, 0)
            self.assertIn(b'usage:', help_result.stdout)
            bad_result = subprocess.run([sys.executable, str(script), '--unknown'], capture_output=True)
            self.assertEqual(bad_result.returncode, 2)
            self.assertFalse((Path(directory) / 'out').exists())


class CopyCoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='frond-build-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'checkout'
        (self.root / 'core/src/common').mkdir(parents=True)
        git(self.root, 'init', '--quiet')
        (self.root / 'core/src/common/value.txt').write_text('one\n')
        (self.root / 'core/src/common/gone.txt').write_text('gone\n')
        (self.root / 'core/.gitignore').write_text('*.o\n')
        (self.root / 'core/src/common/built.o').write_text('object\n')
        git(self.root, 'add', 'core')
        self.destination = self.root / 'build/copy'

    def copy(self):
        with contextlib.redirect_stdout(io.StringIO()):
            copy_core.copy(self.destination, self.root)

    def test_copy_update_and_removal(self):
        self.copy()
        value = self.destination / 'src/common/value.txt'
        self.assertEqual(value.read_text(), 'one\n')
        self.assertFalse((self.destination / 'src/common/built.o').exists())
        built = self.destination / 'src/common/other.o'
        built.write_text('kept\n')
        (self.root / 'core/src/common/value.txt').write_text('two\n')
        (self.root / 'core/src/common/new.txt').write_text('new\n')
        (self.root / 'core/src/common/gone.txt').unlink()
        self.copy()
        self.assertEqual(value.read_text(), 'two\n')
        self.assertTrue((self.destination / 'src/common/new.txt').is_file())
        self.assertFalse((self.destination / 'src/common/gone.txt').exists())
        self.assertEqual(built.read_text(), 'kept\n')

    def test_unmarked_directory_is_preserved(self):
        self.destination.mkdir(parents=True)
        saved = self.destination / 'user.session'
        saved.write_text('private marker')
        with self.assertRaises(ValueError):
            self.copy()
        self.assertEqual(saved.read_text(), 'private marker')
        self.assertFalse((self.destination / 'src').exists())


class PackagePolicyTests(unittest.TestCase):
    def test_runtime_dependencies_are_required(self):
        deps = ', '.join(policy.RUNTIME_PACKAGES)
        policy.check_dependencies(deps)
        for missing in ('mtools', 'libslirp0'):
            with self.subTest(missing=missing), self.assertRaisesRegex(ValueError, missing):
                policy.check_dependencies(deps.replace(missing, 'unrelated'))
        with self.assertRaises(ValueError):
            policy.check_dependencies(deps.replace('libslirp0', 'libslirp0 | unrelated'))

    def test_private_files_links_and_wrong_modes_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            for name, mode in policy.FILES.items():
                file = stage / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text('synthetic')
                file.chmod(mode)
            policy.check_stage(stage)
            private = stage / 'usr/share/doc/frond/private.frond'
            private.write_text('private data')
            with self.assertRaises(ValueError):
                policy.check_stage(stage)
            private.unlink()
            private.symlink_to(stage / 'usr/bin/frond-desktop')
            with self.assertRaisesRegex(ValueError, 'symlink'):
                policy.check_stage(stage)
            private.unlink()
            (stage / 'usr/bin/frond-desktop').chmod(0o777)
            with self.assertRaisesRegex(ValueError, 'modes'):
                policy.check_stage(stage)

    def test_synthetic_package_is_repeatable_and_excludes_private_inputs(self):
        # Exercise real strip, ELF dependency discovery, dpkg-deb, and archive inspection.
        # This never packages Frond binaries or takes inputs from a user's home directory.
        with tempfile.TemporaryDirectory(prefix='frond-synthetic-deb-') as temporary:
            root = Path(temporary)
            for name in ('out', 'packaging', 'doc', 'licences'):
                (root / name).mkdir(parents=True)
            (root / 'VERSION').write_text('0.2.0\n')
            (root / 'packaging/debian-revision').write_text('1\n')
            for name in ('LICENSE', 'packaging/org.frond.Frond.desktop', 'packaging/frond.svg',
                         'packaging/frond-mime.xml', 'packaging/copyright', 'doc/using-frond.md',
                         'doc/developer-reference.md', 'doc/testing.md', 'doc/performance.md',
                         'doc/adding-a-device.md',
                         *(f'licences/{name}' for name in policy.LICENCE_FILES)):
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text('Synthetic fixture, not a Frond release.\n')
            source = root / 'dummy.c'
            source.write_text('#include <stdio.h>\n#include <slirp/libslirp.h>\n'
                              'int main(int argc, char **argv) {\n'
                              'if (argc > 3) puts(slirp_version_string());\n'
                              'puts("Synthetic-fixture 0.2.0"); return 0; }\n')
            flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'slirp'], text=True).split()
            subprocess.run(['cc', str(source), '-o', str(root / 'out/frond'), *flags], check=True)
            for name in ('frond-desktop', 'frond-68k'):
                shutil.copyfile(root / 'out/frond', root / 'out' / name)
                (root / 'out' / name).chmod(0o755)
            btvirt = root / 'build/bluetooth-provider/emulator/btvirt'
            btvirt.parent.mkdir(parents=True)
            shutil.copyfile(root / 'out/frond', btvirt)
            btvirt.chmod(0o755)
            phone = root / 'build/bluetooth-opp-peer/bluetooth-opp-peer'
            phone.parent.mkdir(parents=True)
            shutil.copyfile(root / 'out/frond', phone)
            phone.chmod(0o755)
            # Seed ignored/private data near every tempting broad-copy input.
            for name in ('out/TX.rom', 'out/private.session', 'doc/private.frond', 'firmware.bin'):
                (root / name).write_text('PRIVATE-MARKER-DO-NOT-PACKAGE')
            preserved = root / 'build/deb/debian/frond/user-file'
            preserved.parent.mkdir(parents=True)
            preserved.write_text('keep me')
            with patch.dict(os.environ, SOURCE_DATE_EPOCH='1700000000'):
                with contextlib.redirect_stdout(io.StringIO()):
                    output = packager.package(root)
                    first = hashlib.sha256(output.read_bytes()).hexdigest()
                    # Change all input mtimes; package bytes should remain equal.
                    for item in root.rglob('*'):
                        os.utime(item, (1800000000, 1800000000))
                    output = packager.package(root)
                self.assertEqual(first, hashlib.sha256(output.read_bytes()).hexdigest())
            self.assertEqual(preserved.read_text(), 'keep me')
            with tempfile.TemporaryFile() as data:
                subprocess.run(['dpkg-deb', '--fsys-tarfile', str(output)], stdout=data, check=True)
                data.seek(0)
                with tarfile.open(fileobj=data) as archive:
                    for member in archive:
                        if member.isfile():
                            self.assertNotIn(b'PRIVATE-MARKER', archive.extractfile(member).read())
                        self.assertEqual(member.mtime, 1700000000)
            with patch.dict(os.environ, SOURCE_DATE_EPOCH='1700000000'):
                (root / 'VERSION').write_text('0.3.0')
                with self.assertRaisesRegex(ValueError, 'expected 0.3.0'):
                    packager.package(root)
                (root / 'VERSION').write_text('0.2.0')
            # Confirm the finished-artifact check rejects an unexpected private file.
            altered = root / 'altered'
            subprocess.run(['dpkg-deb', '--raw-extract', str(output), str(altered)], check=True)
            (altered / 'usr/share/doc/frond/private.rom').write_text('private')
            bad = root / 'bad.deb'
            subprocess.run(['dpkg-deb', '--root-owner-group', '--build', str(altered), str(bad)],
                           stdout=subprocess.DEVNULL, check=True)
            architecture = subprocess.check_output(['dpkg', '--print-architecture'], text=True).strip()
            with self.assertRaisesRegex(ValueError, 'inventory'):
                policy.check_deb(bad, '0.2.0-1', architecture)
            (altered / 'usr/share/doc/frond/private.rom').unlink()
            (altered / 'DEBIAN/private.session').write_text('private')
            subprocess.run(['dpkg-deb', '--root-owner-group', '--build', str(altered), str(bad)],
                           stdout=subprocess.DEVNULL, check=True)
            with self.assertRaisesRegex(ValueError, 'control files'):
                policy.check_deb(bad, '0.2.0-1', architecture)
            (root / 'out/frond-desktop').unlink()
            (root / 'out/frond-desktop').symlink_to(root / 'out/frond')
            with patch.dict(os.environ, SOURCE_DATE_EPOCH='1700000000'):
                with self.assertRaisesRegex(ValueError, 'regular built binary'):
                    packager.package(root)


if __name__ == '__main__':
    unittest.main()
