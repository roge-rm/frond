#!/usr/bin/env python3
"""Runs the same desktop control socket commands against out/frond (an ARM smoke session) and
out/frond-68k (a IIIx ROM booted to the launcher), checking the replies, the pushed link events, save
and reload, and that commands a device can't do fail cleanly as "unsupported".

Private firmware and sessions: --arm-session, else FROND_ARM_SESSION, else the palm-tx smoke machine
(build/smoke/machines/palm-tx, or an older build/smoke/palm-tx.session); --rom, else the generic OS 3.5 EZ ROM in the
firmware folder. A side with neither is skipped. FROND_CORE picks another ARM core binary. Scratch goes in build/core-control-*.
"""
import argparse
import fcntl
import os
from pathlib import Path
import shutil
import select
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

PATTERN = ROOT / 'tests/fixtures/frond-pattern.pdb'
PATTERN_NAME = 'frond-pattern.sunvox'


class Client:
    """One core on a private control socket, as the desktop starts it."""

    def __init__(self, core, args, root, name, piped=False):
        """piped: stdout and stderr go through a pipe copied to the log, which drop() closes, as the
        desktop's pipes lose their reader when it dies."""
        self.socket_dir = tempfile.mkdtemp(prefix='frond-control-')
        address = str(Path(self.socket_dir) / 'control')
        listener = socket.socket(socket.AF_UNIX)
        listener.bind(address)
        listener.listen(1)
        self.log = (root / f'{name}.log').open('wb')
        self.pipe, self.copier, self.copying = None, None, False
        output = self.log
        if piped:
            self.pipe, output = os.pipe()
            self.copying = True
            self.copier = threading.Thread(target=self.copy_output, daemon=True)
            self.copier.start()
        runner = ['heavy', 'emulator'] if shutil.which('heavy') else []
        try:
            self.process = subprocess.Popen([*runner, str(core), *map(str, args), '--no-sound', '--desktop-socket', address],
                                            stdout=output, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                            start_new_session=True,
                                            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'))
        finally:
            if piped:
                os.close(output)
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

    def until_text(self, command, done, timeout=90):
        """Repeats command until done(text) holds; returns the text."""
        end = time.monotonic() + timeout
        while not done(text := self.ok(command)):
            assert time.monotonic() < end, f'{command}: {text!r} after {timeout} s'
            time.sleep(0.2)
        return text

    def quit(self):
        self.ok('quit')
        assert self.process.wait(timeout=30) == 0, 'core exit status'
        self.close()

    def copy_output(self):
        while self.copying:
            ready, _, _ = select.select([self.pipe], [], [], 0.1)
            if ready:
                data = os.read(self.pipe, 65536)
                if not data:
                    break
                self.log.write(data)
                self.log.flush()

    def close_output(self):
        """Stops reading the core's output and closes the pipe, so its next write gets EPIPE."""
        if self.copier:
            self.copying = False
            self.copier.join()
            self.copier = None
            os.close(self.pipe)
            self.pipe = None

    def drop(self):
        """Closes the socket as a killed desktop would, with no quit, and the output pipe if there is one."""
        self.stream.close()
        self.connection.close()
        self.connection = None
        self.close_output()

    def close(self):
        if self.connection:
            self.connection.close()
            self.connection = None
        if self.process.poll() is None:
            palm_boot.stop_guest(self.process)
        self.close_output()
        self.log.close()
        shutil.rmtree(self.socket_dir, ignore_errors=True)


def unix_modes(c):
    """The unix: endpoint through the live mode commands and their state replies, for serial and infrared."""
    folder = Path(tempfile.mkdtemp(prefix='frond-'))  # short, so the path fits a socket address
    try:
        for command, state, code in (('serial-mode', 'serial-state', 'serial'), ('irda-mode', 'irda-state', 'infrared')):
            path = folder / f'{code}.sock'

            def until(text):
                end = time.monotonic() + 20
                while text not in c.ok(state):
                    assert time.monotonic() < end, f'{state}: no "{text}" in {c.ok(state)}'
                    time.sleep(0.1)

            def connect():
                client = socket.socket(socket.AF_UNIX)
                client.settimeout(10)
                client.connect(str(path))
                return client

            c.fails(command, 'unix:', code='argument')
            # The serial state reads "unix PATH waiting"; the infrared one "unix:PATH PATH waiting".
            for text in (c.ok(command, f'unix:{path}', code=code), c.ok(state)):
                assert text.startswith('unix') and f'{path} waiting' in text, text
            assert any(name == code and text.startswith('unix') and f'{path} ' in text for name, text in c.events), c.events
            assert stat.S_ISSOCK(os.lstat(path).st_mode)
            client = connect()
            until(f'{path} connected')
            client.sendall(b'hello')
            second = connect()
            assert second.recv(1) == b'', 'a second client was kept'
            second.close()
            assert 'connected' in c.ok(state)
            client.close()
            until(f'{path} waiting')
            client = connect()
            until(f'{path} connected')
            # tcp and back, then off: the socket file goes each time. The change comes right after
            # bytes from the peer, which the UART may still hold.
            client.sendall(b'hello')
            assert '127.0.0.1:' in c.ok(command, 'tcp:0', code=code)
            client.close()
            assert not path.exists()
            assert f'{path} ' in c.ok(command, f'unix:{path}', code=code)
            assert c.ok(command, 'off', code=code) == 'off'
            assert not path.exists()
            # Refusals leave the link off and the path as it was.
            os.chmod(folder, 0o755)
            text = c.fails(command, f'unix:{path}', code=code)
            assert '0700' in text, text
            assert c.ok(state).startswith('off') and not path.exists()
            os.chmod(folder, 0o700)
            path.write_text('keep')
            assert 'not a socket' in c.fails(command, f'unix:{path}', code=code)
            assert path.read_text() == 'keep'
            path.unlink()
            assert 'too long' in c.fails(command, f'unix:{folder}/{"a" * 120}', code=code)
            c.fails(command, 'unix:relative.sock', code=code)
            c.fails(command, f'unix:{folder}/none/x.sock', code=code)
            assert c.ok(state).startswith('off')
            # A leftover socket file with no listener is replaced, and quitting removes it.
            assert f'{path} ' in c.ok(command, f'unix:{path}', code=code)
            assert c.ok(command, 'off', code=code) == 'off'
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def usb_unix(c, folder, path):
    """--usb unix:PATH from the start, then usb-mode with unix: live (it shares the serial parser)."""
    def until(text):
        end = time.monotonic() + 20
        while text not in c.ok('usb-state'):
            assert time.monotonic() < end, f'usb-state: no "{text}" in {c.ok("usb-state")}'
            time.sleep(0.1)

    until(f'unix {path} waiting')
    assert stat.S_ISSOCK(os.lstat(path).st_mode)
    client = socket.socket(socket.AF_UNIX)
    client.settimeout(10)
    client.connect(str(path))
    until(f'unix {path} connected')
    client.close()
    until(f'unix {path} waiting')
    assert c.ok('usb-mode', 'off', code='usb') == 'off' and not path.exists()
    moved = folder / 'usb-moved.sock'
    assert c.ok('usb-mode', f'unix:{moved}', code='usb').startswith(f'unix {moved} waiting')
    assert stat.S_ISSOCK(os.lstat(moved).st_mode)
    assert '127.0.0.1:' in c.ok('usb-mode', 'tcp:0', code='usb') and not moved.exists()
    c.fails('usb-mode', 'unix:', code='argument')
    c.fails('usb-mode', 'unix:relative.sock', code='usb')
    os.chmod(folder, 0o755)
    assert '0700' in c.fails('usb-mode', f'unix:{path}', code='usb')
    os.chmod(folder, 0o700)
    assert c.ok('usb-state').startswith('off') and not path.exists()
    assert c.ok('usb-mode', f'unix:{path}', code='usb').startswith(f'unix {path} waiting')
    assert c.ok('usb-mode', 'off', code='usb') == 'off' and not path.exists()


def palm_database(path, name, kind, creator, resource):
    """Writes a minimal .prc (an empty resource database) or a .pdb with one record."""
    header = name.encode().ljust(32, b'\0') + struct.pack('>HHIIIIII', 1 if resource else 0, 1, 0, 0, 0, 0, 0, 0)
    header += kind + creator + struct.pack('>IIH', 0, 0, 0 if resource else 1)
    path.write_bytes(header + (b'' if resource else struct.pack('>IBBBB', 80 + 8, 0, 0, 0, 1) + b'\0\0' + b'data'))
    return path


def delete_application(c, listing_text, app):
    """What the Android app does for Delete: the application first, then the others with its creator."""
    entries = [line.split(' ') for line in listing_text.splitlines()]
    named = [(bytes.fromhex(e[3]).decode('latin-1'), e[1]) for e in entries if len(e) == 4]
    creator = dict(named)[app]
    c.ok('delete-db', app, code='deleted')
    for name, owner in named:
        if owner == creator and name != app:
            c.ok('delete-db', name, code='deleted')


def common(c, root, device, unsupported, saved):
    """The checks both runners share, ending with a save to saved."""
    assert c.ok('device-state', code='device') == device
    c.fails('not-a-command', code='command')
    c.fails('device-state', 'unexpected', code='argument')
    c.fails('window-id', code='unsupported')  # the dummy video driver has no X11 window
    c.fails('window-id', 'x', code='argument')
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
    unix_modes(c)
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
    # Delete after the save, so the reload checks still find the database; install it again after.
    c.fails('delete-db', '', code='argument')
    c.fails('delete-db', 'No Such Database', code='missing')
    c.ok('delete-db', PATTERN_NAME, code='deleted')
    assert PATTERN_NAME.encode().hex() not in c.ok('db-list', code='databases'), 'deleted database still listed'
    assert c.ok('install', PATTERN, code=('installed', 'needs-reset'))
    # An application and its data go together, and a ROM database is refused.
    app = palm_database(root / 'frond-test-app.prc', 'frond-test-app', b'appl', b'FTst', True)
    data = palm_database(root / 'frond-test-data.pdb', 'frond-test-data', b'DATA', b'FTst', False)
    for name, path in (('frond-test-data', data), ('frond-test-app', app)):
        c.ok('install', path, code=('installed', 'needs-reset'))
        assert name.encode().hex() in c.ok('db-list', code='databases'), f'{name} not listed'
    delete_application(c, c.ok('db-list', code='databases'), 'frond-test-app')
    left = c.ok('db-list', code='databases')
    assert 'frond-test-app'.encode().hex() not in left and 'frond-test-data'.encode().hex() not in left, left
    assert PATTERN_NAME.encode().hex() in left, 'delete took another creator\'s database'
    refused = [c.request('delete-db', name)[1] for name in ('Launcher', 'Memo Pad', 'Memos-PMem')]
    assert 'rom' in refused, f'no ROM database refused: {refused}'
    c.ok('guest-ready')


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
        # A lone file is extracted to <stem>/ on load, so the save goes to <stem>/<stem>.frond.
        saved = next((p for p in (copy / (session.name + '.frond'), *sorted(copy.glob('*.frond'))) if p.is_file()),
                     copy) if copy.is_dir() else copy.parent / copy.stem / (copy.stem + '.frond')
        common(c, work, device, unsupported, saved)
        c.fails('serial-mode', 'modem', code='argument')  # the snap-on modem is frond-68k's
        # The infrared phone takes the IR port, and the IR keyboard and the phone keep each other out.
        c.events.clear()
        assert c.ok('irda-mode', 'phone', code='infrared') == 'phone idle'
        assert c.ok('irda-state', code='infrared') == 'phone idle'
        assert c.ok('irda-stats', code='infrared').startswith('frames_in=0 ')
        c.ok('speed-state')
        assert ('infrared', 'phone idle') in c.events, c.events
        c.fails('keyboard-attach', 'on', code='infrared')
        assert c.ok('irda-mode', 'pty', code='infrared').startswith('pty /dev/pts/')
        assert c.ok('irda-mode', 'off', code='infrared') == 'off'
        assert c.ok('keyboard-attach', 'on', code='keyboard') == 'attached'
        c.fails('irda-mode', 'phone', code='infrared')
        assert c.ok('keyboard-attach', 'off', code='keyboard') == 'detached'
        if not copy.is_dir():
            # Saving back to the lone file goes to the folder it was loaded into and leaves the file as it was.
            before, stamp = copy.read_bytes(), saved.stat().st_mtime_ns
            c.ok('save', copy, code='saved')
            assert copy.read_bytes() == before, 'lone session file changed'
            assert saved.stat().st_mtime_ns != stamp, 'lone save missed the folder it was loaded into'
        # A lone file or .frond whose <stem>/ is another machine's folder is refused and the folder kept.
        other = work / 'other'
        other.mkdir()
        files = {other / 'other.frond': b'another machine', other / 'sd.img': b'its card'}
        for path, data in files.items():
            path.write_bytes(data)
        lone = work / 'other.session'
        lone.write_bytes(b'not this machine')
        for target in (lone, work / 'other.frond'):
            c.fails('save', target, code='save')
        assert all(path.read_bytes() == data for path, data in files.items()), 'another machine changed'
        assert sorted(other.iterdir()) == sorted(files) and lone.read_bytes() == b'not this machine'
        # A new .frond beside someone's lone .frond goes to its own <stem>/ and leaves that file alone.
        beside = work / 'beside'
        beside.mkdir()
        (beside / 'palm-tx.session.frond').write_bytes(b'a lone session')
        c.ok('save', beside / 'other.frond', code='saved')
        assert (beside / 'palm-tx.session.frond').read_bytes() == b'a lone session', 'lone .frond overwritten'
        assert (beside / 'other/other.frond').stat().st_size > 100000, 'new session not in its own folder'
        # Saving over a lone v6 .frond goes to <stem>/, where loading would extract it, and keeps the file.
        v6 = work / 'v6'
        v6.mkdir()
        old = v6 / 'old.frond'
        old.write_bytes(struct.pack('<II', 0x19800819, 6) + b'palm-tx'.ljust(32, b'\0') + b'v6 body')
        before = old.read_bytes()
        c.ok('save', old, code='saved')
        assert old.read_bytes() == before, 'lone v6 .frond rewritten'
        assert (v6 / 'old/old.frond').stat().st_size > 100000, 'v6 save not where loading extracts it'
        c.quit()
    finally:
        c.close()
    # Reload the save as a lone .frond (it runs in its parent) and save back to it in place. A session with
    # cards can't be lone, so its whole folder is copied instead.
    alone = work / 'alone'
    cards = [p for p in saved.parent.iterdir() if p != saved and not p.name.startswith('.')]
    if cards:
        shutil.copytree(saved.parent, alone, ignore=shutil.ignore_patterns('*.tmp-*'))
        lone = alone / saved.name
    else:
        alone.mkdir()
        lone = alone / 'tx.frond'
        shutil.copy2(saved, lone)
    usb_folder = Path(tempfile.mkdtemp(prefix='frond-'))  # short, so the path fits a socket address
    usb_path = usb_folder / 'usb.sock'
    c = Client(core, [lone, '--usb', f'unix:{usb_path}'], root, 'arm-reload')
    try:
        assert c.ok('device-state') == device
        if 'usb-mode' not in [command for command, _ in unsupported]:
            usb_unix(c, usb_folder, usb_path)
        stamp = lone.stat().st_mtime_ns
        c.ok('save', lone, code='saved')
        assert lone.stat().st_mtime_ns != stamp, 'lone .frond not saved back'
        assert cards or sorted(alone.iterdir()) == [lone], sorted(alone.iterdir())
        c.quit()
    finally:
        c.close()
        shutil.rmtree(usb_folder, ignore_errors=True)
    work_folder(core, session, work, root, device)
    # A copy of the machine folder saved after common(), which still has the pattern database.
    gone = work / 'gone'
    shutil.copytree(saved.parent, gone, ignore=shutil.ignore_patterns('*.tmp-*'))
    disconnect(core, [gone / saved.name], gone / saved.name, root, 'arm')
    return device


def core_locked(target):
    """True while a core holds <target>.core, as the desktop checks it."""
    try:
        fd = os.open(f'{target}.core', os.O_RDONLY)
    except FileNotFoundError:
        return False
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return False
    except BlockingIOError:
        return True
    finally:
        os.close(fd)


def disconnect(core, args, target, root, name):
    """With --save-on-disconnect, as the desktop starts the core. The socket closing without a quit
    saves the session to target and the core exits 0, even with its output pipe closed at the same
    time. It holds target.core locked for its run. No save after a quit still unread when the socket
    closed or after a protocol error, and a failed save exits non-zero."""
    folder = target.parent
    run = lambda log, piped=False: Client(core, [*args, '--save-on-disconnect', target], root, f'{name}-{log}', piped)
    stamp = target.stat().st_mtime_ns
    c = run('gone', piped=True)
    try:
        c.until_ok('guest-ready')
        c.ok('delete-db', PATTERN_NAME, code='deleted')
        assert core_locked(target), 'target.core not locked while the core runs'
        # A second core on the same session is refused before it looks for a launcher.
        short = tempfile.mkdtemp(prefix='frond-')
        try:
            second = subprocess.run([str(core), *map(str, args), '--save-on-disconnect', str(target),
                                     '--desktop-socket', str(Path(short) / 'none')],
                                    capture_output=True, text=True, timeout=60,
                                    env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'))
        finally:
            shutil.rmtree(short, ignore_errors=True)
        assert second.returncode != 0 and 'another core is running' in second.stderr, second.stderr
        c.drop()
        assert c.process.wait(timeout=60) == 0, f'core exit status {c.process.returncode} after the socket and its output closed'
    finally:
        c.close()
    assert target.stat().st_mtime_ns != stamp, 'session not saved when the socket closed'
    assert not core_locked(target), 'target.core still locked after the core exited'
    c = run('gone-reload')
    try:
        c.until_ok('guest-ready')
        assert PATTERN_NAME.encode().hex() not in c.ok('db-list', code='databases'), 'saved state not the latest'
        stamp = target.stat().st_mtime_ns
        # An install keeps the core busy, so the quit sent with it is still unread when the socket closes.
        c.connection.sendall(f'{c.serial + 1}\tinstall\t{str(PATTERN).encode().hex()}\n{c.serial + 2}\tquit\t\n'.encode())
        c.drop()
        assert c.process.wait(timeout=60) == 0, 'core exit status after a quit and a closed socket'
    finally:
        c.close()
    assert target.stat().st_mtime_ns == stamp, 'a quit then a closed socket saved the session'
    c = run('gone-protocol')
    try:
        c.until_ok('guest-ready')
        c.connection.sendall(b'not a request\n')
        c.process.wait(timeout=60)
        c.drop()
    finally:
        c.close()
    assert target.stat().st_mtime_ns == stamp, 'a protocol error saved the session'
    c = run('gone-fails')
    try:
        c.until_ok('guest-ready')
        folder.chmod(0o500)
        c.drop()
        assert c.process.wait(timeout=60) != 0, 'a failed save on disconnect exited 0'
    finally:
        folder.chmod(0o700)
        c.close()
    assert target.stat().st_mtime_ns == stamp, 'a save into a read-only folder changed the session'
    for log, saved in (('gone-reload', False), ('gone-protocol', False), ('gone-fails', False)):
        text = (root / f'{name}-{log}.log').read_text(errors='replace')
        assert ('session saved to' in text) == saved, f'{log}: {text[-400:]}'
    assert 'could not be saved' in (root / f'{name}-gone-fails.log').read_text(errors='replace')


def files_of(folder):
    """Every file under folder with its bytes and mtime."""
    return {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in sorted(folder.rglob('*')) if p.is_file()}


def work_folder(core, session, work, root, device):
    """Run with --work-folder, as the desktop runs a session from outside its machines folder: saves
    back to the session go to the work folder and leave the original's files as they were."""
    original = work / 'wf-original'
    original.mkdir()
    source = original / session.name
    if session.is_dir():
        shutil.copytree(session, source, ignore=shutil.ignore_patterns('*.tmp-*'))
    else:
        shutil.copy2(session, source)
    before = files_of(original)
    run = work / 'wf-run'
    run.mkdir()
    c = Client(core, [source, '--work-folder', run], root, 'arm-work-folder')
    try:
        assert c.ok('device-state') == device
        targets = [source]
        if source.is_dir():
            targets += [p for p in source.glob('*.frond') if p.is_file()]
        for target in targets:
            stamp = {p: p.stat().st_mtime_ns for p in run.glob('*.frond')}
            c.ok('save', target, code='saved')
            saved = [p for p in run.glob('*.frond') if p.stat().st_mtime_ns != stamp.get(p)]
            assert len(saved) == 1 and saved[0].stat().st_size > 100000, \
                f'save to {target.name} not in the work folder: {sorted(run.iterdir())}'
            assert files_of(original) == before, f'save to {target.name} changed the original'
        c.quit()
    finally:
        c.close()
    c = Client(core, [saved[0]], root, 'arm-work-folder-reload')
    try:
        assert c.ok('device-state') == device
        c.quit()
    finally:
        c.close()


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
        # The snap-on modem takes the serial port's place, and a link takes it back.
        c.events.clear()
        assert c.ok('serial-mode', 'modem', code='serial') == 'modem idle'
        assert c.ok('serial-state', code='serial') == 'modem idle'
        assert c.ok('serial-mode', 'pty', code='serial').startswith('pty /dev/pts/')
        assert c.ok('serial-mode', 'modem', code='serial') == 'modem idle'
        c.ok('speed-state')
        assert ('serial', 'modem idle') in c.events, c.events
        # The cradle's DTR reaches the modem: HotSync opens the port, gives up on the modem's echo and
        # closes it again, so DTR rises with the line driver and drops with it.
        assert c.ok('serial-stats', code='serial') == 'power=off dtr=off dtr_rises=0 dtr_drops=0'
        c.ok('hotsync', code='hotsync')

        def closed(text):
            fields = dict(word.split('=') for word in text.split())
            return fields['power'] == 'off' and fields['dtr'] == 'off' and int(fields['dtr_drops']) >= 1
        # HotSync is done when the port stays closed for a second, as it retries more than once.
        text = None
        while (seen := c.until_text('serial-stats', closed)) != text:
            text = seen
            time.sleep(1)
        stats = dict(word.split('=') for word in text.split())
        assert stats['dtr_rises'] == stats['dtr_drops'], stats
        assert c.ok('serial-mode', 'off', code='serial') == 'off'
        # The infrared phone takes the IR port's place, and a link takes it back.
        c.events.clear()
        assert c.ok('irda-mode', 'phone', code='infrared') == 'phone idle'
        assert c.ok('irda-state', code='infrared') == 'phone idle'
        assert c.ok('irda-stats', code='infrared').startswith('port=')
        assert c.ok('irda-mode', 'pty', code='infrared').startswith('pty /dev/pts/')
        assert c.ok('irda-mode', 'phone', code='infrared') == 'phone idle'
        c.ok('speed-state')
        assert ('infrared', 'phone idle') in c.events, c.events
        assert c.ok('irda-mode', 'off', code='infrared') == 'off'
        # The IIIx's own: speed, contrast, backlight, launch. The cradle HotSync button is the row above.
        c.ok('speed', 'uncapped', code='speed')
        assert c.ok('speed-state').endswith('uncapped')
        c.ok('speed', 'original', code='speed')
        c.fails('speed', 'fast', code='argument')
        assert c.ok('display', 'contrast=70') == 'contrast 70, smooth on'
        c.fails('display', 'contrast=101', code='argument')
        assert c.ok('backlight-state') in ('on', 'off')
        c.ok('launch-app', 'Memo Pad', code='launched')
        c.fails('launch-app', '', code='argument')
        # Each step finishes before the next, as a person would wait: db-list runs at Memo Pad's event
        # loop, and the save waits for the boot to reach the UI. A reset during the ROM's store header
        # update (about 2 ms after each key press and every 5 s) formats RAM on a real IIIx too.
        c.ok('db-list', code='databases')
        c.ok('reset', code='done')
        c.until_ok('guest-ready')
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
    gone = work / 'gone' / 'gone.frond'
    gone.parent.mkdir()
    shutil.copy2(saved, gone)
    disconnect(core, [gone, '--device', 'palm-iiix'], gone, root, 'iiix')
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
    # Palm OS 3.1 has no IrCOMM, so frond-68k refuses the infrared phone there.
    old = palm_boot.palm_images() / 'Palm/IIIx/Palm-IIIx-3.1-en.rom'
    if old.is_file():
        refused = subprocess.run([str(core), '--rom', str(old), '--device', 'palm-iiix', '--irda', 'phone', '--run-for', '1'],
                                 capture_output=True, text=True, timeout=60,
                                 env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'))
        assert refused.returncode != 0 and 'needs Palm OS 3.3 or later' in refused.stderr, refused.stderr
    both = subprocess.run([str(core), str(saved), '--run-for', '1', '--desktop-socket', 'x'],
                          capture_output=True, text=True, timeout=60)
    assert both.returncode == 2, both.stderr
    alone = subprocess.run([str(core), str(saved), '--run-for', '1', '--save-on-disconnect', str(saved)],
                           capture_output=True, text=True, timeout=60)
    assert alone.returncode == 2 and 'needs --desktop-socket' in alone.stderr, alone.stderr


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
    failed = ran = 0
    skipped = []
    with tempfile.TemporaryDirectory(prefix='core-control-', dir=ROOT / 'build') as work:
        root = (args.keep or Path(work)).resolve()
        root.mkdir(parents=True, exist_ok=True)
        for name, ready, run in (('frond', session and session.exists(),
                                  lambda: check_arm(Path(os.environ.get('FROND_CORE') or ROOT / 'out/frond'), session.resolve(), root)),
                                 ('frond-68k', rom.is_file(), lambda: check_68k(ROOT / 'out/frond-68k', rom.resolve(), root))):
            if not ready:
                print(f'core-control {name}: skipped, no {"ARM smoke session" if name == "frond" else f"IIIx ROM at {rom}"}')
                skipped.append(name)
                continue
            ran += 1
            try:
                result = run()
                print(f'core-control {name}: ok' + (f' ({result})' if result else ''), flush=True)
            except AssertionError as error:
                failed += 1
                print(f'core-control {name}: FAIL, {error}', flush=True)
    if not ran:
        print('core-control: FAIL, every side was skipped, nothing was checked')
        return 1
    print('core-control: ' + ('FAIL' if failed else 'ok') + (f' (skipped {", ".join(skipped)})' if skipped else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
