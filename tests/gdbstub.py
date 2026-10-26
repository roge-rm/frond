#!/usr/bin/env python3
"""Exercise the real native stub and ARM/Thumb CPU over TCP, without firmware."""
from pathlib import Path
import os
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
GDB = os.environ.get('FROND_GDB') or shutil.which('gdb-multiarch')


def frame(data):
    if isinstance(data, str):
        data = data.encode()
    escaped = b''
    for value in data:
        escaped += b'}' + bytes([value ^ 0x20]) if value in b'$#}*' else bytes([value])
    return b'$' + escaped + b'#' + f'{sum(escaped) & 255:02x}'.encode()


class Rsp:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=3)
        self.sock.settimeout(3)

    def byte(self):
        byte = self.sock.recv(1)
        if not byte:
            raise EOFError('debugger connection closed')
        return byte

    def packet(self):
        assert self.byte() == b'$'
        wire = b''
        data = b''
        escaped = False
        while True:
            byte = self.byte()
            if byte == b'#' and not escaped:
                break
            wire += byte
            if escaped:
                data += bytes([byte[0] ^ 0x20])
                escaped = False
            elif byte == b'}':
                escaped = True
            else:
                data += byte
        checksum = self.byte() + self.byte()
        assert int(checksum, 16) == sum(wire) & 255
        self.sock.sendall(b'+')
        return data.decode()

    def send(self, data):
        self.sock.sendall(frame(data))
        assert self.byte() == b'+'

    def request(self, data):
        self.send(data)
        return self.packet()

    def monitor(self, command):
        self.send('qRcmd,' + command.encode().hex())
        output = ''
        while True:
            result = self.packet()
            if result.startswith('O') and result != 'OK':
                output += bytes.fromhex(result[1:]).decode()
            elif result == 'OK' or result.startswith('E') or not result:
                return result, output
            else:
                return 'OK', output + bytes.fromhex(result).decode()

    def reg(self, number):
        return int.from_bytes(bytes.fromhex(self.request(f'p{number:x}')), 'little')

    def set_reg(self, number, value):
        assert self.request(f'P{number:x}={struct.pack("<I", value).hex()}') == 'OK'

    def read(self, addr, length):
        return bytes.fromhex(self.request(f'm{addr:x},{length:x}'))

    def write(self, addr, data):
        assert self.request(f'M{addr:x},{len(data):x}:{data.hex()}') == 'OK'


