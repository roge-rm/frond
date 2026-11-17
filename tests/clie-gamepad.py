#!/usr/bin/env python3
"""CLIÉ game controller (PEGA-GC10) on the TG50: plug-in handshake, buttons, Hold, live detach and
re-attach, and a save and reload with the pad in.

  tests/clie-gamepad.py

Runs out/frond headless on a copy of the TG50 smoke session (FROND_TG50_SESSION, else
FROND_ARM_SESSIONS/sony-clie-tg50, else build/smoke/machines/sony-clie-tg50 or the older
build/smoke/sony-clie-tg50.session) over the desktop control socket. The pad plugs in live
(`gamepad on`), and the ROM must answer its ID (gamepad-state "identified", one reply). Left
(Date Book), A (To Do), B (Memo), Home (launcher) and Left again must give the factory mapping's
four different screens, the last the same as the first. With Hold on a press sends nothing and
changes nothing. Detach and attach again identifies again; a save with the pad in reloads
attached and A still opens To Do. Skipped
without the session. The run goes in build/test-clie-gamepad/TIME/ (mode 0700); only the logs and
snapshots stay after a pass.
"""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

CORE = Path(os.environ.get('FROND_CORE', ROOT / 'out/frond'))


def session():
    candidates = []
    if os.environ.get('FROND_TG50_SESSION'):
        candidates.append(Path(os.environ['FROND_TG50_SESSION']))
    if os.environ.get('FROND_ARM_SESSIONS'):
        candidates.append(Path(os.environ['FROND_ARM_SESSIONS']) / 'sony-clie-tg50')
    candidates += [ROOT / 'build/smoke/machines/sony-clie-tg50', ROOT / 'build/smoke/sony-clie-tg50.session']
    for path in candidates:
        if path.is_dir() or path.is_file():
            return path
    return None


class Client:
    """One core on a private control socket, as the desktop starts it."""

    def __init__(self, args, log):
        self.socket_dir = tempfile.mkdtemp(prefix='frond-pad-')
        address = str(Path(self.socket_dir) / 'control')
        listener = socket.socket(socket.AF_UNIX)
        listener.bind(address)
        listener.listen(1)
        self.log = log.open('wb')
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
        env.pop('FROND_CLIE_GAMEPAD', None)
        self.process = subprocess.Popen([str(CORE), *map(str, args), '--no-sound', '--desktop-socket', address],
                                        stdout=self.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                        start_new_session=True, env=env)
        try:
            listener.settimeout(120)
            self.connection, _ = listener.accept()
        finally:
            listener.close()
        self.connection.settimeout(120)
        self.stream = self.connection.makefile('rb')
        self.serial = 0
        assert self.stream.readline() == b'0\tevent\tready\t\n', 'no ready event'

    def request(self, command, argument=''):
        self.serial += 1
        self.connection.sendall(f'{self.serial}\t{command}\t{str(argument).encode().hex()}\n'.encode())
        while True:
            line = self.stream.readline()
            assert line.endswith(b'\n'), f'{command}: connection closed'
            fields = line.rstrip(b'\n').split(b'\t')
            if fields[0] == b'0' and fields[1] == b'event':
                continue
            assert fields[0] == str(self.serial).encode(), fields
            return fields[1] == b'ok', fields[2].decode(), bytes.fromhex(fields[3].decode()).decode()

    def ok(self, command, argument=''):
        ok, code, text = self.request(command, argument)
        assert ok, f'{command} {argument!r}: {code} {text}'
        return text

    def until(self, command, done, timeout=60, argument=''):
        end = time.monotonic() + timeout
        while True:
            ok, code, text = self.request(command, argument)
            if ok and done(text):
                return text
            assert ok or code == 'not-ready', f'{command}: {code} {text}'
            assert time.monotonic() < end, f'{command}: {text!r} after {timeout} s'
            time.sleep(0.2)

    def quit(self):
        self.ok('quit')
        assert self.process.wait(timeout=60) == 0, 'core exit status'
        self.close()

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        self.connection.close()
        self.log.close()
        shutil.rmtree(self.socket_dir, ignore_errors=True)


def state(c):
    text = c.ok('gamepad-state')
    fields = dict(f.split('=') for f in text.split() if '=' in f)
    return text, {k: int(v) for k, v in fields.items()}


def replay(c, work, name, lines):
    """Runs (ms, event) lines as an input replay and waits for its end."""
    path = work / f'{name}.replay'
    path.write_text(''.join(f'{at} {event}\n' for at, event in lines))
    c.ok('input-replay', path)
    end = time.monotonic() + 90
    while c.request('replay-state')[1] != 'finished':
        assert time.monotonic() < end, f'{name}: replay still running after 90 s'
        time.sleep(0.2)


def press(c, work, name, button, settle=3000):
    """Presses a pad button for 150 ms and snapshots the screen settle ms after."""
    shot = work / f'{name}.png'
    replay(c, work, name, [(0, f'key-down pad-{button}'), (150, f'key-up pad-{button}'),
                           (settle, f'snapshot "{shot}"')])
    return shot


