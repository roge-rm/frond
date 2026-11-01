#!/usr/bin/env python3
"""Quick real-firmware check per device, for after shared core changes.

  tests/smoke.py sessions [DEVICE...]   cold boot through setup, save build/smoke/DEVICE.session
  tests/smoke.py run [DEVICE...]        per device: app and tap, save and reload, supported media and sound

Sessions, firmware copies and everything made here stay private under build/smoke/. A device without a
ready session or firmware is skipped with the command that makes it. A new device is an entry in SMOKE.
"""
import argparse
import array
import importlib.util
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests'))
import palm_boot
from palm_boot import DEVICES
from sd_results import ControlError, export_if_ready, read_log

spec = importlib.util.spec_from_file_location('frond_run', ROOT / 'tools/frond-run.py')
frond_run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(frond_run)

CORE = ROOT / 'out/frond'
OUT = ROOT / 'build/smoke'
PALM = Path('firmware/Palm')
SDK = ROOT.parent / 'palm-os-sdk/sdk-5r3'

# nor, nand: read-only firmware under PALM. setup: function in tools/palm_boot.py that takes a cold boot to
# the launcher. app: database name launched from the launcher, taps: pen taps inside it, area: the part of
# the screen (x0, y0, x1, y1) the app and the taps change. reminder: a check in tools/palm_boot.py for a
# one-time setup reminder that comes due once the session is old enough, dismissed with OK before the steps.
SMOKE = {
    'palm-tx': {'nor': 'TX/Palm-TX-5.4-nor.bin', 'nand': 'TX/Palm-TX-5.4-nand.bin', 'setup': 'tx_setup',
                'app': 'Date Book', 'taps': [(100, 100)], 'area': (0, 0, 320, 448)},
    'palm-e2': {'nor': 'Tungsten-E2/Palm-Tungsten-E2-nor.bin', 'nand': 'Tungsten-E2/Palm-Tungsten-E2-nand.bin',
                'setup': 'e2_setup', 'app': 'Date Book', 'taps': [(100, 100)], 'area': (0, 0, 320, 320)},
    'palm-zire-72': {'nor': 'Zire-72/Palm-Zire72-nor.bin', 'nand': None, 'setup': 'zire_setup',
                     'app': 'Calculator', 'taps': [(60, 200)], 'area': (0, 0, 320, 320),
                     'reminder': 'zire_reminder'},
    'sony-clie-tg50': {'nor': PALM.parent / 'Sony/CLIE-TG50/Sony-Clie-TG50-nor.bin',
                       'nand': None, 'setup': 'tg50_setup', 'app': 'Date Book',
                       'taps': [(100, 100)], 'area': (0, 0, 320, 320),
                       'media': 'memory-stick-focused-qualification', 'audio': 'sony-general'},
}
PROBES = {'sd': 'FrondSDProbe.prc', 'audio': 'FrondAudioProbe.prc'}
SD_MIB = 32


class Fail(Exception):
    pass


def need(condition, message):
    if not condition:
        raise Fail(message)


class SmokeBoot(frond_run.SocketBoot):
    def tap(self, root, x, y, hold_ms=150):
        self.replay([f'0 pen-down {x} {y}', f'{hold_ms} pen-up'])

    def key(self, root, name, hold_ms=200):
        self.replay([f'0 key-down {name}', f'{hold_ms} key-up {name}'])


