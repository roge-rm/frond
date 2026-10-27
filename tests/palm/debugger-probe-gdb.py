"""Sourced by real GDB; private TX acceptance, never a synthetic RSP peer."""
import hashlib
import json
import os
from pathlib import Path
import struct
import traceback
import gdb

config = json.loads(Path(os.environ['FROND_TX_DEBUG_CONFIG']).read_text())
output = Path(config['output'])
events = []
result = {'status': 'error', 'events': events}


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def command(text):
    event = {'command': text}
    events.append(event)
    try:
        event['output'] = gdb.execute(text, to_string=True)
    except gdb.error as exc:
        event['error'] = str(exc)
        raise
    return event['output']


def monitor(text):
    answer = command('monitor ' + text)
    return json.loads(answer) if answer.lstrip().startswith('{') else answer


def memory(address, size):
    return bytes(gdb.selected_inferior().read_memory(address, size))


def step(offset, next_offset, d0=None, status=0, phase='boundary'):
    before = monitor('pace')
    check(before['pc'] == entry + offset, 'wrong pre-step PC')
    monitor('pace-step')
    check(monitor('pace') == before, 'arming executed an instruction')
    check(monitor('pace-last-step')['reason'] == 'armed', 'missing armed state')
    command('continue')
    value = monitor('pace-last-step')
    check(value['available'] and value['instruction_pc'] == entry + offset,
          'wrong attempted instruction')
    check((value['pc'], value['status'], value['phase']) ==
          (entry + next_offset, status, phase), 'wrong step outcome: ' + str(value))
    if d0 is not None:
        check(value['d'][0] == d0, 'wrong D0 after step')
    return value


