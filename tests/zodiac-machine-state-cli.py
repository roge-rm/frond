#!/usr/bin/env python3
"""Zodiac machine states through the boot probe: one process saves, another loads and ends like an uninterrupted run."""
from pathlib import Path
import gzip
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'build/zodiac-boot-probe'
BUDGET = 15000


def display_rom():
    """The 3x2 display guest of zodiac-machine-frame.py: every access from the PC the graphics models expect."""
    rom = bytearray(0x7d0000)

    def word(at, value):
        struct.pack_into('<I', rom, at, value)

    def branch(at, target):
        word(at, 0xea000000 | (((target - at - 8) // 4) & 0xffffff))

    for at in (0, 0x20000):
        word(at + 8, 0xfeedbeef)
        rom[at + 0x10:at + 0x19] = b'PalmCard\0'
        word(at + 0x80, 0x54707776)
        word(at + 0x84, 0x4d583161)
    word(0x2006c, len(rom) - 0x20000)
    ops = [(0xdf000, 0x00220028, True, 0x3e00, 4), (0xdf008, 0x0022002c, True, 0x11111e01, 4)]
    setup = (ROOT / 'tests/zodiac-graphics-setup.h').read_text()
    interface = setup[setup.index('inline void configureGraphicsInterface'):setup.index('inline void configureGraphicsExternal')]
    for pc, address, size, value in re.findall(r'(?:Write)?\{(0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*(\d+),\s*(0x[0-9a-f]+|0)\}', interface):
        pc, address, size, value = int(pc, 16), int(address, 16), int(size), int(value, 0)
        ops.append((pc - 0x20000000, address, address != 0x16000010, value, size))
    ops += [(0xb31b0, 0x160101a4, True, 0x7800, 4), (0xb31bc, 0x16010180, True, 0x100007, 4)]
    external = setup[setup.index('inline void configureGraphicsExternal'):setup.index('inline void configureGraphicsController')]
    for pc, offset, value, write in re.findall(r'(?:Operation)?\{(0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*(true|false)\}', external):
        ops.append((int(pc, 16) - 0x20000000, 0x16010000 + int(offset, 16), write == 'true', int(value, 16), 4))
    ops.append((0xc6888, 0x1601019c, True, 0x7bbb7444, 4))
    for pc, address, write, value in [
            (0xacb98, 0x160113e8, False, 0), (0xacba8, 0x160113e8, True, 0x400), (0xacbb4, 0x16010144, True, 0x2440),
            (0xacbc4, 0x160110a8, True, 0x1fff1fff), (0xacbd0, 0x160104dc, True, 0xc0000),
            (0xb8e80, 0x16010214, True, 0x10150000), (0xb8e88, 0x16010220, True, 0), (0xb8f04, 0x16010008, True, 0x04150000),
            (0xb8f10, 0x16010230, True, 0x80000000), (0xb8f20, 0x16010210, True, 0x0800000d)]:
        ops.append((pc, address, write, value, 4))
    for pc, address, value in [(0xe6000, 0x16800000, 0x07e0f800), (0xe6008, 0x16800004, 0x0000001f),
                               (0xe6010, 0x16800008, 0x0000ffff), (0xe6018, 0x1680000c, 0x0000ffe0)]:
        ops.append((pc, address, True, value, 4))

    def display(pc, offset, value):
        ops.append((pc, 0x16010000 + offset, True, value, 4))

    display(0xaeb68, 0x4d8, 0x78)
    for pc, offset, value in [(0xad720, 0x418, 0x800000), (0xad72c, 0x41c, 8), (0xad738, 0x414, 6)]:
        display(pc, offset, value)
    display(0xaeb68, 0x4d8, 0x7b)
    display(0xaeb68, 0x4d8, 0x78)
    for pc, offset, value in [
            (0xad65c, 0x42c, 0x40001), (0xad668, 0x430, 0x30001), (0xad2e0, 0x410, 3), (0xad404, 0x540, 0x80604020),
            (0xad410, 0x544, 0xffdfbf9f), (0xad418, 0x548, 0), (0xad420, 0x578, 0x80604020), (0xad428, 0x58c, 0xffdfbf9f),
            (0xad430, 0x590, 0), (0xad438, 0x594, 0x80604020), (0xad440, 0x598, 0xffdfbf9f), (0xad448, 0x59c, 0),
            (0xad528, 0x538, 0x800000), (0xad3b8, 0x524, 0x102aa), (0xad2ec, 0x414, 0x1800406), (0xad2f8, 0x420, 0x60008),
            (0xad304, 0x424, 0x40001), (0xad310, 0x428, 0x30001)]:
        display(pc, offset, value)
    display(0xaeb68, 0x4d8, 0x7b)
    display(0xaeb68, 0x4d8, 0x78)
    display(0xad8b4, 0x414, 0x1801c06)
    display(0xaeb68, 0x4d8, 0x7b)
    branch(0, 0xd0000)
    for i, (pc, address, write, value, size) in enumerate(ops):
        scratch = 0xd0000 + i * 0x200
        word(scratch, 0xe59f0010)
        word(scratch + 4, 0xe59f6010)
        word(scratch + 8, 0xe59fe010)
        branch(scratch + 12, pc)
        word(scratch + 24, address)
        word(scratch + 28, value)
        word(scratch + 32, 0x20000000 + scratch + 0x40)
        word(pc, (0xe5c06000 if write else 0xe5d06000) if size == 1 else (0xe5806000 if write else 0xe5906000))
        word(pc + 4, 0xe12fff1e)
        for wait in range(64):
            word(scratch + 0x40 + wait * 4, 0xe1a00000)
        branch(scratch + 0x140, 0xd0000 + (i + 1) * 0x200 if i + 1 < len(ops) else 0xf0000)
    branch(0xf0000, 0xf0000)
    return rom, len(ops)


def main():
    with tempfile.TemporaryDirectory(prefix='zodiac-machine-state-cli-', dir=ROOT / 'build') as work:
        work = Path(work)
        rom, count = display_rom()
        rom_path = work / 'display.rom'
        rom_path.write_bytes(rom)
        # Each access takes 71 instructions. One save is inside the setup, the other after the display is on.
        early, late = 71 * 30 + 9, 71 * count + 500
        assert late < BUDGET
        scenario = ['--bootstrap-peripherals', '--graphics-profile=virtual5744-compatible-8mib', '--graphics-raw-mode1=712',
                    '--asp-u-sample=0x9b00', '--asp-px1-input=high', '--trace=peripherals']

        def run(engine, *flags, code=2, budget=BUDGET, options=scenario):
            result = subprocess.run([str(PROBE), str(rom_path), 'linked', str(budget), engine, *options, *flags],
                                    capture_output=True, text=True, timeout=60)
            assert result.returncode == code, (flags, result.returncode, result.stdout[-3000:], result.stderr)
            return result

        def end(result):
            """The result lines, from the stop line on."""
            lines = result.stdout.splitlines()
            stops = [n for n, line in enumerate(lines) if line.startswith('stop=')]
            assert len(stops) == 1, result.stdout[-3000:]
            return lines[stops[0]:]

        def events(result, after):
            """The trace lines between a marker line and the stop line, without the state notes."""
            lines = result.stdout.splitlines()
            start = [n for n, line in enumerate(lines) if line.startswith(after)]
            assert len(start) == 1, (after, result.stdout[:3000])
            stop = [n for n, line in enumerate(lines) if line.startswith('stop=')][0]
            return [line for line in lines[start[0] + 1:stop] if not line.startswith('state-')]

        def without_blocks(lines):
            return [re.sub(r'jit-blocks=\d+', 'jit-blocks=', line) for line in lines]

        def frame(name):
            return '--frame-output=' + str(work / name)

        expected = b'P6\n3 2\n255\n' + bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 255, 255, 0])
        states = {}
        for engine in ('exact', 'fast'):
            first, second, again = (work / (engine + name) for name in ('-early.state', '-late.state', '-late-again.state'))
            plain = run(engine, frame(engine + '-plain.ppm'))
            assert 'stop=instruction-budget' in plain.stdout and 'graphics-frame lcd=1 width=3 height=2' in plain.stdout
            assert (work / (engine + '-plain.ppm')).read_bytes() == expected

            saver = run(engine, frame(engine + '-saver.ppm'), '--save-state', str(first), '--save-after', str(early),
                        '--save-state', str(second), '--save-after', str(late))
            assert f'state-saved={first} retired={early}' in saver.stdout and f'state-saved={second} retired={late}' in saver.stdout
            assert re.search(r'^state-build="[0-9a-f]*\+?" core-revision=\d+$', saver.stdout, re.M), saver.stdout[:3000]
            # Saving drops translated code, so only the Fast block count may differ from the uninterrupted run.
            assert without_blocks(end(saver)) == without_blocks(end(plain))
            if engine == 'exact':
                assert end(saver) == end(plain)
            assert events(saver, 'state-build=') == events(plain, 'Diagnostic RAM capacity')[-len(events(saver, 'state-build=')):]
            assert (work / (engine + '-saver.ppm')).read_bytes() == expected
            for path in (first, second):
                packed = path.read_bytes()
                assert packed[:2] == b'\x1f\x8b' and gzip.decompress(packed)[:8] == b'FRZ2SAVE'
            assert len(gzip.decompress(first.read_bytes())) > 8 * 1024 * 1024 > len(first.read_bytes())

            # The second process starts from the early state and saves the late one again.
            loader = run(engine, frame(engine + '-loader.ppm'), '--load-state', str(first),
                         '--save-state', str(again), '--save-after', str(late))
            assert (f'state-loaded={first} retired={early} engine-changed=0 core-revision-differs=0 build-note-differs=0'
                    in loader.stdout)
            assert end(loader) == end(saver)
            assert gzip.decompress(again.read_bytes()) == gzip.decompress(second.read_bytes())
            assert (work / (engine + '-loader.ppm')).read_bytes() == expected
            traced = events(loader, 'state-loaded=')
            assert len(traced) > 20 and traced == events(saver, 'state-build=')[-len(traced):]

            # A third starts from the late state, with the frame already pending in it.
            last = run(engine, frame(engine + '-last.ppm'), '--load-state', str(second))
            assert end(last) == end(saver)
            assert (work / (engine + '-last.ppm')).read_bytes() == expected
            traced = events(last, 'state-loaded=')
            assert traced == events(saver, 'state-build=')[len(events(saver, 'state-build=')) - len(traced):]
            # At the budget already: nothing runs and the loaded result is printed.
            still = run(engine, '--load-state', str(second), budget=late)
            assert f'stop=instruction-budget step={late - 1} ' in still.stdout and not events(still, 'state-loaded=')
            states[engine] = (first, second)

            # An existing state file is kept unless asked.
            before = first.read_bytes()
            kept = run(engine, '--save-state', str(first), '--save-after', str(early + 1), code=1)
            assert f'Cannot save state {first}: {first} exists' in kept.stderr and first.read_bytes() == before
            run(engine, '--save-state', str(again), '--save-after', str(early + 1), '--save-state-overwrite')
            assert gzip.decompress(again.read_bytes()) != gzip.decompress(second.read_bytes())
            assert not list(work.glob('*.tmp.*'))

        # The other engine's state loads and says so, and the run ends the same but for the Fast counters.
        for engine, other in (('exact', 'fast'), ('fast', 'exact')):
            crossed = run(engine, frame('crossed.ppm'), '--load-state', str(states[other][0]))
            assert 'engine-changed=1' in crossed.stdout
            strip = lambda lines: [re.sub(r'jit-(blocks|instructions)=\d+', '', line) for line in lines]
            assert strip(end(crossed)) == strip(end(run(engine)))
            assert (work / 'crossed.ppm').read_bytes() == expected

        # Refusals say why, with the build.
        first = states['exact'][0]
        changed = [flag if not flag.startswith('--graphics-raw-mode1') else '--graphics-raw-mode1=700' for flag in scenario]
        refused = run('exact', '--load-state', str(first), code=1, options=changed)
        assert 'load refused: options-mismatch section=IDNT detail=graphicsRawMode1' in refused.stderr, refused.stderr
        assert re.search(r'state core revision \d+, this build \d+, [0-9a-f]*\+?\)', refused.stderr), refused.stderr
        assert 'stop=' not in refused.stdout
        other_rom = bytearray(rom)
        other_rom[0x7c0000] ^= 1
        rom_path.write_bytes(other_rom)
        assert 'load refused: rom-mismatch section=IDNT detail=rom' in run('exact', '--load-state', str(first), code=1).stderr
        rom_path.write_bytes(rom)
        raw = bytearray(gzip.decompress(first.read_bytes()))
        raw[len(raw) // 2] ^= 1
        damaged = work / 'damaged.state'
        damaged.write_bytes(gzip.compress(bytes(raw), 1))
        assert 'load refused: corrupt-section section=' in run('exact', '--load-state', str(damaged), code=1).stderr
        damaged.write_bytes(first.read_bytes()[:len(first.read_bytes()) // 2])
        assert 'is damaged' in run('exact', '--load-state', str(damaged), code=1).stderr
        # A state that was never packed loads too.
        damaged.write_bytes(gzip.decompress(first.read_bytes()))
        assert f'state-loaded={damaged} retired={early}' in run('exact', '--load-state', str(damaged)).stdout
        assert 'cannot open' in run('exact', '--load-state', str(work / 'missing.state'), code=1).stderr

        # Flag errors.
        usage = 'Supply --save-state FILE --save-after N, with N rising when repeated'
        new = str(work / 'new.state')
        for flags in (('--save-state', new), ('--save-state', new, '--save-after'), ('--save-state', new, '--save-after', 'x'),
                      ('--save-state', new, '--save-after', '-1'), ('--save-state', new, '--load-state', new),
                      ('--save-state', new, '--save-after', '9', '--save-state', new + '2', '--save-after', '9')):
            assert usage in run('exact', *flags, code=1).stderr
        assert 'Supply one --load-state FILE' in run('exact', '--load-state', code=1).stderr
        assert 'Supply one --load-state FILE' in run('exact', '--load-state', new, '--load-state', new, code=1).stderr
        for flags in (('--save-state', new, '--save-after', str(BUDGET + 1)), ('--save-state-overwrite',),
                      ('--load-state-any-revision',),
                      ('--save-state', new, '--save-after', '9', '--diagnostic-snapshot=' + str(work / 'snapshot'))):
            assert 'State flags:' in run('exact', *flags, code=1).stderr
        # The trace window counts from reset, so it's refused with a load and fine with a save.
        assert '--load-state does not go with --trace-window' in run(
            'exact', '--load-state', str(first), '--trace-window=10000000/1', code=1).stderr
        windowed = run('exact', '--save-state', new, '--save-after', str(early), '--trace-window=10000000/1').stdout
        assert f'state-saved={new} retired={early}' in windowed and 'trace-window-emitted=' in windowed
        assert Path(new).read_bytes() == first.read_bytes()
        Path(new).unlink()
        assert 'Unknown diagnostic option: --save-after' in run('exact', '--save-after', '9', code=1).stderr
        assert not Path(new).exists() and not list(work.glob('*.tmp.*'))
        assert rom_path.read_bytes() == rom
    print('PASS: probe states saved in one process continue in another like an uninterrupted run, Exact and Fast, '
          'with the frame, and refusals say why')


if __name__ == '__main__':
    main()