class Run:
    """One core process on the desktop control socket, run under a heavy emulator slot."""

    def __init__(self, device, work, session=None, sound=None, timeout=900):
        self.work, self.deadline = work, time.monotonic() + timeout
        self.socket_dir = tempfile.mkdtemp(prefix='frond-smoke-')
        address = str(Path(self.socket_dir) / 'control')
        listener = socket.socket(socket.AF_UNIX)
        listener.bind(address)
        listener.listen(1)
        cfg = SMOKE[device]
        if session:
            boot = [str(session)] + (['--device', device] if device != 'palm-tx' else [])
        else:
            boot = ['--device', device, str(PALM / cfg['nor'])] + (['--nand', str(PALM / cfg['nand'])] if cfg['nand'] else [])
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
        command = ['heavy', 'emulator', str(CORE), *boot, '--desktop-socket', address]
        if sound:
            env.update(SDL_AUDIODRIVER='disk', SDL_DISKAUDIOFILE=str(sound))
        else:
            command.append('--no-sound')
        self.log = (work / 'core.log').open('ab')
        self.connection = None
        self.process = subprocess.Popen(command, stdout=self.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                        env=env, start_new_session=True)
        try:
            listener.settimeout(300)  # includes waiting for an emulator slot
            self.connection, _ = listener.accept()
        finally:
            listener.close()
        self.control = frond_run.Control(self.connection, self.process, self.deadline, 60, [])
        need(self.control.line(self.deadline) == ['0', 'event', 'ready', ''], 'no ready event')
        self.boot = SmokeBoot(self.control, work)
        self.tap, self.wait_frame, self.frame = self.boot.tap, self.boot.wait_frame, self.boot.frame

    def ready(self, seconds=60):
        self.control.retry_ready('guest-ready', '', seconds)

    def require(self, command, argument=''):
        result = self.control.request(command, argument)
        need(result['ok'], f'{command}: {result["code"]} {result["message"]}')
        return result

    def request(self, command, argument='', ok=True, deadline=None):
        """The request() that tests/sd_results.py expects."""
        result = self.control.request(command, argument, deadline)
        if ok is None:
            return result['ok'], result['message']
        if result['ok'] != ok:
            raise ControlError(result['code'], result['message'])
        return result['message']

    def install_and_launch(self, prc, name):
        self.ready()
        installed = self.control.retry_ready('install', prc, 60)
        if installed['code'] == 'needs-reset':
            self.require('reset')
            self.ready()
        self.control.retry_ready('launch-app', name, 60)

    def close(self):
        try:
            if self.process.poll() is None:
                self.control.request('quit')
                self.process.wait(timeout=30)
        except Exception:
            pass
        frond_run.stop(self.process)
        if self.connection:
            self.connection.close()
        self.log.close()
        shutil.rmtree(self.socket_dir, ignore_errors=True)


def differing(a, b, area):
    """Fraction of the pixels in area that differ between two frames."""
    x0, y0, x1, y1 = area
    count = sum(1 for y in range(y0, min(y1, len(a))) for x in range(x0, x1)
                if a[y][4 * x:4 * x + 3] != b[y][4 * x:4 * x + 3])
    return count / ((x1 - x0) * (y1 - y0))


def card_image(path, host_file):
    with path.open('wb') as card:
        card.truncate(SD_MIB << 20)
    subprocess.run(['mformat', '-i', str(path), '-v', 'FROND', '::'], check=True, capture_output=True)
    subprocess.run(['mcopy', '-i', str(path), str(host_file), '::/HOST.TXT'], check=True)


def build_probes():
    """The SD and audio probe apps, built once into build/smoke/probes/."""
    for probe, prc in PROBES.items():
        if not (OUT / 'probes' / probe / prc).is_file():
            subprocess.run([sys.executable, str(ROOT / 'tools/build-palm-probe.py'), '--sdk', str(SDK), '--probe', probe,
                            '--output', str(OUT / 'probes' / probe)], cwd=ROOT, check=True, capture_output=True)
    return {probe: OUT / 'probes' / probe / prc for probe, prc in PROBES.items()}


def sd_probe(run, path):
    """Waits for a complete SD probe run and checks the host file was read and matched.

    Palm OS 5.2.8 (Zire 72) reports vfsErrFileEOF (2A07) for a read that reaches the end, so that counts as read."""
    end = time.monotonic() + 90
    while time.monotonic() < end:
        if run.request('guest-ready', ok=None)[0] and export_if_ready(run.request, path):
            lines = read_log(path)
            begin = max((i for i, line in enumerate(lines) if line.startswith('begin ')), default=None)
            if begin is not None and lines[-1].startswith('complete '):
                fields = {line.split()[0]: line.split()[1:] for line in lines[begin:]}
                for key, ok in (('host-open', ('err=0000',)), ('host-read', ('err=0000', 'err=2A07')),
                                ('host-match', ('err=0000',)), ('guest-open', ('err=0000',)),
                                ('guest-read', ('err=0000', 'err=2A07')), ('guest-match', ('err=0000',)),
                                ('complete', ('err=0000',))):
                    need(fields.get(key, ['missing'])[0].upper().replace('ERR=', 'err=') in ok, f'SD probe {key}: {fields.get(key)}')
                need(fields['host-read'][1] == 'value=16', f'SD probe read {fields["host-read"][1]} bytes, not 16')
                return
        time.sleep(1)
    raise Fail('SD probe did not complete')


