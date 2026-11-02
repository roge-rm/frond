#!/usr/bin/env python3
"""Build the optional virtual OPP test peer from the pinned provider's test host."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REVISION = "82667d713949447d7c94725432c61b33689ae68a"


def run(*args, cwd=None):
    subprocess.run(list(map(str, args)), cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'build/bluetooth-provider')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/bluetooth-opp-peer')
    parser.add_argument('--test', action='store_true')
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip()
    changes = subprocess.check_output(['git', 'diff', '--no-ext-diff', '--no-color', 'HEAD'], cwd=source, text=True)
    if revision != REVISION or changes != (ROOT / 'tools/bluez-bdaddr.patch').read_text():
        parser.error('provider source must match the pinned revision and qualified extension')
    expected = subprocess.check_output(['git', 'show', REVISION + ':emulator/bthost.c'], cwd=source)
    original = (source / 'emulator/bthost.c').read_bytes()
    if original != expected:
        parser.error('bthost.c must match the pinned upstream source')
    output.mkdir(parents=True, exist_ok=True)
    stage = output / 'emulator'
    stage.mkdir(exist_ok=True)
    (stage / 'bthost.c').write_bytes(original)
    run('patch', '--batch', '-p1', '-i', ROOT / 'tools/bluez-bthost-config.patch', cwd=output)
    patched = output / 'frond-bthost.c'
    shutil.copyfile(stage / 'bthost.c', patched)
    flags = ['-std=gnu11', '-O2', '-g', '-DHAVE_CONFIG_H', '-I' + str(source),
             '-I' + str(source / 'lib'), '-I' + str(source / 'emulator'), '-I' + str(output)]
    objects = [source / 'emulator/smp.o', source / 'src/.libs/libshared-mainloop.a',
               source / 'lib/.libs/libbluetooth-internal.a']
    run('cc', *flags, '-c', patched, '-o', output / 'bthost.o')
    run('cc', *flags, ROOT / 'tools/bluetooth-opp-peer.c', output / 'bthost.o',
        *objects, '-o', output / 'bluetooth-opp-peer')
    if args.test:
        run('cc', *flags, ROOT / 'tests/bluetooth-bthost-config.c', *objects,
            '-o', output / 'config-test')
        run(output / 'config-test')
    print('Built peer:', output / 'bluetooth-opp-peer')
    print('Provider source and executable unchanged; upstream bthost SHA256:', hashlib.sha256(original).hexdigest())


if __name__ == '__main__':
    main()
