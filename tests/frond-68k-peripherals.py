#!/usr/bin/env python3
"""frond-68k peripheral and workflow checks on private IIIx and CLIE T415 ROMs: piezo tones, backlight, sleep
and wake, beaming a memo both ways with a IIIx, serial HotSync with pilot-link over a PTY, TCP and a Unix socket, the unix: endpoints, the ARM card swap flags refused, and installing, launching, listing
and exporting databases. Each ROM boots to the launcher once; every check starts from that
session. --only ppp also dials out over serial PPP to slirp and fetches example.com, --only
modem does the same through the snap-on modem (--serial modem) and --only irphone through the
infrared phone (--irda phone). --only modemsync HotSyncs over the modem both ways: Modem Sync to the
HotSync number through --modem-sync pty, then Network HotSync over PPP to pilot-xfer on net:.

ROMs come from --rom, else the IIIx 3.1 ROM in the private firmware folder; with none the checks
are skipped. A ROM whose name has T400 runs as the CLIE T415, which has no serial port (USB cradle), so its
serial checks are skipped and the others run at 320x320 through palm_boot.HalfCore. The probe PRCs are built with tools/build-palm-probe.py when m68k-palmos-gcc and the
Palm OS 3.5 SDK are there (--sdk), else the checks that need them are skipped."""
import argparse
import os
import re
import signal
import socket
import stat
import array
import shutil
import subprocess
import sys
import tempfile
import time
import wave
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

CHECKS = ['tones', 'backlight', 'contrast', 'sleep', 'beam', 'serial', 'usb', 'unix', 'card-flags', 'databases', 'ppp', 'modem', 'irphone',
          'modemsync']
# Reach example.com over the internet, so they only run when asked for with --only.
OPT_IN = {'ppp', 'modem', 'irphone', 'modemsync'}
T415_ROM = 'Sony/CLIE-T400/Sony-Clie-T400.rom'
# The device under test; main() sets it from each ROM's name.
DEVICE = 'palm-iiix'
HELLO = Path.home() / 'Projects/palm/hello/Hello.prc'
FRONDBENCH = ROOT / 'build/frondbench/FrondBench.prc'
PROBES = {'tone68k': 'FrondTone68k', 'alarm68k': 'FrondAlarm68k', 'ppp68k': 'FrondPPP68k'}


class Skip(Exception):
    pass


def tones(path, gap_ms=20):
    """(start s, length s, frequency Hz) of each burst of rising edges in a mono WAV."""
    with wave.open(str(path)) as w:
        rate, data = w.getframerate(), array.array('h', w.readframes(w.getnframes()))
    bursts, edges = [], []
    for i in range(1, len(data)):
        if data[i] - data[i - 1] > 2000:
            if edges and i - edges[-1] > rate * gap_ms / 1000:
                bursts.append(edges)
                edges = []
            edges.append(i)
    if edges:
        bursts.append(edges)
    return [(e[0] / rate, (e[-1] - e[0]) / rate, (len(e) - 1) / ((e[-1] - e[0]) / rate))
            for e in bursts if len(e) > 2]


