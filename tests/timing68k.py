#!/usr/bin/env python3
"""Runs the 68k timing probe on a IIIx and checks each loop against the 68000 manual's clocks.

Build the probe first: python3 tools/build-palm-probe.py --probe timing68k --sdk SDK-3.5
Run: python3 tests/timing68k.py --rom PRIVATE.rom --output build/timing68k-run [--speed original]
The probe's RAM witness is read from a saved session. Each loop's expected clocks per pass are
the manual's count plus the wait states the guest set (frond-68k's bus-wait) on each bus cycle,
and the measured clocks per pass must be within --tolerance of it. The 100 Hz tick interrupt runs
during every loop, so measured passes are a little slower than the bare count."""
import argparse
import re
from pathlib import Path
import struct
import sys
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import palm_boot  # noqa: E402

CLOCK = 16580608
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
]


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
            if version != 2:
                continue
            body = convert(payload[at + 32:at + 32 + 12 * loops])
            values = struct.unpack(f'>{3 * loops}I', body)
            return done, per_second, list(zip(values[:loops], values[loops:2 * loops], values[2 * loops:]))
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
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--probe', type=Path, default=Path('build/timing68k/FrondTiming68k.prc'))
    parser.add_argument('--speed', default='original', choices=['original', 'uncapped'])
    parser.add_argument('--tolerance', type=float, default=3.0, help='percent')
    parser.add_argument('--frondbench', type=Path, help='run this FrondBench.prc instead of the probe')
    parser.add_argument('--core', type=Path, help='another frond-68k build, e.g. an older one to compare')
    args = parser.parse_args()
    if args.core:
        palm_boot.CORE_68K = args.core.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    core = palm_boot.Core(['--device', 'palm-iiix', args.rom], args.output / 'guest.log', '--speed', args.speed)
    try:
        palm_boot.iiix_setup(core, args.output)
        bus = core.send('bus-wait', ('bus wait:', 'unknown')).strip()
        print(bus)
        rom_wait = ram_wait = 0
        if bus.startswith('bus wait:'):
            rom_wait = int(bus.split('rom ')[1].split('/')[1].split(',')[0])
            ram_wait = int(bus.split('ram ')[1].split('/')[1])
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
    failures = 0
    print(f'{"loop":8} {"count":>7} {"ticks":>5} {"manual":>7} {"+wait":>7} {"measured":>9} {"diff":>7}')
    for (name, program, ram, rom, _), (count, ticks, clocks) in zip(LOOPS, rows):
        expected = clocks + ram_wait * (program + ram) + rom_wait * rom
        measured = ticks / per_second * CLOCK / count
        diff = 100 * (measured / expected - 1)
        ok = abs(diff) <= args.tolerance
        failures += not ok
        print(f'{name:8} {count:7} {ticks:5} {clocks:7} {expected:7} {measured:9.1f} {diff:+6.1f}%{"" if ok else "  FAIL"}')
    if failures and args.speed == 'original':
        raise SystemExit(f'{failures} loops off by more than {args.tolerance}%')


if __name__ == '__main__':
    main()
