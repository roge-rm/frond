#!/usr/bin/env python3
"""Optional real-provider H4 contract test; never installs or accesses an adapter."""
import contextlib
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


class H4:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(3)
        self.socket.connect(str(path))

    def exact(self, count):
        data = b""
        while len(data) < count:
            chunk = self.socket.recv(count - len(data))
            assert chunk, "provider disconnected"
            data += chunk
        return data

    def event(self):
        assert self.exact(1) == b"\x04"
        header = self.exact(2)
        return header[0], self.exact(header[1])

    def command(self, opcode, payload=b""):
        self.socket.sendall(b"\x01" + struct.pack("<HB", opcode, len(payload)) + payload)
        event, data = self.event()
        if event == 0x0e:
            assert int.from_bytes(data[1:3], "little") == opcode
            return data[3:]
        assert event == 0x0f and int.from_bytes(data[2:4], "little") == opcode
        return data[:1]

    def close(self):
        self.socket.close()



def connect_pair(source, receiver, address):
    source_address = source.command(0x1009)[1:]
    assert receiver.command(0x0c1a, b"\x02") == b"\0"
    create = address + struct.pack("<HBBHB", 0xcc18, 2, 0, 0, 1)
    assert source.command(0x0405, create) == b"\0"
    event, request = receiver.event()
    assert event == 4 and request[:6] == source_address and request[9] == 1
    assert receiver.command(0x0409, source_address + b"\0") == b"\0"
    event, complete = source.event()
    assert event == 3 and complete[0] == 0 and complete[3:9] == address
    source_handle = int.from_bytes(complete[1:3], "little")
    event, complete = receiver.event()
    assert event == 3 and complete[0] == 0 and complete[3:9] == source_address
    return source_handle, int.from_bytes(complete[1:3], "little")


def disconnected(host, handle):
    event, data = host.event()
    assert event == 5 and data == b"\0" + struct.pack("<H", handle) + b"\x08", (event, data.hex())


@contextlib.contextmanager
def connected_controllers(path):
    with contextlib.closing(H4(path)) as source, contextlib.closing(H4(path)) as receiver:
        for host in (source, receiver):
            assert host.command(0x0c03) == b"\0"
        address = receiver.command(0x1009)[1:]
        source_handle, receiver_handle = connect_pair(source, receiver, address)
        yield source, receiver, source_handle, receiver_handle


def send_acl(source, handle, payload):
    source.socket.sendall(b"\x02" + struct.pack("<HH", handle | 0x2000, len(payload)) + payload)
    event, data = source.event()
    assert event == 0x13 and data == b"\x01" + struct.pack("<HH", handle, 1)


def receive_acl(receiver, handle, payload):
    assert receiver.exact(1) == b"\x02"
    assert receiver.exact(4) == struct.pack("<HH", handle | 0x2000, len(payload))
    assert receiver.exact(len(payload)) == payload


def check_stream_transport(path):
    # The second header starts in the first read, after a complete command.
    # Waiting for the first completion makes that split deterministic without
    # issuing a second complete command before the HCI command credit returns.
    with contextlib.closing(H4(path)) as host:
        assert host.command(0x0c03) == b"\0"
        host.socket.sendall(bytes.fromhex("0109100001"))
        event, data = host.event()
        assert event == 0x0e and data[1:3] == bytes.fromhex("0910")
        host.socket.sendall(bytes.fromhex("030c00"))
        assert host.event() == (0x0e, bytes.fromhex("01030c00"))
    print("PASS optional provider: a coalesced command plus partial next header executes each command exactly once")

    with connected_controllers(path) as (source, receiver, source_handle, receiver_handle):
        payload = bytes((i * 73 + 17) & 255 for i in range(65535))
        send_acl(source, source_handle, payload)
        receive_acl(receiver, receiver_handle, payload)
    print("PASS optional provider: complete 65535-byte ACL payload has no header-length overflow")

    def payload(index):
        return struct.pack("<I", index) + bytes((i * 73 + index * 31) & 255 for i in range(1020))

    with connected_controllers(path) as (source, receiver, source_handle, receiver_handle):
        receiver.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        # Respect the source controller's completion credit while deliberately
        # withholding reads at the destination controller. Replies must retain
        # partial-write suffixes until that destination starts reading again.
        for index in range(512):
            send_acl(source, source_handle, payload(index))
        for index in range(512):
            receive_acl(receiver, receiver_handle, payload(index))
    print("PASS optional provider: 512 KiB ACL backpressure preserves every frame and byte")

    with connected_controllers(path) as (source, receiver, source_handle, receiver_handle):
        receiver.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        lost = False
        for index in range(2200):
            data = payload(index)
            source.socket.sendall(b"\x02" + struct.pack("<HH", source_handle | 0x2000, len(data)) + data)
            event, complete = source.event()
            if event == 5:
                assert complete == b"\0" + struct.pack("<H", source_handle) + b"\x08"
                lost = True
                break
            assert event == 0x13 and complete == b"\x01" + struct.pack("<HH", source_handle, 1)
        assert lost, "bounded provider output overflow must terminate the link"
        with contextlib.closing(H4(path)) as healthy:
            assert healthy.command(0x0c03) == b"\0"
    print("PASS optional provider: bounded queue overflow reports link loss and leaves other controllers usable")


