#!/usr/bin/env python3
"""Carry Frond Bluetooth controller connections over the network.

serve SOCKET listen:PORT   Accept peers on PORT (all interfaces). Each TCP connection gets
                           its own new connection to SOCKET, a local btvirt's bt-server-bredr,
                           so each remote Frond gets its own virtual controller.
join HOST:PORT SOCKET      Make a Unix socket a local Frond can use with --bluetooth SOCKET.
                           Each connection the core makes is carried to HOST:PORT, another
                           machine's serve or an Android Frond hosting Bluetooth.

Bytes pass unchanged. Closing either side of a pair closes the other."""
import argparse
import json
import os
import signal
import socket
import stat
import sys
import threading
import time

CHUNK = 65536


def port(text):
    text = str(text).strip()
    if not text.isdigit() or not 1 <= int(text) <= 65535:
        raise ValueError(text)
    return int(text)


def listen_port(text):
    text = str(text).strip()
    if not text.startswith('listen:'):
        raise ValueError(text)
    return port(text[7:])


def address(text):
    """HOST:PORT or [IPv6]:PORT."""
    text = str(text).strip()
    if text.startswith('['):
        host, sep, p = text[1:].partition(']:')
        if not sep or not host:
            raise ValueError(text)
    else:
        host, sep, p = text.rpartition(':')
        if not sep or not host or ':' in host:
            raise ValueError(text)
    return host, port(p)


def keepalive(s):
    # Notices a peer that vanished without closing in about two minutes.
    s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    for name, value in (('TCP_KEEPIDLE', 60), ('TCP_KEEPINTVL', 15), ('TCP_KEEPCNT', 4)):
        if hasattr(socket, name):
            s.setsockopt(socket.IPPROTO_TCP, getattr(socket, name), value)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)


class Relay:
    def __init__(self):
        self.lock = threading.Lock()
        self.pairs = set()
        self.done = threading.Event()
        self.stats = {'connections': 0, 'failed': 0, 'bytes_to_socket': 0, 'bytes_to_network': 0}

    def pair(self, network, local, label):
        """Pumps bytes both ways until either side closes, then closes both."""
        with self.lock:
            self.pairs.add((network, local))
            self.stats['connections'] += 1
        print(f'{label} connected.', flush=True)
        closed = threading.Event()

        def close():
            if closed.is_set():
                return
            closed.set()
            for s in (network, local):
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

        def pump(source, sink, key):
            try:
                while True:
                    data = source.recv(CHUNK)
                    if not data:
                        break
                    sink.sendall(data)
                    with self.lock:
                        self.stats[key] += len(data)
            except OSError:
                pass
            close()

        threads = [threading.Thread(target=pump, args=(network, local, 'bytes_to_socket'), daemon=True),
                   threading.Thread(target=pump, args=(local, network, 'bytes_to_network'), daemon=True)]
        for t in threads:
            t.start()

        def reap():
            for t in threads:
                t.join()
            network.close()
            local.close()
            with self.lock:
                self.pairs.discard((network, local))
            print(f'{label} closed.', flush=True)
        threading.Thread(target=reap, daemon=True).start()

    def close_all(self):
        with self.lock:
            pairs = list(self.pairs)
        for pair in pairs:
            for s in pair:
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    def wait_idle(self, seconds=2):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            with self.lock:
                if not self.pairs:
                    return
            time.sleep(.02)


def serve(path, listen, seconds, relay):
    dual = socket.has_dualstack_ipv6()
    server = socket.create_server(('', listen), family=socket.AF_INET6 if dual else socket.AF_INET,
                                  backlog=8, reuse_port=False, dualstack_ipv6=dual)
    print(f'Serving {path} on port {listen}.', flush=True)
    started = time.monotonic()
    try:
        while seconds is None or time.monotonic() - started < seconds:
            server.settimeout(.5 if seconds is None else max(.01, min(.5, seconds - (time.monotonic() - started))))
            try:
                peer, where = server.accept()
            except socket.timeout:
                continue
            peer.settimeout(None)
            host = where[0].removeprefix('::ffff:')
            local = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                local.connect(path)
            except OSError as e:
                print(f'Peer {host}: could not reach {path}: {e.strerror or e}.', flush=True)
                local.close()
                peer.close()
                with relay.lock:
                    relay.stats['failed'] += 1
                continue
            keepalive(peer)
            relay.pair(peer, local, f'Peer {host}')
    finally:
        server.close()


def unix_server(path):
    """A private Unix listener at path. A stale socket nobody answers on is replaced."""
    if os.path.lexists(path):
        if not stat.S_ISSOCK(os.lstat(path).st_mode):
            raise OSError(f'{path} exists and is not a socket')
        probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            probe.connect(path)
        except OSError:
            os.unlink(path)
        else:
            raise OSError(f'{path} is in use')
        finally:
            probe.close()
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    old = os.umask(0o177)
    try:
        server.bind(path)
    finally:
        os.umask(old)
    server.listen(8)
    return server


def join(host, remote, path, seconds, relay):
    server = unix_server(path)
    name = f'[{host}]:{remote}' if ':' in host else f'{host}:{remote}'
    print(f'Joining {name} through {path}.', flush=True)
    started = time.monotonic()
    count = 0
    try:
        while seconds is None or time.monotonic() - started < seconds:
            server.settimeout(.5 if seconds is None else max(.01, min(.5, seconds - (time.monotonic() - started))))
            try:
                local, _ = server.accept()
            except socket.timeout:
                continue
            local.settimeout(None)
            count += 1
            try:
                peer = socket.create_connection((host, remote), timeout=10)
            except OSError as e:
                print(f'Controller {count}: could not reach {name}: {e.strerror or e}.', flush=True)
                local.close()
                with relay.lock:
                    relay.stats['failed'] += 1
                continue
            peer.settimeout(None)
            keepalive(peer)
            relay.pair(peer, local, f'Controller {count} to {name}')
    finally:
        server.close()
        try:
            os.unlink(path)
        except OSError:
            pass


def interrupted(*_):
    raise KeyboardInterrupt


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('command', choices=('serve', 'join'))
    p.add_argument('first', help='serve: SOCKET; join: HOST:PORT')
    p.add_argument('second', help='serve: listen:PORT; join: SOCKET')
    p.add_argument('--seconds', type=float, help='optional bounded run')
    a = p.parse_args()
    if a.seconds is not None and not 0 < a.seconds <= 86400:
        p.error('seconds must be positive and at most 86400')
    path = a.first if a.command == 'serve' else a.second
    if not os.path.isabs(path) or len(os.fsencode(path)) >= 108:
        p.error('SOCKET must be an absolute path shorter than 108 bytes')
    try:
        if a.command == 'serve':
            listen = listen_port(a.second)
        else:
            host, remote = address(a.first)
    except ValueError as e:
        p.error(f'{e}: use serve SOCKET listen:PORT or join HOST:PORT SOCKET, with a port 1..65535')
    relay = Relay()
    # Stopping with SIGTERM also removes the join socket.
    signal.signal(signal.SIGTERM, interrupted)
    try:
        if a.command == 'serve':
            serve(path, listen, a.seconds, relay)
        else:
            join(host, remote, path, a.seconds, relay)
    except KeyboardInterrupt:
        pass
    except OSError as e:
        p.exit(1, str(e) + '\n')
    relay.close_all()
    relay.wait_idle()
    with relay.lock:
        print(json.dumps(relay.stats), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
