#!/usr/bin/env python3
"""ARM installs as after a HotSync install, on available ARM devices' smoke sessions. An app
installed while the launcher shows appears in it at once, matching the launcher reopened by hand,
and gets sysAppLaunchCmdSyncNotify. A PDB installed with Memo Pad in front notifies its creator app
and Memo Pad stays; Memo Pad waits for events with no timeout, and a RAM export, the database list
and a launch work with it in front too. With the creator app itself in front it gets no notify, and
another app's install leaves it in front. Two installs in a row at the launcher work, an event
the notified app queues (a switch to Memo Pad) isn't overridden by the relaunch, an install while a
launch is under way doesn't keep the launcher, and a session saved by the next console command after an install at
the launcher loads with the launcher listing the app.

Sessions: --session DEVICE=PATH, else FROND_ARM_SESSIONS/DEVICE (a folder of machine folders), else
build/smoke/machines/DEVICE; a device with none is skipped, printing SKIPPED, and with
FROND_REQUIRE_SESSIONS=1 any skip fails the run. --device picks devices. The sync probe
logs each notify and launch in FrondSyncLog; the busy probe is the other app. Needs m68k-palmos-gcc
and the Palm OS 3.5 SDK (--sdk) for them, else it's skipped. --evidence keeps the logs and frames."""
import argparse
import os
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

PROBE = ROOT / 'build/sync68k-probe/FrondSync68k.prc'
OTHER = ROOT / 'build/busy68k-probe/FrondBusy68k.prc'
# The console's install replies; the command's echo holds none of them.
REPLIES = ('installation', 'database corrupt', 'out of memory', 'db cannot', 'unable to', 'unknown error')
DEVICES = tuple(name for name in palm_boot.FRONTEND_DEVICES if name in palm_boot.DEVICES)


def build_probes(sdk):
    for probe, prc in (('sync68k', PROBE), ('busy68k', OTHER)):
        if not prc.is_file() and shutil.which('m68k-palmos-gcc') and (sdk / 'include/PalmOS.h').is_file():
            subprocess.run([sys.executable, 'tools/build-palm-probe.py', '--probe', probe, '--sdk', str(sdk)],
                           cwd=ROOT, check=True, capture_output=True)
    return PROBE.is_file() and OTHER.is_file()


def data_pdb(path, name=b'FrondSyncData', creator=b'FrSy'):
    """A one-record PDB, by default FrondSyncData with the probe's creator."""
    header = bytearray(78)
    header[0:len(name)] = name
    header[60:64], header[64:68] = b'DATA', creator
    header[76:78] = (1).to_bytes(2, 'big')
    entry = (78 + 8 + 2).to_bytes(4, 'big') + bytes(4)
    path.write_bytes(bytes(header) + entry + bytes(2) + b'data\0')
    return path


class Device:
    """How to get each launcher's main list in view and how to reopen it by hand."""

    def __init__(self, name):
        self.name = name
        self.profile = palm_boot.DEVICES[name]

    def body(self, rows):
        """A frame without the title bar's clock, and on the T|X its status bar's clock too."""
        if not rows:
            return rows
        return rows[26:-32] if len(rows) == 480 else rows[26:]

    def ready(self, core, root):
        """From the smoke session to the launcher's main list."""
        if self.name == 'palm-zire-72':
            # The smoke session's setup Reminder comes up over the launcher a few seconds in; OK
            # clears it.
            start, seen = time.monotonic(), False
            while time.monotonic() - start < 60:
                rows = core.frame(root / 'boot.png')
                if palm_boot.zire_reminder(rows):
                    core.tap(root, 50, 280)
                    seen = True
                elif palm_boot.launcher_wallpaper(rows, 40) and (seen or time.monotonic() - start > 20):
                    break
                time.sleep(1)
        elif self.name == 'palm-tx':
            core.wait_frame(root / 'boot.png', palm_boot.launcher_wallpaper, 60)
            # Favorites to Applications.
            time.sleep(2)
            core.tap(root, 140, 55)
        else:
            core.wait_frame(root / 'boot.png', palm_boot.launcher_check(self.name), 60)
        return self.still(core, root / 'before.png')

    def reopen(self, core, root):
        """Home by hand, as the user would."""
        if self.name == 'palm-tx':
            core.key(root, 'home')
            time.sleep(3)
            core.tap(root, 140, 55)
        elif self.profile['home_key']:
            core.key(root, 'home-key')
        elif self.profile['home_x'] >= 0:
            core.tap(root, self.profile['home_x'], self.profile['home_y'])
        else:
            raise AssertionError(f'{self.name}: no Home control in profile')

    def still(self, core, path, seconds=20):
        """A frame once two in a row agree."""
        end, last = time.monotonic() + seconds, None
        while True:
            rows = core.frame(path)
            if (rows is not None and self.body(rows) == self.body(last)) or time.monotonic() > end:
                return rows
            last = rows
            time.sleep(1)