def check_controller_addresses(path):
    addresses = []
    for _ in range(8):
        with contextlib.closing(H4(path)) as host:
            assert host.command(0x0c03) == b"\0"
            address = host.command(0x1009)[1:]
            assert len(address) == 6 and address[5] == 0x02
            assert address not in addresses, "a new controller reused a departed peer identity"
            addresses.append(address)
            assert host.command(0x0c03) == b"\0"
            assert host.command(0x1009)[1:] == address
        time.sleep(0.02)
    print("PASS optional provider: distinct controller epochs have distinct addresses; HCI reset preserves each address")


def main():
    assert len(sys.argv) == 2, "usage: bluetooth-provider.py /path/to/pinned/btvirt"
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="frond-bt-provider-") as temporary:
        directory = Path(temporary)
        path = directory / "bt-server-bredr"
        with (directory / "provider.log").open("wb") as log:
            process = subprocess.Popen([str(binary), "-s" + str(directory)], stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 5
                while not path.exists():
                    assert process.poll() is None, "provider exited before listening"
                    assert time.monotonic() < deadline, "provider socket timeout"
                    time.sleep(0.02)
                with contextlib.closing(H4(path)) as source, contextlib.closing(H4(path)) as receiver:
                    for host in (source, receiver):
                        assert host.command(0x0c03) == b"\0"
                    # Wire order is least significant octet first. Readback must
                    # come from the real provider, and inquiry must use it too.
                    address = bytes.fromhex("2a0000000002")  # Synthetic, locally administered identity.
                    assert receiver.command(0xfc01, address) == b"\0"
                    assert receiver.command(0x1009) == b"\0" + address
                    assert receiver.command(0xfc01, address[:5]) == b"\x12"
                    assert receiver.command(0x1009) == b"\0" + address
                    for mode in (0, 1, 2, 3):
                        assert receiver.command(0x0c1a, bytes([mode])) == b"\0"
                        assert receiver.command(0x0c19) == bytes([0, mode])
                        assert source.command(0x0401, bytes.fromhex("338b9e0100")) == b"\0"
                        seen = []
                        while True:
                            event, data = source.event()
                            if event == 0x01:
                                assert data == b"\0"
                                break
                            assert event == 0x02, (event, data.hex())
                            assert data[0] == 1
                            seen.append(data[1:7])
                        assert seen == ([address] if mode & 1 else []), (mode, seen)
                    print("PASS optional provider: actual address write/readback, invalid length refusal, inquiry modes0/1/2/3 and exact discovered identity")
                    first_handle, _ = connect_pair(source, receiver, address)
                    receiver.close()  # Controller disappears without an HCI disconnect command.
                    disconnected(source, first_handle)
                    with contextlib.closing(H4(path)) as replacement:
                        assert replacement.command(0x0c03) == b"\0"
                        assert replacement.command(0xfc01, address) == b"\0"
                        _, remote_handle = connect_pair(source, replacement, address)
                        assert source.command(0x0c03) == b"\0"
                        disconnected(replacement, remote_handle)
                        # A fresh epoch really reconnects; reset/leave must not
                        # leave an orphaned handle in either virtual controller.
                        local_handle, _ = connect_pair(source, replacement, address)
                        replacement.close()
                        disconnected(source, local_handle)
                    print("PASS optional provider: abrupt controller leave/reset notify the real peer and fresh address/handle epochs reconnect")
                # Ensure a peer whose socket is also closed cannot SIGPIPE the
                # provider while it publishes the first controller's departure.
                left, right = H4(path), H4(path)
                try:
                    for host in (left, right):
                        assert host.command(0x0c03) == b"\0"
                    right_address = right.command(0x1009)[1:]
                    connect_pair(left, right, right_address)
                    process.send_signal(signal.SIGSTOP)
                    left.close()
                    right.close()
                    process.send_signal(signal.SIGCONT)
                    time.sleep(0.05)
                    assert process.poll() is None, "simultaneous peer closure killed provider"
                    with contextlib.closing(H4(path)) as healthy:
                        assert healthy.command(0x0c03) == b"\0"
                finally:
                    process.send_signal(signal.SIGCONT)
                    left.close()
                    right.close()
                print("PASS optional provider: simultaneous controller closure leaves the provider usable")
                check_stream_transport(path)
                check_controller_addresses(path)
            finally:
                process.send_signal(signal.SIGINT)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    main()
