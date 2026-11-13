#!/usr/bin/env python3
"""Runs the 68k timing probe on a IIIx (or another 68k device) and checks each loop against the 68000 manual's clocks.

Build the probe first: python3 tools/build-palm-probe.py --probe timing68k --sdk PALM-OS-SDK/sdk-3.5
Run: python3 tests/timing68k.py --rom PRIVATE.rom --output build/timing68k-run [--speed original] [--device palm-iiix]
The probe's RAM witness is read from a saved session. Each loop's expected clocks per pass are
the manual's count plus the wait states the guest set (frond-68k's bus-wait) on each bus cycle,
stretched by the bus time LCD DMA and DRAM refresh take (bus-timing): the loop's share of time on
the bus times the DMA's share of each LCD line, and its share on DRAM times refresh's share. The
measured clocks per pass must be within --tolerance of it, give or take one tick. On SDRAM (the
VZ devices) RAM waits by page, so the checker follows each loop's rows from the addresses the
version 4 probe records. The 100 Hz tick interrupt runs during
every loop, so measured passes are a little slower than the bare count."""
import argparse
import re
from pathlib import Path
import struct
import sys
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import palm_boot  # noqa: E402

RESULTS = re.compile(rb'FrondBench 1\.0\n[^\0]{0,380}?Checksum [0-9A-Fa-f]+\n')
MAGIC = struct.pack('>4I', 0x46723638, 0x54494d45, 0x5749544e, 0x45535331)

# name, then bus cycles per pass from program space in RAM, data in RAM, data in ROM and data in
# the registers (zero wait states), from the manual's n(r/w) counts.
LOOPS = [
    ('branch', 3, 0, 0, 0),
    ('nop', 11, 0, 0, 0),
    ('read', 7, 8, 0, 0),
    ('call', 7, 4, 0, 0),
    ('mulu0', 4, 0, 0, 0),
    ('mulu1', 4, 0, 0, 0),
    ('divu', 5, 0, 0, 0),
    ('dbra', 206, 0, 0, 0),
    ('lsl', 4, 0, 0, 0),
    ('movem', 8, 24, 0, 0),
    ('rom', 7, 0, 8, 0),
    ('reg', 7, 0, 0, 4),
    ('clr', 7, 8, 0, 0),
    ('st', 7, 8, 0, 0),
]

# Each loop's pass for SDRAM, one entry per instruction: its byte offset from the loop's label,
# its program reads and its data accesses as (where, offset, words, write), where is 'data'
# (the loop's data address, read-before-write for 'rmw'), 'stack' (from the stack pointer) or
# 'off' (not SDRAM: ROM or the registers). Data accesses come before the program reads, as in
# frond-68k; for these loops the real 68000's order changes rows at the same points.
def _reads(offsets, reads):
    return [(at, reads, []) for at in offsets]

_TAIL = lambda at: [(at, 1, []), (at + 2, 2, [])]  # subq.l #1,d0 and a taken bne.s
SDRAM_PASSES = [
    [(0, 1, []), (2, 2, [])],
    _reads(range(0, 16, 2), 1) + _TAIL(16),
    [(at, 1, [('data', 0, 2, False)]) for at in range(0, 8, 2)] + _TAIL(8),
    [(0, 2, [('stack', -4, 2, True)]), (-2, 2, [('stack', -4, 2, False)])] + _TAIL(2),
    [(0, 1, [])] + _TAIL(2),
    [(0, 1, [])] + _TAIL(2),
    [(0, 1, []), (2, 1, [])] + _TAIL(4),
    [(0, 2, [])] + [(4, 2, [])] * 99 + [(4, 3, [])] + _TAIL(8),
    [(0, 1, [])] + _TAIL(2),
    # movem.l to -(sp) writes from sp-4 down, as frond-68k does; (sp)+ reads back up.
    [(0, 2, [('stack', -4 - 4 * k, 2, True) for k in range(6)]),
     (4, 3, [('stack', -24 + 4 * k, 2, False) for k in range(6)])] + _TAIL(8),
    [(at, 1, [('off', 0, 2, False)]) for at in range(0, 8, 2)] + _TAIL(8),
    [(at, 1, [('off', 0, 1, False)]) for at in range(0, 8, 2)] + _TAIL(8),
    [(at, 1, [('rmw', 0, 1, True)]) for at in range(0, 8, 2)] + _TAIL(8),
    [(at, 1, [('rmw', 0, 1, True)]) for at in range(0, 8, 2)] + _TAIL(8),
]