def log(core, root, name):
    """FrondSyncLog's records as text, oldest first, from a RAM export."""
    path = root / f'{name}.zip'
    reply = core.send(f'db-export "{path}"', ('export written', 'export FAILED', 'not ready'), 120)
    if 'written' not in reply:
        raise AssertionError(f'export for FrondSyncLog: {reply.strip()}')
    with zipfile.ZipFile(path) as archive:
        entries = [n for n in archive.namelist() if n.startswith('FrondSyncLog.')]
        data = archive.read(entries[0]) if entries else b''
    path.unlink()
    if not data:
        return []
    count = int.from_bytes(data[76:78], 'big')
    offsets = [int.from_bytes(data[78 + 8 * i:82 + 8 * i], 'big') for i in range(count)] + [len(data)]
    return [data[offsets[i]:offsets[i + 1]].split(b'\0')[0].decode() for i in range(count)]


def expect_log(core, root, name, wanted):
    got = log(core, root, name)
    if got != wanted:
        raise AssertionError(f'{name}: FrondSyncLog {got}, expected {wanted}')


def install(core, path=PROBE):
    reply = core.send(f'install "{path}"', REPLIES, 60)
    if 'successful' not in reply:
        raise AssertionError(f'install {path.name}: {reply.strip()}')


def settle(core, path, wanted, seconds=15):
    """Frames until one matches wanted(rows), else the last one."""
    end = time.monotonic() + seconds
    while True:
        rows = core.frame(path)
        if wanted(rows) or time.monotonic() > end:
            return rows
        time.sleep(0.5)


def front(core, device, root, name, app):
    """Launches app from where the core is and returns its still frame."""
    core.launch(app)
    time.sleep(2)
    return device.still(core, root / f'{name}.png')


def stays(core, device, root, name, before):
    """The screen holds still for 3 s after an install, so the app in front wasn't relaunched."""
    time.sleep(3)
    if device.body(core.frame(root / f'{name}.png')) != device.body(before):
        raise AssertionError(f'{name}: the screen changed')


def copy_session(session, work):
    """A copy of the machine folder (or lone session file) to run from, kept out of the evidence."""
    session = Path(session)
    folder = session if session.is_dir() else session.parent
    target = work / 'machine'
    shutil.copytree(folder, target)
    frond = sorted(target.glob('*.frond'))
    return frond[0] if session.is_dir() else target / session.name


def saved_after_install(device, session, work, root, after):
    """Install and save-session in one write, so the save runs at the first chance after the
    install's safe point; the relaunch is already handed to Palm OS by then."""
    saved = work / 'saved' / 'saved.frond'
    saved.parent.mkdir()
    core = palm_boot.Core([session, '--device', device.name], root / 'save-log.txt')
    try:
        device.ready(core, root)
        mark = len(core.lines)
        core.send(f'install "{PROBE}"\nsave-session "{saved}"', 'wrote session', 120)
        if not any('installation successful' in line for line in core.lines[mark:]):
            raise AssertionError('install before the save: ' + ''.join(core.lines[mark:]).strip())
    finally:
        core.close()
    core = palm_boot.Core([saved, '--device', device.name], root / 'saved-log.txt')
    try:
        loaded = settle(core, root / 'saved-loaded.png', lambda rows: device.body(rows) == device.body(after), 40)
        if device.body(loaded) != device.body(after):
            raise AssertionError('the session saved right after the install lost the launcher relaunch')
        expect_log(core, root, 'saved', ['sync'])
    finally:
        core.close()


