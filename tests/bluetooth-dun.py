#!/usr/bin/env python3
"""The Bluetooth phone (bluetooth-opp-peer --dun) on a private btvirt: a second peer finds its DUN
service, connects RFCOMM, dials and brings PPP's LCP and IPCP up."""
import argparse
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


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
            address = wait_for(server, 'BDADDR_LE ', 10, server_log)
            ready = wait_for(server, 'READY ', 10, server_log)
            if not address or not ready:
                raise RuntimeError('the phone did not come up')
            client = subprocess.run([str(args.peer), str(socket), '--dun-dial', address.split()[1]],
                                    stdout=subprocess.PIPE, text=True, timeout=60)
            client_log = client.stdout.splitlines()
            # The phone's own view of the call, read once the client has gone.
            wait_for(server, 'DUN_STATE off', 5, server_log)
            def seen(log, text):
                return any(text in line for line in log)
            for text in ('CLIENT_DUN_CHANNEL 9', 'DUN_DIALLED CONNECT 33600', 'DUN_LCP_UP',
                         'DUN_IPCP_UP address=10.0.2.15 dns=10.0.2.3', 'HCI_DISCONNECT_COMPLETE'):
                if not seen(client_log, text):
                    failures.append(f'client: no {text}')
            if client.returncode:
                failures.append(f'client exit {client.returncode}')
            for text in ('SDP opcode=', 'DUN_CONNECTED channel=9', 'DUN_STATE dialling', 'DUN_STATE online 33600',
                         'DUN_CLOSED', 'DUN_STATE off'):
                if not seen(server_log, text):
                    failures.append(f'phone: no {text}')
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
    print('PASS Bluetooth DUN: SDP, RFCOMM channel 9, CONNECT 33600, LCP and IPCP up (10.0.2.15, DNS 10.0.2.3)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
