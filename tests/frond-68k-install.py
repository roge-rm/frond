#!/usr/bin/env python3
"""frond-68k installs on the private IIIx 3.1 ROM, as after a HotSync install. An app installed
while the launcher shows appears in it at once, matching the launcher reopened by hand, and gets
sysAppLaunchCmdSyncNotify. A PDB installed with Memo Pad in front notifies its creator app and
Memo Pad stays; with that app itself in front it gets no notify, and another app's install leaves
it in front. Two installs in a row at the launcher work, and an install while a launch is under
way doesn't keep the launcher.

The sync probe logs each notify and launch in FrondSyncLog; the busy probe is the other app.
Needs m68k-palmos-gcc and the Palm OS 3.5 SDK (--sdk) for them, else it's skipped. --evidence
keeps the logs and frames."""
import argparse
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

PROBE = ROOT / 'build/sync68k-probe/FrondSync68k.prc'
OTHER = ROOT / 'build/busy68k-probe/FrondBusy68k.prc'


def build_probes(sdk):
    for probe, prc in (('sync68k', PROBE), ('busy68k', OTHER)):
        if not prc.is_file() and shutil.which('m68k-palmos-gcc') and (sdk / 'include/PalmOS.h').is_file():
            subprocess.run([sys.executable, 'tools/build-palm-probe.py', '--probe', probe, '--sdk', str(sdk)],
                           cwd=ROOT, check=True, capture_output=True)
    return PROBE.is_file() and OTHER.is_file()


def data_pdb(path):
    """FrondSyncData, a one-record PDB with the probe's creator."""
    header = bytearray(78)
    header[0:13] = b'FrondSyncData'
    header[60:64], header[64:68] = b'DATA', b'FrSy'
    header[76:78] = (1).to_bytes(2, 'big')
    entry = (78 + 8 + 2).to_bytes(4, 'big') + bytes(4)
    path.write_bytes(bytes(header) + entry + bytes(2) + b'data\0')
    return path


def log(core, root, name):
    """FrondSyncLog's records as text, oldest first."""
    path = root / f'{name}.pdb'
    reply = core.send(f'db-export-db FrondSyncLog "{path}"', 'export', 60)
    if 'written' not in reply:
        raise AssertionError(f'FrondSyncLog export: {reply.strip()}')
    data = path.read_bytes()
    count = int.from_bytes(data[76:78], 'big')
    offsets = [int.from_bytes(data[78 + 8 * i:82 + 8 * i], 'big') for i in range(count)] + [len(data)]
    return [data[offsets[i]:offsets[i + 1]].split(b'\0')[0].decode() for i in range(count)]


def expect_log(core, root, name, wanted):
    got = log(core, root, name)
    if got != wanted:
        raise AssertionError(f'{name}: FrondSyncLog {got}, expected {wanted}')


def install(core, path=PROBE):
    reply = core.send(f'install "{path}"', 'install', 60)
    if 'successful' not in reply:
        raise AssertionError(f'install {path.name}: {reply.strip()}')


def body(rows):
    """A frame below the title bar, which holds the launcher's clock."""
    return rows[14:] if rows else rows


def settle(core, path, wanted, seconds=10):
    """Frames until one matches wanted(rows), else the last one."""
    end = time.monotonic() + seconds
    while True:
        rows = core.frame(path)
        if wanted(rows) or time.monotonic() > end:
            return rows
        time.sleep(0.5)


def stays(core, root, name, before):
    """The screen holds still for 3 s after an install, so the app in front wasn't relaunched."""
    time.sleep(3)
    if core.frame(root / f'{name}.png') != before:
        raise AssertionError(f'{name}: the screen changed')


