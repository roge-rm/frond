#!/usr/bin/env python3
"""Runs the same desktop control socket commands against out/frond (an ARM smoke session) and
out/frond-68k (a IIIx ROM booted to the launcher), checking the replies, the pushed link events, save
and reload, and that commands a device can't do fail cleanly as "unsupported".

Private firmware and sessions: --arm-session, else FROND_ARM_SESSION, else the palm-tx smoke machine
(build/smoke/machines/palm-tx, or an older build/smoke/palm-tx.session); --rom, else the generic OS 3.5 EZ ROM in the
firmware folder. A side with neither is skipped. Scratch goes in build/core-control-*.
"""
import argparse
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

PATTERN = ROOT / 'tests/fixtures/frond-pattern.pdb'
PATTERN_NAME = 'frond-pattern.sunvox'


class Client:
    """One core on a private control socket, as the desktop starts it."""

    def __init__(self, core, args, root, name):
        self.socket_dir = tempfile.mkdtemp(prefix='frond-control-')
        address = str(Path(self.socket_dir) / 'control')
        listener = socket.socket(socket.AF_UNIX)
        listener.bind(address)
        listener.listen(1)
        self.log = (root / f'{name}.log').open('wb')
        runner = ['heavy', 'emulator'] if shutil.which('heavy') else []
        self.process = subprocess.Popen([*runner, str(core), *map(str, args), '--no-sound', '--desktop-socket', address],
                                        stdout=self.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                        start_new_session=True,
                                        env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'))
        self.connection = None
        try:
            listener.settimeout(300)  # includes waiting for an emulator slot
            self.connection, _ = listener.accept()
        finally:
            listener.close()
        self.connection.settimeout(120)
        self.stream = self.connection.makefile('rb')
        self.serial, self.events = 0, []
        assert self.stream.readline() == b'0\tevent\tready\t\n', 'no ready event'

    def request(self, command, argument=''):
        """Sends one request; returns (ok, code, text). Events that come first are kept."""
        self.serial += 1
        self.connection.sendall(f'{self.serial}\t{command}\t{str(argument).encode().hex()}\n'.encode())
        while True:
            line = self.stream.readline()
            assert line.endswith(b'\n'), f'{command}: connection closed'
            fields = line.rstrip(b'\n').split(b'\t')
            assert len(fields) == 4, fields
            if fields[0] == b'0' and fields[1] == b'event':
                self.events.append((fields[2].decode(), bytes.fromhex(fields[3].decode()).decode()))
                continue
            assert fields[0] == str(self.serial).encode() and fields[1] in (b'ok', b'error'), fields
            return fields[1] == b'ok', fields[2].decode(), bytes.fromhex(fields[3].decode()).decode()

    def ok(self, command, argument='', code=None):
        ok, got, text = self.request(command, argument)
        assert ok, f'{command} {argument!r}: {got} {text}'
        assert code is None or got in ((code,) if isinstance(code, str) else code), f'{command}: code {got}'
        return text

    def fails(self, command, argument='', code=None):
        ok, got, text = self.request(command, argument)
        assert not ok, f'{command} {argument!r} should fail: {got} {text}'
        assert code is None or got == code, f'{command} {argument!r}: code {got}, wanted {code} ({text})'
        return text

    def until_ok(self, command, argument='', timeout=90):
        end = time.monotonic() + timeout
        while True:
            ok, code, text = self.request(command, argument)
            if ok:
                return text
            assert code == 'not-ready', f'{command}: {code} {text}'
            assert time.monotonic() < end, f'{command}: still not ready after {timeout} s'
            time.sleep(1)

    def quit(self):
        self.ok('quit')
        assert self.process.wait(timeout=30) == 0, 'core exit status'
        self.close()

    def close(self):
        if self.connection:
            self.connection.close()
            self.connection = None
        if self.process.poll() is None:
            palm_boot.stop_guest(self.process)
        self.log.close()
        shutil.rmtree(self.socket_dir, ignore_errors=True)


def common(c, root, device, unsupported, saved):
    """The checks both runners share, ending with a save to saved."""
    assert c.ok('device-state', code='device') == device
    c.fails('not-a-command', code='command')
    c.fails('device-state', 'unexpected', code='argument')
    c.fails('save', 'relative.frond', code='argument')
    blocker = root / 'not-a-folder'
    blocker.write_text('')
    c.fails('save', blocker / 'state.frond')
    for command in ('network-state', 'keyboard-state', 'usb-state', 'sd-state', 'speed-state', 'serial-state',
                    'irda-state', 'microphone-state'):
        c.ok(command)
    c.fails('serial-mode', 'bogus', code='argument')
    assert c.ok('serial-mode', 'pty', code='serial').startswith('pty /dev/pts/')
    c.ok('speed-state')
    assert any(name == 'serial' and text.startswith('pty ') for name, text in c.events), c.events
    assert c.ok('serial-mode', 'off', code='serial') == 'off'
    assert c.ok('irda-mode', 'pty', code='infrared').startswith('pty /dev/pts/')
    assert c.ok('irda-mode', 'off', code='infrared') == 'off'
    c.ok('display', 'smooth=on')
    c.fails('display', 'smooth=maybe', code='argument')
    c.fails('display', 'jog=screen', code='argument')
    for command, argument in unsupported:
        c.fails(command, argument, code='unsupported')
    c.until_ok('guest-ready')
    assert c.ok('install', PATTERN, code=('installed', 'needs-reset'))
    listing = c.ok('db-list', code='databases')
    assert PATTERN_NAME.encode().hex() in listing, 'installed database not listed'
    archive = root / f'{device} RAM.zip'
    c.ok('export', archive, code='exported')
    with zipfile.ZipFile(archive) as z:
        assert any('frond-pattern' in name for name in z.namelist()), z.namelist()
    one = root / f'{device} one.zip'
    c.ok('export-db', f'{one}\n{PATTERN_NAME}', code='exported')
    with zipfile.ZipFile(one) as z:
        assert any('frond-pattern' in name for name in z.namelist()), z.namelist()
    c.fails('export-db', f'{one}', code='argument')
    before = saved.stat().st_mtime_ns if saved.exists() else 0
    c.ok('save', saved, code='saved')
    assert saved.stat().st_size > 100000 and saved.stat().st_mtime_ns != before, 'session not written'


def check_arm(core, session, root):
    work = root / 'arm'
    work.mkdir()
    copy = work / session.name
    if session.is_dir():
        shutil.copytree(session, copy, ignore=shutil.ignore_patterns('*.tmp-*'))
    else:
        shutil.copy2(session, copy)
    c = Client(core, [copy], root, 'arm')
    try:
        device = c.ok('device-state')
        unsupported = [('ms-state', ''), ('ms-files', '/')]
        if device == 'palm-tx':
            unsupported.append(('hold', 'on'))
        # As the desktop does, the ARM core saves over the session it runs in its machine folder.
        saved = next((p for p in (copy / (session.name + '.frond'), *sorted(copy.glob('*.frond'))) if p.is_file()),
                     copy) if copy.is_dir() else copy
        common(c, work, device, unsupported, saved)
        c.quit()
    finally:
        c.close()
    c = Client(core, [saved], root, 'arm-reload')
    try:
        assert c.ok('device-state') == device
        c.quit()
    finally:
        c.close()
    return device


def check_68k(core, rom, root):
    work = root / 'iiix'
    work.mkdir()
    launcher = work / 'launcher.frond'
    boot = palm_boot.Core(['--rom', rom, '--device', 'palm-iiix'], work / 'boot.txt')
    try:
        palm_boot.iiix_setup(boot, work)
        boot.send(f'save-session "{launcher}"', 'wrote session')
    finally:
        boot.close()
    # The arguments the desktop passes, ARM-only ones included.
    c = Client(core, [launcher, '--work-folder', work, '--window-scale', '1.5', '--smooth-lcd', '--engine', 'fast',
                      '--fast-audio'], root, 'iiix')
    try:
        unsupported = [('usb-mode', 'pty'), ('bluetooth-state', ''), ('bluetooth-mode', 'off'), ('hold', 'on'),
                       ('keyboard-attach', 'on'), ('keyboard-key', '1e d'), ('microphone-mode', 'on'),
                       ('network-mode', 'internet'), ('sd-create', '32'), ('sd-eject', ''), ('ms-state', ''),
                       ('guest-orientation', ''), ('rotate', ''), ('replay-snapshots', '')]
        assert c.ok('hold-state') == 'unavailable'
        saved = work / 'saved.frond'
        common(c, work, 'palm-iiix', unsupported, saved)
        # The IIIx's own: cradle HotSync button, speed, contrast, backlight, launch.
        c.ok('hotsync', code='hotsync')
        c.ok('speed', 'uncapped', code='speed')
        assert c.ok('speed-state').endswith('uncapped')
        c.ok('speed', 'original', code='speed')
        c.fails('speed', 'fast', code='argument')
        assert c.ok('display', 'contrast=70') == 'contrast 70, smooth on'
        c.fails('display', 'contrast=101', code='argument')
        assert c.ok('backlight-state') in ('on', 'off')
        c.ok('launch-app', 'Memo Pad', code='launched')
        c.fails('launch-app', '', code='argument')
        c.ok('reset', code='done')
        c.ok('save', saved, code='saved')
        c.quit()
    finally:
        c.close()
    c = Client(core, [saved, '--device', 'palm-iiix'], root, 'iiix-reload')
    try:
        assert c.ok('device-state') == 'palm-iiix'
        c.until_ok('guest-ready')
        assert PATTERN_NAME.encode().hex() in c.ok('db-list'), 'database lost on reload'
        c.quit()
    finally:
        c.close()
    # A bad socket path or a missing launcher stops the core with an error, not a hang.
    # The missing socket sits in a short temporary folder so a long work path can't hit the 108-byte limit.
    short = tempfile.mkdtemp(prefix='frond-')
    try:
        bad = subprocess.run([str(core), str(saved), '--desktop-socket', str(Path(short) / 'none')],
                             capture_output=True, text=True, timeout=60,
                             env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'))
    finally:
        shutil.rmtree(short, ignore_errors=True)
    assert bad.returncode != 0 and 'cannot connect' in bad.stderr, bad.stderr
    both = subprocess.run([str(core), str(saved), '--run-for', '1', '--desktop-socket', 'x'],
                          capture_output=True, text=True, timeout=60)
    assert both.returncode == 2, both.stderr


def arm_session(given):
    if given:
        return given
    if os.environ.get('FROND_ARM_SESSION'):
        return Path(os.environ['FROND_ARM_SESSION'])
    for path in (ROOT / 'build/smoke/machines/palm-tx', ROOT / 'build/smoke/palm-tx.session/palm-tx.session.frond',
                 ROOT / 'build/smoke/palm-tx.session'):
        if path.exists():
            return path
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--arm-session', type=Path)
    parser.add_argument('--rom', type=Path)
    parser.add_argument('--keep', type=Path, help='keep logs, saves and exports here')
    args = parser.parse_args()
    session = arm_session(args.arm_session)
    rom = args.rom or palm_boot.palm_images() / 'Palm/Generic-OS-3.5-EZ/Palm-OS-3.5-en-ez.rom'
    (ROOT / 'build').mkdir(exist_ok=True)
    failed = 0
    with tempfile.TemporaryDirectory(prefix='core-control-', dir=ROOT / 'build') as work:
        root = (args.keep or Path(work)).resolve()
        root.mkdir(parents=True, exist_ok=True)
        for name, ready, run in (('frond', session and session.exists(),
                                  lambda: check_arm(ROOT / 'out/frond', session.resolve(), root)),
                                 ('frond-68k', rom.is_file(), lambda: check_68k(ROOT / 'out/frond-68k', rom.resolve(), root))):
            if not ready:
                print(f'core-control {name}: skipped, no {"ARM smoke session" if name == "frond" else "IIIx ROM"}')
                continue
            try:
                result = run()
                print(f'core-control {name}: ok' + (f' ({result})' if result else ''), flush=True)
            except AssertionError as error:
                failed += 1
                print(f'core-control {name}: FAIL, {error}', flush=True)
    print('core-control: ' + ('FAIL' if failed else 'ok'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