def pcm_check(path):
    """Signed 16-bit PCM from SDL's disk driver: real signal, not silence."""
    data = path.read_bytes() if path.is_file() else b''
    need(len(data) >= 4, 'no sound written')
    samples = array.array('h', data[:len(data) // 2 * 2])
    peak = max(abs(min(samples)), abs(max(samples)))
    loud = sum(1 for s in samples if abs(s) > 64)
    need(peak > 256 and loud > 4410, f'sound is silent (peak {peak}, {loud} loud samples)')
    return peak


def sony_general(rows):
    # The fixed General heading in the measured Sony Preferences page.
    return (rows is not None and len(rows) == 320 and
            [sum(max(rows[y][4*x:4*x+3]) < 80 for x in range(250, 318))
             for y in (6, 8, 10, 12, 14)] == [1, 5, 3, 13, 18])


def sony_beeper(run, work):
    run.boot.key(work, 'hard1', hold_ms=600)
    run.wait_frame(work / 'sound-datebook.png', palm_boot.tg50_datebook, 30)
    run.control.retry_ready('launch-app', 'Preferences', 60)
    run.wait_frame(work / 'sound-prefs.png',
                   lambda rows: sony_general(rows) or palm_boot.tg50_date_time_prefs(rows) or palm_boot.tg50_formats(rows), 30)
    run.boot.tap(work, 245, 12, hold_ms=300)
    run.wait_frame(work / 'sound-pref-menu.png', palm_boot.tg50_pref_menu, 30)
    run.boot.tap(work, 240, 145, hold_ms=300)
    run.wait_frame(work / 'sound-general.png', sony_general, 30)
    run.boot.replay(['1000 pen-down 189 114', '1300 pen-up',
                     '3000 pen-down 213 137', '3300 pen-up',
                     '6000 pen-down 189 146', '6300 pen-up',
                     '8000 pen-down 210 170', '8300 pen-up'])
    run.wait_frame(work / 'sound-selected.png', sony_general, 30)


def missing(device):
    cfg = SMOKE[device]
    for name in (cfg['nor'], cfg['nand']):
        if name and not (PALM / name).is_file():
            return f'firmware {PALM / name} is missing'
    return None


def workdir(device):
    work = OUT / 'work' / device
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True, mode=0o700)
    return work


def make_session(device):
    work = workdir(device)
    run = Run(device, work)
    try:
        getattr(palm_boot, SMOKE[device]['setup'])(run.boot, work)
        run.ready()
        time.sleep(1)
        session = OUT / f'{device}.session'
        session.unlink(missing_ok=True)
        run.require('save', session)
        session.chmod(0o600)
    finally:
        run.close()


def clear_reminder(device, work, session):
    """Dismisses a due setup reminder and returns a session without it, or the session unchanged.

    The RTC follows the host clock, so an alarm set during setup comes due as soon as an older session loads,
    within a second, as it would on the real device."""
    run = Run(device, work, session)
    try:
        check = getattr(palm_boot, SMOKE[device]['reminder'])
        time.sleep(3)
        if not check(run.frame(work / 'reminder.png')):
            return session, None
        run.tap(work, 50, 280)
        run.wait_frame(work / 'reminder-cleared.png', lambda rows: rows is not None and not check(rows), 30)
        run.ready()
        cleared = work / 'cleared.session'
        run.require('save', cleared)
        return cleared, 'setup reminder dismissed'
    finally:
        run.close()


def smoke(device, probes):
    cfg = SMOKE[device]
    work = workdir(device)
    session = OUT / f'{device}.session'
    steps = []
    if cfg.get('reminder'):
        session, cleared = clear_reminder(device, work, session)
        if cleared:
            steps.append(cleared)

    if cfg.get('media', 'sd') == 'sd':
        # A card holding HOST.TXT, read by the SD probe app.
        run = Run(device, work, session)
        try:
            host = work / 'HOST.TXT'
            host.write_bytes(b'Hello from host\n')
            card = work / 'card.img'
            card_image(card, host)
            run.ready()
            run.require('sd-insert', card)
            time.sleep(3)
            run.install_and_launch(probes['sd'], 'Frond SD probe')
            time.sleep(3)
            sd_probe(run, work / 'sd.zip')
            steps.append('sd')
        finally:
            run.close()

    else:
        steps.append('Memory Stick not exercised (covered by focused storage qualification)')

    # Sound, captured from SDL's disk audio driver.
    pcm = work / 'audio.raw'
    run = Run(device, work, session, sound=pcm)
    try:
        if cfg.get('audio', 'sndstream') == 'sony-general':
            sony_beeper(run, work)
        else:
            run.install_and_launch(probes['audio'], 'Frond audio probe')
        end = time.monotonic() + 40
        while True:
            time.sleep(2)
            try:
                peak = pcm_check(pcm)
                break
            except Fail:
                if time.monotonic() > end:
                    raise
        source = 'Sony General beeper' if cfg.get('audio') == 'sony-general' else 'sound'
        steps.append(f'{source} (peak {peak})')
    finally:
        run.close()

    # An app, a tap in it, save, then reload in a new process. A running app may not answer guest-ready.
    run = Run(device, work, session)
    try:
        launcher = run.frame(work / 'launcher.png')
        if device == 'sony-clie-tg50':
            run.boot.key(work, 'hard1', hold_ms=600)
            run.wait_frame(work / 'datebook-ready.png', palm_boot.tg50_datebook, 30)
            run.ready()
        else:
            run.ready()
            run.control.retry_ready('launch-app', cfg['app'], 60)
            time.sleep(3)
        opened = run.frame(work / 'app.png')
        need(opened and differing(launcher, opened, cfg['area']) > .05, f'{cfg["app"]} did not open')
        for x, y in cfg['taps']:
            run.tap(work, x, y)
        time.sleep(1)
        tapped = run.frame(work / 'tapped.png')
        need(differing(opened, tapped, cfg['area']) > 0, 'the tap changed nothing')
        steps.append('app+tap')
        run.require('save', work / 'saved.session')
    finally:
        run.close()
    run = Run(device, work, work / 'saved.session')
    try:
        again = run.frame(work / 'reloaded.png')
        need(again and differing(tapped, again, cfg['area']) < .01, 'the reloaded screen differs from the saved one')
        steps.append('save+reload')
    finally:
        run.close()
    return steps


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('action', choices=['sessions', 'run'])
    p.add_argument('devices', nargs='*', choices=list(SMOKE))
    args = p.parse_args()
    for device in SMOKE:
        need(device in DEVICES, f'{device} is not in tools/palm_boot.py')
    need(CORE.is_file(), 'build the core first: make')
    OUT.mkdir(parents=True, exist_ok=True)
    OUT.chmod(0o700)
    failed, probes = [], None
    for device in args.devices or list(SMOKE):
        started = time.monotonic()
        reason = missing(device)
        if reason:
            print(f'SKIP {device}: {reason}')
            continue
        try:
            if args.action == 'sessions':
                make_session(device)
                print(f'ok   {device}: build/smoke/{device}.session ({time.monotonic() - started:.0f} s)', flush=True)
                continue
            if not (OUT / f'{device}.session').is_file():
                print(f'SKIP {device}: no ready session, make it with: make smoke-sessions')
                continue
            cfg = SMOKE[device]
            if cfg.get('media', 'sd') == 'sd' or cfg.get('audio', 'sndstream') == 'sndstream':
                probes = probes or build_probes()
            steps = smoke(device, probes or {})
            print(f'ok   {device}: {", ".join(steps)} ({time.monotonic() - started:.0f} s)', flush=True)
        except (Fail, ControlError, frond_run.RunError, AssertionError, OSError, subprocess.CalledProcessError) as error:
            print(f'FAIL {device}: {error} ({time.monotonic() - started:.0f} s; see build/smoke/work/{device}/)', flush=True)
            failed.append(device)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
