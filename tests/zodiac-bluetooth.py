#!/usr/bin/env python3
"""The Zodiac's Bluetooth on the real ROM (ZODIAC_ROM, ZODIAC_STATE): the radio turned on in Preferences
against a private btvirt, then FrondBluetoothProbe sends its payload to bluetooth-opp-peer over OBEX. Then the
other way: the peer pushes to the idle Zodiac, holding after 64 bytes, and the rest comes in while the test makes
guest calls (database lists), which the Zodiac's received bytes must not upset. Skipped without the ROM, the
state or the peers."""
import argparse
import hashlib
import os
import re
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
# Radio on, the probe launched (it registers its type) and left open, idle. Then the peer pushes, held after 64
# bytes until "go", and guest calls run while the rest comes in. Not in the default run: the ROM unloads BtlsLib
# once Preferences closes, so the idle Zodiac's radio is off and the peer's page goes unanswered.
RECEIVE_STEPS = ('tap 220 15 1500; tap 250 15 1500; tap 213 56 1500; tap 200 76 4000; install {prc}; '
                 'launch Frond Bluetooth exchange; wait 3000; signal ready; wait 25000; shot held; signal go; '
                 'calls 40; wait 8000; shot received; export FrondBTReceived')


def start_radio(args, folder):
    radio = subprocess.Popen([str(args.btvirt), f'-s{folder}'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    socket = folder / 'bt-server-bredr'
    for _ in range(50):
        if socket.exists():
            break
        time.sleep(0.1)
    return radio, socket


def stop(processes):
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
        process.wait(timeout=10)


# The Zodiac's BD_ADDR in wire order, from its Read_BD_ADDR answer in the H4 log.
def zodiac_address(log):
    match = re.search(r'zodiac bt < 04 0e 0a 01 09 10 00((?: [0-9a-f]{2}){6})', log.read_text(errors='replace'))
    return match.group(1).replace(' ', '') if match else None


def receive(args, engine):
    with tempfile.TemporaryDirectory(prefix='zodiac-bt-', dir=ROOT / 'build') as folder:
        folder = Path(args.keep or folder).resolve()
        folder.mkdir(mode=0o700, parents=True, exist_ok=True)
        folder.chmod(0o700)
        received = folder / 'received'
        radio, socket = start_radio(args, folder)
        processes = [radio]
        try:
            env = dict(os.environ, BT=str(socket), ZODIAC_TEST_FAST=args.fast, FROND_ZODIAC_BT_LOG='1',
                       STEPS=RECEIVE_STEPS.format(prc=args.probe.resolve()))
            out, err = folder / 'out.txt', folder / 'err.txt'
            with open(out, 'w') as stdout, open(err, 'w') as stderr:
                test = subprocess.Popen([str(args.test), 'steps', str(folder)], env=env, stdout=stdout, stderr=stderr)
            processes.append(test)
            end = time.monotonic() + 600
            while not (folder / 'ready').exists() and test.poll() is None and time.monotonic() < end:
                time.sleep(0.2)
            address = zodiac_address(err)
            if not address:
                test.kill()
                print(f'FAIL: no BD_ADDR in the H4 log ({engine})')
                return 1
            peer_log = folder / 'peer.txt'
            with open(peer_log, 'w') as log:
                peer = subprocess.Popen([str(args.peer), str(socket), str(received), '--push', address, '--gate',
                                         str(folder / 'go')], stdout=log, stderr=subprocess.STDOUT)
            processes.insert(1, peer)
            code = test.wait(timeout=1200)
            output = out.read_text()
            sys.stdout.write(output)
            peer_text = peer_log.read_text()
            deliveries = err.read_text(errors='replace')
            in_calls = deliveries.count('in a guest call')
            print(f'deliveries: {deliveries.count("zodiac bt deliver at doze")} at the doze, '
                  f'{deliveries.count("zodiac bt deliver at irq")} at the IRQ handler, {in_calls} in guest calls')
            if code:
                sys.stdout.write(deliveries[-3000:])
                print(f'FAIL: the machine run ended with {code} ({engine})')
                return 1
            if 'OPP_PUSH_HELD' not in peer_text or 'OPP_PUSH_COMPLETE' not in peer_text:
                sys.stdout.write(peer_text[-2000:])
                print(f'FAIL: the push didn\'t complete ({engine})')
                return 1
            if 'calls: 40 of 40 lists' not in output or 'payload ok' not in output:
                print(f'FAIL: guest calls or the received payload ({engine})')
                return 1
            if not in_calls:
                print(f'FAIL: no bytes came in during a guest call ({engine})')
                return 1
        finally:
            stop(processes)
    print(f'zodiac bluetooth OPP receive with guest calls ({engine}): ok')
    return 0


# InkStorm, the Zodiac's Bluetooth chat, between two Zodiacs on one btvirt: A hosts a chat room and invites B
# from Discovery Results; once B has joined A writes a stroke and sends it, and B's chat shows it.
INK_A = ('launch InkStorm; wait 4000; tap 40 294 8000; tap 188 294 3000; tap 224 80 25000; shot found; '
         'tap 21 107 1000; tap 50 294 15000; shot room; signal {top}/room; waitfor {top}/joined 180000; stroke 40 255 230 300; '
         'stroke 40 300 230 255; tap 296 262 3000; shot sent; signal {top}/sent; waitfor {top}/done 120000')
INK_B = ('launch InkStorm; wait 4000; tap 40 294 8000; tap 188 294 3000; waitfor {top}/room 180000; wait 10000; '
         'shot joined; signal {top}/joined; waitfor {top}/sent 120000; wait 5000; shot ink; signal {top}/done')


def read_ppm(path):
    parts = path.read_bytes().split(b'\n', 3)
    width, height = map(int, parts[1].split())
    return width, height, parts[3]


# Pixels of the chat area (left of the scroll bar, between the title and the input box) that differ.
def chat_changes(before, after):
    width, _, a = read_ppm(before)
    _, _, b = read_ppm(after)
    return sum(1 for y in range(30, 230) for x in range(0, 295)
               if a[(y * width + x) * 3:(y * width + x) * 3 + 3] != b[(y * width + x) * 3:(y * width + x) * 3 + 3])


def inkstorm(args, engine):
    with tempfile.TemporaryDirectory(prefix='zodiac-bt-', dir=ROOT / 'build') as folder:
        folder = Path(args.keep or folder).resolve()
        folder.mkdir(mode=0o700, parents=True, exist_ok=True)
        folder.chmod(0o700)
        radio, socket = start_radio(args, folder)
        processes = [radio]
        try:
            runs = []
            for name, steps in (('a', INK_A), ('b', INK_B)):
                (folder / name).mkdir(mode=0o700)
                env = dict(os.environ, BT=str(socket), ZODIAC_TEST_FAST=args.fast, FROND_ZODIAC_BT_LOG='1',
                           STEPS=steps.format(top=folder))
                with open(folder / name / 'out.txt', 'w') as stdout, open(folder / name / 'err.txt', 'w') as stderr:
                    runs.append(subprocess.Popen([str(args.test), 'steps', str(folder / name)], env=env, stdout=stdout,
                                                 stderr=stderr))
                processes.append(runs[-1])
            codes = [run.wait(timeout=1500) for run in runs]
        finally:
            stop(processes)
        for name in 'ab':
            sys.stdout.write(f'{name}: ' + (folder / name / 'out.txt').read_text().replace('\n', f'\n{name}: '))
            print()
        if any(codes):
            print(f'FAIL: the machine runs ended with {codes} ({engine})')
            return 1
        joined, ink = folder / 'b/joined.ppm', folder / 'b/ink.ppm'
        if not joined.exists() or not ink.exists():
            print(f'FAIL: B never got to the chat room ({engine})')
            return 1
        width, _, data = read_ppm(joined)
        grey = data[(130 * width + 150) * 3:(130 * width + 150) * 3 + 3]
        changed = chat_changes(joined, ink)
        print(f'B chat area: background {grey.hex()}, {changed} pixels changed by the stroke')
        if grey == b'\xff\xff\xff' or changed < 50:
            print(f'FAIL: B didn\'t show A\'s stroke ({engine})')
            return 1
    print(f'zodiac bluetooth InkStorm chat between two Zodiacs ({engine}): ok')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test', type=Path, default=ROOT / 'build/test-zodiac-bluetooth')
    parser.add_argument('--btvirt', type=Path, default=ROOT / 'build/bluetooth-provider/emulator/btvirt')
    parser.add_argument('--peer', type=Path, default=ROOT / 'build/bluetooth-opp-peer/bluetooth-opp-peer')
    parser.add_argument('--probe', type=Path, default=ROOT / 'build/bluetooth-probe/FrondBluetoothProbe.prc')
    parser.add_argument('--fast', choices=['0', '1'], default='1')
    parser.add_argument('--only', choices=['send', 'receive', 'inkstorm'])
    parser.add_argument('--keep', type=Path, help='a new folder for the receive run, kept after it')
    args = parser.parse_args()
    if not os.environ.get('ZODIAC_ROM') or not os.environ.get('ZODIAC_STATE'):
        print('SKIP: set ZODIAC_ROM and ZODIAC_STATE')
        return 0
    for path in (args.btvirt, args.peer, args.probe):
        if not path.exists():
            print(f'SKIP: {path} is missing')
            return 0
    if args.only == 'receive':
        return receive(args, "Fast" if args.fast == "1" else "Exact")
    if args.only == 'inkstorm':
        return inkstorm(args, "Fast" if args.fast == "1" else "Exact")
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
    engine = "Fast" if args.fast == "1" else "Exact"
    print(f'zodiac bluetooth OPP send ({engine}): ok')
    return 0 if args.only == 'send' else inkstorm(args, engine)


if __name__ == '__main__':
    sys.exit(main())
