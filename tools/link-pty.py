#!/usr/bin/env python3
"""Put a PTY in front of a TCP endpoint, for programs that only open serial devices.

pilot-link and terminal programs take the printed /dev/pts/N (or --link PATH) as their port. The
endpoint is PORT (a local Frond's --serial or --usb tcp:PORT), HOST:PORT (connect, such as an Android
Frond waiting for a peer) or listen:PORT (wait on all interfaces for one peer, such as an Android
Frond connecting here). Bytes pass both ways unchanged. The PTY stays open between programs, so
pilot-link can run again and again; each program finds it raw and empty, and TCP bytes that come while no
program has it open are dropped. The run ends when the TCP side closes."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import select
import socket
import time
import termios
import tty

spec = importlib.util.spec_from_file_location('link_irda', Path(__file__).resolve().parent / 'link-irda.py')
link_irda = importlib.util.module_from_spec(spec)
spec.loader.exec_module(link_irda)
CHUNK = 4096
# TCP bytes waiting for the program on the PTY. When it's full, TCP is read no further until the program
# catches up.
PENDING_MAX = 64 * 1024


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


def reset_slave(name):
    """Makes the PTY raw and drops what no program read, for the next program to open it."""
    fd = os.open(name, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        tty.setraw(fd, termios.TCSANOW)
        termios.tcflush(fd, termios.TCIFLUSH)
    finally:
        os.close(fd)


def hung_up(master):
    """True while no program has the PTY open."""
    poller = select.poll()
    poller.register(master, select.POLLIN)
    return any(event & select.POLLHUP for _, event in poller.poll(0))


def relay(end, link=None, seconds=None):
    """Runs until the TCP side closes or seconds pass. Returns counts for the result line: from_tcp written
    to the PTY, to_tcp read from it, dropped with no program to take them."""
    started = time.monotonic()
    master, slave = os.openpty()
    name = os.ttyname(slave)
    tty.setraw(slave)
    # Only programs hold the slave, so the master sees a hangup when the last one closes it.
    os.close(slave)
    os.set_blocking(master, False)
    received, sent, dropped, reason = 0, 0, 0, 'deadline'
    pending = bytearray()
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
            return {'reason': reason, 'from_tcp': 0, 'to_tcp': 0, 'dropped': 0}
        peer.setblocking(True)
        peer.settimeout(None)
        peer.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f'Linked {name} and {link_irda.name(end)}.', flush=True)
        hung = True
        poller = select.poll()
        while seconds is None or time.monotonic() - started < seconds:
            if hung and not hung_up(master):
                hung = False
            poller.register(peer, select.POLLRDHUP | (select.POLLIN if len(pending) < PENDING_MAX else 0))
            if hung:
                try:
                    poller.unregister(master)
                except KeyError:
                    pass
            else:
                poller.register(master, select.POLLIN | (select.POLLOUT if pending else 0))
            events = dict(poller.poll(100))
            if not hung and master in events:
                event = events[master]
                if event & (select.POLLIN | select.POLLHUP | select.POLLERR):
                    # Read before the hangup is handled, so what a program wrote just before closing still goes out.
                    while True:
                        try:
                            data = os.read(master, CHUNK)
                        except (BlockingIOError, OSError):
                            data = b''
                        if not data:
                            break
                        peer.sendall(data)
                        sent += len(data)
                if event & select.POLLHUP:
                    hung = True
                    dropped += len(pending)
                    pending.clear()
                    try:
                        reset_slave(name)
                    except OSError:
                        pass
                elif pending and event & select.POLLOUT:
                    try:
                        n = os.write(master, pending)
                    except (BlockingIOError, OSError):
                        n = 0
                    del pending[:n]
                    received += n
            event = events.get(peer.fileno(), 0)
            if event & (select.POLLIN | select.POLLHUP | select.POLLERR):
                try:
                    data = peer.recv(CHUNK)
                except OSError:
                    data = b''
                if not data:
                    reason = 'peer disconnected'
                    break
                if hung and hung_up(master):
                    dropped += len(data)
                else:
                    # A program may have opened the PTY during the poll.
                    hung = False
                    pending += data
            elif event & select.POLLRDHUP:
                # Closed with bytes still unread while the program isn't keeping up: those are lost.
                reason = 'peer disconnected'
                break
        return {'reason': reason, 'from_tcp': received, 'to_tcp': sent, 'dropped': dropped}
    finally:
        if peer:
            peer.close()
        os.close(master)
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
