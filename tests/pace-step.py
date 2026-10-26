#!/usr/bin/env python3
"""One-attempt 68k stepping through the actual optimized interpreter and GDB."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import unittest

spec = importlib.util.spec_from_file_location('pace_debug', Path(__file__).with_name('pace-debug.py'))
pace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pace)
gdb = pace.gdb


class PaceStep(unittest.TestCase):
    target_args = ['--pace']
    setUp = pace.PaceDebugger.setUp
    cleanup_target = pace.PaceDebugger.cleanup_target
    monitor = pace.PaceDebugger.monitor
    enter = pace.PaceDebugger.enter

    def boundary(self, program=None, pc=0x31000, a0=0):
        self.enter(program, pc, a0)
        self.monitor(f'pace-break {pc:x}')
        self.assertEqual(self.rsp.request('c'), 'S05')

    def step(self, command='c'):
        self.monitor('pace-step')
        before = self.monitor('pace')
        self.assertEqual(self.monitor('pace-last-step')['reason'], 'armed')
        self.assertEqual(self.monitor('pace'), before) # arming never executes
        self.assertEqual(self.rsp.request(command), 'S05')
        result = self.monitor('pace-last-step')
        self.assertTrue(result['available'])
        self.assertEqual(result['instruction_pc'], before['pc'])
        return result

    def test_variable_length_and_breakpoint_step_over(self):
        self.boundary('203c12345678528060fc') # MOVE.L #imm,D0; ADDQ.L; BRA
        first = self.step()
        self.assertEqual((first['pc'], first['d'][0], first['status']), (0x31006, 0x12345678, 0))
        self.assertEqual(first['phase'], 'boundary')
        self.monitor('pace-break 31006')
        self.assertEqual(self.rsp.request('c'), 'S05') # not armed: breakpoint remains
        self.assertEqual(self.monitor('pace')['d'][0], 0x12345678)
        second = self.step('vCont;c:1')
        self.assertEqual((second['pc'], second['d'][0]), (0x31008, 0x12345679))
        branch = self.step('vCont;c')
        self.assertEqual(branch['pc'], 0x31006)
        self.assertEqual(branch['d'][0], 0x12345679)
        # NACK replays only the monitor reply, never the instruction.
        self.rsp.send('qRcmd,' + b'pace-last-step'.hex())
        reply = self.rsp.packet()
        self.rsp.sock.sendall(b'-')
        self.assertEqual(self.rsp.packet(), reply)
        self.assertEqual(self.monitor('pace')['d'][0], 0x12345679)

    def test_conditional_branch_call_return_and_pace_exit(self):
        self.rsp.write(0x30000 + 16 * 4, struct.pack('<I', 0x33ffc))
        self.rsp.write(0x33ffc, bytes.fromhex('fffffff0'))
        self.rsp.set_reg(14, 0x15000)
        self.boundary('70077209d0816602700061024e7552804e75')
        expected = [(0x31002, 7, 0x33ffc), (0x31004, 7, 0x33ffc),
                    (0x31006, 16, 0x33ffc), (0x3100a, 16, 0x33ffc),
                    (0x3100e, 16, 0x33ff8), (0x31010, 17, 0x33ff8),
                    (0x3100c, 17, 0x33ffc)]
        for pc, d0, sp in expected:
            value = self.step()
            self.assertEqual((value['pc'], value['d'][0], value['a'][7]), (pc, d0, sp))
            self.assertEqual(value['status'], 0)
        returned = self.step()
        self.assertEqual((returned['pc'], returned['status'], returned['phase']),
                         (0xfffffff0, 50, 'dispatch'))
        self.assertEqual(self.monitor('pace')['phase'], 'dispatch')
        self.assertEqual(self.rsp.monitor('pace-step')[0], 'E16')
        self.assertEqual(self.rsp.request('Z0,15000,4'), 'OK')
        self.assertEqual(self.rsp.request('c'), 'S05')
        self.assertEqual(self.rsp.reg(15), 0x15000)
        self.assertFalse(self.monitor('pace')['available'])
        self.assertEqual(self.monitor('pace-last-step'), returned)

    def test_exception_and_trap_dispatch(self):
        cases = [('4afc', 4, 'exception'), ('80fc0000', 5, 'exception'),
                 ('a123', 10, 'exception'), ('ffff', 11, 'exception'),
                 ('4e4fa0c7', 47, 'dispatch'), ('4e40', 32, 'dispatch'), ('4e48', 40, 'dispatch')]
        for program, status, phase in cases:
            with self.subTest(program=program):
                self.boundary(program)
                self.monitor('pace-stop on') # one stop even with both options
                value = self.step()
                self.assertEqual((value['status'], value['phase']), (status, phase))
                self.assertEqual(self.monitor('pace')['phase'], phase)
                self.assertEqual(self.rsp.monitor('pace-step')[0], 'E16')
                self.monitor('pace-clear')
                self.assertEqual(self.rsp.request('Z0,15000,4'), 'OK')
                self.assertEqual(self.rsp.request('c'), 'S05')
                self.assertEqual(self.rsp.reg(15), 0x15000)
                self.assertFalse(self.monitor('pace')['available'])
                self.assertEqual(self.monitor('pace-last-step'), value)

    def test_partial_fault_and_fetch_fault(self):
        self.rsp.write(0x30004, struct.pack('<I', 0x12345678))
        self.boundary('20c0', a0=0xffffe)
        value = self.step()
        self.assertEqual((value['status'], value['fault_address'], value['fault_write']),
                         (49, 0x100000, True))
        self.assertEqual(self.rsp.read(0xffffe, 2).hex(), '1234')
        self.assertEqual(self.monitor('pace-last'), value)
        self.assertEqual(self.rsp.monitor('pace-step')[0], 'E16')
        self.assertEqual(self.rsp.request('Z0,10,4'), 'OK')
        self.assertEqual(self.rsp.request('c'), 'S05')
        self.assertEqual(self.rsp.reg(25) & 31, 0x17)
        self.assertEqual(struct.unpack('<I', self.rsp.read(0x21004, 4))[0], 1)
        self.boundary(pc=0x200000)
        value = self.step()
        self.assertEqual((value['status'], value['pc'], value['fault_address'], value['fault_write']),
                         (49, 0x200000, 0x200000, False))

    def test_partial_movem_does_not_replay_completed_store(self):
        self.rsp.write(0x30004, struct.pack('<II', 0x12345678, 0x90abcdef))
        self.boundary('48d00003', a0=0xffffc) # MOVEM.L D0-D1,(A0)
        value = self.step()
        self.assertEqual((value['status'], value['fault_address'], value['fault_write']),
                         (49, 0x100000, True))
        self.assertEqual(self.rsp.read(0xffffc, 4).hex(), '12345678')
        self.assertEqual(self.rsp.request('Z0,10,4'), 'OK')
        self.assertEqual(self.rsp.request('c'), 'S05')
        self.assertEqual(struct.unpack('<I', self.rsp.read(0x21004, 4))[0], 1)

    def test_extension_fetch_fault(self):
        self.rsp.write(0xffffe, bytes.fromhex('203c')) # MOVE.L #imm,D0, missing immediate
        self.boundary(pc=0xffffe)
        value = self.step()
        self.assertEqual((value['status'], value['instruction_pc'], value['fault_address'], value['fault_write']),
                         (49, 0xffffe, 0x100000, False))
        self.assertEqual(self.rsp.monitor('pace-step')[0], 'E16')

    def test_self_branch_armed_cancel_and_native_guards(self):
        self.assertEqual(self.rsp.monitor('pace-step')[0], 'E16')
        self.boundary('60fe') # BRA . must still complete one attempt
        for _ in range(3):
            self.assertEqual(self.step()['pc'], 0x31000)
        self.monitor('pace-step')
        for command in ['s', 'vCont;s', 'vCont;s:1', 'c31000', 'P0=00000000']:
            self.assertEqual(self.rsp.request(command), 'E16')
        self.assertEqual(self.monitor('pace-last-step')['reason'], 'armed')
        self.monitor('pace-step off')
        self.assertEqual(self.monitor('pace-last-step')['reason'], 'cancelled')
        self.assertEqual(self.rsp.request('c'), 'S05') # old breakpoint, not a step
        self.assertEqual(self.monitor('pace-last-step')['reason'], 'cancelled')

    def test_ctrl_c_cancels_before_attempt(self):
        self.boundary()
        for after_step in (False, True):
            if after_step:
                self.step()
            before = self.monitor('pace')
            self.monitor('pace-step')
            self.rsp.sock.sendall(gdb.frame('c') + b'\x03')
            self.assertEqual(self.rsp.byte(), b'+')
            self.assertEqual(self.rsp.packet(), 'S02')
            self.assertEqual(self.monitor('pace'), before)
            self.assertEqual(self.monitor('pace-last-step')['reason'], 'interrupted-before-instruction')

    def test_pending_guest_irq_does_not_step_native_handler(self):
        # A synthetic MMIO write raises IRQ during the first interpreter step.
        self.boundary('20c0705560fe', a0=0x22008)
        first = self.step()
        self.assertEqual((first['status'], first['pc']), (0, 0x31002))
        self.monitor('pace-step')
        self.assertEqual(self.rsp.request('c'), 'S05')
        self.assertEqual(self.monitor('pace-last-step')['reason'], 'interrupted-before-instruction')
        self.assertFalse(self.monitor('pace')['available'])
        self.assertEqual(self.rsp.reg(15), 0x18)
        self.assertEqual(self.rsp.reg(6), 0) # IRQ sentinel hasn't executed
        self.assertEqual(self.rsp.request('Z0,1c,4'), 'OK')
        self.assertEqual(self.rsp.request('c'), 'S05')
        self.assertEqual(self.rsp.reg(6), 0x55)

    def test_detach_eof_and_reconnect_discard_armed_step(self):
        self.boundary()
        for disconnect in ('detach', 'eof-armed', 'eof-running'):
            self.monitor('pace-step')
            if disconnect == 'detach':
                self.assertEqual(self.rsp.request('D'), 'OK')
            elif disconnect == 'eof-running':
                self.rsp.send('c')
            self.rsp.sock.close()
            self.rsp = gdb.Rsp(self.port)
            self.assertEqual(self.rsp.request('?'), 'S05')
            self.assertEqual(self.monitor('pace-last-step'), {'version': 1, 'available': False})
            self.rsp.send('c')
            self.rsp.sock.sendall(b'\x03')
            self.assertEqual(self.rsp.packet(), 'S02')
            self.assertTrue(self.monitor('pace')['available'])

    def test_reset_restore_discard_completed_step(self):
        for action in (1, 2):
            self.boundary('4afc')
            self.step()
            self.assertEqual(self.rsp.request('Z0,15000,4'), 'OK')
            self.assertEqual(self.rsp.request('c'), 'S05')
            self.rsp.set_reg(15, 0x10008)
            self.assertEqual(self.rsp.request('Z0,10000,4'), 'OK')
            self.rsp.write(0x22000, struct.pack('<I', action))
            self.assertEqual(self.rsp.request('c'), 'S05')
            self.assertEqual(self.monitor('pace-last-step'), {'version': 1, 'available': False})
            self.enter('4afc')
            self.assertEqual(self.rsp.request('c'), 'S05')
            self.assertEqual(self.rsp.reg(15), 0x15000) # no old step or breakpoint

    def test_reset_restore_cancel_inflight_step(self):
        for action in (1, 2):
            # The first stepped MMIO store requests a reset/restore after this
            # CPU batch. Arm a second step before allowing that request to run.
            self.rsp.write(0x30004, struct.pack('<I', action << 24)) # guest store is big endian
            self.boundary('20c0705560fe', a0=0x22000)
            self.step()
            self.monitor('pace-step')
            self.assertEqual(self.rsp.request('Z0,10000,4'), 'OK')
            self.assertEqual(self.rsp.request('c'), 'S05')
            self.assertEqual(self.rsp.reg(15), 0x10000)
            self.assertEqual(self.monitor('pace-last-step'), {'version': 1, 'available': False})
            self.assertFalse(self.monitor('pace')['available'])

    @unittest.skipUnless(gdb.GDB, 'gdb-multiarch required for client proof')
    def test_real_gdb_partial_write_and_abort_dispatch(self):
        self.enter('20c0', a0=0xffffe)
        self.rsp.write(0x30004, struct.pack('<I', 0x12345678))
        self.rsp.set_reg(15, 0x10008)
        self.assertEqual(self.rsp.request('D'), 'OK')
        self.rsp.sock.close()
        commands = [f'target remote 127.0.0.1:{self.port}',
                    'set $r0 = 0x30000', 'set $sp = 0x35000', 'set $pc = 0x14000',
                    'monitor pace-break 31000', 'continue', 'monitor pace-step', 'continue',
                    'monitor pace-last-step', 'x/hx 0xffffe', 'break *0x10', 'continue',
                    'printf "ABORT_PC=%x STORES=%u\\n", $pc, *(unsigned int*)0x21004', 'detach']
        result = subprocess.run([gdb.GDB, '-nx', '-batch'] +
                                [arg for command in commands for arg in ('-ex', command)],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        output = result.stdout + result.stderr
        self.assertIn('"phase":"exception"', output)
        self.assertIn('"status":49', output)
        self.assertIn('"fault_address":1048576', output)
        self.assertIn('0x3412', output) # ARM GDB displays the big-endian halfword little endian
        self.assertIn('ABORT_PC=10 STORES=1', output)

    @unittest.skipUnless(gdb.GDB, 'gdb-multiarch required for client proof')
    def test_real_gdb_one_store_and_no_extra_instruction(self):
        # A store followed by a second counted store: extra stepping is observable.
        self.enter('20c020c060fe', a0=0x20000)
        self.rsp.write(0x30004, struct.pack('<I', 0x12345678))
        self.rsp.set_reg(15, 0x10008)
        self.assertEqual(self.rsp.request('D'), 'OK')
        self.rsp.sock.close()
        commands = [f'target remote 127.0.0.1:{self.port}',
                    'set $r0 = 0x30000', 'set $sp = 0x35000', 'set $pc = 0x14000',
                    'monitor pace-break 31000', 'continue', 'monitor pace',
                    'monitor pace-step', 'continue', 'monitor pace-last-step',
                    'x/wx 0x21004', 'monitor pace-step', 'continue',
                    'monitor pace-last-step', 'x/wx 0x21004', 'detach']
        result = subprocess.run([gdb.GDB, '-nx', '-batch'] +
                                [arg for command in commands for arg in ('-ex', command)],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        output = result.stdout + result.stderr
        self.assertIn('"pc":200706', output)
        self.assertIn('"pc":200708', output)
        self.assertIn('0x00000001', output)
        self.assertIn('0x00000002', output)


if __name__ == '__main__':
    unittest.main()
