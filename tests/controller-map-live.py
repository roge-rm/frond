#!/usr/bin/env python3
"""A controller map changed while an ARM machine runs takes effect at once, as the desktop's
Controls dialog sends it: controller-map and app-active take their argument over the control socket,
and a virtual game controller's B, left out of the map, does nothing until
controller-map gives it the first app button, then changes the screen.

  tests/controller-map-live.py [SESSION]

Runs out/frond headless on a copy of the T|X smoke machine (SESSION, else FROND_ARM_SESSION, else
build/smoke/machines/palm-tx); skipped without it. The controller is tests/controller-vpad.c,
built here and loaded with LD_PRELOAD; without cc or SDL2's headers only the socket half runs.
The run goes in build/test-controller-map-live/TIME/ (mode 0700); only the logs stay after a pass.
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
    subprocess.run(['cc', '-shared', '-fPIC', '-O1', str(ROOT / 'tests/controller-vpad.c'), '-o', str(shim),
                    '-ldl', *cflags.stdout.split()], check=True)
    return shim


class Core:
    def __init__(self, session, root, shim):
        self.socket_dir = tempfile.mkdtemp(prefix='frond-cml-')
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


def main():
    session = session_path()
    if not session:
        print('controller-map-live: no T|X smoke machine, skipped')
        return
    root = ROOT / 'build/test-controller-map-live' / time.strftime('%Y%m%d-%H%M%S')
    root.mkdir(parents=True)
    os.chmod(root.parent, 0o700)
    os.chmod(root, 0o700)
    work = root / 'machine'
    shutil.copytree(session.parent, work)
    shim = build_shim(root)
    core = Core(work / session.name, root, shim)
    try:
        assert core.ok('app-active', 'off') == 'off'
        assert core.ok('app-active', 'on') == 'on'
        assert core.ok('controller-map', 'b:app1,a:centre') == 'a:centre,b:app1'
        assert core.ok('controller-map', 'none') == 'none'
        assert core.ok('controller-map', '') == 'none'
        # A map that leaves B unused.
        unused = core.ok('controller-map', 'leftshoulder:app1')
        assert unused == 'leftshoulder:app1', unused
        if shim:
            core.ok('guest-ready')
            time.sleep(5)
            before = core.frame(root / 'before.png')
            core.press(BUTTON_B)
            time.sleep(3)
            assert core.frame(root / 'unused.png') == before, 'B pressed something under a map without it'
            assert core.ok('controller-map', 'b:app1') == 'b:app1'
            core.press(BUTTON_B)
            end = time.monotonic() + 20
            while core.frame(root / 'mapped.png') == before:
                assert time.monotonic() < end, 'B mapped while running pressed nothing'
                time.sleep(1)
        else:
            print('controller-map-live: no cc or SDL2 headers, controller presses skipped')
    finally:
        core.close()
    assert 'vpad: attached' in (root / 'core.log').read_text(errors='replace') or not shim, 'no virtual controller'
    shutil.rmtree(work)
    for path in root.iterdir():
        if path.suffix in ('.png', '.replay', '.so', '.fifo'):
            path.unlink()
    print('controller-map-live: ok')


if __name__ == '__main__':
    main()
