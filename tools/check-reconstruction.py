#!/usr/bin/env python3
"""Compare two independent reconstructions of the pinned core and ordered patches."""

import hashlib
from pathlib import Path
import tempfile

from prepare import ROOT, prepare


def inventory(root):
    return {str(path.relative_to(root)): (path.stat().st_mode & 0o777,
                                        hashlib.sha256(path.read_bytes()).hexdigest())
            for path in sorted(root.rglob('*')) if path.is_file()}


def main():
    with tempfile.TemporaryDirectory(prefix='frond-reconstruction-') as work:
        first, second = (Path(work) / name for name in ('first', 'second'))
        prepare(first)
        prepare(second)
        a, b = inventory(first), inventory(second)
        if a != b:
            raise SystemExit('Independent source reconstructions differ')
        print(f'Independent source reconstructions match ({len(a)} files)')
        existing = ROOT / 'build/source'
        if existing.exists():
            changed = [name for name in a if not (existing / name).is_file()
                       or inventory_entry(existing / name) != a[name]]
            if changed:
                raise SystemExit('Current prepared source differs: ' + ', '.join(changed))
            print('Current prepared source matches all reconstructed inputs')


def inventory_entry(path):
    return path.stat().st_mode & 0o777, hashlib.sha256(path.read_bytes()).hexdigest()


if __name__ == '__main__':
    main()
