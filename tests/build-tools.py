#!/usr/bin/env python3
"""Firmware-free preparation and packaging regressions; packages contain dummy ELF only."""

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
import prepare
import package_policy as policy

spec = importlib.util.spec_from_file_location('package_deb', ROOT / 'tools/package-deb.py')
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


class PrepareTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='frond-build-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'checkout'
        self.root.mkdir()
        self.upstream = self.root / 'vendor/cloudpilot'
        self.upstream.mkdir(parents=True)
        git(self.root, 'init', '--quiet')
        git(self.upstream, 'init', '--quiet')
        for name in ('common/value.txt', 'uarm/value.txt', 'argparse/LICENSE', 'Makefile.common'):
            target = self.upstream / 'src' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('one\n')
        (self.upstream / 'LICENSE').write_text('synthetic license\n')
        git(self.upstream, 'add', '.')
        git(self.upstream, '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid',
            'commit', '--quiet', '-m', 'Synthetic upstream')
        self.revision = git(self.upstream, 'rev-parse', 'HEAD')
        git(self.root, 'update-index', '--add', '--cacheinfo', f'160000,{self.revision},vendor/cloudpilot')
        (self.root / 'tools').mkdir()
        (self.root / 'tools/prepare.py').write_text('synthetic preparation identity')
        (self.root / 'patches').mkdir()
        self.first = self.make_patch('0001-first.patch', 'one', 'two')
        self.second = self.make_patch('0003-second.patch', 'two', 'three')
        self.destination = self.root / 'build/source'

    def make_patch(self, name, before, after):
        target = self.root / 'patches' / name
        target.write_text(f'--- a/src/common/value.txt\n+++ b/src/common/value.txt\n@@ -1 +1 @@\n-{before}\n+{after}\n')
        return target

    def stage(self):
        with contextlib.redirect_stdout(io.StringIO()):
            prepare.prepare(self.destination, self.root, self.upstream, self.revision)

    def test_order_idempotence_and_dirty_upstream_preserved(self):
        (self.upstream / 'src/common/value.txt').write_text('private local edit\n')
        self.stage()
        value = self.destination / 'src/common/value.txt'
        self.assertEqual(value.read_text(), 'three\n')
        marker = self.destination / '.prepared'
        before = marker.stat().st_mtime_ns
        value.write_text('local generated edit\n')
        self.stage()
        self.assertEqual(marker.stat().st_mtime_ns, before)
        self.assertEqual(value.read_text(), 'local generated edit\n')
        self.assertEqual((self.upstream / 'src/common/value.txt').read_text(), 'private local edit\n')

    def test_removed_patch_preserves_source(self):
        self.stage()
        self.second.unlink()
        with self.assertRaisesRegex(ValueError, 'Preserve local edits'):
            self.stage()
        self.assertEqual((self.destination / 'src/common/value.txt').read_text(), 'three\n')

    def test_changed_patch_preserves_source(self):
        self.stage()
        self.first.write_text(self.first.read_text() + '\n')
        with self.assertRaisesRegex(ValueError, 'inputs changed'):
            self.stage()
        self.assertTrue((self.destination / '.prepared').is_file())

    def test_duplicate_patch_prefix_is_rejected(self):
        self.make_patch('0001-duplicate.patch', 'two', 'three')
        with self.assertRaisesRegex(ValueError, 'unique four-digit'):
            self.stage()
        self.assertFalse(self.destination.exists())

    def test_gitlink_mismatch_is_rejected(self):
        git(self.root, 'update-index', '--cacheinfo', '160000,' + '1' * 40 + ',vendor/cloudpilot')
        with self.assertRaisesRegex(ValueError, 'gitlink disagrees'):
            self.stage()

    def test_unmarked_directory_and_symlink_are_preserved(self):
        self.destination.mkdir(parents=True)
        saved = self.destination / 'user.session'
        saved.write_text('private marker')
        with self.assertRaises(ValueError):
            self.stage()
        self.assertEqual(saved.read_text(), 'private marker')
        link = self.root / 'source-link'
        link.symlink_to(self.destination)
        with self.assertRaises(ValueError):
            prepare.prepare(link, self.root, self.upstream, self.revision)
        self.assertEqual(saved.read_text(), 'private marker')

    def test_patch_failure_does_not_publish_partial_source(self):
        self.second.write_text('invalid patch\n')
        with self.assertRaises(subprocess.CalledProcessError):
            self.stage()
        self.assertFalse(self.destination.exists())
        self.assertEqual(list(self.destination.parent.glob('.frond-prepare-*')), [])


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
            for name in ('out', 'packaging', 'doc', 'vendor/cloudpilot/src/argparse',
                         'vendor/cloudpilot/src/common/zip'):
                (root / name).mkdir(parents=True)
            (root / 'VERSION').write_text('0.2.0\n')
            (root / 'packaging/debian-revision').write_text('1\n')
            for name in ('LICENSE', 'packaging/org.frond.Frond.desktop', 'packaging/frond.svg',
                         'packaging/frond-mime.xml', 'packaging/copyright', 'doc/desktop.md',
                         'doc/peripherals.md', 'vendor/cloudpilot/src/argparse/LICENSE',
                         'vendor/cloudpilot/src/common/zip/UNLICENSE'):
                (root / name).write_text('Synthetic fixture, not a Frond release.\n')
            source = root / 'dummy.c'
            source.write_text('#include <stdio.h>\n#include <slirp/libslirp.h>\n'
                              'int main(int argc, char **argv) {\n'
                              'if (argc > 3) puts(slirp_version_string());\n'
                              'puts("Synthetic-fixture 0.2.0"); return 0; }\n')
            flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'slirp'], text=True).split()
            subprocess.run(['cc', str(source), '-o', str(root / 'out/frond'), *flags], check=True)
            shutil.copyfile(root / 'out/frond', root / 'out/frond-desktop')
            (root / 'out/frond-desktop').chmod(0o755)
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
