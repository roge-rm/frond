#!/usr/bin/env python3
"""frond-68k host commands while a Palm app is busy, on the private IIIx 3.1 ROM. The busy probe
runs 45 guest seconds without calling the event loop, writing its log database all the while. An
export sent then waits for the next event loop call and, finding none in 30 guest seconds, fails
with an error while the session runs on. A database list sent behind it waits until the probe is
idle, and a second export then holds the probe's "done" record. Install, export and launch then
run at once.

Needs m68k-palmos-gcc and the Palm OS 3.5 SDK (--sdk) for the probe, else it's skipped. --evidence
keeps the logs."""
import argparse
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

PROBE = ROOT / 'build/busy68k-probe/FrondBusy68k.prc'
HELLO = Path.home() / 'Projects/palm/hello/Hello.prc'


def build_probe(sdk):
    if not PROBE.is_file() and shutil.which('m68k-palmos-gcc') and (sdk / 'include/PalmOS.h').is_file():
        subprocess.run([sys.executable, 'tools/build-palm-probe.py', '--probe', 'busy68k', '--sdk', str(sdk)],
                       cwd=ROOT, check=True, capture_output=True)
    return PROBE.is_file()


def check(rom, root):
    session = root / 'launcher.frond'
    core = palm_boot.Core(['--rom', rom, '--device', 'palm-iiix'], root / 'boot.txt')
    try:
        palm_boot.iiix_setup(core, root)
        core.send(f'save-session "{session}"', 'wrote session')
    finally:
        core.close()
    core = palm_boot.Core([session, '--device', 'palm-iiix', '--speed', 'original'], root / 'log.txt')
    try:
        reply = core.send(f'install "{PROBE}"', 'install', 60)
        if 'successful' not in reply:
            raise AssertionError(f'probe install: {reply.strip()}')
        core.launch('Frond busy')
        started = time.monotonic()
        # Without a safe point the export runs in the middle of the probe's log writes; it can
        # take the core down, so try a few times.
        busy, written = root / 'busy.pdb', 0
        for attempt in range(10):
            try:
                reply = core.send(f'db-export-db FrondBusyLog "{busy}"', 'export', 90)
            except AssertionError:
                if core.process.poll() is not None:
                    tail = next((line.strip() for line in core.lines if 'ASSERTION' in line),
                                core.lines[-1].strip() if core.lines else '')
                    raise AssertionError(f'the core quit on export {attempt + 1} while the app was busy: {tail}')
                raise
            if 'written' not in reply:
                break
            written += 1
            busy.unlink(missing_ok=True)
        waited = time.monotonic() - started
        if 'export failed' not in reply or 'busy' not in reply:
            raise AssertionError(f'{written} exports while the app was busy, then: {reply.strip()}')
        if busy.exists():
            raise AssertionError('busy export wrote a file')
        listed = core.send('db-list', 'db-list:', 90).strip()
        if 'RAM databases' not in listed:
            raise AssertionError(f'db-list after the busy stretch: {listed}')
        done = root / 'done.pdb'
        reply = core.send(f'db-export-db FrondBusyLog "{done}"', 'export', 60)
        data = done.read_bytes() if done.is_file() else b''
        if 'written' not in reply or b'done\0' not in data:
            raise AssertionError(f'idle export: {reply.strip()}')
        records = int.from_bytes(data[76:78], 'big') if len(data) > 78 else 0
        note = ''
        if HELLO.is_file():
            reply = core.send(f'install "{HELLO}"', 'install', 30)
            if 'successful' not in reply:
                raise AssertionError(f'idle install: {reply.strip()}')
            note = ', Hello installed'
        reply = core.send(f'db-export "{root / "ram.zip"}"', 'export', 30)
        if 'written' not in reply or 'FrondBusyLog.pdb' not in zipfile.ZipFile(root / 'ram.zip').namelist():
            raise AssertionError(f'idle db-export: {reply.strip()}')
        reply = core.send('launch "Frond busy"', 'launch', 30)
        if 'started' not in reply:
            raise AssertionError(f'idle launch: {reply.strip()}')
        if core.process.poll() is not None:
            raise AssertionError('the core quit')
        return f'busy export refused after {waited:.0f} s, then {records} log records exported{note}'
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
        print('frond-68k busy: skipped, no IIIx ROM')
        return 0
    if not build_probe(args.sdk.expanduser()):
        print('frond-68k busy: skipped, no busy probe (needs m68k-palmos-gcc and --sdk)')
        return 0
    with tempfile.TemporaryDirectory(prefix='frond-68k-busy-') as work:
        root = args.evidence or Path(work)
        root.mkdir(parents=True, exist_ok=True)
        try:
            print(f'frond-68k busy: ok, {check(rom, root)}')
            return 0
        except AssertionError as error:
            print(f'frond-68k busy: FAIL, {error}')
            return 1


if __name__ == '__main__':
    sys.exit(main())