def asleep(core, device, root):
    """Host calls while the Palm sleeps (power key): each is answered, the core stays up, and after
    waking the guest takes host calls again and its screen is live."""
    core.key(root, 'power')
    end = time.monotonic() + 30
    while core.frame(root / 'asleep.png') is not None:
        if time.monotonic() > end:
            raise AssertionError('the power key did not put the Palm to sleep')
        time.sleep(1)
    replies = {}
    for name, command, wanted in (('install', f'install "{PROBE}"', REPLIES),
                                  ('db-list', 'db-list', ('FrondSyncLog', 'not ready', 'list failed')),
                                  ('launch', 'launch "Memo Pad"', ('launch', 'not ready', 'could not', 'failed'))):
        started = time.monotonic()
        replies[name] = core.send(command, wanted, 120).strip() if name != 'launch' else \
            (core.send(command), time.sleep(5), 'sent')[2]
        print(f'frond-arm install: {device.name} asleep {name}: {replies[name]!r} after {time.monotonic() - started:.1f} s', flush=True)
        if core.process.poll() is not None:
            raise AssertionError(f'the core quit on {name} while asleep')
    core.key(root, 'power')
    live = settle(core, root / 'woken.png', lambda rows: rows is not None, 30)
    if live is None:
        raise AssertionError('the Palm did not wake')
    time.sleep(3)
    install(core)
    reply = core.send('db-list', ('FrondSyncLog', 'not ready', 'list failed'), 60)
    if 'FrondSyncLog' not in reply:
        raise AssertionError(f'db-list after waking: {reply.strip()}')
    first, second = device.still(core, root / 'woken-1.png'), None
    time.sleep(2)
    second = core.frame(root / 'woken-2.png')
    if second is None:
        raise AssertionError('the screen went dark after waking')


def check(device, session, work, root):
    session = copy_session(session, work)
    pdb = data_pdb(root / 'FrondSyncData.pdb')
    core = palm_boot.Core([session, '--device', device.name], root / 'log.txt')
    try:
        # A new app at the launcher: listed at once, as the launcher reopened by hand.
        before = device.ready(core, root)
        install(core)
        settle(core, root / 'after-install.png', lambda rows: device.body(rows) != device.body(before))
        after = device.still(core, root / 'after-install.png')
        if device.body(after) == device.body(before):
            raise AssertionError('the launcher did not change after the install')
        expect_log(core, root, 'at-launcher', ['sync'])
        front(core, device, root, 'memo', 'Memo Pad')
        device.reopen(core, root)
        reopened = settle(core, root / 'reopened.png', lambda rows: device.body(rows) == device.body(after))
        if device.body(reopened) != device.body(after):
            raise AssertionError('the launcher after the install differs from the launcher reopened by hand')

        # A PDB with Memo Pad in front: the probe gets the notify, Memo Pad stays. Memo Pad waits
        # with no timeout, so each host call here wakes it first; the RAM export and the database
        # list leave it in front too.
        memo = front(core, device, root, 'memo-2', 'Memo Pad')
        install(core, pdb)
        logged = ['sync', 'sync']
        stays(core, device, root, 'memo-after-pdb', memo)
        expect_log(core, root, 'pdb-memo', logged)
        reply = core.send('db-list', ('FrondSyncLog', 'not ready', 'list failed'), 60)
        if 'FrondSyncLog' not in reply:
            raise AssertionError(f'db-list with Memo Pad in front: {reply.strip()}')
        stays(core, device, root, 'memo-after-export', memo)

        # The probe launched over Memo Pad: its own PDB brings no notify, and another app's install
        # leaves it there (a relaunch would log "launch" again).
        mark = len(core.lines)
        core.send('launch "Frond sync"')
        time.sleep(3)
        if any('launch' in line and ('could not' in line or 'failed' in line) for line in core.lines[mark:]):
            raise AssertionError('launch with Memo Pad in front: ' + ''.join(core.lines[mark:]).strip())
        probe = device.still(core, root / 'probe.png')
        if device.body(probe) == device.body(memo):
            raise AssertionError('launch with Memo Pad in front left Memo Pad')
        logged.append('launch')
        expect_log(core, root, 'launch-memo', logged)
        install(core, pdb)
        stays(core, device, root, 'probe-after-pdb', probe)
        install(core, OTHER)
        stays(core, device, root, 'probe-after-app', probe)
        expect_log(core, root, 'probe-front', logged)

        # Two installs in a row at the launcher, which now lists the busy probe too.
        device.reopen(core, root)
        time.sleep(2)
        listed = device.still(core, root / 'reopened-2.png')
        install(core)
        install(core)
        logged += ['sync', 'sync']
        expect_log(core, root, 'two-installs', logged)
        twice = settle(core, root / 'after-two-installs.png', lambda rows: device.body(rows) == device.body(listed))
        if device.body(twice) != device.body(listed):
            raise AssertionError('two installs in a row left no launcher listing the apps')

        # An event the notified app queued is handled first: with FrondSyncSwitch (no app has its
        # creator, so it brings no notify) installed, the probe's notify switches to Memo Pad,
        # queuing an appStopEvent for the launcher, and the launcher isn't relaunched over it.
        install(core, data_pdb(root / 'FrondSyncSwitch.pdb', b'FrondSyncSwitch', b'FrSw'))
        install(core)
        logged.append('sync')
        switched = settle(core, root / 'notify-switch.png', lambda rows: device.body(rows) == device.body(memo))
        if device.body(switched) != device.body(memo):
            raise AssertionError('the launcher was relaunched over the event the notified app queued')
        expect_log(core, root, 'notify-switch', logged)
        device.reopen(core, root)
        time.sleep(2)
        device.still(core, root / 'reopened-3.png')

        # A launch already under way wins over the relaunch.
        mark = len(core.lines)
        core.send(f'launch "Memo Pad"\ninstall "{PROBE}"', REPLIES, 60)
        moved = settle(core, root / 'launch-pending.png', lambda rows: device.body(rows) == device.body(memo))
        if device.body(moved) != device.body(memo):
            raise AssertionError('an install during a launch kept the launcher in front: ' +
                                 ''.join(core.lines[mark:]).strip())
        if core.process.poll() is not None:
            raise AssertionError('the core quit')
        if 'power' in device.profile['buttons'].split('|'):
            asleep(core, device, root)
        if any('sync notify or launcher relaunch failed' in line for line in core.lines):
            raise AssertionError('a sync notify or relaunch failed')
    finally:
        core.close()
    saved_after_install(device, session, work, root, after)


