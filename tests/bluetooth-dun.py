#!/usr/bin/env python3
"""The Bluetooth phone (bluetooth-opp-peer --dun) on a private btvirt: it takes its fixed address, and a
second peer finds its DUN service, connects RFCOMM, dials and brings PPP's LCP and IPCP up, twice:
once hanging up by dropping the link, once by RFCOMM DISC."""
import argparse
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
# The phone's fixed BD_ADDR in wire order (02:46:52:4E:44:50).
PHONE_ADDRESS = '50444e524602'


def reader(process, lines):
    for line in process.stdout:
        lines.put(line.rstrip('\n'))
    lines.put(None)


def wait_for(lines, prefix, seconds, log):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            line = lines.get(timeout=max(0.01, end - time.monotonic()))
        except queue.Empty:
            break
        if line is None:
            break
        log.append(line)
        if line.startswith(prefix):
            return line
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--btvirt', type=Path, default=ROOT / 'build/bluetooth-provider/emulator/btvirt')
    parser.add_argument('--peer', type=Path, default=ROOT / 'build/bluetooth-opp-peer/bluetooth-opp-peer')
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix='bt-dun-', dir=ROOT / 'build') as folder:
        sockets = Path(folder)
        sockets.chmod(0o700)
        radio = subprocess.Popen([str(args.btvirt), f'-s{sockets}'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        processes = [radio]
        server_log, client_log = [], []
        try:
            socket = sockets / 'bt-server-bredr'
            # The phone waits for the socket itself.
            phone = subprocess.Popen([str(args.peer), str(socket), '--dun'], stdout=subprocess.PIPE, text=True)
            processes.append(phone)
            server = queue.Queue()
            threading.Thread(target=reader, args=(phone, server), daemon=True).start()
            ready = wait_for(server, 'READY ', 10, server_log)
            if not ready:
                raise RuntimeError('the phone did not come up')
            # The phone writes its fixed address (0xfc01) and reads it back before it's ready.
            addresses = [line.split()[1] for line in server_log if line.startswith('BDADDR_LE ')]
            if not addresses or addresses[-1] != PHONE_ADDRESS:
                raise RuntimeError(f'phone address {addresses[-1:] or "missing"}, not {PHONE_ADDRESS}')

            def seen(log, text):
                return any(text in line for line in log)

            # Two calls in a row on the same handle: the first ends with the link dropped, the
            # second with an RFCOMM DISC while the link is still up.
            handles = []
            for call, extra, closed in ((1, [], 'DUN_CLOSED link'), (2, ['--disc-hangup'], 'DUN_CLOSED disc')):
                start = len(server_log)
                client = subprocess.run([str(args.peer), str(socket), '--dun-dial', PHONE_ADDRESS, *extra],
                                        stdout=subprocess.PIPE, text=True, timeout=60)
                client_log.append(f'-- call {call}')
                client_log.extend(client.stdout.splitlines())
                # The phone's own view of the call, read once the client has gone.
                wait_for(server, 'DUN_STATE off', 5, server_log)
                calls = client.stdout.splitlines()
                phone = server_log[start:]
                for text in ('CLIENT_DUN_CHANNEL 9', 'DUN_DIALLED CONNECT 33600', 'DUN_LCP_UP',
                             'DUN_IPCP_UP address=10.0.2.15 dns=10.0.2.3', 'HCI_DISCONNECT_COMPLETE'):
                    if not seen(calls, text):
                        failures.append(f'call {call} client: no {text}')
                if client.returncode:
                    failures.append(f'call {call} client exit {client.returncode}')
                for text in ('SDP opcode=', 'DUN_CONNECTED channel=9', 'DUN_STATE dialling', 'DUN_STATE online 33600',
                             closed, 'DUN_STATE off'):
                    if not seen(phone, text):
                        failures.append(f'call {call} phone: no {text}')
                handles += [line.split('handle=')[1] for line in phone if line.startswith('DUN_CONNECTED ')]
            # btvirt hands the second link the first one's handle; that's the case being checked.
            if len(handles) != 2 or handles[0] != handles[1]:
                failures.append(f'handles {handles}, expected the same one twice')
        except Exception as error:
            failures.append(str(error))
        finally:
            for process in reversed(processes):
                process.terminate()
                try:
                    process.wait(5)
                except subprocess.TimeoutExpired:
                    process.kill()
    if failures:
        print('\n'.join(['phone:', *[l for l in server_log if not l.startswith('H4 ')], 'client:',
                         *[l for l in client_log if not l.startswith('H4 ')]]), file=sys.stderr)
        print('FAIL ' + '; '.join(failures))
        return 1
    print(f'PASS Bluetooth DUN: address {PHONE_ADDRESS}, SDP, RFCOMM channel 9, CONNECT 33600, LCP and IPCP up (10.0.2.15, DNS 10.0.2.3), '
          'twice on one handle, link drop and DISC hang up')
    return 0


if __name__ == '__main__':
    sys.exit(main())
