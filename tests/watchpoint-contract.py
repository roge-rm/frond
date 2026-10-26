#!/usr/bin/env python3
"""Diagnostic only: reproduce ARM GDB's extra step for a post-access watch stop.

A test-local RSP adapter converts a native breakpoint AFTER a real ARM store to
T05watch. No watchpoint support is added to the emulator. The actual CPU runs
both the store and any step GDB issues. A passing test confirms the blocker,
not a working watchpoint implementation.
"""
import importlib.util
from pathlib import Path
import socket
import subprocess
import threading
import unittest

spec = importlib.util.spec_from_file_location('gdbstub_tests', Path(__file__).with_name('gdbstub.py'))
gdb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gdb)


class PostAccessAdapter:
    def __init__(self, backend_port):
        self.backend_port = backend_port
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.listener.settimeout(10)
        self.port = self.listener.getsockname()[1]
        self.commands = []
        self.error = None
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        backend = client = None
        try:
            client, _ = self.listener.accept()
            client.settimeout(10)
            peer = gdb.Rsp.__new__(gdb.Rsp)
            peer.sock = client
            backend = gdb.Rsp(self.backend_port)
            watch = False
            last = b''
            for _ in range(1024): # bounded synthetic protocol conversation
                lead = client.recv(1, socket.MSG_PEEK)
                if not lead:
                    break
                if lead in (b'+', b'-'):
                    client.recv(1)
                    if lead == b'-' and last:
                        client.sendall(last)
                    continue
                command = peer.packet() # validates checksum and ACKs it
                self.commands.append(command)
                if command in ('Z2,20000,4', 'z2,20000,4'):
                    watch = command.startswith('Z')
                    result = backend.request(('Z' if watch else 'z') + '0,12004,4')
                else:
                    result = backend.request(command)
                    if watch and command in ('c', 'vCont;c', 'vCont;c:1') and result == 'S05':
                        assert backend.reg(15) == 0x12004
                        # The store completed. Mislabel its next boundary as an
                        # ARM hardware watchpoint, exactly the rejected contract.
                        self.commands.append('POST_ACCESS_WATCH_STOP_PC=12004')
                        result = 'T05watch:20000;'
                last = gdb.frame(result)
                client.sendall(last)
                if command == 'D':
                    break
            else:
                raise RuntimeError('adapter packet budget exhausted')
        except Exception as exc:
            self.error = exc
        finally:
            if backend:
                backend.sock.close()
            if client:
                client.close()
            self.listener.close()


@unittest.skipUnless(gdb.GDB, 'gdb-multiarch required to reproduce watchpoint client contract')
class WatchpointContract(unittest.TestCase):
    target_args = ['--pace']
    setUp = gdb.Debugger.setUp
    cleanup_target = gdb.Debugger.cleanup_target

    def test_post_access_reply_causes_one_extra_native_instruction(self):
        self.assertEqual(self.rsp.request('Z2,20000,4'), '') # product still rejects it
        self.assertEqual(self.rsp.request('D'), 'OK')
        self.rsp.sock.close()
        adapter = PostAccessAdapter(self.port)
        commands = [f'target remote 127.0.0.1:{adapter.port}', 'set $cpsr = 0x13',
                    'set $pc = 0x12000', 'set $r2 = 0x20000', 'set $r1 = 42', 'set $r3 = 0',
                    'watch *(unsigned int*)0x20000', 'continue',
                    'printf "WATCH_PC=%x NEXT_LOAD=%x WRITES=%u\\n", $pc, $r3, *(unsigned int*)0x21004',
                    'detach']
        try:
            result = subprocess.run([gdb.GDB, '-nx', '-batch'] +
                                    [arg for cmd in commands for arg in ('-ex', cmd)],
                                    capture_output=True, text=True, timeout=20)
        finally:
            adapter.thread.join(timeout=3)
        self.assertFalse(adapter.thread.is_alive(), 'adapter failed to finish')
        self.assertIsNone(adapter.error, repr(adapter.error))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        output = result.stdout + result.stderr
        self.assertIn('WATCH_PC=12008 NEXT_LOAD=11223344 WRITES=1', output)
        stop = adapter.commands.index('POST_ACCESS_WATCH_STOP_PC=12004')
        followup = adapter.commands[stop + 1:]
        automatic_resumes = [cmd for cmd in followup
                             if cmd in ('s', 'c') or cmd.startswith(('vCont;s', 'vCont;c'))]
        self.assertEqual(len(automatic_resumes), 1, adapter.commands)
        if automatic_resumes[0] in ('c', 'vCont;c', 'vCont;c:1'):
            # ARM GDB can implement this extra step as a temporary breakpoint
            # plus continue, not an RSP s packet. Swallowing s cannot fix it.
            self.assertIn('Z0,12008,4', followup)
            self.assertLess(followup.index('Z0,12008,4'), followup.index(automatic_resumes[0]))
        print('DIAGNOSTIC: post-access stop at 0x12004 -> GDB automatic ' + automatic_resumes[0] +
              ' -> 0x12008; next load executed, store count=1. Watchpoints remain unsupported.')


if __name__ == '__main__':
    unittest.main()