def sdram_pass(sdram, steps, code, data):
    """(SDRAM wait clocks, SDRAM bus cycles) for one pass of a loop once its rows have settled,
    by the VZ manual's page timing: a read waits 1 on a page hit and 3 on a miss, one more each
    with CAS latency 2, a write 0 and 1; without continuous page mode everything misses."""
    open_rows = {}

    def access(address, write, words):
        hit, miss = (0, 1) if write else (sdram['cl'], sdram['cl'] + 2)
        if not sdram['cpm']:
            return words * miss
        bank = sum(((address >> bit) & 1) << shift
                   for bit, shift in ((sdram['high'], 1), (sdram['low'], 0)) if bit != 255)
        row = address >> sdram['page']
        first = hit if open_rows.get(bank) == row else miss
        open_rows[bank] = row
        return first + (words - 1) * hit

    total = cycles = 0
    for _ in range(2):
        total = cycles = 0
        for at, reads, accesses in steps:
            for where, offset, words, write in accesses:
                if where == 'off':
                    continue
                address = (data + offset) & 0xffffffff
                if where == 'rmw':
                    # Read then write the same words: the write always hits after the read.
                    total += access(address, False, words) + access(address, True, words)
                    cycles += 2 * words
                else:
                    total += access(address, write, words)
                    cycles += words
            if reads:
                total += access(code + at, False, reads)
                cycles += reads
    return total, cycles


def session_payloads(data):
    """Every zlib stream in a saved session, inflated."""
    found = []
    for at in range(len(data) - 2):
        if data[at] == 0x78 and (data[at] << 8 | data[at + 1]) % 31 == 0:
            try:
                decoder = zlib.decompressobj()
                payload = decoder.decompress(data[at:])
            except zlib.error:
                continue
            if decoder.eof and len(payload) > 1 << 20:
                found.append(payload)
    return found or [data]


def witness(path):
    """The probe's witness: (done, ticks per second, [(count, ticks, clocks)])."""
    swap = lambda raw: bytes(b for pair in zip(raw[1::2], raw[0::2]) for b in pair)
    for payload in session_payloads(path.read_bytes()):
        # Cloudpilot keeps RAM in host order, 16-bit words byte-swapped on little-endian hosts.
        for magic, convert in ((MAGIC, lambda raw: raw), (swap(MAGIC), swap)):
            at = payload.find(magic)
            if at < 0:
                continue
            head = convert(payload[at:at + 32])
            version, done, per_second, loops = struct.unpack_from('>4I', head, 16)
            if version not in (2, 3, 4):
                continue
            # Version 4 adds each loop's label and data addresses.
            columns = 5 if version == 4 else 3
            body = convert(payload[at + 32:at + 32 + 4 * columns * loops])
            values = struct.unpack(f'>{columns * loops}I', body)
            return done, per_second, list(zip(*(values[k * loops:(k + 1) * loops] for k in range(columns))))
    return None


def frondbench_results(path):
    """FrondBench's saved results text, or None."""
    swap = lambda raw: bytes(b for pair in zip(raw[1::2], raw[0::2]) for b in pair)
    for payload in session_payloads(path.read_bytes()):
        for memory in (payload, swap(payload), swap(payload[1:])):
            found = RESULTS.search(memory)
            if found:
                return found.group().decode('latin-1')
    return None


