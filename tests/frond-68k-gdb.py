#!/usr/bin/env python3
"""frond-68k's GDB stub with prc-tools' m68k-palmos-gdb 5.3 on a private IIIx ROM: install the
debug Hello app, point the stub at its COFF, launch it, then break at PilotMain, next over a call,
step into one, finish, bt and info registers.

The app comes from build/hello-debug (Hello.prc and the COFF hello, built from ~/Projects/palm/hello
with -g). Skipped without m68k-palmos-gdb, the app or a ROM."""
import argparse
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

APP = ROOT / 'build/hello-debug'

COMMANDS = '''set confirm off
set remotetimeout 120
set height 0
target remote 127.0.0.1:{port}
break PilotMain
continue
next
next
next
step
finish
bt
info registers
p $pc
detach
'''


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def check(out):
    """The stops gdb printed, in order; raises when one is missing."""
    want = [
        ('break', r'Breakpoint 1, PilotMain \(cmd=0'),
        ('next over RomVersionCompatible', r'^163\s'),
        ('next', r'^166\s'),
        ('next', r'^167\s'),
        ('step into AppStart', r'AppStart \(\) at hello\.c'),
        ('finish', r'in PilotMain \(cmd=0.*\n.*\n?Value returned'),
        ('bt', r'^#0 .*PilotMain'),
        ('info registers', r'^pc\s+0x'),
    ]
    find(out, want)


MULTIARCH = '''set confirm off
set height 0
set architecture m68k
set endian big
target remote 127.0.0.1:{port}
break *{pilotmain:#x}
continue
info registers pc
stepi
x/2i $pc
detach
'''


def check_multiarch(out, pilotmain):
    """gdb-multiarch without symbols: the address breakpoint at PilotMain and a stepi past it."""
    find(out, [('break', rf'Breakpoint 1, {pilotmain:#010x} in'),
               ('info registers', rf'^pc\s+{pilotmain:#x}'),
               ('stepi', r'^=> 0x'),
               ('detach', r'detached')])


STOPPED = '''set confirm off
set height 0
set architecture m68k
set endian big
target remote 127.0.0.1:{port}
break *{pilotmain:#x}
shell sleep 20
'''


def stop_while_attached(core, gdb_path, port, pilotmain, root):
    """gdb-stop with gdb attached and a breakpoint set must clear it, so launching the app runs on."""
    (root / 'stop.gdb').write_text(STOPPED.format(port=port, pilotmain=pilotmain))
    gdb = subprocess.Popen([gdb_path, '-nx', '-batch', '-x', str(root / 'stop.gdb')],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                           text=True, errors='replace', cwd=root)
    try:
        time.sleep(4)
        core.send('gdb-stop')
        time.sleep(1)
        core.launch('Hello')
        time.sleep(3)
        if core.process.poll() is not None:
            raise AssertionError('the core exited on a breakpoint after gdb-stop')
    finally:
        gdb.kill()
        gdb.communicate()


def find(out, want):
    pos = 0
    for name, pattern in want:
        m = re.compile(pattern, re.M).search(out, pos)
        if not m:
            raise AssertionError(f'{name}: no /{pattern}/ after offset {pos}')
        pos = m.end()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path)
    parser.add_argument('--session', type=Path, help='a launcher session instead of booting the ROM')
    parser.add_argument('--gdb', default=shutil.which('m68k-palmos-gdb'))
    parser.add_argument('--evidence', type=Path)
    parser.add_argument('--trace', action='store_true', help='gdb remote packet trace')
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--multiarch', action='store_true',
                        help='gdb-multiarch with an address breakpoint, as it cannot read COFF')
    args = parser.parse_args()
    rom = args.rom or palm_boot.palm_images() / 'Palm/Generic-OS-3.5-EZ/Palm-OS-3.5-en-ez.rom'
    if args.multiarch:
        args.gdb = shutil.which('gdb-multiarch')
    if not args.gdb or not (APP / 'Hello.prc').is_file() or not (args.session or rom.is_file()):
        print('frond-68k gdb: skipped, needs m68k-palmos-gdb, build/hello-debug and a ROM')
        return 0
    with tempfile.TemporaryDirectory(prefix='frond-68k-gdb-') as work:
        root = (args.evidence or Path(work)).resolve()
        root.mkdir(parents=True, exist_ok=True)
        port = free_port()
        image = ['--device', 'palm-iiix', args.session] if args.session else ['--rom', rom, '--device', 'palm-iiix']
        core = palm_boot.Core(image, root / 'log.txt', '--gdb', port)
        try:
            if not args.session:
                palm_boot.iiix_setup(core, root)
            core.send(f'install "{APP / "Hello.prc"}"', 'installation')
            mark = len(core.lines)
            core.send(f'gdb-app "{APP / "hello"}"', 'gdb-app')
            if args.multiarch:
                moved = next(int(m.group(1)) for line in core.lines[mark:]
                             if (m := re.search(r'relocated by (-?\d+) bytes', line)))
                nm = subprocess.run(['m68k-palmos-nm', str(APP / 'hello')], capture_output=True, text=True, check=True)
                pilotmain = moved + int(next(line.split()[0] for line in nm.stdout.splitlines()
                                             if line.endswith(' PilotMain')), 16)
                commands = MULTIARCH.format(port=port, pilotmain=pilotmain)
            else:
                commands = COMMANDS.format(port=port)
            if args.trace:
                commands = 'set debug remote 1\n' + commands
            (root / 'cmds.gdb').write_text(commands)
            gdb = subprocess.Popen([args.gdb, '-nx', '-batch', '-x', str(root / 'cmds.gdb'),
                                    *([] if args.multiarch else [str(APP / 'hello')])],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                   text=True, errors='replace', cwd=root)
            core.launch('Hello')
            try:
                out, _ = gdb.communicate(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                gdb.kill()
                out, _ = gdb.communicate()
                core.frame(root / 'timeout.png')
                (root / 'gdb.txt').write_text(out)
                print(f'frond-68k gdb: FAIL, gdb timed out after {args.timeout} s')
                return 1
            (root / 'gdb.txt').write_text(out)
            if args.multiarch:
                try:
                    check_multiarch(out, pilotmain)
                    stop_while_attached(core, args.gdb, port, pilotmain, root)
                except AssertionError as error:
                    print(f'frond-68k gdb: FAIL, {error}')
                    return 1
        finally:
            core.close()
        try:
            check_multiarch(out, pilotmain) if args.multiarch else check(out)
        except AssertionError as error:
            print(f'frond-68k gdb: FAIL, {error}')
            if not args.evidence:
                print(out[-3000:])
            return 1
    print('frond-68k gdb' + (' (gdb-multiarch)' if args.multiarch else '') + ': ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
