#!/usr/bin/env python3
"""A IIIx session saved while Palm OS is booting after a soft reset reloads with its RAM store kept.

With an installed database, the check soft-resets with the GDB stub watching low memory. It stops
once the boot code has cleared timGlobalsP and has just written its RAM check pattern at address 0,
saves there, and then expects the database to still be there both in the process that carries on
and after reloading the save. A core that sets the guest's date through timGlobalsP on load writes
over that pattern, and the boot code formats RAM.

Run: python3 tests/frond-68k-boot-save.py [--rom PRIVATE.rom]. --rom defaults to the generic OS 3.5
EZ ROM in the firmware folder; skipped without it. Scratch goes in build/boot-save-*.
"""
import argparse
import importlib.util
import socket
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

spec = importlib.util.spec_from_file_location('core_control', ROOT / 'tests/core-control.py')
core_control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(core_control)

CORE = ROOT / 'out/frond-68k'
TIM_GLOBALS_P = 0x178  # timGlobalsP in the fixed low-memory globals


class Gdb:
    """Just enough of the GDB remote protocol for watchpoints and memory reads."""

    def __init__(self, port):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=60)
        self.buffer = b''

    def send(self, data):
        self.socket.sendall(f'${data}#{sum(data.encode()) & 0xff:02x}'.encode())

    def receive(self):
        while True:
            start = self.buffer.find(b'$')
            end = self.buffer.find(b'#', start)
            if start >= 0 and end >= 0 and len(self.buffer) >= end + 3:
                packet = self.buffer[start + 1:end].decode()
                self.buffer = self.buffer[end + 3:]
                self.socket.sendall(b'+')
                return packet
            more = self.socket.recv(4096)
            assert more, 'gdb stub closed the connection'
            self.buffer += more

    def command(self, data):
        self.send(data)
        return self.receive()

    def word(self, address):
        return int(self.command(f'm{address:x},4'), 16)

    def close(self):
        self.socket.close()


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def has_pattern(c):
    return core_control.PATTERN_NAME.encode().hex() in c.ok('db-list')


def check(rom, root):
    saved = root / 'booting.frond'
    c = core_control.Client(CORE, [rom, '--device', 'palm-iiix'], root, 'boot-save')
    try:
        c.until_ok('guest-ready')
        c.ok('install', core_control.PATTERN)
        assert has_pattern(c), 'install: database not listed'
        port = free_port()
        c.ok('gdb-start', port)
        gdb = Gdb(port)
        try:
            gdb.command('qSupported')
            gdb.command('?')
            assert gdb.command(f'Z2,{TIM_GLOBALS_P:x},4') == 'OK', 'watchpoint refused'
            gdb.send('c')
            c.ok('reset')
            # The boot code clears low memory a word at a time.
            for _ in range(8):
                gdb.receive()
                if gdb.word(TIM_GLOBALS_P) == 0:
                    break
                gdb.send('c')
            else:
                raise AssertionError('timGlobalsP was never cleared after the reset')
            gdb.command(f'z2,{TIM_GLOBALS_P:x},4')
            assert gdb.command('Z2,0,4') == 'OK', 'watchpoint refused'
            gdb.send('c')
            stop = gdb.receive()
            assert 'watch:00000000' in stop, f'no write at address 0: {stop}'
            assert gdb.word(TIM_GLOBALS_P) == 0, 'timGlobalsP set before the RAM check'
            c.ok('save', saved)
            gdb.command('z2,0,4')
            gdb.send('D')
        finally:
            gdb.close()
        c.ok('gdb-stop')
        c.until_ok('guest-ready', timeout=60)
        assert has_pattern(c), 'carrying on: database gone'
        c.quit()
    finally:
        c.close()
    c = core_control.Client(CORE, [saved, '--device', 'palm-iiix'], root, 'boot-save-reload')
    try:
        c.until_ok('guest-ready', timeout=60)
        assert has_pattern(c), 'reload: RAM was formatted, database gone'
        c.quit()
    finally:
        c.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--rom', type=Path)
    args = parser.parse_args()
    rom = args.rom or palm_boot.palm_images() / 'Palm/Generic-OS-3.5-EZ/Palm-OS-3.5-en-ez.rom'
    if not rom.is_file():
        print('frond-68k boot save: skipped, no IIIx ROM')
        return 0
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='boot-save-', dir=ROOT / 'build') as work:
        try:
            check(rom.resolve(), Path(work))
        except AssertionError as error:
            print(f'frond-68k boot save: FAIL, {error}')
            return 1
    print('frond-68k boot save: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
