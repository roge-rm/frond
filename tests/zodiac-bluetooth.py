#!/usr/bin/env python3
"""The Zodiac's Bluetooth on the real ROM (ZODIAC_ROM, ZODIAC_STATE): the radio turned on in Preferences
against a private btvirt, then FrondBluetoothProbe sends its payload to bluetooth-opp-peer over OBEX. Skipped
without the ROM, the state or the peers."""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAYLOAD_SHA256 = 'f3d193f0b904a2bbaaa972f2ef80bdeb19266e8340dbcb3626c26f2f98c4327c'
# Preferences' panel list to Bluetooth, Radio On; the probe's Send, the peer in Discovery Results, OK.
STEPS = ('tap 220 15 1500; tap 250 15 1500; tap 213 56 1500; tap 200 76 4000; shot radio; install {prc}; '
         'launch Frond Bluetooth exchange; tap 70 136 15000; wait 6000; shot found; tap 22 107 1000; '
         'tap 50 294 15000; shot sent')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test', type=Path, default=ROOT / 'build/test-zodiac-bluetooth')
    parser.add_argument('--btvirt', type=Path, default=ROOT / 'build/bluetooth-provider/emulator/btvirt')
    parser.add_argument('--peer', type=Path, default=ROOT / 'build/bluetooth-opp-peer/bluetooth-opp-peer')
    parser.add_argument('--probe', type=Path, default=ROOT / 'build/bluetooth-probe/FrondBluetoothProbe.prc')
    parser.add_argument('--fast', choices=['0', '1'], default='1')
    args = parser.parse_args()
    if not os.environ.get('ZODIAC_ROM') or not os.environ.get('ZODIAC_STATE'):
        print('SKIP: set ZODIAC_ROM and ZODIAC_STATE')
        return 0
    for path in (args.btvirt, args.peer, args.probe):
        if not path.exists():
            print(f'SKIP: {path} is missing')
            return 0
    with tempfile.TemporaryDirectory(prefix='zodiac-bt-', dir=ROOT / 'build') as folder:
        folder = Path(folder)
        folder.chmod(0o700)
        received = folder / 'received'
        radio = subprocess.Popen([str(args.btvirt), f'-s{folder}'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        processes = [radio]
        try:
            socket = folder / 'bt-server-bredr'
            for _ in range(50):
                if socket.exists():
                    break
                time.sleep(0.1)
            peer = subprocess.Popen([str(args.peer), str(socket), str(received)], stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True)
            processes.append(peer)
            time.sleep(1)
            env = dict(os.environ, BT=str(socket), ZODIAC_TEST_FAST=args.fast,
                       STEPS=STEPS.format(prc=args.probe.resolve()))
            run = subprocess.run([str(args.test), 'steps', str(folder)], env=env, capture_output=True, text=True,
                                 timeout=1200)
            sys.stdout.write(run.stdout)
            if run.returncode:
                sys.stdout.write(run.stderr)
                print(f'FAIL: the machine run ended with {run.returncode}')
                return 1
        finally:
            for process in reversed(processes):
                process.terminate()
                process.wait(timeout=10)
        if not received.exists():
            print('FAIL: the peer received nothing')
            return 1
        digest = hashlib.sha256(received.read_bytes()).hexdigest()
        if digest != PAYLOAD_SHA256:
            print(f'FAIL: payload {digest}')
            return 1
    print(f'zodiac bluetooth OPP send ({"Fast" if args.fast == "1" else "Exact"}): ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
