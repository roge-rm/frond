#!/usr/bin/env python3
"""tools/link-bluetooth.py over loopback: one Unix connection per TCP connection, both directions,
closing either side, join and serve chained, stale sockets and bad arguments."""
from pathlib import Path
import importlib.util
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

TOOL = str(Path(__file__).resolve().parents[1] / 'tools/link-bluetooth.py')


def free_port():
    s = socket.socket()
    s.bind(('', 0))
    port = s.getsockname()[1]
    s.close()
    return port


def receive(peer, size):
    data = bytearray()
    while len(data) < size:
        chunk = peer.recv(min(4093, size - len(data)))
        if not chunk:
            break
        data.extend(chunk)
    return bytes(data)


def start(*args):
    p = subprocess.Popen([sys.executable, TOOL, *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return p


def stop(p):
    if p.poll() is None:
        p.terminate()
    out, err = p.communicate(timeout=10)
    return out, err


def connect_tcp(port):
    end = time.monotonic() + 10
    while True:
        try:
            return socket.create_connection(('127.0.0.1', port), timeout=10)
        except OSError:
            if time.monotonic() > end:
                raise
            time.sleep(.05)


def connect_unix(path):
    end = time.monotonic() + 10
    while True:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        try:
            s.connect(path)
            return s
        except OSError:
            s.close()
            if time.monotonic() > end:
                raise
            time.sleep(.05)


class Tests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.closing = []

    def tearDown(self):
        for s in self.closing:
            s.close()
        self.dir.cleanup()

    def unix_listener(self, name='bt-server-bredr'):
        path = os.path.join(self.dir.name, name)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.bind(path)
        s.listen(8)
        s.settimeout(10)
        self.closing.append(s)
        return path, s

    def test_serve_each_peer_gets_a_controller(self):
        path, btvirt = self.unix_listener()
        port = free_port()
        p = start('serve', path, f'listen:{port}')
        try:
            self.assertIn('Serving', p.stdout.readline())
            a = connect_tcp(port)
            b = connect_tcp(port)
            self.closing += [a, b]
            ca, _ = btvirt.accept()
            cb, _ = btvirt.accept()
            self.closing += [ca, cb]
            ca.settimeout(10)
            cb.settimeout(10)
            # Which accepted connection belongs to which peer: find out by sending.
            a.sendall(b'A')
            first = ca.recv(1)
            if not first:
                self.fail('no data')
            if first != b'A':
                ca, cb = cb, ca
                self.assertEqual(ca.recv(1), b'A')
            b.sendall(b'B')
            self.assertEqual(cb.recv(1), b'B')
            big = bytes(range(256)) * 2048
            back = bytes(reversed(range(256))) * 2049
            t = threading.Thread(target=a.sendall, args=(big,))
            u = threading.Thread(target=ca.sendall, args=(back,))
            t.start()
            u.start()
            self.assertEqual(receive(ca, len(big)), big)
            self.assertEqual(receive(a, len(back)), back)
            t.join()
            u.join()
            # The controller closing ends its peer only.
            ca.close()
            self.assertEqual(a.recv(1), b'')
            b.sendall(b'still')
            self.assertEqual(receive(cb, 5), b'still')
            # The peer closing ends its controller connection.
            b.close()
            self.assertEqual(cb.recv(1), b'')
        finally:
            out, err = stop(p)
        stats = json.loads(out.strip().splitlines()[-1])
        self.assertEqual(stats['connections'], 2, err)
        self.assertEqual(stats['bytes_to_socket'], 1 + 1 + len(big) + 5)
        self.assertEqual(stats['bytes_to_network'], len(back))

    def test_serve_without_btvirt_closes_the_peer(self):
        path = os.path.join(self.dir.name, 'missing')
        port = free_port()
        p = start('serve', path, f'listen:{port}')
        try:
            self.assertIn('Serving', p.stdout.readline())
            a = connect_tcp(port)
            self.closing.append(a)
            a.settimeout(10)
            self.assertEqual(a.recv(1), b'')
        finally:
            out, _ = stop(p)
        self.assertEqual(json.loads(out.strip().splitlines()[-1])['failed'], 1)

    def test_join_and_serve_chained(self):
        # A core's connection through join, the network and serve reaches btvirt.
        path, btvirt = self.unix_listener()
        port = free_port()
        joined = os.path.join(self.dir.name, 'join')
        server = start('serve', path, f'listen:{port}')
        client = start('join', f'127.0.0.1:{port}', joined)
        try:
            self.assertIn('Serving', server.stdout.readline())
            self.assertIn('Joining', client.stdout.readline())
            self.assertEqual(os.stat(joined).st_mode & 0o777, 0o600)
            cores = []
            for n in range(3):
                core = connect_unix(joined)
                self.closing.append(core)
                controller, _ = btvirt.accept()
                controller.settimeout(10)
                self.closing.append(controller)
                core.sendall(bytes([1, 3, 12, n]))
                self.assertEqual(receive(controller, 4), bytes([1, 3, 12, n]))
                controller.sendall(bytes([4, 14, n]))
                self.assertEqual(receive(core, 3), bytes([4, 14, n]))
                cores.append((core, controller))
            # The core closing (reset or sleep) closes its controller; the others stay.
            cores[0][0].close()
            self.assertEqual(cores[0][1].recv(1), b'')
            cores[1][0].sendall(b'ok')
            self.assertEqual(receive(cores[1][1], 2), b'ok')
        finally:
            out, err = stop(client)
            stop(server)
        self.assertFalse(os.path.exists(joined), 'join removes its socket')
        self.assertEqual(json.loads(out.strip().splitlines()[-1])['connections'], 3, err)

    def test_join_unreachable_host_closes_the_core(self):
        joined = os.path.join(self.dir.name, 'join')
        p = start('join', f'127.0.0.1:{free_port()}', joined)
        try:
            self.assertIn('Joining', p.stdout.readline())
            core = connect_unix(joined)
            self.closing.append(core)
            self.assertEqual(core.recv(1), b'')
            self.assertIn('could not reach', p.stdout.readline())
        finally:
            stop(p)

    def test_join_slow_host_does_not_hold_up_the_next_core(self):
        # A host that doesn't answer (its accept queue full, so SYNs are dropped) holds each connection
        # for the connect timeout; the core's next connection is still taken meanwhile.
        spec = importlib.util.spec_from_file_location('link_bluetooth', TOOL)
        tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tool)
        tool.CONNECT_SECONDS = 2
        host = socket.socket()
        host.bind(('127.0.0.1', 0))
        host.listen(0)
        self.closing.append(host)
        for _ in range(3):
            filler = socket.socket()
            filler.setblocking(False)
            filler.connect_ex(host.getsockname())
            self.closing.append(filler)
        time.sleep(.2)
        joined = os.path.join(self.dir.name, 'join')
        relay = tool.Relay()
        t = threading.Thread(target=tool.join, args=('127.0.0.1', host.getsockname()[1], joined, 6, relay), daemon=True)
        t.start()
        started = time.monotonic()
        cores = [connect_unix(joined) for _ in range(2)]
        self.closing += cores
        for core in cores:
            core.settimeout(10)
            self.assertEqual(core.recv(1), b'')
        # One after the other would take twice the timeout.
        self.assertLess(time.monotonic() - started, 3.5)
        t.join(10)
        self.assertEqual(relay.stats['failed'], 2)

    def test_join_replaces_a_stale_socket_and_refuses_a_live_one(self):
        path, _ = self.unix_listener('live')
        p = subprocess.run([sys.executable, TOOL, 'join', '127.0.0.1:5032', path, '--seconds', '1'],
                           capture_output=True, text=True, timeout=10)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('in use', p.stderr)
        stale = os.path.join(self.dir.name, 'stale')
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.bind(stale)
        s.close()
        p = subprocess.run([sys.executable, TOOL, 'join', '127.0.0.1:5032', stale, '--seconds', '0.3'],
                           capture_output=True, text=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertFalse(os.path.exists(stale))
        plain = os.path.join(self.dir.name, 'plain')
        Path(plain).write_text('x')
        p = subprocess.run([sys.executable, TOOL, 'join', '127.0.0.1:5032', plain, '--seconds', '0.3'],
                           capture_output=True, text=True, timeout=10)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(Path(plain).read_text(), 'x')

    def test_bad_arguments_refused(self):
        sock = os.path.join(self.dir.name, 's')
        for args in (('serve', sock, '5032'), ('serve', sock, 'listen:0'), ('serve', 'relative', 'listen:5032'),
                     ('join', 'host', sock), ('join', 'host:', sock), ('join', 'a:b:c', sock),
                     ('join', 'host:70000', sock), ('join', 'host:5032', 'relative')):
            p = subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True, timeout=10)
            self.assertEqual(p.returncode, 2, args)


if __name__ == '__main__':
    unittest.main()