def check(rom, root):
    session = root / 'launcher.frond'
    core = palm_boot.Core(['--rom', rom, '--device', 'palm-iiix'], root / 'boot.txt')
    try:
        palm_boot.iiix_setup(core, root)
        core.send(f'save-session "{session}"', 'wrote session')
    finally:
        core.close()
    pdb = data_pdb(root / 'FrondSyncData.pdb')
    core = palm_boot.Core([session, '--device', 'palm-iiix'], root / 'log.txt')
    try:
        # A new app at the launcher: listed at once, as the launcher reopened by hand.
        before = core.wait_frame(root / 'before.png', palm_boot.iiix_launcher, 60)
        install(core)
        after = settle(core, root / 'after-install.png',
                       lambda rows: body(rows) != body(before) and palm_boot.iiix_launcher(rows))
        if body(after) == body(before):
            raise AssertionError('the launcher did not change after the install')
        # Let the launcher finish drawing.
        time.sleep(2)
        after = core.frame(root / 'after-install.png')
        expect_log(core, root, 'at-launcher', ['sync'])
        core.launch('Memo Pad')
        settle(core, root / 'memo.png', lambda rows: not palm_boot.iiix_launcher(rows))
        core.launch('Launcher')
        reopened = settle(core, root / 'reopened.png', lambda rows: body(rows) == body(after))
        if body(reopened) != body(after):
            raise AssertionError('the launcher after the install differs from the launcher reopened')

        # A PDB with Memo Pad in front: the probe gets the notify, Memo Pad stays.
        core.launch('Memo Pad')
        memo = settle(core, root / 'memo-2.png', lambda rows: not palm_boot.iiix_launcher(rows))
        time.sleep(1)
        memo = core.frame(root / 'memo-2.png')
        install(core, pdb)
        stays(core, root, 'memo-after-pdb', memo)
        expect_log(core, root, 'pdb-memo', ['sync', 'sync'])

        # The probe in front: its own PDB brings no notify, and another app's install leaves it
        # there (a relaunch would log "launch" again).
        core.launch('Frond sync')
        probe = settle(core, root / 'probe.png', lambda rows: not palm_boot.iiix_launcher(rows))
        time.sleep(1)
        probe = core.frame(root / 'probe.png')
        install(core, pdb)
        stays(core, root, 'probe-after-pdb', probe)
        install(core, OTHER)
        stays(core, root, 'probe-after-app', probe)
        expect_log(core, root, 'probe-front', ['sync', 'sync', 'launch'])

        # Two installs in a row at the launcher.
        core.launch('Launcher')
        settle(core, root / 'reopened-2.png', lambda rows: palm_boot.iiix_launcher(rows))
        install(core)
        install(core)
        expect_log(core, root, 'two-installs', ['sync', 'sync', 'launch', 'sync', 'sync'])
        twice = settle(core, root / 'after-two-installs.png', palm_boot.iiix_launcher)
        if not palm_boot.iiix_launcher(twice):
            raise AssertionError('two installs in a row left no launcher')

        # A launch already under way wins over the relaunch.
        core.send('launch "Memo Pad"')
        install(core)
        moved = settle(core, root / 'launch-pending.png', lambda rows: body(rows) == body(memo))
        if body(moved) != body(memo):
            raise AssertionError('an install during a launch kept the launcher in front')
        if core.process.poll() is not None:
            raise AssertionError('the core quit')
        return ('launcher lists the app at once; Memo Pad and the probe stay in front; '
                'PDB notifies only the app not in front; a pending launch wins')
    finally:
        core.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path)
    parser.add_argument('--sdk', type=Path, default=ROOT.parent / 'palm-os-sdk/sdk-3.5')
    parser.add_argument('--evidence', type=Path, help='keep logs and frames here')
    args = parser.parse_args()
    rom = args.rom or palm_boot.palm_images() / 'Palm/IIIx/Palm-IIIx-3.1-en.rom'
    if not rom.is_file():
        print('frond-68k install: skipped, no IIIx ROM')
        return 0
    if not build_probes(args.sdk.expanduser()):
        print('frond-68k install: skipped, no probes (needs m68k-palmos-gcc and --sdk)')
        return 0
    with tempfile.TemporaryDirectory(prefix='frond-68k-install-') as work:
        root = args.evidence or Path(work)
        root.mkdir(parents=True, exist_ok=True)
        try:
            print(f'frond-68k install: ok, {check(rom, root)}')
            return 0
        except AssertionError as error:
            print(f'frond-68k install: FAIL, {error}')
            return 1


if __name__ == '__main__':
    sys.exit(main())
