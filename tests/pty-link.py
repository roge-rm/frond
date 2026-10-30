#!/usr/bin/env python3
"""tools/link-pty.py: bytes both ways between a PTY and TCP, the PTY reopened between programs, and EOF."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
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


class Tests(unittest.TestCase):
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
                self.assertEqual(result, {'reason': 'peer disconnected', 'from_tcp': 2 * len(payload), 'to_tcp': 2 * len(payload)})
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
