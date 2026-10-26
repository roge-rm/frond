#!/usr/bin/env python3
"""Reconstruct the pinned native core without changing upstream or existing source."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / 'vendor/cloudpilot'
REVISION = '064d5d578bcd1f9072de384d05aa217f83db2f7f'
SOURCE_PATHS = ('src/common', 'src/uarm', 'src/argparse', 'src/Makefile.common', 'LICENSE')


def inputs(root=ROOT, upstream=UPSTREAM, revision=REVISION):
    actual = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != revision:
        raise ValueError(f'Expected upstream {revision}, got {actual}; restore the pinned submodule')
    gitlink = subprocess.check_output(['git', '-C', str(root), 'ls-files', '--stage',
                                       'vendor/cloudpilot'], text=True).split()
    if gitlink[:2] != ['160000', revision]:
        raise ValueError('vendor/cloudpilot gitlink disagrees with tools/prepare.py revision')
    patches = sorted((root / 'patches').glob('*.patch'))
    numbers = [p.name[:4] for p in patches]
    if not patches or any(not re.fullmatch(r'\d{4}-.+\.patch', p.name) for p in patches) or len(set(numbers)) != len(numbers):
        raise ValueError('Patches need unique four-digit prefixes; gaps are allowed')
    return {'upstream': revision,
            'prepare_sha256': hashlib.sha256((root / 'tools/prepare.py').read_bytes()).hexdigest(),
            'patches': [{'name': p.name, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                        for p in patches]}


def prepare(destination, root=ROOT, upstream=UPSTREAM, revision=REVISION):
    manifest = inputs(root, upstream, revision)
    stamp = json.dumps(manifest, indent=2) + '\n'
    # Never remove or replace a pre-existing tree, including one with a legacy stamp.
    if destination.exists() or destination.is_symlink():
        marker = destination / '.prepared'
        if not destination.is_symlink() and marker.is_file() and marker.read_text() == stamp:
            return
        raise ValueError(f'Prepared inputs changed or source is unmarked: {destination}. '
                         'Preserve local edits and use a fresh worktree for reconstruction.')
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Build beside the destination, then rename only a complete tree into place.
    with tempfile.TemporaryDirectory(prefix='.frond-prepare-', dir=destination.parent) as work:
        workspace = Path(work)
        staged = workspace / 'source'
        # A separate Git directory prevents git apply from discovering the parent checkout.
        subprocess.run(['git', 'init', '--quiet', str(workspace)], check=True)
        with tempfile.TemporaryFile() as archive:
            subprocess.run(['git', '-C', str(upstream), 'archive', revision, *SOURCE_PATHS],
                           stdout=archive, check=True)
            archive.seek(0)
            with tarfile.open(fileobj=archive) as sources:
                sources.extractall(staged, filter='data')
        for patch in manifest['patches']:
            subprocess.run(['git', 'apply', '--directory=source', str(root / 'patches' / patch['name'])],
                           cwd=workspace, check=True)
        (staged / '.prepared').write_text(stamp)
        staged.rename(destination)
    print(f'Prepared native core from {revision}: {destination}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=Path, default=ROOT / 'build/source',
                        help='new destination; existing trees are preserved')
    args = parser.parse_args()
    try:
        prepare(args.destination.absolute())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