class Machine:
    """One frond-68k process on a session, with helpers for the checks."""

    def __init__(self, image, root, name, *args, device=None):
        self.root = root / name
        self.root.mkdir(parents=True, exist_ok=True)
        self.device = device or DEVICE
        self.core = palm_boot.Core([image, '--device', self.device], self.root / 'log.txt', *args)
        self.launcher = palm_boot.launcher_check(self.device)
        # 320x320 devices run the IIIx's coordinates and checks at half size.
        if palm_boot.rom_device(self.device)['setup'] == 'iiix-hires':
            self.core = palm_boot.HalfCore(self.core)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.core.close()

    def send(self, command, expect, timeout=30):
        return self.core.send(command, expect, timeout).strip()

    def frame(self, name):
        return self.core.frame(self.root / name)

    def install(self, prc):
        reply = self.send(f'install "{prc}"', 'install')
        if 'successful' not in reply:
            raise AssertionError(f'{prc.name}: {reply}')

    def wait_line(self, text, timeout, mark=0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for line in self.core.lines[mark:]:
                if text in line:
                    return line
            time.sleep(0.1)
        raise AssertionError(f'no "{text}" in {timeout} s')


def check_tones(rom, session, root, probes):
    """The tone probe's 440, 1000 and 2500 Hz, 300 ms each, and the Info sound, in the WAV and
    the tone log."""
    wav = root / 'tones' / 'tones.wav'
    with Machine(session, root, 'tones', '--audio-output', wav, '--tone-log') as m:
        m.install(probes['tone68k'])
        m.core.launch('Frond tones')
        m.wait_line('tone: 500', 30)
        m.wait_line('tone: -1.0', 5, len(m.core.lines) - 1)
        state = m.send('sound-state', 'sound:')
    if 'off, no device, tones' not in state:
        raise AssertionError(state)
    found = tones(wav)
    expected = [(440, .3), (1000, .3), (2500, .3)]
    if len(found) < 4:
        raise AssertionError(f'tones {found}')
    # The guest ends a tone on its own timer, which on OS 3.1 steps about 16 ms, so a tone runs 300 to 336 ms
    # depending on where the start falls in a step.
    for (frequency, length), (_, got_length, got) in zip(expected, found[-4:-1]):
        if abs(got - frequency) > frequency * 0.02 or abs(got_length - length) > 0.04:
            raise AssertionError(f'{frequency} Hz for {length} s, got {got:.1f} Hz for {got_length:.3f} s')
    # The Info sound: 500 Hz, short.
    if abs(found[-1][2] - 500) > 10 or found[-1][1] > 0.2:
        raise AssertionError(f'info sound {found[-1]}')
    gaps = [found[i + 1][0] - found[i][0] - found[i][1] for i in range(len(found) - 4, len(found) - 2)]
    if any(abs(g - 0.3) > 0.05 for g in gaps):
        raise AssertionError(f'gaps {gaps}')
    return 'tones ' + ', '.join(f'{f:.1f} Hz {l * 1000:.0f} ms' for _, l, f in found[-4:])


def panel(rows):
    """The LCD's top left pixel, the panel colour on the launcher."""
    return tuple(rows[100][0:3])


def check_backlight(rom, session, root, probes):
    """Holding power for 2.5 s turns the backlight on and the panel green; again turns it off.
    The lit state survives a save and reload."""
    with Machine(session, root, 'backlight') as m:
        r = m.root
        if 'off' not in m.send('backlight-state', 'backlight:'):
            raise AssertionError('backlight on at start')
        unlit = panel(m.frame('unlit.png'))
        m.core.key(r, 'power', 2500)
        time.sleep(1)
        if 'on' not in m.send('backlight-state', 'backlight:'):
            raise AssertionError('backlight not on after holding power')
        lit = panel(m.frame('lit.png'))
        # The IIIx's backlight is green; the T415's is a lighter blue-green, so it only has to change the panel.
        if m.device == 'palm-iiix':
            if not (lit[1] > lit[0] + 40 and lit[1] > lit[2] + 40) or lit == unlit:
                raise AssertionError(f'lit panel {lit} not green (unlit {unlit})')
        elif not (lit[1] > lit[0] + 30 and lit[2] > lit[0] + 30):
            raise AssertionError(f'lit panel {lit} not blue-green (unlit {unlit})')
        m.send(f'save-session "{r / "lit.frond"}"', 'wrote session')
        m.core.key(r, 'power', 2500)
        time.sleep(1)
        if 'off' not in m.send('backlight-state', 'backlight:') or panel(m.frame('off.png')) != unlit:
            raise AssertionError('backlight not off after holding power again')
    with Machine(r / 'lit.frond', root, 'backlight-reload') as m:
        time.sleep(1)
        if 'on' not in m.send('backlight-state', 'backlight:') or panel(m.frame('reloaded.png')) != lit:
            raise AssertionError('backlight not on after reload')
    return f'panel {unlit} lit {lit}'


def check_contrast(rom, session, root, probes):
    """The Contrast setting (display contrast=) changes the panel, is kept in a saved session and
    comes back after a reload."""
    with Machine(session, root, 'contrast') as m:
        r = m.root
        frames = {}
        for value in (0, 50, 100):
            if f'contrast {value}' not in m.send(f'display contrast={value}', 'display:'):
                raise AssertionError(f'contrast {value} not taken')
            time.sleep(1)
            frames[value] = m.frame(f'contrast-{value}.png')
        if len({tuple(map(bytes, f)) for f in frames.values()}) != 3:
            raise AssertionError('contrast 0, 50 and 100 do not all look different')
        m.send(f'save-session "{r / "contrast.frond"}"', 'wrote session')
    with Machine(r / 'contrast.frond', root, 'contrast-reload') as m:
        time.sleep(1)
        if m.frame('reloaded.png') != frames[100]:
            raise AssertionError('contrast 100 not back after reload')
    return 'contrast 0, 50 and 100 differ, 100 kept through save and reload'


def check_sleep(rom, session, root, probes):
    """Power sleeps and wakes; the alarm probe sets auto-off to 5 s and an alarm 20 s ahead. The
    machine turns itself off, is saved asleep and quit; a fresh process loads it asleep and the
    alarm wakes it at its time, with the probe's tones and its alarm text on the LCD. A machine
    saved asleep also wakes from the power button after a reload."""
    with Machine(session, root, 'sleep-power') as m:
        r = m.root
        time.sleep(1)
        m.core.key(r, 'power', 150)
        time.sleep(2)
        if m.frame('asleep.png') is not None:
            raise AssertionError('LCD on after power')
        m.send(f'save-session "{r / "asleep.frond"}"', 'wrote session')
        m.core.key(r, 'power', 150)
        time.sleep(2)
        rows = m.frame('awake.png')
        if rows is None or not m.launcher(rows):
            raise AssertionError('power did not wake to the launcher')
    with Machine(r / 'asleep.frond', root, 'sleep-power-reload') as m:
        time.sleep(1)
        if m.frame('asleep.png') is not None:
            raise AssertionError('reloaded asleep session has the LCD on')
        m.core.key(m.root, 'power', 150)
        time.sleep(2)
        rows = m.frame('awake.png')
        if rows is None or not m.launcher(rows):
            raise AssertionError('power did not wake the reloaded session')

    with Machine(session, root, 'sleep-alarm', '--tone-log') as m:
        r = m.root
        m.install(probes['alarm68k'])
        mark = len(m.core.lines)
        m.core.launch('Frond alarm')
        launched = time.monotonic() - 3
        time.sleep(9)
        if m.frame('auto-off.png') is not None:
            raise AssertionError('auto-off did not turn the LCD off')
        m.send(f'save-session "{r / "auto-off.frond"}"', 'wrote session')
        if any('tone: 1501' in line for line in m.core.lines[mark:]):
            raise AssertionError('alarm went off early')
    with Machine(r / 'auto-off.frond', root, 'sleep-alarm-reload', '--tone-log') as m:
        time.sleep(1)
        if m.frame('asleep.png') is not None:
            raise AssertionError('reloaded auto-off session has the LCD on')
        m.wait_line('tone: 1501', max(5, launched + 30 - time.monotonic()))
        woke = time.monotonic() - launched
        m.wait_line('tone: 1799', 5)
        rows = m.frame('alarm.png')
        if rows is None:
            raise AssertionError('alarm did not turn the LCD on')
    return f'alarm woke the reloaded machine {woke:.1f} s after launch'


DIGITS = {'1': (61, 80), '2': (80, 80), '3': (100, 80), '4': (61, 96), '5': (80, 96), '6': (100, 96),
          '7': (61, 112), '8': (80, 112), '9': (100, 112), '0': (80, 128)}
# Beam Memo in Memo Pad's Record menu, in IIIx (160x160) coordinates: IIIx, T415.
BEAM_MEMO = {'palm-iiix': (30, 42), 'sony-clie-t415': (30, 47)}
# The receiving side's Yes button.
ACCEPT = (28, 146)
# The IIIx session a T415 beams with, set by main(); the IIIx uses its own.
PEER = {}


def make_memo(m, digits):
    """A new memo with digits typed on the 123 keyboard, then Done in the keyboard."""
    m.core.launch('Memo Pad')
    for x, y in [(20, 150), (128 if m.device == 'palm-iiix' else 131, 213 if m.device == 'palm-iiix' else 218)] + \
            [DIGITS[d] for d in digits] + [(22, 146)]:
        m.core.tap(m.root, x, y)
        time.sleep(0.7)


def memo_text(m, name):
    """The MemoDB as exported from the running machine."""
    path = m.root / name
    path.unlink(missing_ok=True)
    try:
        reply = m.core.send(f'db-export-db MemoDB "{path.absolute()}"', 'export', 20).strip()
    except AssertionError:
        # A dialog waiting for a tap holds the host call back.
        return b''
    return path.read_bytes() if 'written' in reply and path.is_file() else b''


def beam_memo(root, name, sender, send_session, receiver, recv_session, digits):
    """One memo from sender to receiver: the receiver accepts it, MemoDB has it, and a fresh process
    loaded from the receiver's saved session still has it."""
    with Machine(send_session, root, f'{name}-send', '--irda', 'tcp:0', device=sender) as a, \
            Machine(recv_session, root, f'{name}-recv', '--irda', 'tcp:0', device=receiver) as b:
        # Each core waits for a free emulator slot first.
        for m in (a, b):
            m.wait_line('from session', 900)
        ports = [re.search(r':(\d+)', m.core.until('irda-state', 'infrared:', 'waiting', 90)).group(1) for m in (a, b)]
        link = subprocess.Popen([sys.executable, str(ROOT / 'tools/link-irda.py'), *ports],
                                stdout=(a.root / 'link.txt').open('w'), stderr=subprocess.STDOUT)
        try:
            make_memo(a, digits)
            a.core.tap(a.root, 8, 202)
            time.sleep(1.5)
            a.core.tap(a.root, *BEAM_MEMO[sender])
            # The beam takes about 5 s. Export waits for Palm OS to be free of dialogs, so Yes is
            # tapped first, twice in case the dialog came late.
            time.sleep(7)
            b.frame('dialog.png')
            for _ in range(2):
                b.core.tap(b.root, *ACCEPT)
                time.sleep(4)
            if digits.encode() not in memo_text(b, 'received.pdb'):
                a.frame('sender.png')
                b.frame('receiver.png')
                raise AssertionError(f'{sender} to {receiver}: memo {digits} never arrived')
            b.frame('received.png')
            b.send(f'save-session "{b.root / "received.frond"}"', 'wrote session')
        finally:
            link.kill()
            link.wait()
    with Machine(b.root / 'received.frond', root, f'{name}-reload', device=receiver) as r:
        time.sleep(1)
        if digits.encode() not in memo_text(r, 'reloaded.pdb'):
            raise AssertionError(f'{sender} to {receiver}: memo {digits} gone after reload')


def check_beam(rom, session, root, probes):
    """Beam a memo from the device under test to a IIIx and back through tools/link-irda.py (the
    guests' own IrLAP and OBEX), the receiver accepting and saving. Each direction is checked in the receiver's MemoDB and again
    after a save and reload."""
    # Two copies of one session share an IrLAP address, so a IIIx has nothing to beam with here.
    peer = PEER.get('palm-iiix') if DEVICE != 'palm-iiix' else None
    if peer is None:
        raise Skip('beams a T415 with a IIIx')
    beam_memo(root, 'beam-out', DEVICE, session, 'palm-iiix', peer, '3551')
    beam_memo(root, 'beam-in', 'palm-iiix', peer, DEVICE, session, '7424')
    return f'memo beamed {DEVICE} to a IIIx and back, each kept after save and reload'


def hotsync(m, port, *args, rate='57600'):
    """pilot-xfer on port, started before the cradle button is pressed; returns its output."""
    process = subprocess.Popen(['pilot-xfer', '-p', port, *map(str, args)], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, cwd=m.root,
                               env={**os.environ, 'PILOTRATE': rate})
    time.sleep(1)
    m.send('hotsync', 'hotsync:')
    try:
        out, _ = process.communicate(timeout=120)
    except subprocess.TimeoutExpired:
        process.kill()
        raise AssertionError(f'pilot-xfer {args} timed out')
    if process.returncode != 0:
        raise AssertionError(f'pilot-xfer {args}: {out.strip()[-300:]}')
    # The HotSync app needs a moment to finish before the next press.
    time.sleep(3)
    return out


class Endpoint:
    """The serial port as pilot-link sees it: the PTY itself, or a PTY from socat for TCP or a Unix socket."""

    def __init__(self, m, mode):
        self.socat = None
        state = m.send('serial-state', 'serial:')
        address = state.split()[2]
        if mode == 'pty':
            self.port = address
            return
        self.port = str(m.root / f'{mode}-tty')
        target = f'unix-connect:{address}' if mode == 'unix' else f'tcp:{address}'
        self.socat = subprocess.Popen(['socat', f'pty,raw,echo=0,link={self.port}', target],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        end = time.monotonic() + 10
        while not Path(self.port).exists() and time.monotonic() < end:
            time.sleep(0.1)

    def close(self):
        if self.socat:
            self.socat.kill()
            self.socat.wait()


def check_serial(rom, session, root, probes):
    """pilot-link over the cradle serial port: install Hello.prc, fetch MemoDB and back up over a
    PTY at 57600, then list over TCP at 115200 in a fresh process loaded from the synced session."""
    if not palm_boot.rom_device(DEVICE)['serial']:
        raise Skip(f'{DEVICE} has no serial port (USB cradle)')
    if not shutil.which('pilot-xfer') or not shutil.which('socat'):
        raise Skip('no pilot-xfer or socat')
    if not HELLO.is_file():
        raise Skip(f'no {HELLO}')
    with Machine(session, root, 'serial-pty', '--serial', 'pty') as m:
        link = Endpoint(m, 'pty')
        hotsync(m, link.port, '-i', HELLO)
        hotsync(m, link.port, '-f', 'MemoDB')
        if not (m.root / 'MemoDB.pdb').is_file():
            raise AssertionError('MemoDB not fetched')
        out = hotsync(m, link.port, '-b', m.root / 'backup')
        if not (m.root / 'backup/Hello.prc').is_file() or '0 files failed' not in out:
            raise AssertionError('backup without Hello.prc: ' + out.strip()[-200:])
        backed = len(list((m.root / 'backup').iterdir()))
        state = m.send('serial-state', 'serial:')
        m.send(f'save-session "{m.root / "synced.frond"}"', 'wrote session')
    with Machine(m.root / 'synced.frond', root, 'serial-tcp', '--serial', 'tcp:0') as t:
        link = Endpoint(t, 'tcp')
        try:
            out = hotsync(t, link.port, '-l', rate='115200')
        finally:
            link.close()
        if 'Hello' not in out:
            raise AssertionError('Hello not listed after reload')
        tcp = t.send('serial-state', 'serial:')
    folder = Path(tempfile.mkdtemp(prefix='fu-'))
    try:
        with Machine(m.root / 'synced.frond', root, 'serial-unix', '--serial', f'unix:{folder}/serial.sock') as u:
            link = Endpoint(u, 'unix')
            try:
                out = hotsync(u, link.port, '-l', rate='115200')
            finally:
                link.close()
            if 'Hello' not in out:
                raise AssertionError('Hello not listed over a Unix socket')
            unix = u.send('serial-state', 'serial:')
    finally:
        shutil.rmtree(folder, ignore_errors=True)
    return (f'{backed} databases backed up; {state.split(", ", 1)[1]}; tcp {tcp.split(", ", 1)[1]}; '
            f'unix {unix.split(", ", 1)[1]}')


def check_usb(rom, session, root, probes):
    """pilot-link over the USB cradle (PDIUSBD12) and a host PTY: install Hello.prc, fetch MemoDB
    and back up, each started by the cradle's HotSync button, then list in a fresh process loaded
    from the synced session (the cable comes out and goes back in on load)."""
    if not palm_boot.rom_device(DEVICE).get('usb'):
        raise Skip(f'{DEVICE} has no USB cradle')
    if not shutil.which('pilot-xfer'):
        raise Skip('no pilot-xfer')
    if not HELLO.is_file():
        raise Skip(f'no {HELLO}')
    with Machine(session, root, 'usb-pty', '--usb', 'pty') as m:
        port = m.send('usb-state', 'usb:').split()[2]
        hotsync(m, port, '-i', HELLO)
        hotsync(m, port, '-f', 'MemoDB')
        if not (m.root / 'MemoDB.pdb').is_file():
            raise AssertionError('MemoDB not fetched')
        out = hotsync(m, port, '-b', m.root / 'backup')
        if not (m.root / 'backup/Hello.prc').is_file() or '0 files failed' not in out:
            raise AssertionError('backup without Hello.prc: ' + out.strip()[-200:])
        backed = len(list((m.root / 'backup').iterdir()))
        enumerated = m.wait_line('usb: enumerated', 5).strip()
        m.send(f'save-session "{m.root / "synced.frond"}"', 'wrote session')
    with Machine(m.root / 'synced.frond', root, 'usb-reload', '--usb', 'pty') as r:
        port = r.send('usb-state', 'usb:').split()[2]
        out = hotsync(r, port, '-l')
        if 'Hello' not in out:
            raise AssertionError('Hello not listed after reload')
        r.send(f'load-session "{m.root / "synced.frond"}"', 'loaded')
        # The load takes the cable out and plugs it back in; the chip is only on the bus while
        # HotSync runs, so the next sync enumerates it again.
        mark = len(r.core.lines)
        out = hotsync(r, port, '-l')
        if 'Hello' not in out:
            raise AssertionError('Hello not listed after load-session')
        r.wait_line('usb: enumerated', 5, mark)
    return f'{backed} databases backed up; {enumerated}; list after reload and after load-session'


def unix_client(path):
    client = socket.socket(socket.AF_UNIX)
    client.settimeout(10)
    client.connect(path)
    return client


def check_unix(rom, session, root, probes):
    """The unix: endpoint for --serial and --irda and the live mode commands: waiting, connected,
    bytes from the host counted, one client, reconnect, off removes the socket file, and the
    refusals (folder mode, a file in the way, a path too long or relative)."""
    folder = Path(tempfile.mkdtemp(prefix='fu-'))
    try:
        serial_path, ir_path = f'{folder}/serial.sock', f'{folder}/ir.sock'
        serial = palm_boot.rom_device(DEVICE)['serial']
        ports = [('irda', ir_path, 'irda-state', 'infrared:', 'irda-mode')]
        if serial:
            ports.insert(0, ('serial', serial_path, 'serial-state', 'serial:', 'serial-mode'))
        with Machine(session, root, 'unix', *(['--serial', f'unix:{serial_path}'] if serial else []),
                     '--irda', f'unix:{ir_path}') as m:
            for name, path, state_command, prefix, mode_command in ports:
                def state():
                    return m.send(state_command, prefix)

                def wait(text):
                    end = time.monotonic() + 10
                    while time.monotonic() < end:
                        if text in state():
                            return
                        time.sleep(0.1)
                    raise AssertionError(f'{name}: no "{text}" in {state()}')

                if f'unix {path} waiting' not in state():
                    raise AssertionError(f'{name}: {state()}')
                if not stat.S_ISSOCK(os.lstat(path).st_mode):
                    raise AssertionError(f'{name}: no socket at {path}')
                client = unix_client(path)
                wait(f'unix {path} connected')
                client.sendall(b'hello')
                wait('rx 5')
                second = unix_client(path)
                if second.recv(1) != b'':
                    raise AssertionError(f'{name}: a second client was kept')
                second.close()
                if 'connected' not in state():
                    raise AssertionError(f'{name}: the first client lost the line')
                client.close()
                wait(f'unix {path} waiting')
                client = unix_client(path)
                wait(f'unix {path} connected')
                client.sendall(b'again')
                wait('rx 10')
                client.close()
                # Live changes: off removes the file, tcp and back to unix work, a new path moves it.
                m.send(f'{mode_command} off', prefix)
                if os.path.lexists(path) or state().split()[1:2] != ['off']:
                    raise AssertionError(f'{name}: off left {path} or state {state()}')
                if 'tcp 127.0.0.1:' not in m.send(f'{mode_command} tcp:0', prefix):
                    raise AssertionError(f'{name}: tcp:0 after off')
                moved = f'{folder}/{name}-moved.sock'
                if f'unix {moved} waiting' not in m.send(f'{mode_command} unix:{moved}', prefix):
                    raise AssertionError(f'{name}: unix: after tcp')
                if not stat.S_ISSOCK(os.lstat(moved).st_mode):
                    raise AssertionError(f'{name}: no socket at {moved}')
                # Refusals leave the link off and the path as it was.
                os.chmod(folder, 0o755)
                reply = m.send(f'{mode_command} unix:{path}', prefix)
                if 'error' not in reply or '0700' not in reply or os.path.lexists(path) or os.path.lexists(moved):
                    raise AssertionError(f'{name}: bad folder mode: {reply.strip()}')
                os.chmod(folder, 0o700)
                Path(path).write_text('keep')
                reply = m.send(f'{mode_command} unix:{path}', prefix)
                if 'error' not in reply or 'not a socket' not in reply or Path(path).read_text() != 'keep':
                    raise AssertionError(f'{name}: file in the way: {reply.strip()}')
                Path(path).unlink()
                reply = m.send(f'{mode_command} unix:{folder}/{"a" * 120}', prefix)
                if 'error' not in reply or 'too long' not in reply:
                    raise AssertionError(f'{name}: long path: {reply.strip()}')
                if 'error' not in m.send(f'{mode_command} unix:relative.sock', prefix):
                    raise AssertionError(f'{name}: relative path')
                if 'error' not in m.send(f'{mode_command} unix:{folder}/none/x.sock', prefix):
                    raise AssertionError(f'{name}: missing folder')
                if 'error' not in m.send(f'{mode_command} unix:', prefix):
                    raise AssertionError(f'{name}: empty path')
                if f'unix {path} waiting' not in m.send(f'{mode_command} unix:{path}', prefix):
                    raise AssertionError(f'{name}: a good path after the refusals')
            m.core.send('quit')
            m.core.process.wait(timeout=30)
            left = sorted(p.name for p in folder.iterdir())
            if left:
                raise AssertionError(f'files left after quit: {left}')
        return ('serial and infrared' if serial else 'infrared') + ': connect, one client, reconnect, live modes, refusals, files removed on quit'
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def check_card_flags(rom, session, root, probes):
    """The ARM core's card swap flags are refused with one line, as a IIIx has no card slot."""
    for flags in (['--card-swap'], ['--card-identity', '0' * 64], ['--card-protected']):
        result = subprocess.run([str(palm_boot.CORE_68K), str(session), *flags, '--run-for', '1'],
                                capture_output=True, text=True, timeout=60)
        lines = result.stderr.splitlines()
        if result.returncode != 2 or result.stdout or len(lines) != 1 or \
                f'{flags[0]} is not supported' not in lines[0]:
            raise AssertionError(f'{flags[0]}: exit {result.returncode}, {result.stdout!r} {result.stderr!r}')
    return '--card-swap, --card-identity and --card-protected refused'


def check_ppp(rom, session, root, probes):
    """PPP over the serial PTY to slirp (tools/palm-pppd.sh): set the Network panel's service to
    Direct Serial, then the PPP probe dials with the guest's own NetLib, looks up example.com and
    fetches it. slirp's counts show the DNS datagram and the TCP connection. OS 3.5 and later."""
    if not palm_boot.rom_device(DEVICE)['serial']:
        raise Skip(f'{DEVICE} has no serial port')
    if not shutil.which('slirp') or not shutil.which('cc'):
        raise Skip('no slirp or cc')
    if '3.1' in rom.name:
        raise Skip('the Network panel steps are for OS 3.5 and later')
    log = root / 'ppp' / 'slirp'
    shutil.rmtree(log, ignore_errors=True)
    log.mkdir(parents=True)
    with Machine(session, root, 'ppp', '--serial', 'pty') as m:
        pty = m.send('serial-state', 'serial:').split()[2]
        helper = subprocess.Popen(['sh', str(ROOT / 'tools/palm-pppd.sh'), '--log', str(log), pty],
                                  stdout=(log / 'helper.txt').open('w'), stderr=subprocess.STDOUT,
                                  start_new_session=True)
        try:
            m.install(probes['ppp68k'])
            m.core.launch('Preferences')
            # Panel picker, Network, the Connection trigger, then Direct Serial on 3.5 and
            # Cradle/Cable on 4.1, whose panel list has Date & Time and whose default service
            # has a two-line user name.
            taps = ((135, 5), (125, 73), (100, 92), (110, 84)) if '4.1' in rom.name else \
                ((135, 5), (125, 62), (100, 87), (110, 121))
            for x, y in taps:
                m.core.tap(m.root, x, y)
                time.sleep(0.7)
            m.frame('network.png')
            m.core.launch('Frond PPP')
            end = time.monotonic() + 90
            while time.monotonic() < end and 'tx 0' in m.send('serial-state', 'serial:'):
                time.sleep(1)
            time.sleep(30)
            m.frame('probe.png')
            state = m.send('serial-state', 'serial:')
        finally:
            os.killpg(helper.pid, signal.SIGTERM)
            helper.wait()
    text = (log / 'slirp_debug').read_text(errors='replace')
    count = lambda pattern: int(found.group(1)) if (found := re.search(pattern, text)) else 0
    dns, connections = count(r'(\d+) datagrams received'), count(r'(\d+) connections established')
    received = count(r'(\d+) data packets \((\d+) bytes\)')
    if not dns or not connections or not received:
        raise AssertionError(f'slirp saw {dns} datagrams, {connections} connections, {received} data packets')
    return f'{dns} DNS datagram, {connections} TCP connection, {received} data packets back; {state.split(", ", 1)[1]}'


def check_modem(rom, session, root, probes):
    """The snap-on modem (--serial modem): pick Palm Modem US/Canada in the Network panel, type a
    phone number on the numeric keyboard, then the PPP probe dials with the guest's own NetLib
    and Modem Manager, through the core's modem and PPP server to slirp, and fetches example.com.
    Passes on the probe's FrondPPPLog showing HTTP 200 and the modem having been online. OS 3.5."""
    if '3.5' not in rom.name or not palm_boot.rom_device(DEVICE)['modem']:
        raise Skip('the Network panel taps are for the IIIx on OS 3.5')
    with Machine(session, root, 'modem', '--serial', 'modem') as m:
        if m.send('serial-state', 'serial:') != 'serial: modem idle':
            raise AssertionError('modem not idle at the start')
        m.install(probes['ppp68k'])
        m.core.launch('Preferences')
        # Panel picker, Network, the Connection trigger, Palm Modem US/Canada, the Phone field,
        # the silkscreen's numeric keyboard, 5551234, Done, then OK in Phone Setup.
        digits = {'1': (62, 75), '2': (80, 75), '3': (98, 75), '4': (62, 91), '5': (80, 91)}
        taps = [(135, 5), (125, 62), (100, 87), (110, 91), (120, 103), (128, 213)]
        taps += [digits[d] for d in '5551234'] + [(16, 152), (12, 152)]
        for x, y in taps:
            m.core.tap(m.root, x, y)
            time.sleep(0.7)
        m.frame('network.png')
        m.core.launch('Frond PPP')
        # The probe writes FrondPPPLog when it's done; it leaves the call up.
        states, reply = [], ''
        log = m.root / 'log.pdb'
        end = time.monotonic() + 150
        while time.monotonic() < end and 'written' not in reply:
            state = m.send('serial-state', 'serial:').split(': ', 1)[1]
            if not states or states[-1] != state:
                states.append(state)
            if 'online' in state:
                reply = m.send(f'db-export-db FrondPPPLog "{log}"', 'export')
            time.sleep(1)
        m.frame('probe.png')
    if 'written' not in reply:
        raise AssertionError(f'no FrondPPPLog: {reply}; modem went {" > ".join(states)}')
    data = log.read_bytes()
    record = data[int.from_bytes(data[78:82], 'big'):]
    lines = record.split(b'\0')[0].decode(errors='replace').splitlines()
    status = next((line for line in lines if line.startswith('HTTP/')), '')
    if ' 200' not in status or 'modem online 33600' not in states:
        raise AssertionError(f'probe: {" | ".join(lines)}; modem went {" > ".join(states)}')
    received = next((line.split()[-1] for line in lines if line.startswith('received')), '?')
    return f'{status}, {received} bytes; modem went {" > ".join(states)}'


def check_irphone(rom, session, root, probes):
    """The infrared phone (--irda phone): a new "IrCOMM to Modem" connection in the Connection panel,
    picked in the Network panel with a phone number, then the PPP probe dials with the guest's own
    NetLib, Modem Manager and IrCOMM, through the core's IrLAP peer, modem and PPP server to slirp,
    and fetches example.com. Passes on the probe's FrondPPPLog showing HTTP 200 and the phone having
    been online. OS 3.5."""
    if '3.5' not in rom.name:
        raise Skip('the Connection and Network panel taps are for OS 3.5')
    with Machine(session, root, 'irphone', '--irda', 'phone') as m:
        if m.send('irda-state', 'infrared:') != 'infrared: phone idle':
            raise AssertionError('phone not idle at the start')
        m.install(probes['ppp68k'])
        m.core.launch('Preferences')
        # Panel picker, Connection, New, the Connection Method trigger, IrCOMM to Modem, OK (the new
        # connection is called Custom); panel picker, Network, the Connection trigger, Custom, the
        # Phone field, the silkscreen's numeric keyboard, 5551234, Done, then OK in Phone Setup.
        digits = {'1': (62, 75), '2': (80, 75), '3': (98, 75), '4': (62, 91), '5': (80, 91)}
        taps = [(135, 5), (125, 18), (16, 151), (110, 53), (100, 87), (20, 147),
                (135, 5), (125, 62), (100, 89), (110, 141), (120, 103), (128, 213)]
        taps += [digits[d] for d in '5551234'] + [(16, 152), (12, 152)]
        for x, y in taps:
            m.core.tap(m.root, x, y)
            time.sleep(0.7)
        m.frame('network.png')
        m.core.launch('Frond PPP')
        # The probe writes FrondPPPLog when it's done; it leaves the call up.
        states, reply = [], ''
        log = m.root / 'log.pdb'
        end = time.monotonic() + 150
        while time.monotonic() < end and 'written' not in reply:
            state = m.send('irda-state', 'infrared:').split(': ', 1)[1]
            if not states or states[-1] != state:
                states.append(state)
            if 'online' in state:
                reply = m.send(f'db-export-db FrondPPPLog "{log}"', 'export')
            time.sleep(1)
        m.frame('probe.png')
    if 'written' not in reply:
        raise AssertionError(f'no FrondPPPLog: {reply}; phone went {" > ".join(states)}')
    data = log.read_bytes()
    record = data[int.from_bytes(data[78:82], 'big'):]
    lines = record.split(b'\0')[0].decode(errors='replace').splitlines()
    status = next((line for line in lines if line.startswith('HTTP/')), '')
    if ' 200' not in status or 'phone online 33600' not in states:
        raise AssertionError(f'probe: {" | ".join(lines)}; phone went {" > ".join(states)}')
    received = next((line.split()[-1] for line in lines if line.startswith('received')), '?')
    return f'{status}, {received} bytes; phone went {" > ".join(states)}'


def modem_states(m, process, limit):
    """serial-state's modem part each second until process ends and the modem is idle again."""
    states = []
    end = time.monotonic() + limit
    while time.monotonic() < end:
        state = m.send('serial-state', 'serial:').split(': ', 1)[1].split(';')[0]
        if not states or states[-1] != state:
            states.append(state)
        if process.poll() is not None and state == 'modem idle':
            break
        time.sleep(0.5)
    return states


def check_modemsync(rom, session, root, probes):
    """HotSync over the snap-on modem. Modem Sync: the HotSync app dials 4687962, the call is
    bridged to the --modem-sync PTY and pilot-install-netsync turns on Network HotSync with 10.0.2.2
    as the Primary PC. Network HotSync: with Modem Sync Prefs on Network the HotSync app dials the
    Network panel's service (Palm Modem US/Canada, 5551234), PPP comes up, the core answers the
    NetSync wakeup and pilot-xfer -l on net:127.0.0.1 answers through slirp. OS 3.5."""
    if '3.5' not in rom.name:
        raise Skip('the HotSync and Network panel taps are for OS 3.5')
    if not shutil.which('pilot-xfer') or not shutil.which('pilot-install-netsync'):
        raise Skip('no pilot-xfer or pilot-install-netsync')
    digits = {'1': (62, 75), '2': (80, 75), '3': (98, 75), '4': (62, 91), '5': (80, 91), '6': (98, 91),
              '7': (62, 107), '8': (80, 107), '9': (98, 107)}

    def taps(m, points):
        for x, y in points:
            m.core.tap(m.root, x, y)
            time.sleep(0.7)

    with Machine(session, root, 'modemsync', '--serial', 'modem', '--modem-sync', 'pty') as m:
        # The startup lines print serial before modem-sync, so ask until the PTY shows.
        state = m.core.until('serial-state', 'serial:', 'hotsync', 30).strip()
        if not state.startswith('serial: modem idle; hotsync pty /dev/pts/'):
            raise AssertionError(f'no HotSync PTY: {state}')
        port = state.split('hotsync ')[1].split()[1]
        # Network panel: Palm Modem US/Canada and 5551234, as in check_modem.
        m.core.launch('Preferences')
        taps(m, [(135, 5), (125, 62), (100, 87), (110, 91), (120, 103), (128, 213)] +
             [digits[d] for d in '5551234'] + [(16, 152), (12, 152)])
        # HotSync app: Modem, Enter phone #, the numeric keyboard, 4687962, Done, OK.
        m.core.launch('HotSync')
        time.sleep(1)
        taps(m, [(98, 50), (80, 131), (128, 213)] + [digits[d] for d in '4687962'] + [(16, 152), (12, 152)])
        netsync = subprocess.Popen(['pilot-install-netsync', '-p', port, '-e', '-n', 'frond', '-a', '10.0.2.2'],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=m.root)
        time.sleep(1)
        taps(m, [(80, 80)])
        direct = modem_states(m, netsync, 120)
        if netsync.poll() is None:
            netsync.kill()
        out = netsync.communicate()[0]
        m.frame('modem-sync.png')
        if 'LANSync....: Enabled' not in out or '10.0.2.2' not in out or 'modem online 33600' not in direct:
            raise AssertionError(f'Modem Sync: {out.strip()[-300:]!r}; modem went {" > ".join(direct)}')
        # Let the HotSync app finish, then Menu, Modem Sync Prefs, Network, OK, Select Service (the
        # Network panel opens on the service set above), Done and the HotSync button.
        time.sleep(8)
        taps(m, [(20, 5), (40, 17), (40, 108), (12, 152), (80, 115), (14, 152)])
        m.frame('network-mode.png')
        net = subprocess.Popen(['pilot-xfer', '-p', 'net:127.0.0.1', '-l'], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, cwd=m.root)
        time.sleep(1)
        taps(m, [(80, 80)])
        network = modem_states(m, net, 180)
        if net.poll() is None:
            net.kill()
        out = net.communicate()[0]
        m.frame('network-sync.png')
        listed = re.search(r'List complete\. (\d+) files found', out)
        if not listed or 'modem online 33600' not in network:
            raise AssertionError(f'Network HotSync: {out.strip()[-300:]!r}; modem went {" > ".join(network)}')
    return (f'Modem Sync set LANSync, modem went {" > ".join(direct)}; Network HotSync listed '
            f'{listed.group(1)} databases, modem went {" > ".join(network)}')


def check_databases(rom, session, root, probes):
    """Install Hello.prc and FrondBench from the console, launch Hello, list the RAM databases,
    export Hello as a PRC and the RAM and all databases as zips, and FrondBench starting."""
    if not HELLO.is_file():
        raise Skip(f'no {HELLO}')
    with Machine(session, root, 'databases') as m:
        r = m.root
        m.install(HELLO)
        bench = FRONDBENCH.is_file()
        if bench:
            m.install(FRONDBENCH)
        m.core.launch('Hello')
        rows = m.frame('hello.png')
        if rows is None or m.launcher(rows):
            raise AssertionError('Hello did not start')
        mark = len(m.core.lines)
        m.send('db-list', 'db-list:')
        listed = [line for line in m.core.lines[mark:] if line.startswith('db: ')]
        if not any(line.split()[1:3] == ['appl', 'DhHl'] and line.rstrip().endswith(' Hello') for line in listed):
            raise AssertionError('db-list has no Hello')
        if bench and not any(line.rstrip().endswith(' FrondBench') for line in listed):
            raise AssertionError('db-list has no FrondBench')
        reply = m.send(f'db-export-db Hello "{r / "Hello.prc"}"', 'export')
        data = (r / 'Hello.prc').read_bytes() if (r / 'Hello.prc').is_file() else b''
        if 'written' not in reply or data[:5] != b'Hello' or data[60:68] != b'applDhHl':
            raise AssertionError(f'Hello export: {reply}')
        if 'export failed' not in m.send('db-export-db NoSuchDatabase "{}"'.format(r / 'none.pdb'), 'export'):
            raise AssertionError('export of a missing database did not fail')
        m.send(f'db-export "{r / "ram.zip"}"', 'export written')
        m.send(f'db-export-all "{r / "all.zip"}"', 'export written')
        ram, everything = (zipfile.ZipFile(r / name).namelist() for name in ('ram.zip', 'all.zip'))
        if 'Hello.prc' not in ram or len(everything) <= len(ram) or zipfile.ZipFile(r / 'ram.zip').read('Hello.prc') != data:
            raise AssertionError(f'zips: ram {len(ram)}, all {len(everything)}')
        started = ''
        if bench:
            m.core.launch('FrondBench')
            rows = m.frame('frondbench.png')
            if rows is None or m.launcher(rows):
                raise AssertionError('FrondBench did not start')
            started = ', FrondBench started'
    return f'{len(listed)} RAM databases, ram.zip {len(ram)}, all.zip {len(everything)}{started}'


def build_probes(root, sdk):
    out = {}
    for probe, name in PROBES.items():
        prc = ROOT / f'build/{probe}-probe/{name}.prc'
        if not prc.is_file() and shutil.which('m68k-palmos-gcc') and (sdk / 'include/PalmOS.h').is_file():
            subprocess.run([sys.executable, 'tools/build-palm-probe.py', '--probe', probe, '--sdk', str(sdk)],
                           cwd=ROOT, check=True, capture_output=True)
        if prc.is_file():
            out[probe] = prc
    return out


NEEDS = {'tones': ['tone68k'], 'sleep': ['alarm68k'], 'backlight': [], 'contrast': [], 'beam': [], 'serial': [], 'usb': [], 'unix': [], 'card-flags': [], 'databases': [],
         'ppp': ['ppp68k'], 'modem': ['ppp68k'], 'irphone': ['ppp68k'], 'modemsync': []}
RUN = {'tones': check_tones, 'backlight': check_backlight, 'contrast': check_contrast, 'sleep': check_sleep, 'beam': check_beam,
       'serial': check_serial, 'usb': check_usb,
       'unix': check_unix, 'card-flags': check_card_flags, 'databases': check_databases, 'ppp': check_ppp,
       'modem': check_modem, 'irphone': check_irphone, 'modemsync': check_modemsync}


def launcher_session(device, rom, root):
    """Boots rom through the device's setup recipe and saves the launcher as a session."""
    root.mkdir(parents=True, exist_ok=True)
    session = root / 'launcher.frond'
    core = palm_boot.Core(['--rom', rom, '--device', device], root / 'boot.txt')
    try:
        # The core may wait for a free emulator slot first.
        end = time.monotonic() + 900
        while time.monotonic() < end and not any(line.startswith('frond-68k:') for line in core.lines):
            time.sleep(1)
        palm_boot.setup_device(device, core, root)
        core.send(f'save-session "{session}"', 'wrote session')
    finally:
        core.close()
    return session


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path, action='append', default=[])
    parser.add_argument('--device', choices=['palm-iiix', 'sony-clie-t415'],
                        help='for ROMs without T400 in their name; default palm-iiix')
    parser.add_argument('--only', action='append', choices=CHECKS)
    parser.add_argument('--sdk', type=Path, default=ROOT.parent / 'palm-os-sdk/sdk-3.5')
    parser.add_argument('--evidence', type=Path, help='keep logs and frames here')
    args = parser.parse_args()
    global DEVICE
    images = palm_boot.palm_images()
    default = images / (T415_ROM if args.device == 'sony-clie-t415' else 'Palm/IIIx/Palm-IIIx-3.1-en.rom')
    roms = args.rom or [p for p in [default] if p.is_file()]
    if not roms:
        print('frond-68k peripherals: skipped, no ROM')
        return 0
    probes = build_probes(ROOT, args.sdk.expanduser())
    failed = 0
    with tempfile.TemporaryDirectory(prefix='frond-68k-peripherals-') as work:
        base = args.evidence or Path(work)
        for rom in roms:
            root = base / rom.stem
            root.mkdir(parents=True, exist_ok=True)
            DEVICE = 'sony-clie-t415' if 'T400' in rom.name else args.device or 'palm-iiix'
            session = launcher_session(DEVICE, rom, root)
            checks = args.only or [c for c in CHECKS if c not in OPT_IN and (DEVICE == 'palm-iiix' or c != 'tones')]
            iiix_rom = images / 'Palm/IIIx/Palm-IIIx-3.1-en.rom'
            if 'beam' in checks and DEVICE != 'palm-iiix' and iiix_rom.is_file():
                PEER['palm-iiix'] = launcher_session('palm-iiix', iiix_rom, root / 'peer-iiix')
            for check in checks:
                try:
                    missing = [p for p in NEEDS[check] if p not in probes]
                    if missing:
                        raise Skip('no ' + ', '.join(missing) + ' probe')
                    print(f'{rom.name} {check}: ok, {RUN[check](rom, session, root, probes)}', flush=True)
                except Skip as skip:
                    print(f'{rom.name} {check}: skipped, {skip}', flush=True)
                except AssertionError as error:
                    failed += 1
                    print(f'{rom.name} {check}: FAIL, {error}', flush=True)
    print('frond-68k peripherals: ' + ('FAIL' if failed else 'ok'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