def sessions(args):
    given = dict(item.split('=', 1) for item in args.session)
    folder = os.environ.get('FROND_ARM_SESSIONS')
    found = {}
    for name in args.device or DEVICES:
        for path in ([given[name]] if name in given else []) + \
                ([Path(folder) / name] if folder else []) + [ROOT / 'build/smoke/machines' / name]:
            if Path(path).exists():
                found[name] = Path(path)
                break
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', action='append', default=[], metavar='DEVICE=PATH')
    parser.add_argument('--device', action='append', choices=DEVICES)
    parser.add_argument('--sdk', type=Path, default=ROOT.parent / 'palm-os-sdk/sdk-3.5')
    parser.add_argument('--evidence', type=Path, help='keep logs and frames here')
    args = parser.parse_args()
    found = sessions(args)
    required = os.environ.get('FROND_REQUIRE_SESSIONS') == '1'
    skips = []

    def skipped(reason):
        print(f'frond-arm install: SKIPPED, {reason}' + (' (FROND_REQUIRE_SESSIONS=1: failing)' if required else ''))
        return 1 if required else 0

    if not found:
        return skipped('no ARM smoke sessions')
    if not build_probes(args.sdk.expanduser()):
        return skipped('no probes (needs m68k-palmos-gcc and --sdk)')
    failed = 0
    for name in args.device or DEVICES:
        if name not in found:
            print(f'frond-arm install: {name} SKIPPED, no session')
            skips.append(name)
            continue
        with tempfile.TemporaryDirectory(prefix='frond-arm-install-') as work:
            work = Path(work)
            root = (args.evidence / name) if args.evidence else work / 'evidence'
            root.mkdir(parents=True)
            try:
                check(Device(name), found[name], work, root)
                print(f'frond-arm install: {name} ok, launcher lists the app at once as reopened by hand; '
                      'Memo Pad and the probe stay in front; PDB notifies only the app not in front; '
                      'install, export, list and launch with Memo Pad in front; '
                      'a pending launch and an event queued by the notified app win; a save right after the install keeps the relaunch', flush=True)
            except AssertionError as error:
                failed += 1
                print(f'frond-arm install: {name} FAIL, {error}', flush=True)
    return 1 if failed or (required and skips) else 0


if __name__ == '__main__':
    sys.exit(main())
