#!/usr/bin/env python3
"""frond-68k peripheral and workflow checks on private IIIx ROMs: piezo tones, backlight, sleep
and wake, serial HotSync with pilot-link over a PTY and TCP, and installing, launching, listing
and exporting databases. Each ROM boots to the launcher once; every check starts from that
session.

ROMs come from --rom, else the IIIx 3.1 ROM in the private firmware folder; with none the checks
are skipped. The probe PRCs are built with tools/build-palm-probe.py when m68k-palmos-gcc and the
Palm OS 3.5 SDK are there (--sdk), else the checks that need them are skipped."""
import argparse
import os
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

CHECKS = ['tones', 'backlight', 'sleep', 'serial', 'databases']
HELLO = Path.home() / 'Projects/palm/hello/Hello.prc'
FRONDBENCH = ROOT / 'build/frondbench/FrondBench.prc'
PROBES = {'tone68k': 'FrondTone68k', 'alarm68k': 'FrondAlarm68k'}


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

    def __init__(self, image, root, name, *args):
        self.root = root / name
        self.root.mkdir(parents=True, exist_ok=True)
        self.core = palm_boot.Core([image, '--device', 'palm-iiix'], self.root / 'log.txt', *args)

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
    for (frequency, length), (_, got_length, got) in zip(expected, found[-4:-1]):
        if abs(got - frequency) > frequency * 0.02 or abs(got_length - length) > 0.03:
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
        if not (lit[1] > lit[0] + 40 and lit[1] > lit[2] + 40) or lit == unlit:
            raise AssertionError(f'lit panel {lit} not green (unlit {unlit})')
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
        if rows is None or not palm_boot.iiix_launcher(rows):
            raise AssertionError('power did not wake to the launcher')
    with Machine(r / 'asleep.frond', root, 'sleep-power-reload') as m:
        time.sleep(1)
        if m.frame('asleep.png') is not None:
            raise AssertionError('reloaded asleep session has the LCD on')
        m.core.key(m.root, 'power', 150)
        time.sleep(2)
        rows = m.frame('awake.png')
        if rows is None or not palm_boot.iiix_launcher(rows):
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
    """The serial port as pilot-link sees it: the PTY itself, or a PTY from socat for TCP."""

    def __init__(self, m, mode):
        self.socat = None
        state = m.send('serial-state', 'serial:')
        address = state.split()[2]
        if mode == 'pty':
            self.port = address
            return
        self.port = str(m.root / 'tcp-tty')
        self.socat = subprocess.Popen(['socat', f'pty,raw,echo=0,link={self.port}', f'tcp:{address}'],
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
    return f'{backed} databases backed up; {state.split(", ", 1)[1]}; tcp {tcp.split(", ", 1)[1]}'


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
        if rows is None or palm_boot.iiix_launcher(rows):
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
            if rows is None or palm_boot.iiix_launcher(rows):
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


NEEDS = {'tones': ['tone68k'], 'sleep': ['alarm68k'], 'backlight': [], 'serial': [], 'databases': []}
RUN = {'tones': check_tones, 'backlight': check_backlight, 'sleep': check_sleep, 'serial': check_serial,
       'databases': check_databases}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path, action='append', default=[])
    parser.add_argument('--only', action='append', choices=CHECKS)
    parser.add_argument('--sdk', type=Path, default=ROOT.parent / 'palm-os-sdk/sdk-3.5')
    parser.add_argument('--evidence', type=Path, help='keep logs and frames here')
    args = parser.parse_args()
    roms = args.rom or [p for p in [palm_boot.palm_images() / 'Palm/IIIx/Palm-IIIx-3.1-en.rom'] if p.is_file()]
    if not roms:
        print('frond-68k peripherals: skipped, no IIIx ROM')
        return 0
    probes = build_probes(ROOT, args.sdk.expanduser())
    failed = 0
    with tempfile.TemporaryDirectory(prefix='frond-68k-peripherals-') as work:
        base = args.evidence or Path(work)
        for rom in roms:
            root = base / rom.stem
            root.mkdir(parents=True, exist_ok=True)
            session = root / 'launcher.frond'
            core = palm_boot.Core(['--rom', rom, '--device', 'palm-iiix'], root / 'boot.txt')
            try:
                palm_boot.iiix_setup(core, root)
                core.send(f'save-session "{session}"', 'wrote session')
            finally:
                core.close()
            for check in args.only or CHECKS:
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
