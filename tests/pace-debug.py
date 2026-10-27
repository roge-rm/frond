#!/usr/bin/env python3
"""Real PACE/ARM execution and debugger protocol, using synthetic RAM only."""
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import unittest

spec = importlib.util.spec_from_file_location('gdbstub_tests', Path(__file__).with_name('gdbstub.py'))
gdb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gdb)


class PaceDebugger(unittest.TestCase):
    target_args = ['--pace']
    setUp = gdb.Debugger.setUp
    cleanup_target = gdb.Debugger.cleanup_target

    def monitor(self, command):
        status, output = self.rsp.monitor(command)
        self.assertEqual(status, 'OK')
        return json.loads(output) if output else None

    def enter(self, program=None, pc=0x31000, a0=0):
        if program is not None:
            self.rsp.write(0x31000, bytes.fromhex(program))
        self.rsp.write(0x30000 + 17 * 4, struct.pack('<I', pc))
        self.rsp.write(0x30000 + 9 * 4, struct.pack('<I', a0))
        self.rsp.set_reg(0, 0x30000)
        self.rsp.set_reg(13, 0x35000)
        self.rsp.set_reg(15, 0x14000)

    def test_attribution_requires_qualified_soc_and_preserves_stop(self):
        def inspect():
            registers, context = self.rsp.request('g'), self.monitor('pace')
            counts = self.rsp.read(0x21000, 8)
            value = self.monitor('attribution')
            self.assertEqual(value['version'], 1)
            self.assertFalse(value['foreground_app']['available'])
            self.assertFalse(value['code_owner']['available'])
            self.assertEqual(self.monitor('attribution'), value)
            self.assertEqual(self.rsp.request('g'), registers)
            self.assertEqual(self.monitor('pace'), context)
            self.assertEqual(self.rsp.read(0x21000, 8), counts)
        inspect()
        self.enter()
        self.monitor('pace-break 31000')
        self.assertEqual(self.rsp.request('c'), 'S05')
        self.assertTrue(self.monitor('pace')['available'])
        inspect()

    def test_registers_breakpoints_and_native_guards(self):
        self.assertFalse(self.monitor('pace')['available'])
        self.assertFalse(self.monitor('pace-last')['available'])
        self.rsp.send('qRcmd,' + b'pace'.hex())
        reply = self.rsp.packet()
        self.rsp.sock.sendall(b'-')
        self.assertEqual(self.rsp.packet(), reply)
        self.enter()
        self.monitor('pace-break 31002')
        self.monitor('pace-break 31002')  # idempotent
        self.assertEqual(self.rsp.request('c'), 'S05')
        state = self.monitor('pace')
        self.assertEqual(state['phase'], 'boundary')
        self.assertIsNone(state['program_identity'])
        self.assertEqual(state['pc'], 0x31002)
        self.assertEqual(state['d'], [7] + [0] * 7)
        self.assertEqual(state['a'], [0] * 7 + [0x34000])
        self.assertEqual(state['sr'], 0x2010)
        self.assertEqual(state['state_pointer'], 0x30000)
        # The original instruction bytes are intact; repeated inspection is inert.
        self.assertEqual(self.rsp.read(0x31000, 6).hex(), '7007528060fc')
        for cmd in ['s', 'vCont;s', 'vCont;s:1', 'c10000', 'P0=00000000',
                    'G' + '00' * 68, 'M31000,2:0000']:
            self.assertEqual(self.rsp.request(cmd), 'E16', cmd)
        self.assertEqual(self.monitor('pace'), state)
        self.assertEqual(self.rsp.request('c'), 'S05') # still at installed breakpoint
        self.assertEqual(self.monitor('pace'), state)
        self.monitor('pace-clear')
        self.monitor('pace-break 31004')
        self.assertEqual(self.rsp.request('c'), 'S05')
        self.assertEqual(self.monitor('pace')['d'][0], 8)
        self.assertEqual(self.monitor('pace')['pc'], 0x31004)

    def test_interrupt_reconnect_and_native_breakpoint_isolation(self):
        self.enter()
        # Native Thumb breakpoint must not match the same 68k address.
        self.assertEqual(self.rsp.request('Z0,31002,2'), 'OK')
        self.rsp.send('c')
        self.rsp.sock.sendall(b'\x03')
        self.assertEqual(self.rsp.packet(), 'S02')
        self.assertTrue(self.monitor('pace')['available'])
        self.monitor('pace-stop on')
        self.monitor('pace-break 31002')
        self.assertEqual(self.rsp.request('D'), 'OK')
        self.rsp.sock.close()
        self.rsp = gdb.Rsp(self.port)
        self.assertEqual(self.rsp.request('?'), 'S05')
        self.assertFalse(self.monitor('pace-last')['available'])
        self.rsp.send('c')
        self.rsp.sock.sendall(b'\x03')
        self.assertEqual(self.rsp.packet(), 'S02') # breakpoint cleared on detach

    def test_exception_context_before_native_dispatch(self):
        for program, expected in [('4afc', 4), ('80fc0000', 5), ('a123', 10), ('ffff', 11)]:
            with self.subTest(program=program):
                self.enter(program)
                self.monitor('pace-stop on')
                self.assertEqual(self.rsp.request('c'), 'S05')
                last = self.monitor('pace-last')
                self.assertEqual(last['status'], expected)
                self.assertEqual(last['instruction_pc'], 0x31000)
                self.assertEqual(last['phase'], 'exception')
                self.assertEqual(self.monitor('pace')['phase'], 'exception')
                self.assertEqual(self.rsp.request('Z0,15000,4'), 'OK')
                self.assertEqual(self.rsp.request('c'), 'S05')
                self.assertEqual(self.rsp.reg(15), 0x15000)
                self.assertFalse(self.monitor('pace')['available'])
                self.assertEqual(self.monitor('pace-last'), last)
                self.assertEqual(self.rsp.request('z0,15000,4'), 'OK')

    def test_fault_context_and_partial_access_not_replayed(self):
        # MOVE.L D0,(A0)+ crosses the end of backing RAM. First half is written;
        # the faulting second half and post-instruction registers are retained.
        self.rsp.write(0x30004, struct.pack('<I', 0x12345678))
        self.enter('20c0', a0=0xffffe)
        self.monitor('pace-stop on')
        self.assertEqual(self.rsp.request('c'), 'S05')
        last = self.monitor('pace-last')
        self.assertEqual(last['instruction_pc'], 0x31000)
        self.assertEqual(last['status'], 49)
        self.assertEqual(last['fault_address'], 0x100000)
        self.assertTrue(last['fault_write'])
        self.assertNotEqual(last['fault_status'], 0)
        self.assertEqual(self.rsp.read(0xffffe, 2).hex(), '1234')
        self.assertEqual(self.monitor('pace-last'), last)
        self.assertEqual(self.rsp.request('Z0,10,4'), 'OK')
        self.assertEqual(self.rsp.request('c'), 'S05')
        self.assertEqual(self.rsp.reg(25) & 31, 0x17) # normal ARM abort dispatch
        self.assertEqual(self.monitor('pace-last'), last)
        self.assertEqual(struct.unpack('<I', self.rsp.read(0x21004, 4))[0], 1)

    def test_fetch_fault_and_native_step_into_pace(self):
        self.enter(pc=0x200000)
        self.assertEqual(self.rsp.request('s'), 'S05')
        state = self.monitor('pace')
        self.assertEqual(state['pc'], 0x200000) # native entry completed, no 68k instruction yet
        self.monitor('pace-stop on')
        self.assertEqual(self.rsp.request('c'), 'S05')
        last = self.monitor('pace-last')
        self.assertEqual(last['fault_address'], 0x200000)
        self.assertFalse(last['fault_write'])
        self.assertEqual(last['pc'], 0x200000)

    def test_reset_and_restore_clear_debugger_metadata(self):
        for action in (1, 2):
            with self.subTest(action=action):
                self.enter('4afc')
                self.monitor('pace-stop on')
                self.monitor('pace-break 31000')
                self.assertEqual(self.rsp.request('c'), 'S05') # pre-instruction
                self.monitor('pace-clear')
                self.assertEqual(self.rsp.request('c'), 'S05') # exception
                self.assertTrue(self.monitor('pace-last')['available'])
                self.monitor('pace-break 31000')
                self.assertEqual(self.rsp.request('Z0,15000,4'), 'OK')
                self.assertEqual(self.rsp.request('c'), 'S05')
                self.assertFalse(self.monitor('pace')['available'])
                self.rsp.set_reg(15, 0x10008)
                self.assertEqual(self.rsp.request('Z0,10000,4'), 'OK')
                self.rsp.write(0x22000, struct.pack('<I', action))
                self.assertEqual(self.rsp.request('c'), 'S05')
                self.assertEqual(self.rsp.reg(15), 0x10000)
                self.assertFalse(self.monitor('attribution')['foreground_app']['available'])
                self.assertFalse(self.monitor('pace-last')['available'])
                self.enter('4afc')
                self.assertEqual(self.rsp.request('c'), 'S05')
                # Neither the old PACE breakpoint nor exception-stop fires;
                # ordinary native breakpoints survive reset/session load.
                self.assertEqual(self.rsp.reg(15), 0x15000)
                self.assertFalse(self.monitor('pace')['available'])
                self.assertEqual(self.monitor('pace-last')['status'], 4)

    def test_monitor_validation_and_bounds(self):
        for cmd in ['pace-break 1', 'pace-break 100000000', 'pace-break 0x1234',
                    'pace-break ', 'pace-break 12 junk', 'pace-stop maybe', 'unknown', 'pace\0']:
            self.assertEqual(self.rsp.monitor(cmd)[0], 'E01', cmd)
        for cmd in ['qRcmd,0', 'qRcmd,zz', 'qRcmd,' + 'ff' * 2045]:
            self.assertEqual(self.rsp.request(cmd), 'E01')
        for addr in range(0, 32, 2):
            self.monitor(f'pace-break {addr:x}')
        self.monitor('pace-break 0')
        self.assertEqual(self.rsp.monitor('pace-break 100')[0], 'E05')
        self.monitor('pace-clear')
        self.monitor('pace-break 100')

    @unittest.skipUnless(gdb.GDB, 'gdb-multiarch not installed')
    def test_real_gdb_monitor(self):
        self.assertEqual(self.rsp.request('D'), 'OK')
        self.rsp.sock.close()
        result = subprocess.run([gdb.GDB, '-nx', '-batch',
            '-ex', f'target remote 127.0.0.1:{self.port}',
            '-ex', 'set $r0 = 0x30000', '-ex', 'set $sp = 0x35000',
            '-ex', 'set $pc = 0x14000', '-ex', 'monitor pace-break 31002',
            '-ex', 'continue', '-ex', 'monitor pace', '-ex', 'monitor pace-clear',
            '-ex', 'detach'], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('"pc":200706', result.stdout + result.stderr)
        self.assertIn('"d":[7,', result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