try:
    command('set pagination off')
    command('set confirm off')
    command('set architecture arm')
    command('set endian little')
    command('set remotetimeout 120')
    command('target remote 127.0.0.1:' + str(config['port']))
    descriptor = config['descriptor']
    code, size, entry, exception = [descriptor[k] for k in ('code', 'codeSize', 'entry', 'exception')]
    # Full resource equality, with a unique PRC resource match, is required.
    live = memory(code, size)
    local = Path(config['probe']).read_bytes()
    count = int.from_bytes(local[76:78], 'big')
    directory = [struct.unpack_from('>4sHI', local, 78 + i*10) for i in range(count)]
    matches = []
    for i, (kind, identifier, offset) in enumerate(directory):
        end = directory[i+1][2] if i+1 < count else len(local)
        if local[offset:end] == live:
            matches.append((kind.hex(), identifier))
    check(matches == [('636f6465', 1)], 'live code must uniquely match local code/1')
    check(code <= entry < exception < code + size, 'entry outside verified resource')
    monitor('pace-break %x' % entry)
    command('continue')
    context = monitor('pace')
    check(context['available'] and context['pc'] == entry, 'code breakpoint did not hit')
    check(monitor('pace') == context, 'snapshot changed while stopped')
    mapping = {'version': 1, 'source': 'probe-descriptor-and-exact-live-resource-match',
               'program_sha256': hashlib.sha256(local).hexdigest(),
               'context_pc': context['pc'], 'context': context,
               'resources': [{'type_hex': matches[0][0], 'id': matches[0][1],
                              'address': code, 'size': size,
                              'sha256': hashlib.sha256(live).hexdigest()}]}
    (output / 'runtime-map.json').write_text(json.dumps(mapping, indent=2) + '\n')
    (output / 'pace-context.json').write_text(json.dumps(context, indent=2) + '\n')
    (output / 'live-code.bin').write_bytes(live)
    value = step(0, 4)
    witness = value['a'][0]
    check(memory(witness, 16) == b'FrDBSTEP-RAM-v01', 'wrong live descriptor')
    check(struct.unpack('>5I', memory(witness+16, 20)) ==
          (1, code, size, entry, exception), 'live descriptor differs from checkpoint')
    check(exception == entry+0x40, 'unexpected probe assembly layout')
    calls = int.from_bytes(memory(witness+36, 4), 'big')
    step(4, 8)
    check(int.from_bytes(memory(witness+36, 4), 'big') == calls+1, 'counter replay/missing store')
    step(8, 0x10)
    check(memory(witness+44, 4).hex() == '01020304', 'wrong multiword store')
    monitor('pace-break %x' % (entry+0x10))
    command('continue')
    check(monitor('pace')['pc'] == entry+0x10, 'breakpoint disappeared after step')
    step(0x10, 0x12, 7)
    check(step(0x12, 0x14, 7)['d'][1] == 9, 'wrong D1 after MOVEQ')
    step(0x14, 0x16, 16)
    step(0x16, 0x1a, 16)
    check(int.from_bytes(memory(witness+40, 4), 'big') == 16, 'wrong stepped store')
    step(0x1a, 0x1e, 16)
    step(0x1e, 0x20, 17)
    step(0x20, 0x24, 17)
    check(int.from_bytes(memory(witness+40, 4), 'big') == 17, 'wrong second store')
    for first, second in [(0x24,0x26), (0x26,0x2c), (0x2c,0x30), (0x30,0x34)]:
        step(first, second)
    syscall = step(0x34, 0x38, status=47, phase='dispatch')
    check(memory(witness+44, 4).hex() == '01020304', 'syscall executed before dispatch')
    try:
        monitor('pace-step')
        raise RuntimeError('pending syscall allowed step')
    except gdb.error as exc:
        check('E16' in str(exc) or 'Rcmd: 16.' in str(exc),
              'unexpected step rejection: ' + str(exc))
    check(monitor('pace-last-step') == syscall, 'dispatch guard changed step record')
    monitor('pace-clear')
    monitor('pace-break %x' % (entry+0x38))
    command('continue')
    check(monitor('pace')['pc'] == entry+0x38, 'syscall return breakpoint missing')
    check(memory(witness+44, 4).hex() == '01010203', 'MemMove missing or dispatched more than once')
    check(int.from_bytes(memory(witness+36, 4), 'big') == calls+1, 'counter replayed during dispatch')
    check(monitor('pace-last-step') == syscall, 'syscall dispatch replayed interpreter instruction')
    step(0x38,0x3c)
    step(0x3c,0x3e)
    monitor('pace-clear')
    monitor('pace-break %x' % exception)
    command('continue')
    check(monitor('pace')['pc'] == exception, 'timed exception path not reached')
    # Native PACE PC is suspended at enter/resume/returnFromCallout. The ROM
    # qualification reports static callout addresses; recover its runtime slide.
    native_pc = int(gdb.parse_and_eval('$pc'))
    static_entry = config['pace_entry']
    slide = native_pc - static_entry
    check(slide in (0, 4, 8), 'unqualified suspended PACE PC; cannot map exception callout')
    monitor('pace-stop on')
    before = monitor('pace')
    monitor('pace-step')
    command('continue')
    fault = monitor('pace-last-step')
    check((fault['instruction_pc'], fault['status'], fault['phase']) ==
          (exception, 5, 'exception'), 'wrong DIVU exception')
    check(monitor('pace-last') == fault, 'exception capture differs from step')
    try:
        monitor('pace-step')
        raise RuntimeError('pending exception allowed step')
    except gdb.error as exc:
        check('E16' in str(exc) or 'Rcmd: 16.' in str(exc),
              'unexpected step rejection: ' + str(exc))
    # These qualified TX ROMs use no slide; do not infer arbitrary runtime maps.
    handler = config['divu_callout']
    command('hbreak *0x%x' % handler)
    monitor('pace-clear')
    command('continue')
    check(not monitor('pace')['available'], 'exception was not dispatched to native code')
    check(int(gdb.parse_and_eval('$pc')) == handler, 'wrong native exception entry')
    check(monitor('pace-last-step') == fault, 'exception instruction replayed')
    # One native step at this entry proves forward execution after the one
    # pending dispatch. It does not claim the firmware recovers from DIVU #0.
    command('delete breakpoints')
    command('stepi')
    check(int(gdb.parse_and_eval('$pc')) != handler, 'native handler did not advance')
    check(monitor('pace-last-step') == fault, 'native handler replayed DIVU')
    result.update(status='ok', mapping=mapping, exception=fault,
                  checks=['exact unique live code/1 match', 'known code breakpoint',
                          'nonexecuting step arm', 'single counter increment',
                          'multiword immediate store', 'D0/D1 arithmetic',
                          'taken branch skips sentinel', 'two distinct memory stores',
                          'breakpoint retained across explicit step',
                          'syscall suspended before dispatch', 'pending dispatch rejects step',
                          'overlapping MemMove dispatches once',
                          'timed DIVU exception captured once',
                          'exception dispatch reaches qualified native handler',
                          'first native handler instruction advances without replay'],
                  exception_dispatch='entered qualified native DIVU callout once and stepped its first instruction',
                  firmware_exception_recovery_verified=False)
    command('detach')
except Exception as exc:
    result.update(error=str(exc), traceback=traceback.format_exc())
finally:
    (output / 'gdb-result.json').write_text(json.dumps(result, indent=2) + '\n')
    if result['status'] != 'ok':
        gdb.execute('quit 1')