def run_frondbench(core, args):
    """Installs and runs FrondBench (Palm OS 3.5 and later) and returns its results text."""
    print(core.send(f'install "{args.frondbench.resolve()}"', 'installation').strip())
    time.sleep(1)
    core.launch('FrondBench')
    time.sleep(3)
    core.tap(args.output, 19, 151)
    start = time.monotonic()
    while time.monotonic() - start < 1800:
        time.sleep(20)
        session = args.output / 'frondbench.session'
        core.send(f'save-session "{session}"', 'wrote session')
        text = frondbench_results(session)
        if text:
            return text
    raise SystemExit('FrondBench did not finish')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--device', default='palm-iiix', help='a 68k profile; its cpu_hz is the clock')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--probe', type=Path, default=Path('build/timing68k-probe/FrondTiming68k.prc'))
    parser.add_argument('--speed', default='original', choices=['original', 'uncapped'])
    parser.add_argument('--tolerance', type=float, default=3.0, help='percent')
    parser.add_argument('--frondbench', type=Path, help='run this FrondBench.prc instead of the probe')
    parser.add_argument('--core', type=Path, help='another frond-68k build, e.g. an older one to compare')
    args = parser.parse_args()
    if args.core:
        palm_boot.CORE_68K = args.core.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    clock = palm_boot.rom_device(args.device)['cpu_hz']
    core = palm_boot.Core(['--device', args.device, args.rom], args.output / 'guest.log', '--speed', args.speed)
    try:
        palm_boot.setup_device(args.device, core, args.output)
        if palm_boot.rom_device(args.device)['setup'] == 'iiix-hires':
            # Low-res apps are doubled on a 320x320 LCD, so taps are too.
            core = palm_boot.HalfCore(core)
        bus = core.send('bus-wait', ('bus wait:', 'unknown')).strip()
        print(bus)
        rom_wait = ram_wait = 0
        sdram = None
        if bus.startswith('bus wait:'):
            rom_wait = int(re.search(r'rom \d+/(\d+)', bus).group(1))
            ram_wait = int(re.search(r'ram \d+/(\d+)', bus).group(1))
            found = re.search(r'sdram cl (\d) cpm (\d) page (\d+) bank (\d+)/(\d+)', bus)
            if found:
                cl, cpm, page, high, low = map(int, found.groups())
                sdram = {'cl': cl, 'cpm': cpm, 'page': page, 'high': high, 'low': low}
        timing = core.send('bus-timing', ('bus timing:', 'unknown')).strip()
        print(timing)
        lcd = refresh = 0.0
        closes = 0.0
        if timing.startswith('bus timing:'):
            numbers = [int(x) for x in re.findall(r'\d+', timing)]
            lcd = numbers[1] / numbers[0] if numbers[0] else 0.0
            refresh = numbers[3] / numbers[2] if numbers[2] else 0.0
            # SDRAM rows closed per clock: every refresh, and every LCD line read from SDRAM.
            closes = sum(1 / n for n in (numbers[0], numbers[2]) if n)
        if args.frondbench:
            print(run_frondbench(core, args))
            core.frame(args.output / 'frondbench.png')
            return
        print(core.send(f'install "{args.probe.resolve()}"', 'installation').strip())
        time.sleep(1)
        core.launch('Frond timing')
        start = time.monotonic()
        result = None
        while time.monotonic() - start < 300:
            time.sleep(2)
            session = args.output / 'probe.session'
            core.send(f'save-session "{session}"', 'wrote session')
            result = witness(session)
            if result and result[0]:
                break
        core.frame(args.output / 'result.png')
        print(core.send('speed-state', 'speed:').strip())
    finally:
        core.close()
    if not result or not result[0]:
        raise SystemExit('the probe did not finish')
    _, per_second, rows = result
    if sdram and len(rows[0]) < 5:
        raise SystemExit('SDRAM needs the version 4 probe, with its loop addresses')
    if sdram and not sdram['cpm']:
        closes = 0.0
    failures = 0
    print(f'{"loop":8} {"count":>7} {"ticks":>5} {"manual":>7} {"+wait":>7} {"+dma":>7} {"measured":>9} {"diff":>7}')
    for index, ((name, program, ram, rom, _), row) in enumerate(zip(LOOPS, rows)):
        count, ticks, clocks = row[:3]
        if not ticks:
            # The probe skips the ROM loop when TimGetTicks isn't in the ROM (a patch in RAM).
            print(f'{name:8} {count:7} not run' + (f', address {row[4]:08x} is not in the ROM' if len(row) > 4 else '')
                  + '  FAIL')
            failures += 1
            continue
        if sdram:
            dram_wait, dram_cycles = sdram_pass(sdram, SDRAM_PASSES[index], row[3], row[4])
            waited = clocks + dram_wait + rom_wait * rom
            # A closed row turns the next read hit into a miss, two more clocks.
            waited += 2 * closes * waited
            bus = 4 * (dram_cycles + rom) + dram_wait + rom_wait * rom
            dram = 4 * dram_cycles + dram_wait
        else:
            waited = clocks + ram_wait * (program + ram) + rom_wait * rom
            # Register accesses stay off the external bus, so only RAM and ROM cycles share it.
            bus = 4 * (program + ram + rom) + ram_wait * (program + ram) + rom_wait * rom
            dram = (4 + ram_wait) * (program + ram)
        expected = waited / (1 - lcd * bus / waited - refresh * dram / waited)
        measured = ticks / per_second * clock / count
        diff = 100 * (measured / expected - 1)
        # The tick count can be one off either way, so the check allows one tick.
        tick = 100 / ticks
        ok = abs(diff) <= args.tolerance + tick
        failures += not ok
        print(f'{name:8} {count:7} {ticks:5} {clocks:7} {waited:7.1f} {expected:7.1f} {measured:9.1f} '
              f'{diff:+6.1f}%{"" if ok else "  FAIL"}')
    if failures and args.speed == 'original':
        raise SystemExit(f'{failures} loops off by more than {args.tolerance}%')


if __name__ == '__main__':
    main()
