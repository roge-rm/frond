#!/usr/bin/env python3
"""Build the virtual Bluetooth peer (OPP and SPP test peer, and the DUN phone) from the pinned provider's test host."""
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
    # The DUN phone's modem, PPP and slirp: libcommon.a and libcommon-net.a from the core.
    parser.add_argument('--common', type=Path, default=ROOT / 'core/src/common')
    parser.add_argument('--test', action='store_true')
    args = parser.parse_args()
    source, output, common = args.source.resolve(), args.output.resolve(), args.common.resolve()
    for library in ('libcommon.a', 'libcommon-net.a'):
        if not (common / library).is_file():
            parser.error(f'{common / library} is missing (make -C core/src/common bin net)')
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
    slirp = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'slirp'], text=True).split()
    run('c++', '-std=c++17', '-O2', '-g', '-I' + str(common), *slirp, '-c', ROOT / 'tools/bluetooth-dun.cpp',
        '-o', output / 'bluetooth-dun.o')
    run('cc', *flags, '-I' + str(ROOT / 'tools'), '-c', ROOT / 'tools/bluetooth-opp-peer.c', '-o', output / 'peer.o')
    run('c++', output / 'peer.o', output / 'bluetooth-dun.o', output / 'bthost.o', *objects,
        common / 'libcommon-net.a', common / 'libcommon.a', *slirp, '-o', output / 'bluetooth-opp-peer')
    if args.test:
        run('cc', *flags, ROOT / 'tests/bluetooth-bthost-config.c', *objects,
            '-o', output / 'config-test')
        run(output / 'config-test')
        run(output / 'bluetooth-opp-peer', '--selftest-dun')
    print('Built peer:', output / 'bluetooth-opp-peer')
    print('Provider source and executable unchanged; upstream bthost SHA256:', hashlib.sha256(original).hexdigest())


if __name__ == '__main__':
    main()
