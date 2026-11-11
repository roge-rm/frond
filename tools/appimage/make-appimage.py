#!/usr/bin/env python3
"""Pack a staged Frond AppDir into out/Frond-VERSION-ARCH.AppImage with appimagetool.

appimagetool and the AppImage runtimes are pinned, checked against their SHA-256 and fetched once
into build/appimage/downloads.
"""

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
DOWNLOADS = ROOT / 'build/appimage/downloads'
TOOL = ('https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage',
        'ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0')
RUNTIMES = {
    'x86_64': ('https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64',
               '2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d'),
    'aarch64': ('https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-aarch64',
                '00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444'),
}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fetch(url, digest):
    path = DOWNLOADS / url.rsplit('/', 1)[1]
    if path.is_file() and sha256(path) == digest:
        return path
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    partial = path.with_name(path.name + '.part')
    with urllib.request.urlopen(url) as response, partial.open('wb') as output:
        output.write(response.read())
    got = sha256(partial)
    if got != digest:
        partial.unlink()
        raise SystemExit(f'{path.name}: expected SHA-256 {digest}, got {got}')
    partial.replace(path)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', required=True, choices=sorted(RUNTIMES))
    args = parser.parse_args()
    version = (ROOT / 'VERSION').read_text().strip()
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise SystemExit('VERSION must contain a release version such as 0.2.0')
    appdir = ROOT / f'build/appimage/{args.arch}/Frond.AppDir'
    if not (appdir / 'AppRun').is_file():
        raise SystemExit(f'No staged AppDir at {appdir}')
    tool = fetch(*TOOL)
    tool.chmod(0o755)
    runtime = fetch(*RUNTIMES[args.arch])
    output = ROOT / f'out/Frond-{version}-{args.arch}.AppImage'
    output.parent.mkdir(exist_ok=True)
    partial = output.with_name(output.name + '.new')
    partial.unlink(missing_ok=True)
    epoch = os.environ.get('SOURCE_DATE_EPOCH') or subprocess.check_output(
        ['git', '-C', str(ROOT), 'log', '-1', '--format=%ct'], text=True).strip()
    # Extract and run instead of mounting, so building doesn't need FUSE.
    env = dict(os.environ, ARCH=args.arch, APPIMAGE_EXTRACT_AND_RUN='1', SOURCE_DATE_EPOCH=epoch)
    subprocess.run([str(tool), '--no-appstream', '--runtime-file', str(runtime), str(appdir), str(partial)],
                   env=env, check=True)
    partial.replace(output)
    print(output)


if __name__ == '__main__':
    main()
