#!/usr/bin/env python3
"""Put a PTY in front of a TCP endpoint, for programs that only open serial devices.

pilot-link and terminal programs take the printed /dev/pts/N (or --link PATH) as their port. The
endpoint is PORT (a local Frond's --serial or --usb tcp:PORT), HOST:PORT (connect, such as an Android
Frond waiting for a peer) or listen:PORT (wait on all interfaces for one peer, such as an Android
Frond connecting here). Bytes pass both ways unchanged. The PTY stays open between programs, so
pilot-link can run again and again; the run ends when the TCP side closes."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import select
import socket
import time
import tty

spec = importlib.util.spec_from_file_location('link_irda', Path(__file__).resolve().parent / 'link-irda.py')
link_irda = importlib.util.module_from_spec(spec)
spec.loader.exec_module(link_irda)
CHUNK = 4096


def connect(end, seconds):
    kind, host, port = end
    if kind != 'listen':
        return socket.create_connection((host, port), timeout=10 if seconds is None else max(.01, min(10, seconds)))
    dual = socket.has_dualstack_ipv6()
    with socket.create_server(('', port), family=socket.AF_INET6 if dual else socket.AF_INET, backlog=1,
                              dualstack_ipv6=dual) as server:
        print(f'Waiting for a peer on port {port}.', flush=True)
        server.settimeout(seconds)
        peer, address = server.accept()
        print(f'Peer {address[0].removeprefix("::ffff:")} connected on port {port}.', flush=True)
        return peer


def relay(end, link=None, seconds=None):
    """Runs until the TCP side closes or seconds pass. Returns counts for the result line."""
    started = time.monotonic()
    master, slave = os.openpty()
    tty.setraw(slave)
    name = os.ttyname(slave)
    received, sent, reason = 0, 0, 'deadline'
    peer = None
    try:
        if link:
            try:
                os.unlink(link)
            except FileNotFoundError:
                pass
            os.symlink(name, link)
        print(f'PTY {name}' + (f' at {link}' if link else ''), flush=True)
        try:
            peer = connect(end, seconds)
        except socket.timeout:
            return {'reason': reason, 'from_tcp': 0, 'to_tcp': 0}
        peer.setblocking(True)
        peer.settimeout(None)
        peer.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f'Linked {name} and {link_irda.name(end)}.', flush=True)
        while seconds is None or time.monotonic() - started < seconds:
            ready = select.select([master, peer], [], [], .1)[0]
            if master in ready:
                try:
                    data = os.read(master, CHUNK)
                except OSError:
                    data = b''
                if data:
                    peer.sendall(data)
                    sent += len(data)
                else:
                    time.sleep(.01)
            if peer in ready:
                try:
                    data = peer.recv(CHUNK)
                except OSError:
                    data = b''
                if not data:
                    reason = 'peer disconnected'
                    break
                os.write(master, data)
                received += len(data)
        return {'reason': reason, 'from_tcp': received, 'to_tcp': sent}
    finally:
        if peer:
            peer.close()
        os.close(master)
        os.close(slave)
        if link and os.path.islink(link) and os.readlink(link) == name:
            os.unlink(link)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('endpoint', help='PORT, HOST:PORT or listen:PORT')
    p.add_argument('--link', help='also make this symlink to the PTY')
    p.add_argument('--seconds', type=float, help='optional bounded run')
    a = p.parse_args()
    try:
        end = link_irda.endpoint(a.endpoint)
    except ValueError as e:
        p.error(f'{e}: use PORT, HOST:PORT or listen:PORT with a port 1..65535')
    if a.seconds is not None and not 0 < a.seconds <= 86400:
        p.error('seconds must be positive and at most 86400')
    if a.link and os.path.lexists(a.link) and not os.path.islink(a.link):
        p.error(f'{a.link} exists and is not a symlink')
    try:
        print(json.dumps(relay(end, a.link, a.seconds)))
    except KeyboardInterrupt:
        return 0
    except OSError as e:
        p.exit(1, str(e) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
