#!/usr/bin/env python3
"""Host calls on a T|X whose native trap table has system patches in it, as UDMH makes them (patch 0258).
Over the GDB stub, MemChunkFree, MemChunkNew, MemHeapFreeBytes, the heap size call at 0x8538 and
SysAppLaunch get hooks that count their calls and chain to the ROM routine, in unused (zero) slots of
the same table. The guest then runs through them; the database list, an install, a one-database
export and a launch must all work, and the sync probe installed after them gets its
sysAppLaunchCmdSyncNotify (it writes FrondSyncLog). Session: FROND_ARM_SESSIONS/palm-tx or
build/smoke/machines/palm-tx, else SKIPPED (FROND_REQUIRE_SESSIONS=1 makes that a failure).
FROND_CORE picks the core. The probe part is skipped without build/sync68k-probe/FrondSync68k.prc."""
import importlib.util
import os
from pathlib import Path
import shutil
import socket
import struct
import sys
import tempfile
import threading
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


core_control = load('core_control', ROOT / 'tests/core-control.py')
gdbstub = load('gdbstub_test', ROOT / 'tests/gdbstub.py')

TABLE = 0x1ff05e3c
PROBE = ROOT / 'build/sync68k-probe/FrondSync68k.prc'
# Trap, its ROM routine (the T|X table entry without patches), the zero slots its hook goes in.
HOOKS = ((0x84b8, 0x2006941c, 0x8ea0), (0x84bc, 0x2006942c, 0x8f14), (0x8538, 0x2006961c, 0x8f48),
         (0x87c8, 0x20327a3c, 0x8f74))
# MemHeapFreeBytes gets a hook that only chains, in two slots.
PLAIN = (0x8520, 0x20434d4c, 0x8ee0)


def counting_hook(original):
    """ARM: count the call at +24, then load pc with the original at +28. r0 is saved on the stack."""
    return struct.pack('<8I', 0xe52d0004, 0xe59f000c, 0xe2800001, 0xe58f0004, 0xe49d0004, 0xe59ff000,
                       0, original)


def plain_hook(original):
    return struct.pack('<2I', 0xe51ff004, original)


class Stub:
    """Stops the guest for each use and lets it run again on detach."""

    def __init__(self, port):
        self.port = port

    def __enter__(self):
        self.rsp = gdbstub.Rsp(self.port)
        self.rsp.sock.settimeout(30)
        assert self.rsp.request('?').startswith('S'), 'the stub did not stop the guest'
        return self.rsp

    def __exit__(self, *exc):
        self.rsp.send('D')
        self.rsp.packet()
        self.rsp.sock.close()


def word(rsp, address):
    return struct.unpack('<I', rsp.read(address, 4))[0]


def patch(port):
    """Writes the hooks and points the table at them, with the CPU stopped outside 68k code."""
    for _ in range(100):
        with Stub(port) as rsp:
            for trap, original, slot in HOOKS + (PLAIN,):
                entry = word(rsp, TABLE + (trap & 0xfff))
                assert entry == original, f'trap {trap:#06x} is {entry:#010x}, not the T|X ROM routine'
                size = 32 if (trap, original, slot) != PLAIN else 8
                assert rsp.read(TABLE + (slot & 0xfff), size) == bytes(size), f'slots at {slot:#06x} are in use'
            # Writes are refused while PACE runs 68k code; try again at another stop.
            if rsp.request(f'M{TABLE + (HOOKS[0][2] & 0xfff):x},4:{counting_hook(HOOKS[0][1])[:4].hex()}') != 'OK':
                time.sleep(0.2)
                continue
            for trap, original, slot in HOOKS:
                rsp.write(TABLE + (slot & 0xfff), counting_hook(original))
            rsp.write(TABLE + (PLAIN[2] & 0xfff), plain_hook(PLAIN[1]))
            for trap, original, slot in HOOKS + (PLAIN,):
                rsp.write(TABLE + (trap & 0xfff), struct.pack('<I', TABLE + (slot & 0xfff)))
            return
    raise AssertionError('the guest never stopped outside 68k code')


