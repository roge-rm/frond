#!/usr/bin/env python3
"""A game controller plugged in and out while an ARM machine runs: the core opens it, its B presses
the mapped app button, and unplugging closes it, twice over. Every SDL joystick and game controller
call the core makes happens on the thread that pumps SDL's events (Windows' joystick backends need it).

  tests/controller-hotplug.py [SESSION]

Runs out/frond (FROND_CORE, else out/frond) headless on a copy of the T|X smoke machine (SESSION,
else FROND_ARM_SESSION, else build/smoke/machines/palm-tx); skipped without it, or without cc and
SDL2's headers. The controller is an SDL virtual one from tests/controller-hotplug.c, built here and
loaded with LD_PRELOAD, which also logs the core's controller calls made off the pumping thread.
The run goes in build/test-controller-hotplug/TIME/ (mode 0700); only the logs stay after a pass.
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
BUTTON_B = 1  # SDL_CONTROLLER_BUTTON_B
STATUS_ROWS = 40  # the clock in the status bar changes on its own


def session_path():
    for given in (sys.argv[1] if len(sys.argv) > 1 else None, os.environ.get('FROND_ARM_SESSION'),
                  ROOT / 'build/smoke/machines/palm-tx'):
        if given and Path(given).exists():
            return palm_boot.session_file(Path(given))
    return None


def build_shim(root):
    cflags = subprocess.run(['pkg-config', '--cflags', '--libs', 'sdl2'], capture_output=True, text=True)
    if cflags.returncode or not shutil.which('cc'):
        return None
    shim = root / 'vpad.so'
    subprocess.run(['cc', '-shared', '-fPIC', '-O1', str(ROOT / 'tests/controller-hotplug.c'), '-o', str(shim),
                    '-ldl', *cflags.stdout.split()], check=True)
    return shim


class Core:
    def __init__(self, session, root, shim):
        self.socket_dir = tempfile.mkdtemp(prefix='frond-chp-')
        address = str(Path(self.socket_dir) / 'control')
        listener = socket.socket(socket.AF_UNIX)
        listener.bind(address)
        listener.listen(1)
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
        self.fifo = None
        if shim:
            fifo = root / 'vpad.fifo'
            os.mkfifo(fifo)
            env.update(LD_PRELOAD=str(shim), FROND_VPAD_FIFO=str(fifo))
        self.log = (root / 'core.log').open('wb')
        runner = ['heavy', 'emulator'] if shutil.which('heavy') else []
        self.process = subprocess.Popen([*runner, str(CORE), str(session), '--no-sound', '--desktop-socket', address],
                                        stdout=self.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                        start_new_session=True, env=env)
        try:
            listener.settimeout(900)  # includes waiting for an emulator slot
            self.connection, _ = listener.accept()
        finally:
            listener.close()
        self.connection.settimeout(120)
        self.stream = self.connection.makefile('rb')
        assert self.stream.readline() == b'0\tevent\tready\t\n', 'no ready event'
        self.serial = 0
        if shim:
            self.fifo = os.open(fifo, os.O_WRONLY)  # the core opens its end on its first event pump

    def request(self, command, argument=''):
        self.serial += 1
        self.connection.sendall(f'{self.serial}\t{command}\t{argument.encode().hex()}\n'.encode())
        while True:
            fields = self.stream.readline().rstrip(b'\n').split(b'\t')
            assert len(fields) == 4, f'{command}: {fields}'
            if fields[0] == b'0':
                continue
            return fields[1] == b'ok', fields[2].decode(), bytes.fromhex(fields[3].decode()).decode()

    def ok(self, command, argument=''):
        ok, code, text = self.request(command, argument)
        assert ok, f'{command} {argument!r}: {code} {text}'
        return text

    def frame(self, path):
        path.unlink(missing_ok=True)
        replay = path.with_suffix('.replay')
        replay.write_text(f'0 snapshot "{path}"\n')
        self.ok('input-replay', str(replay))
        end = time.monotonic() + 10
        while not path.is_file():
            assert time.monotonic() < end, 'no frame'
            time.sleep(0.1)
        time.sleep(0.3)
        return palm_boot.png_rows(path)[STATUS_ROWS:]

    def send(self, line):
        os.write(self.fifo, f'{line}\n'.encode())

    def press(self, button):
        for down in (1, 0):
            os.write(self.fifo, f'b {button} {down}\n'.encode())
            time.sleep(0.4)

    def close(self):
        try:
            self.request('quit')
            self.process.wait(timeout=60)
        except (OSError, AssertionError, subprocess.TimeoutExpired):
            pass
        finally:
            palm_boot.stop_guest(self.process)
            if self.fifo is not None:
                os.close(self.fifo)
            self.connection.close()
            self.log.close()
            shutil.rmtree(self.socket_dir, ignore_errors=True)


def wait_log(root, text, count, seconds=20):
    end = time.monotonic() + seconds
    while (root / 'core.log').read_text(errors='replace').count(text) < count:
        assert time.monotonic() < end, f'no {text!r} ({count}) in core.log'
        time.sleep(0.2)


def main():
    session = session_path()
    if not session:
        print('controller-hotplug: no T|X smoke machine, skipped')
        return
    root = ROOT / 'build/test-controller-hotplug' / time.strftime('%Y%m%d-%H%M%S')
    root.mkdir(parents=True)
    os.chmod(root.parent, 0o700)
    os.chmod(root, 0o700)
    shim = build_shim(root)
    if not shim:
        print('controller-hotplug: no cc or SDL2 headers, skipped')
        shutil.rmtree(root)
        return
    work = root / 'machine'
    shutil.copytree(session.parent, work)
    core = Core(work / session.name, root, shim)
    try:
        assert core.ok('app-active', 'on') == 'on'
        assert core.ok('controller-map', 'b:app1') == 'b:app1'
        core.ok('guest-ready')
        time.sleep(5)
        before = core.frame(root / 'before.png')
        for round in (1, 2):
            core.send('attach')
            wait_log(root, 'vpad: attached', round)
            wait_log(root, 'vpad: opened Frond test pad', round)
            if round == 1:
                time.sleep(1)
                core.press(BUTTON_B)
                end = time.monotonic() + 20
                while core.frame(root / 'mapped.png') == before:
                    assert time.monotonic() < end, 'B on the plugged-in controller pressed nothing'
                    time.sleep(1)
            time.sleep(1)
            core.send('detach')
            wait_log(root, 'vpad: detached', round)
            wait_log(root, 'vpad: closed', round)
        time.sleep(1)
    finally:
        core.close()
    log = (root / 'core.log').read_text(errors='replace')
    off = sorted({line for line in log.splitlines() if 'off-thread' in line or 'off the SDL thread' in line})
    assert not off, 'controller calls off the SDL thread: ' + '; '.join(off)
    assert log.count('vpad: closed') == 2, 'a controller closed more than once'
    shutil.rmtree(work)
    for path in root.iterdir():
        if path.suffix in ('.png', '.replay', '.so', '.fifo'):
            path.unlink()
    print('controller-hotplug: ok')


if __name__ == '__main__':
    main()