def differ(a, b):
    """The share of pixels that differ between two snapshots."""
    ra, rb = palm_boot.png_rows(a), palm_boot.png_rows(b)
    total = changed = 0
    for x, y in zip(ra, rb):
        for i in range(0, min(len(x), len(y)), 4):
            total += 1
            changed += x[i:i + 3] != y[i:i + 3]
    return changed / max(total, 1)


def main():
    machine = session()
    if not machine or not CORE.exists():
        print('clie gamepad: SKIP, no TG50 smoke session or out/frond')
        return 0
    base = ROOT / 'build/test-clie-gamepad'
    base.mkdir(mode=0o700, parents=True, exist_ok=True)
    base.chmod(0o700)
    work = base / time.strftime('%Y%m%d-%H%M%S')
    work.mkdir(mode=0o700)
    copy = work / ('start' if machine.is_dir() else 'start' + machine.suffix)
    (shutil.copytree if machine.is_dir() else shutil.copy2)(machine, copy)
    saved = work / 'attached' / 'attached.frond'
    saved.parent.mkdir(mode=0o700)
    c = None
    try:
        c = Client([copy], work / 'first.log')
        c.until('guest-ready', lambda _: True, 120)
        assert c.ok('gamepad-state') == 'detached', 'the pad is attached before gamepad on'
        ok, code, _ = c.request('gamepad-key', 'home d')
        assert not ok and code == 'detached', 'a pad key went through with no pad'

        # Plug in: GPIO3 low, SonySys's ID exchange, one AA reply.
        c.ok('gamepad', 'on')
        text = c.until('gamepad-state', lambda t: 'identified' in t, 30)
        _, n = state(c)
        assert n['replies'] == 1 and n['ids'] >= 1, f'plug-in: {text}'
        print(f'clie gamepad: plugged in, {text}')

        # Each press goes from an app to another, so IrDA takes the UART after each switch and the
        # next press has the pad identify again first.
        datebook = press(c, work, 'left-datebook', 'left')
        _, after = state(c)
        assert after['sent'] >= n['sent'] + 2, f'Left sent {after["sent"] - n["sent"]} bytes'
        todo = press(c, work, 'a-todo', 'a')
        memo = press(c, work, 'b-memo', 'b')
        home = press(c, work, 'home-launcher', 'home')
        datebook2 = press(c, work, 'left-datebook-again', 'left')
        shots = {'Date Book': datebook, 'To Do': todo, 'Memo': memo, 'launcher': home}
        names = list(shots)
        for i, a in enumerate(names):
            for b in names[i + 1:]:
                assert differ(shots[a], shots[b]) > 0.008, f'{a} and {b} look the same'
        assert differ(datebook, datebook2) < 0.003, 'Left from the launcher is not Date Book'
        print(f'clie gamepad: Left, A, B, Home and Left reached the guest, {c.ok("gamepad-state")}')

        # Hold: no bytes and no change.
        replay(c, work, 'hold-on', [(0, 'key-down pad-hold'), (50, 'key-up pad-hold')])
        text, before = state(c)
        assert ' hold' in text, text
        held = press(c, work, 'hold-a', 'a')
        _, after = state(c)
        assert after['sent'] == before['sent'], f'Hold on, still sent {after["sent"] - before["sent"]} bytes'
        assert differ(datebook, held) < 0.003, 'a press with Hold on changed the screen'
        replay(c, work, 'hold-off', [(0, 'key-down pad-hold'), (50, 'key-up pad-hold')])
        assert ' hold' not in c.ok('gamepad-state')

        # Unplug and plug in again while running.
        assert c.ok('gamepad', 'off') == 'detached'
        assert not c.request('gamepad-key', 'a d')[0]
        c.ok('gamepad', 'on')
        text = c.until('gamepad-state', lambda t: 'identified' in t, 30)
        print(f'clie gamepad: plugged in again, {text}')
        memo2 = press(c, work, 'b-memo-reattached', 'b')
        assert differ(memo, memo2) < 0.003, 'B after re-attach is not Memo'

        # Save with the pad in, reload in a new process.
        c.ok('save', saved)
        c.quit()
        c = Client([saved], work / 'reload.log')
        c.until('guest-ready', lambda _: True, 120)
        text = c.ok('gamepad-state')
        assert text.startswith('attached'), f'reloaded as {text}'
        print(f'clie gamepad: reloaded {text}')
        todo2 = press(c, work, 'reload-a-todo', 'a')
        assert differ(todo, todo2) < 0.003, 'A after the reload is not To Do'
        c.ok('gamepad', 'off')
        assert c.ok('gamepad-state') == 'detached'
        c.quit()
        c = None
    except AssertionError as error:
        print(f'clie gamepad: FAIL, {error} (see {work})')
        return 1
    finally:
        if c:
            c.close()
    # The machine copies hold firmware state.
    shutil.rmtree(copy, ignore_errors=True) if copy.is_dir() else copy.unlink(missing_ok=True)
    shutil.rmtree(saved.parent, ignore_errors=True)
    print('clie gamepad: ok, the ROM identified the pad, Left, A, B and Home reached the guest, Hold sent nothing, '
          'live detach and re-attach and a save and reload with the pad in')
    return 0


if __name__ == '__main__':
    sys.exit(main())
