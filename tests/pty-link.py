#!/usr/bin/env python3
"""tools/link-pty.py: bytes both ways between a PTY and TCP, the PTY reopened between programs, and EOF;
TCP bytes with no program on the PTY dropped, and each program finding the PTY raw."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import termios
import time
import tty
import unittest

TOOL = Path(__file__).resolve().parents[1] / 'tools/link-pty.py'


def read_exactly(fd, size):
    data = bytearray()
    while len(data) < size:
        chunk = os.read(fd, size - len(data))
        if not chunk:
            break
        data.extend(chunk)
    return bytes(data)


def recv_exactly(peer, size):
    data = bytearray()
    while len(data) < size:
        chunk = peer.recv(size - len(data))
        if not chunk:
            break
        data.extend(chunk)
    return bytes(data)


class Linked:
    """Runs the tool against a local listener, yielding (process, peer, link)."""

    def __init__(self, temp):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen()
        self.listener.settimeout(10)
        self.link = os.path.join(temp, 'palm')
        self.process = subprocess.Popen([sys.executable, str(TOOL), str(self.listener.getsockname()[1]), '--link', self.link],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def __enter__(self):
        assert self.process.stdout.readline().startswith('PTY /dev/')
        self.peer, _ = self.listener.accept()
        self.peer.settimeout(10)
        assert 'Linked' in self.process.stdout.readline()
        return self.process, self.peer, self.link

    def __exit__(self, *_):
        self.peer.close()
        self.listener.close()
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        self.process.stdout.close()
        self.process.stderr.close()


class Tests(unittest.TestCase):
    def test_close_ends_with_nobody_on_the_pty(self):
        # More than the PTY's own buffer, sent with no program on it, then TCP closed.
        with tempfile.TemporaryDirectory() as temp, Linked(temp) as (process, peer, link):
            peer.sendall(bytes(200_000))
            peer.close()
            out, err = process.communicate(timeout=3)
            self.assertEqual(process.returncode, 0, err)
            result = json.loads(out.splitlines()[-1])
            self.assertEqual(result, {'reason': 'peer disconnected', 'from_tcp': 0, 'to_tcp': 0, 'dropped': 200_000})

    def test_next_program_finds_it_raw_and_empty(self):
        with tempfile.TemporaryDirectory() as temp, Linked(temp) as (process, peer, link):
            fd = os.open(link, os.O_RDWR | os.O_NOCTTY)
            attrs = termios.tcgetattr(fd)
            self.assertFalse(attrs[3] & (termios.ECHO | termios.ICANON))
            attrs[3] |= termios.ECHO | termios.ICANON
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            os.write(fd, b'x')
            self.assertEqual(recv_exactly(peer, 1), b'x')
            # Unread by this program, so the next one shouldn't see it.
            peer.sendall(b'stale\n')
            time.sleep(.3)
            os.close(fd)
            time.sleep(.5)
            peer.sendall(b'gone')
            time.sleep(.3)
            fd = os.open(link, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            try:
                self.assertFalse(termios.tcgetattr(fd)[3] & (termios.ECHO | termios.ICANON))
                with self.assertRaises(BlockingIOError):
                    os.read(fd, 100)
                os.set_blocking(fd, True)
                peer.sendall(b'fresh')
                self.assertEqual(read_exactly(fd, 5), b'fresh')
            finally:
                os.close(fd)
            peer.close()
            out, err = process.communicate(timeout=3)
            result = json.loads(out.splitlines()[-1])
            # to_tcp has the first program's echo of stale.
            self.assertEqual(result, {'reason': 'peer disconnected', 'from_tcp': 11, 'to_tcp': 7, 'dropped': 4})

    def test_duplex_reopen_and_disconnect(self):
        with tempfile.TemporaryDirectory() as temp, socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            listener.settimeout(10)
            link = os.path.join(temp, 'palm')
            process = subprocess.Popen([sys.executable, str(TOOL), str(listener.getsockname()[1]), '--link', link],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                self.assertTrue(process.stdout.readline().startswith('PTY /dev/'))
                peer, _ = listener.accept()
                peer.settimeout(10)
                self.assertIn('Linked', process.stdout.readline())
                payload = bytes(range(256)) * 8
                for _ in range(2):  # a second program opening the PTY works the same
                    fd = os.open(link, os.O_RDWR | os.O_NOCTTY)
                    tty.setraw(fd)
                    os.write(fd, payload)
                    self.assertEqual(recv_exactly(peer, len(payload)), payload)
                    peer.sendall(payload[::-1])
                    self.assertEqual(read_exactly(fd, len(payload)), payload[::-1])
                    os.close(fd)
                peer.close()
                out, err = process.communicate(timeout=10)
                self.assertEqual(process.returncode, 0, err)
                result = json.loads(out.splitlines()[-1])
                self.assertEqual(result, {'reason': 'peer disconnected', 'from_tcp': 2 * len(payload), 'to_tcp': 2 * len(payload), 'dropped': 0})
                self.assertFalse(os.path.lexists(link))
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_bad_endpoint_refused(self):
        done = subprocess.run([sys.executable, str(TOOL), 'nowhere:'], capture_output=True, text=True)
        self.assertEqual(done.returncode, 2)


if __name__ == '__main__':
    unittest.main()