class Debugger(unittest.TestCase):
    def setUp(self):
        self.log = tempfile.TemporaryFile()
        self.target = subprocess.Popen([str(ROOT / 'build/test-gdbstub-target')] + getattr(self, 'target_args', []),
                                       stdout=self.log, stderr=self.log)
        self.addCleanup(self.cleanup_target)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.log.seek(0)
            match = re.search(rb'GDB listening on 127\.0\.0\.1:(\d+)', self.log.read())
            if match:
                self.port = int(match[1])
                break
            if self.target.poll() is not None:
                self.fail('synthetic target exited before listening')
            time.sleep(.01)
        else:
            self.fail('target did not start')
        self.rsp = Rsp(self.port)
        self.assertEqual(self.rsp.request('?'), 'S05')

    def cleanup_target(self):
        if hasattr(self, 'rsp'):
            self.rsp.sock.close()
        self.target.terminate()
        try:
            self.target.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.target.kill()
            self.target.wait()
        self.log.close()

    def arm(self, pc=0x10000):
        self.rsp.set_reg(25, 0x13)
        self.rsp.set_reg(15, pc)

    def thumb(self, pc=0x11000):
        self.rsp.set_reg(25, 0x33)
        self.rsp.set_reg(15, pc)

    def counters(self):
        return struct.unpack('<II', self.rsp.read(0x21000, 8))

    def test_discovery_xml_and_registers(self):
        self.assertEqual(self.rsp.request('qSupported:multiprocess+'),
                         'PacketSize=1000;qXfer:features:read+')
        xml = ''
        while True:
            chunk = self.rsp.request(f'qXfer:features:read:target.xml:{len(xml):x},43')
            self.assertIn(chunk[0], 'ml')
            xml += chunk[1:]
            if chunk[0] == 'l':
                break
        tree = ET.fromstring(xml)
        self.assertEqual(tree.find('architecture').text, 'arm')
        self.assertEqual([int(r.attrib['regnum']) for r in tree.findall('.//reg')], list(range(16)) + [25])
        for command, reply in [('qAttached', '1'), ('qC', 'QC1'), ('qfThreadInfo', 'm1'),
                               ('qsThreadInfo', 'l'), ('Hg0', 'OK'), ('Hc-1', 'OK'), ('vCont?', 'vCont;c;s')]:
            self.assertEqual(self.rsp.request(command), reply)
        self.rsp.set_reg(0, 0xdeadbeef)
        self.rsp.set_reg(15, 0x10004)
        self.assertEqual(self.rsp.reg(0), 0xdeadbeef)
        self.assertEqual(self.rsp.reg(15), 0x10004)
        values = list(struct.unpack('<17I', bytes.fromhex(self.rsp.request('g'))))
        self.assertEqual(values[0], 0xdeadbeef)
        self.assertEqual(values[15], 0x10004)
        values[0], values[15], values[16] = 55, 0x11000, 0x33
        self.assertEqual(self.rsp.request('G' + struct.pack('<17I', *values).hex()), 'OK')
        self.assertEqual(self.rsp.request('s'), 'S05')
        self.assertEqual(self.rsp.reg(0), 5)
        self.assertEqual(self.rsp.reg(15), 0x11002)
        self.assertTrue(self.rsp.reg(25) & 0x20)
        self.arm()
        self.assertEqual(self.rsp.request('s'), 'S05')
        self.assertEqual(self.rsp.reg(0), 1)
        self.assertEqual(self.rsp.reg(15), 0x10004)

    def test_memory_bounds_errors_and_partial_writes(self):
        self.rsp.write(0x30001, b'\x12\x34\x56\x78\x9a\xbc\xde')
        self.assertEqual(self.rsp.read(0x30001, 7), bytes.fromhex('123456789abcde'))
        self.assertEqual(self.rsp.request('m30001,0'), '')
        self.assertEqual(self.rsp.request('M30001,0:'), 'OK')
        self.assertEqual(self.rsp.request('m31000,800'), '00' * 0x800)
        for request in ['m200000,4', 'm100000,4', 'M200000,4:01020304', 'M100000,4:01020304']:
            self.assertEqual(self.rsp.request(request), 'E0e')
        self.rsp.write(0xffffc, b'abcd')
        self.assertEqual(self.rsp.request('Mffffc,8:0102030405060708'), 'E0e')
        self.assertEqual(self.rsp.read(0xffffc, 4), bytes([1, 2, 3, 4]))
        self.assertEqual(self.rsp.request('mffffc,8'), 'E0e')
        for request in ['m,1', 'm0,', 'mffffffff,2', 'm100000000,1', 'm0,801',
                        'M0,ffffffff:', 'M0,1:gg', 'M0,1:0000', 'M0,2:00',
                        'p', 'p100000000', 'p10', 'P0=123', 'P0=00000000junk',
                        'P19=00000000', 'G' + '00' * 68, 'sfoo', 'Z0,,4', 'Z0,10001,4']:
            self.assertEqual(self.rsp.request(request), 'E01', request)
        # Invalid whole-register input must leave every register unchanged.
        self.rsp.set_reg(0, 123)
        self.assertEqual(self.rsp.request('G' + '00' * 67 + 'gg'), 'E01')
        self.assertEqual(self.rsp.reg(0), 123)
        self.assertEqual(self.rsp.request('vUnknown'), '')
        self.assertEqual(self.rsp.request('k'), '')

    def test_cpsr_mode_banks_and_bulk_write_order(self):
        self.rsp.set_reg(13, 0x12340000)  # Supervisor SP.
        self.rsp.set_reg(25, 0x10)
        self.rsp.set_reg(13, 0x56780000)  # User/system shared SP.
        self.rsp.set_reg(25, 0x1f)
        self.assertEqual(self.rsp.reg(25) & 31, 0x1f)
        self.assertEqual(self.rsp.reg(13), 0x56780000)
        self.rsp.set_reg(25, 0x10)
        self.assertEqual(self.rsp.reg(25) & 31, 0x10)
        self.rsp.set_reg(25, 0x13)
        self.assertEqual(self.rsp.reg(13), 0x12340000)
        values = list(range(16)) + [0x1f]
        values[13], values[15] = 0x11110000, 0x10000
        self.assertEqual(self.rsp.request('G' + struct.pack('<17I', *values).hex()), 'OK')
        self.assertEqual(self.rsp.reg(13), 0x11110000)
        self.rsp.set_reg(25, 0x13)
        self.assertEqual(self.rsp.reg(13), 0x12340000)

    def test_checksum_fragmentation_retransmit_and_overflow(self):
        self.rsp.sock.sendall(b'$P0=78563412#00')
        self.assertEqual(self.rsp.byte(), b'-')
        self.assertEqual(self.rsp.reg(0), 0)
        for byte in frame('P0=78563412'):
            self.rsp.sock.sendall(bytes([byte]))
        self.assertEqual(self.rsp.byte(), b'+')
        self.assertEqual(self.rsp.packet(), 'OK')
        self.rsp.sock.sendall(b'-')
        self.assertEqual(self.rsp.packet(), 'OK')
        self.assertEqual(self.rsp.reg(0), 0x12345678)
        self.rsp.sock.sendall(frame('q' + 'x' * 4096))
        self.assertEqual(self.rsp.byte(), b'-')
        self.assertEqual(self.rsp.request('q' + 'x' * 4095), '')
        self.assertEqual(self.rsp.request(b'X0,4:$#}*'), '')
        self.rsp.sock.sendall(b'$?#zz')
        self.assertEqual(self.rsp.byte(), b'-')
        self.assertEqual(self.rsp.request('?'), 'S05')
        # A new start marker resynchronizes an abandoned packet.
        self.rsp.sock.sendall(b'$incomplete' + frame('?'))
        self.assertEqual(self.rsp.byte(), b'+')
        self.assertEqual(self.rsp.packet(), 'S05')
        self.assertEqual(self.rsp.request(b'P0=00000000\0junk'), 'E01')
        self.assertEqual(self.rsp.reg(0), 0x12345678)

    def test_arm_thumb_breakpoints_and_step(self):
        for thumb, start, next_pc, expected in [(False, 0x10000, 0x10004, 1), (True, 0x11000, 0x11002, 5)]:
            (self.thumb if thumb else self.arm)(start)
            for kind in [0, 1]:
                bp = f'{kind},{next_pc:x},{2 if thumb else 4}'
                self.assertEqual(self.rsp.request('Z' + bp), 'OK')
                self.assertEqual(self.rsp.request('Z' + bp), 'OK')
                self.assertEqual(self.rsp.request('c'), 'S05')
                self.assertEqual(self.rsp.reg(15), next_pc)
                self.assertEqual(self.rsp.reg(0), expected)
                self.assertEqual(self.rsp.request('z' + bp), 'OK')
                self.assertEqual(self.rsp.request('z' + bp), 'OK')
                self.assertEqual(self.rsp.request('vCont;s:1'), 'S05')
                self.assertEqual(self.rsp.reg(0), expected + 1)
                (self.thumb if thumb else self.arm)(start)
        self.arm()
        self.assertEqual(self.rsp.request('s10004'), 'S05')
        self.assertEqual(self.rsp.reg(15), 0x10008)
        # Continue from address zero must not silently keep the old PC.
        self.assertEqual(self.rsp.request('Z0,0,4'), 'OK')
        self.assertEqual(self.rsp.request('c0'), 'S05')
        self.assertEqual(self.rsp.reg(15), 0)
        for i in range(1, 16):
            self.assertEqual(self.rsp.request(f'Z0,{i*4:x},4'), 'OK')
        self.assertEqual(self.rsp.request('Z0,100,4'), 'E05')
        self.assertEqual(self.rsp.request('Z0,0,4'), 'OK')

    def test_debugger_writes_invalidate_arm_thumb_decodes(self):
        for thumb, pc, before, after in [(False, 0x10000, 1, b'\x09\x00\xa0\xe3'),
                                         (True, 0x11000, 5, b'\x09\x20')]:
            (self.thumb if thumb else self.arm)(pc)
            self.assertEqual(self.rsp.request('s'), 'S05')
            self.assertEqual(self.rsp.reg(0), before)
            self.rsp.write(pc, after)
            self.rsp.set_reg(15, pc)
            self.assertEqual(self.rsp.request('s'), 'S05')
            self.assertEqual(self.rsp.reg(0), 9)
        # A failed write still invalidates code in its successfully written prefix.
        self.arm(0xffffc)
        self.rsp.write(0xffffc, bytes.fromhex('0100a0e3'))
        self.assertEqual(self.rsp.request('s'), 'S05')
        self.assertEqual(self.rsp.reg(0), 1)
        self.assertEqual(self.rsp.request('Mffffc,8:0900a0e300000000'), 'E0e')
        self.rsp.set_reg(15, 0xffffc)
        self.assertEqual(self.rsp.request('s'), 'S05')
        self.assertEqual(self.rsp.reg(0), 9)

    def test_steps_through_branches_and_data_aborts(self):
        self.arm(0x10008)
        self.assertEqual(self.rsp.request('s'), 'S05')
        self.assertEqual(self.rsp.reg(15), 0x10004)
        self.thumb(0x11004)
        self.assertEqual(self.rsp.request('s'), 'S05')
        self.assertEqual(self.rsp.reg(15), 0x11002)
        for thumb, pc in [(False, 0x12000), (True, 0x13000)]:
            (self.thumb if thumb else self.arm)(pc)
            self.rsp.set_reg(2, 0x100000)  # Mapped virtual section, no physical RAM.
            self.assertEqual(self.rsp.request('s'), 'S05')
            self.assertEqual(self.rsp.reg(15), 0x10)
            self.assertEqual(self.rsp.reg(25) & 31, 0x17)  # Abort mode, ARM state.
            self.assertFalse(self.rsp.reg(25) & 0x20)

    def test_watchpoints_are_explicitly_unsupported(self):
        # ARM GDB expects traps before an access and steps after a watch stop.
        # Reject insertion and removal for every watchpoint type.
        for kind in [2, 3, 4]:
            for operation in ['Z', 'z']:
                self.assertEqual(self.rsp.request(f'{operation}{kind},20000,4'), '')
        self.arm(0x12000)
        self.rsp.set_reg(1, 42)
        self.rsp.set_reg(2, 0x20000)
        before = self.counters()
        self.assertEqual(self.rsp.request('s'), 'S05')
        self.assertEqual(self.rsp.reg(15), 0x12004)
        self.assertEqual(self.counters(), (before[0], before[1] + 1))

    def reconnect(self):
        self.rsp.sock.close()
        self.rsp = Rsp(self.port)
        self.assertEqual(self.rsp.request('?'), 'S05')
        self.assertIsNone(self.target.poll())

    def test_detach_disconnect_interrupt_and_reconnect(self):
        self.rsp.set_reg(0, 123)
        self.assertEqual(self.rsp.request('Z0,10004,4'), 'OK')
        self.assertEqual(self.rsp.request('D'), 'OK')
        self.assertEqual(self.rsp.sock.recv(1), b'')
        self.reconnect()
        self.assertNotEqual(self.rsp.reg(0), 123)  # execution continued after detach
        # Old breakpoint is cleared; interrupt stops a running target.
        self.rsp.send('c')
        self.rsp.sock.sendall(b'\x03')
        self.assertEqual(self.rsp.packet(), 'S02')
        self.assertEqual(self.rsp.request('Z0,10004,4'), 'OK')
        # Disconnect at a stop, halfway through an untrusted packet.
        self.rsp.sock.sendall(b'$M0,ffffffff:')
        self.reconnect()
        self.rsp.send('vCont;c')
        self.rsp.sock.sendall(b'\x03')
        self.assertEqual(self.rsp.packet(), 'S02')
        # Disconnect while running also leaves the process and listener alive.
        self.rsp.send('c')
        self.reconnect()
        # Write error / RST must not raise SIGPIPE or terminate the target.
        self.rsp.sock.sendall(frame('g'))
        self.rsp.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        self.reconnect()
        self.assertEqual(self.rsp.request('s'), 'S05')

    def test_listener_is_loopback_only(self):
        # Linux /proc provides the actual kernel bind address, without relying
        # on a route or a second interface being present on the test machine.
        lines = Path('/proc/net/tcp').read_text().splitlines()[1:]
        listeners = [line.split()[1] for line in lines
                     if line.split()[3] == '0A' and int(line.split()[1].split(':')[1], 16) == self.port]
        self.assertEqual(listeners, [f'0100007F:{self.port:04X}'])

    def test_cli_rejects_invalid_ports_before_loading_firmware(self):
        for port in ['0', '65536', '4294967295']:
            result = subprocess.run([str(ROOT / 'out/frond'), '/nonexistent/frond.session',
                                     '--gdb', port], capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('GDB port must be 1..65535', result.stderr)

    @unittest.skipUnless(GDB, 'install gdb-multiarch or set FROND_GDB for the client smoke test')
    def test_real_gdb_xml_registers_and_step(self):
        self.assertEqual(self.rsp.request('D'), 'OK')
        self.rsp.sock.close()
        commands = ['set architecture arm', 'set endian little',
                    f'target remote 127.0.0.1:{self.port}',
                    'set $cpsr = 0x13', 'set $pc = 0x10000', 'stepi',
                    'printf "ARM_RESULT=%u PC=%x\\n", $r0, $pc',
                    'set $cpsr = 0x33', 'set $pc = 0x11000', 'stepi',
                    'printf "THUMB_RESULT=%u PC=%x\\n", $r0, $pc',
                    'set $cpsr = 0x13', 'set $pc = 0x10000',
                    'break *0x10004', 'continue',
                    'printf "BREAK_RESULT=%u PC=%x\\n", $r0, $pc',
                    'detach']
        args = [GDB, '-batch', '-nx']
        for command in commands:
            args += ['-ex', command]
        result = subprocess.run(args, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('ARM_RESULT=1 PC=10004', result.stdout)
        self.assertIn('THUMB_RESULT=5 PC=11002', result.stdout)
        self.assertIn('BREAK_RESULT=1 PC=10004', result.stdout)
        self.assertNotIn('rejected', result.stderr.lower())


if __name__ == '__main__':
    unittest.main(verbosity=2)
