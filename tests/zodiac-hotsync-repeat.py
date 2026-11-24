#!/usr/bin/env python3
"""HotSync a Zodiac session over the Sync Manager's USB route, pressing again as soon as each sync is done.

SESSION is a private Zodiac session with HotSync open in the cradle. --sync-folder is copied first, so a
folder the session has already synced with makes each sync a fast one. Checks sync-start from USB off, a
`hotsync` press right after a finished sync and a sync-start right after one.
"""
import argparse
from pathlib import Path
import runpy
import shutil
import time

ROOT = Path(__file__).resolve().parents[1]


def wait_sync(client, seconds):
    """The sync manager's next done or failed event."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        client.request('usb-state')
        for name, value in client.events:
            if name == 'sync' and value.split(' ', 1)[0] in ('done', 'failed'):
                return value
        time.sleep(0.05)
    raise AssertionError(f'no sync ended in {seconds} s: {client.events[-6:]}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    parser.add_argument('evidence', type=Path)
    parser.add_argument('--sync-folder', type=Path)
    parser.add_argument('--engine', default='fast', choices=('exact', 'fast'))
    parser.add_argument('--seconds', type=float, default=300, help='longest wait for one sync')
    args = parser.parse_args()
    evidence = args.evidence.resolve()
    evidence.mkdir(mode=0o700)
    session = evidence / 'session.frond'
    shutil.copy(args.session, session)
    folder = evidence / 'sync'
    if args.sync_folder:
        shutil.copytree(args.sync_folder, folder)

    client_type = runpy.run_path(str(ROOT / 'tests/core-control.py'))['Client']
    client = client_type(ROOT / 'out/frond', [session, '--device', 'tapwave-zodiac-2', '--engine', args.engine,
                                              '--sync-folder', folder], evidence, 'core')
    try:
        time.sleep(2)
        assert client.ok('cradle-state') == 'attached'
        assert client.ok('usb-state') == 'off'
        # The Zodiac's USB only comes up once HotSync runs, so the press can't wait for it.
        client.events.clear()
        client.ok('sync-start', 'local', code='sync')
        result = wait_sync(client, args.seconds)
        assert result.startswith('done usb'), result
        print(f'PASS sync-start from USB off: {result}')
        for command, argument, code in (('hotsync', '', 'hotsync'), ('sync-start', 'local', 'sync')):
            client.events.clear()
            client.ok(command, argument, code=code)
            result = wait_sync(client, args.seconds)
            assert result.startswith('done usb'), f'{command} right after a sync: {result}'
            print(f'PASS {command} right after a sync: {result}')
        client.quit()
    finally:
        client.close()


if __name__ == '__main__':
    main()
