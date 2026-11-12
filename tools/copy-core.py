#!/usr/bin/env python3
"""Copy core/ to another folder for a build that must not share core/'s objects.

Copies the tracked and untracked (not ignored) files of the working tree, so build output in
core/ is left behind. Running it again updates only the files that changed and removes the
ones that are gone, so objects already built in the copy are kept."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
STAMP = '.frond-core-copy'


def core_files(root=ROOT):
    listed = subprocess.check_output(['git', '-C', str(root), 'ls-files', '-z', '-co', '--exclude-standard',
                                      '--', 'core'])
    files = sorted({name for name in listed.decode().split('\0') if name})
    # ls-files lists tracked files deleted in the working tree too.
    return [name[len('core/'):] for name in files if (root / name).is_file() or (root / name).is_symlink()]


def copy(destination, root=ROOT):
    stamp = destination / STAMP
    if (destination.exists() or destination.is_symlink()) and not stamp.is_file():
        raise ValueError(f'{destination} exists and is not a copy of core/ (an older prepared tree?); '
                         'leaving it alone, remove it to copy afresh')
    old = set(stamp.read_text().splitlines()) if stamp.is_file() else set()
    files = core_files(root)
    destination.mkdir(parents=True, exist_ok=True)
    for name in files:
        source, target = root / 'core' / name, destination / name
        if source.is_symlink():
            if target.is_symlink() and os.readlink(target) == os.readlink(source):
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists() or target.is_symlink():
                target.unlink()
            target.symlink_to(os.readlink(source))
            continue
        data = source.read_bytes()
        if target.is_file() and not target.is_symlink() and target.read_bytes() == data:
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.is_symlink():
            target.unlink()
        # A fresh write gives changed files a new time, so make rebuilds them.
        target.write_bytes(data)
        shutil.copymode(source, target)
    for name in sorted(old - set(files)):
        gone = destination / name
        if gone.is_file() or gone.is_symlink():
            gone.unlink()
    stamp.write_text(''.join(name + '\n' for name in files))
    print(f'Copied core/ to {destination}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path, help='folder to copy core/ to')
    args = parser.parse_args()
    try:
        copy(args.destination.absolute())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
