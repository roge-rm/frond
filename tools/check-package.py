#!/usr/bin/env python3
"""Check a locally built Debian artifact without installing it."""
import argparse
from pathlib import Path
import subprocess

from package_policy import check_deb

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('package', type=Path)
args = parser.parse_args()
version = (ROOT / 'VERSION').read_text().strip() + '-' + (ROOT / 'packaging/debian-revision').read_text().strip()
architecture = subprocess.check_output(['dpkg', '--print-architecture'], text=True).strip()
try:
    check_deb(args.package, version, architecture)
except (ValueError, OSError, subprocess.CalledProcessError) as error:
    parser.exit(1, f'{error}\n')
print('Package metadata, runtime dependencies, ownership and file inventory pass')