def counts(port):
    with Stub(port) as rsp:
        for trap, _, slot in HOOKS + (PLAIN,):
            assert word(rsp, TABLE + (trap & 0xfff)) == TABLE + (slot & 0xfff), f'trap {trap:#06x} lost its hook'
        return {trap: word(rsp, TABLE + (slot & 0xfff) + 24) for trap, _, slot in HOOKS}


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def run(session, root):
    core = Path(os.environ.get('FROND_CORE') or ROOT / 'out/frond')
    port = free_port()

    def release():
        # --gdb waits for a debugger before the first instruction; let it run.
        for _ in range(600):
            try:
                with Stub(port):
                    return
            except OSError:
                time.sleep(0.5)

    threading.Thread(target=release, daemon=True).start()
    c = core_control.Client(core, [session, '--device', 'palm-tx', '--gdb', port], root, 'palm-tx')
    notes = []
    try:
        c.until_ok('guest-ready', timeout=180)
        patch(port)
        time.sleep(3)
        before = counts(port)
        notes.append('guest calls through the hooks ' + ', '.join(f'{t:#06x} {n}' for t, n in before.items()))
        if not before[0x84b8]:
            raise AssertionError('the guest made no MemChunkFree call through its hook')
        c.until_ok('guest-ready')
        listing = c.ok('db-list', code='databases')
        assert 'MemoDB'.encode().hex() in listing, 'MemoDB not listed'
        c.ok('install', core_control.PATTERN, code=('installed', 'needs-reset'))
        assert core_control.PATTERN_NAME.encode().hex() in c.ok('db-list'), 'installed database not listed'
        one = root / 'one.zip'
        c.ok('export-db', f'{one}\n{core_control.PATTERN_NAME}', code='exported')
        with zipfile.ZipFile(one) as z:
            assert any('frond-pattern' in name for name in z.namelist()), z.namelist()
        c.ok('launch-app', 'Memo Pad', code='launched')
        # Memo Pad waits with no timeout, so guest-ready says not-ready from here; host calls wake it.
        time.sleep(3)
        notes.append('db-list, install, export-db, launch-app ok')
        if PROBE.is_file():
            c.ok('install', PROBE, code=('installed', 'needs-reset'))
            time.sleep(3)
            assert 'FrondSyncLog'.encode().hex() in c.ok('db-list'), 'the probe got no sync notify'
            notes.append('sync notify ok')
        else:
            notes.append('sync notify SKIPPED, no probe')
        after = counts(port)
        notes.append('hook calls after ' + ', '.join(f'{t:#06x} {n}' for t, n in after.items()))
        c.quit()
    finally:
        c.close()
    log = (root / 'palm-tx.log').read_text(errors='replace')
    if 'calling its ROM routine' not in log:
        raise AssertionError('the core did not report the patched traps')
    return '; '.join(notes)


def main():
    folder = os.environ.get('FROND_ARM_SESSIONS')
    required = os.environ.get('FROND_REQUIRE_SESSIONS') == '1'
    session = next((p for p in ([Path(folder) / 'palm-tx'] if folder else []) + [ROOT / 'build/smoke/machines/palm-tx']
                    if p.exists()), None)
    if not session:
        print('patched traps: palm-tx SKIPPED, no session')
        return 1 if required else 0
    with tempfile.TemporaryDirectory(prefix='patched-traps-', dir=ROOT / 'build') as work:
        copy = Path(work) / 'palm-tx'
        shutil.copytree(session, copy)
        frond = sorted(copy.glob('*.frond'))
        try:
            print(f'patched traps: palm-tx ok, {run(frond[0] if frond else copy, Path(work))}', flush=True)
        except AssertionError as error:
            print(f'patched traps: palm-tx FAIL, {error}', flush=True)
            log = (Path(work) / 'palm-tx.log').read_text(errors='replace').splitlines()
            print('\n'.join(line for line in log if 'native' in line or 'error' in line.lower())[-2000:])
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
