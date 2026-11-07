#!/usr/bin/env python3
"""Card swap on real firmware: --card-swap loads, take-out and put-in over the desktop control socket.

  tests/card-swap-rom.py [--device DEVICE ...]

Each device runs on a copy of its smoke session (FROND_ARM_SESSIONS/DEVICE, else
build/smoke/machines/DEVICE); a device without one is skipped.

Swap cases, SD on the T|X, a classic Memory Stick and a Memory Stick Pro on the TG50: a load with
the card taken out, a load in another machine with it put in (refused without --card-swap or an
identity), take-out of a candidate and of a saved card on a running machine, put-in on a running
machine then a save and a plain load, and a load refused for a card the device doesn't take.

Change counter cases, SD on the T|X, Tungsten E2 and Zire 72, classic and Pro sticks on the TG50:
machine a saves asleep with its card in, a copy of it writes to the card and saves, and the card
goes back to a. a resumes with the card ejected, then inserted after a short run with a `card`
event; the eject has to wake a (on the TG50 a wake counted by power-state while the slot is still
ejected), and a has to read what the copy wrote (the SD probe's GUEST.TXT, the copy's file listed on
a stick). The card back unchanged resumes inserted, with no wake. Machine copies go in
build/step-cards-c2/rom-TIME/ (mode 0700) and are deleted after a pass; the logs stay.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402
import smoke  # noqa: E402

DEVICES = ('palm-tx', 'palm-e2', 'palm-zire-72', 'sony-clie-tg50')
PROBE = ROOT / 'build/smoke/probes/sd/FrondSDProbe.prc'
# The identity a frontend keeps in card.json: 32 bytes, here hex text as the core makes them.
IDENTITY = b'c1c1c1c1frondcardswaptest0248c1c'.hex()


def sessions(devices):
    folder = os.environ.get('FROND_ARM_SESSIONS')
    found = {}
    for name in devices:
        for path in ([Path(folder) / name] if folder else []) + [ROOT / 'build/smoke/machines' / name]:
            if path.exists():
                found[name] = path
                break
    return found


class Case:
    def __init__(self, device, session, root):
        self.device, self.session, self.root = device, session, root / device
        self.root.mkdir(mode=0o700, parents=True)
        self.count = 0

    def machine(self, name):
        """A fresh copy of the smoke session, named `name`."""
        dest = self.root / name
        if self.session.is_dir():
            shutil.copytree(self.session, dest)
        else:
            dest.mkdir()
            shutil.copy2(self.session, dest / f'{name}.frond')
        return dest

    def run(self, folder, *extra, timeout=300):
        self.count += 1
        work = self.root / f'run-{self.count}'
        work.mkdir()
        return smoke.Run(self.device, work, folder, timeout=timeout, extra=extra)

    def refused(self, folder, *extra, why=''):
        """The core stops at load: no ready, an error, a non-zero status."""
        device = [] if self.device == 'palm-tx' else ['--device', self.device]
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
        done = subprocess.run([str(smoke.CORE), str(folder), *device, *extra, '--no-sound', '--run-for', '1'],
                              capture_output=True, text=True, env=env, timeout=120)
        self.count += 1
        (self.root / f'refused-{self.count}.log').write_text(done.stdout + done.stderr)
        smoke.need(done.returncode != 0, f'{folder.name} {" ".join(extra)} loaded; it should be refused')
        if why:
            smoke.need(why in done.stdout + done.stderr, f'{folder.name}: expected "{why}"')


def settled(folder, name):
    names = [p.name for p in folder.iterdir()]
    leftovers = [n for n in names if '.tmp-' in n or '.gen-' in n or '.journal-' in n]
    smoke.need(name in names and not leftovers, f'{folder.name}: not settled: {sorted(names)}')


def card_state(run, slot, settle=None):
    """The slot's state; with `settle`, waits up to 20 s for it, as a swapped card goes in after a
    short guest run."""
    end = time.monotonic() + (20 if settle else 0)
    while True:
        state = run.require(f'{slot}-state')['message']
        if state == settle or time.monotonic() >= end:
            return state
        time.sleep(0.2)


def check_slot(case, slot, name, make, kind_check):
    """The swap cases for one card. `make` is the size for sd-create or ms-create."""
    swap = ['--card-swap', '--card-identity', IDENTITY]
    # A machine with a saved card.
    a = case.machine('a')
    run = case.run(a)
    try:
        run.require(f'{slot}-create', make)
        run.require('save', a)
    finally:
        run.close()
    settled(a, name)
    drawer = case.root / 'drawer'
    drawer.mkdir()

    # Taken out while off: refused as saved, empty with --card-swap.
    (a / name).rename(drawer / name)
    case.refused(a)
    run = case.run(a, '--card-swap')
    try:
        smoke.need(card_state(run, slot) == 'none', f'{slot} still in after a swap load')
    finally:
        run.close()

    # Put in another machine while off: refused without the flag or an identity.
    b = case.machine('b')
    (drawer / name).rename(b / name)
    case.refused(b)
    case.refused(b, '--card-swap', why='--card-identity')
    run = case.run(b, *swap)
    try:
        smoke.need(card_state(run, slot, 'inserted') == 'inserted', f'{slot} not inserted after a swap load')
        kind_check(run)
        run.require('save', b)
        # Take-out of a saved card: only once ejected, then settled in the folder.
        smoke.need(not run.request(f'{slot}-take-out', ok=None)[0], 'took out an inserted card')
        run.require(f'{slot}-eject')
        reply = run.require(f'{slot}-take-out')['message'].split()
        smoke.need(reply == [IDENTITY, 'writable'], f'take-out replied {reply}')
        smoke.need(card_state(run, slot) == 'none', 'slot not empty after take-out')
        settled(b, name)
        (b / name).rename(drawer / name)
        run.require('save', b)
    finally:
        run.close()

    # Put-in on a running machine, a save, and a plain load.
    c = case.machine('c')
    run = case.run(c)
    try:
        smoke.need(not run.request(f'{slot}-put-in', IDENTITY, ok=None)[0], 'put in a card that is not there')
        (drawer / name).rename(c / name)
        smoke.need(not run.request(f'{slot}-put-in', 'nothex', ok=None)[0], 'put in without an identity')
        run.require(f'{slot}-put-in', f'{IDENTITY} protected')
        smoke.need(card_state(run, slot) == 'inserted', 'not inserted after put-in')
        run.require('save', c)
    finally:
        run.close()
    run = case.run(c)
    try:
        smoke.need(card_state(run, slot) == 'inserted', 'not inserted after put-in, save and load')
        kind_check(run)
        if slot == 'ms':
            smoke.need(run.require('ms-protection')['message'] == 'read-only', 'put-in protection not kept')
        run.require(f'{slot}-eject')
        reply = run.require(f'{slot}-take-out')['message'].split()
        smoke.need(reply == [IDENTITY, 'protected'], f'take-out after put-in replied {reply}')
    finally:
        run.close()
    settled(c, name)
    (c / name).rename(drawer / name)


def large_take_out(case, slot, name, make):
    """Take-out of a card on a candidate, never saved, loads in another machine."""
    d = case.machine('d')
    run = case.run(d)
    try:
        run.require(f'{slot}-create', make)
        smoke.need(any(p.name.startswith('.frondsd.tmp-live-') for p in d.iterdir()), 'no candidate')
        run.require(f'{slot}-eject')
        identity = run.require(f'{slot}-take-out')['message'].split()[0]
    finally:
        run.close()
    settled(d, name)
    e = case.machine('e')
    (d / name).rename(e / name)
    run = case.run(e, '--card-swap', '--card-identity', identity)
    try:
        smoke.need(card_state(run, slot, 'inserted') == 'inserted', 'candidate card not inserted in another machine')
        run.require('save', e)
    finally:
        run.close()
    run = case.run(e)
    run.close()
    return e / name


def tx(case, found):
    check_slot(case, 'sd', 'sd.frondsd', '32', lambda run: None)
    card = large_take_out(case, 'sd', 'sd.frondsd', '256')
    # A card the device doesn't take: an SD card in the TG50.
    if 'sony-clie-tg50' in found:
        tg50 = Case('sony-clie-tg50', found['sony-clie-tg50'], case.root.parent / 'tx-refusal')
        f = tg50.machine('f')
        card.rename(f / 'sd.frondsd')
        tg50.refused(f, '--card-swap', '--card-identity', IDENTITY, why='not supported by device profile')


def tg50(case, found):
    def classic(run):
        smoke.need(run.require('ms-type')['message'] == 'classic 16', 'not the classic stick')

    def pro(run):
        smoke.need(run.require('ms-type')['message'] == 'pro 256', 'not the Pro stick')

    check_slot(case, 'ms', 'ms.frondms', '16', classic)
    shutil.rmtree(case.root / 'drawer')
    for name in 'abc':
        shutil.rmtree(case.root / name)
    check_slot(case, 'ms', 'ms.frondmsp', '256', pro)
    card = large_take_out(case, 'ms', 'ms.frondmsp', '256')
    # A Memory Stick Pro in the T|X.
    if 'palm-tx' in found:
        txcase = Case('palm-tx', found['palm-tx'], case.root.parent / 'tg50-refusal')
        f = txcase.machine('f')
        card.rename(f / 'ms.frondmsp')
        txcase.refused(f, '--card-swap', '--card-identity', IDENTITY, why='Memory Stick Pro is not supported')


def black(rows):
    """The LCD is off (RGBA rows)."""
    return rows is None or not any(max(row[i] for i in range(0, len(row), 4)) > 40 or
                                   max(row[i] for i in range(1, len(row), 4)) > 40 or
                                   max(row[i] for i in range(2, len(row), 4)) > 40 for row in rows[::16])


def lcd_on(run, work, name, seconds):
    """True once the screen is lit within `seconds`."""
    end = time.monotonic() + seconds
    while True:
        if not black(run.frame(work / f'{name}.png')):
            return True
        if time.monotonic() >= end:
            return False
        time.sleep(0.5)


def power_off(run, work, name):
    """The power key, until the screen goes dark."""
    for attempt in range(3):
        run.boot.key(work, 'power', hold_ms=200)
        end = time.monotonic() + 10
        while time.monotonic() < end:
            if black(run.frame(work / f'{name}-{attempt}.png')):
                time.sleep(2)
                return
            time.sleep(0.5)
    raise smoke.Fail(f'{name}: the Palm did not go to sleep')


def power_on(run, work, name):
    for attempt in range(3):
        if lcd_on(run, work, f'{name}-{attempt}', 3):
            return
        run.boot.key(work, 'power', hold_ms=200)
        time.sleep(2)
    smoke.need(lcd_on(run, work, name, 5), f'{name}: the Palm did not wake')


def datebook(run, work, name):
    """Wake the TG50 into Date Book's day view, where the control socket can reach the stick
    (the CLIE launcher is not an application boundary). hard1 opens Date Book."""
    for attempt in range(6):
        rows = run.frame(work / f'{name}.png')
        if not black(rows) and palm_boot.tg50_datebook(rows):
            run.ready(60)
            return
        run.boot.key(work, 'power' if black(rows) else 'hard1', hold_ms=200 if black(rows) else 600)
        time.sleep(3)
    raise smoke.Fail(f'{name}: no Date Book day view')


def power(run):
    """The core's power-state: whether the guest is asleep, and its wakes since the core started."""
    state, wakes = run.require('power-state')['message'].split()
    return state == 'asleep', int(wakes.split('=', 1)[1])


def swap_resume(case, run, slot, name, stick=False):
    """After a swap load: the slot is ejected first and the card goes in after a short guest run,
    with a card event. Returns whether the guest woke by itself, the guest having been asleep at
    the save: from the screen, or for a stick from power-state, which has to count a wake while
    the slot still reads ejected (the TG50 wakes on the eject but keeps its screen dark)."""
    first = card_state(run, slot)
    woke = False
    if stick:
        end = time.monotonic() + 20
        while time.monotonic() < end:
            wakes = power(run)[1]
            state = card_state(run, slot)
            woke = woke or (wakes > 0 and state == 'ejected')
            if state != 'ejected':
                break
            time.sleep(0.05)
    inserted = card_state(run, slot, 'inserted')
    if not stick:
        woke = lcd_on(run, case.root, f'{name}-wake', 15)
    case.notes.append(f'{name}: first state {first}, then {inserted}; asleep at save, '
                      f'{"woke by itself" if woke else "stayed asleep"}')
    smoke.need(first == 'ejected', f'{name}: the card was {first} first, not ejected')
    smoke.need(inserted == 'inserted', f'{name}: the card did not go back in')
    smoke.need(('card', 'inserted') in run.control.events, f'{name}: no card event when the card went in')
    return woke


def sd_probe(case, run, name):
    """Runs the SD probe and returns its newest complete record block as {label: (err, value)}."""
    work = case.root
    run.ready()
    before = 0
    try:
        if smoke.export_if_ready(run.request, work / f'{name}-before.zip'):
            before = sum(1 for line in smoke.read_log(work / f'{name}-before.zip') if line.startswith('begin '))
    except smoke.ControlError:
        pass  # no log yet
    run.control.retry_ready('launch-app', 'Frond SD probe', 60)
    end = time.monotonic() + 120
    while time.monotonic() < end:
        time.sleep(2)
        if not (run.request('guest-ready', ok=None)[0] and smoke.export_if_ready(run.request, work / f'{name}.zip')):
            continue
        lines = smoke.read_log(work / f'{name}.zip')
        begins = [i for i, line in enumerate(lines) if line.startswith('begin ')]
        if len(begins) > before and lines[-1].startswith('complete '):
            block = {}
            for line in lines[begins[-1]:]:
                label, err, value = line.split()
                block[label] = (err.split('=')[1].upper(), int(value.split('=')[1]))
            (work / f'{name}.txt').write_text('\n'.join(lines[begins[-1]:]) + '\n')
            smoke.need(block['complete'][0] == '0000', f'{name}: the SD probe failed: {block}')
            return block
    raise smoke.Fail(f'{name}: the SD probe did not complete')


def sd_counter(case):
    """The SD change counter: a card written in another machine remounts; unchanged, it doesn't."""
    work = case.root
    app = smoke.SMOKE[case.device]['app']
    if smoke.SMOKE[case.device].get('reminder'):
        case.session, _ = smoke.clear_reminder(case.device, work, case.session)
    host = work / 'HOST.TXT'
    host.write_bytes(b'Hello from host\n')
    image = work / 'counter.img'
    smoke.card_image(image, host)
    a = case.machine('counter-a')
    run = case.run(a)
    try:
        run.ready()
        run.require('sd-insert', image)
        time.sleep(3)
        run.ready()
        installed = run.control.retry_ready('install', str(PROBE), 60)
        if installed['code'] == 'needs-reset':
            run.require('reset')
            run.ready()
        run.control.retry_ready('launch-app', app, 60)
        time.sleep(2)
        power_off(run, work, 'a-sleep')
        run.require('save', a)
    finally:
        run.close()
    image.unlink()

    # A copy of a writes to the card and saves, then the card goes back to a.
    b = case.root / 'counter-b'
    shutil.copytree(a, b)
    run = case.run(b)
    try:
        power_on(run, work, 'b-wake')
        sd_probe(case, run, 'b-probe')
        run.require('save', b)
        run.require('sd-eject')
        identity = run.require('sd-take-out')['message'].split()[0]
        run.require('save', b)
    finally:
        run.close()
    settled(b, 'sd.frondsd')
    (a / 'sd.frondsd').unlink()
    (b / 'sd.frondsd').rename(a / 'sd.frondsd')
    swap = ['--card-swap', '--card-identity', identity]
    case.refused(a)
    run = case.run(a, *swap)
    try:
        if not swap_resume(case, run, 'sd', 'a-resume'):
            power_on(run, work, 'a-resume-wake')
        # a never wrote GUEST.TXT itself: it reads the copy's.
        again = sd_probe(case, run, 'a-resume-probe')
        smoke.need(again['prior-open'][0] == '0000' and again['prior-match'][1] == 0 and again['host-match'][0] == '0000',
                   'the remounted card is not the one the copy wrote')
        run.control.retry_ready('launch-app', app, 60)
        time.sleep(2)
        power_off(run, work, 'a-sleep-2')
        run.require('save', a)
    finally:
        run.close()

    # Unchanged since a's own save: no events, the same volume.
    run = case.run(a, *swap)
    try:
        smoke.need(card_state(run, 'sd') == 'inserted', 'an unchanged card did not resume inserted')
        smoke.need(not lcd_on(run, work, 'a-unchanged-wake', 5), 'an unchanged card woke the Palm')
        power_on(run, work, 'a-unchanged-on')
        sd_probe(case, run, 'a-unchanged-probe')
    finally:
        run.close()
    case.notes.append('SD card unchanged: resumed inserted, no wake')


def stick_request(run, work, command, argument, seconds=300):
    """A Memory Stick files request, retried until the stick is mounted. The TG50 may go to sleep
    while its driver scans or formats a stick: a second dark frame in a row gets the power key.
    Away from Date Book (inserting a stick can open the launcher), hard1 goes back to it."""
    end = time.monotonic() + seconds
    dark = 0
    while True:
        result = run.control.request(command, argument)
        if result['ok']:
            return result['message']
        smoke.need(time.monotonic() < end, f'{command}: {result["code"]} {result["message"]}')
        rows = run.frame(work / 'stick-wait.png')
        dark = dark + 1 if black(rows) else 0
        if dark == 2:
            run.boot.key(work, 'power', hold_ms=200)
            dark = 0
        elif result['code'] == 'not-ready' and not black(rows) and not palm_boot.tg50_datebook(rows):
            run.boot.key(work, 'hard1', hold_ms=600)
        time.sleep(2)


def stick_files(run, work):
    """The names ms-files lists at the root."""
    message = stick_request(run, work, 'ms-files', '/')
    return {bytes.fromhex(line.split('\t')[2]).decode() for line in message.splitlines()[1:] if line}


def stick_put(run, work, name):
    stick_request(run, work, 'ms-put', hexpath(host_file(work, name)) + '\n' + hexpath('/' + name))


def stick_counter(case, kind):
    """The stick change counter: a classic or Pro stick written in another machine remounts."""
    work = case.root
    classic = kind == 'classic'
    name = 'ms.frondms' if classic else 'ms.frondmsp'
    a = case.machine(f'counter-{kind}-a')
    run = case.run(a, timeout=1200)
    run.control.command_timeout = 600
    try:
        datebook(run, work, f'{kind}-a-wake')
        if card_state(run, 'ms') == 'inserted':
            run.require('ms-eject')
            time.sleep(2)
        run.require('ms-create', '16' if classic else '256')
        if classic:
            format_stick(run, work)
        files = stick_files(run, work)
        stick_put(run, work, 'A.TXT')
        smoke.need('A.TXT' in stick_files(run, work), f'{kind}: the file put is not listed')
        power_off(run, work, f'{kind}-a-sleep')
        run.require('save', a)
    finally:
        run.close()

    b = case.root / f'counter-{kind}-b'
    shutil.copytree(a, b)
    run = case.run(b, timeout=900)
    run.control.command_timeout = 600
    try:
        datebook(run, work, f'{kind}-b-wake')
        stick_put(run, work, 'B.TXT')
        smoke.need('B.TXT' in stick_files(run, work), f'{kind}: B.TXT not listed in the copy')
        run.require('save', b)
        run.require('ms-eject')
        identity = run.require('ms-take-out')['message'].split()[0]
    finally:
        run.close()
    (a / name).unlink()
    (b / name).rename(a / name)
    swap = ['--card-swap', '--card-identity', identity]
    case.refused(a)
    run = case.run(a, *swap, timeout=900)
    run.control.command_timeout = 600
    try:
        smoke.need(swap_resume(case, run, 'ms', f'{kind}-a-resume', stick=True),
                   f'{kind}: the eject did not wake the TG50 before the stick went back in')
        datebook(run, work, f'{kind}-a-resume-wake')
        listed = stick_files(run, work)
        smoke.need({'A.TXT', 'B.TXT'} <= listed, f'{kind}: the remounted stick lists {sorted(listed)}')
        case.notes.append(f'{kind} stick lists {sorted(listed - files)} after the pair')
        power_off(run, work, f'{kind}-a-sleep-2')
        run.require('save', a)
    finally:
        run.close()

    run = case.run(a, *swap)
    try:
        smoke.need(card_state(run, 'ms') == 'inserted', f'{kind}: an unchanged stick did not resume inserted')
        time.sleep(5)
        smoke.need(power(run)[1] == 0, f'{kind}: an unchanged stick woke the Palm')
        smoke.need(not run.control.events, f'{kind}: card event for an unchanged stick')
    finally:
        run.close()
    for folder in (a, b):
        shutil.rmtree(folder)


def hexpath(path):
    return str(path).encode().hex()


def host_file(work, name):
    path = work / name
    path.write_bytes(f'{name} from Frond\n'.encode())
    return path


def format_stick(run, work):
    """A new classic stick: the Palm's Unrecognized Format prompt, then OK, and the format."""
    def prompt(rows):
        return rows is not None and len(rows) == 320 and sum(
            1 for x in range(320) if rows[140][4*x] < 80 and rows[140][4*x+1] < 40 and rows[140][4*x+2] > 120) > 250
    end = time.monotonic() + 300
    dark = 0
    while not prompt(rows := run.frame(work / 'unrecognized-format.png')):
        dark = dark + 1 if black(rows) else 0
        if dark == 2:
            run.boot.key(work, 'power', hold_ms=200)
            dark = 0
        smoke.need(time.monotonic() < end, 'no Unrecognized Format prompt')
        time.sleep(2)
    run.tap(work, 49, 294)
    time.sleep(5)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--device', action='append', choices=DEVICES)
    args = parser.parse_args()
    found = sessions(args.device or DEVICES)
    if not found:
        print('card swap on firmware: SKIP, no smoke sessions (make smoke-sessions or FROND_ARM_SESSIONS)')
        return 0
    root = ROOT / 'build/step-cards-c2' / time.strftime('rom-%Y%m%d-%H%M%S')
    root.mkdir(parents=True)
    root.chmod(0o700)
    failed = False
    for device in args.device or DEVICES:
        if device not in found:
            print(f'{device}: SKIP, no smoke session')
            continue
        case = Case(device, found[device], root)
        case.notes = []
        try:
            if device == 'palm-tx':
                tx(case, found)
            if device == 'sony-clie-tg50':
                tg50(case, found)
                stick_counter(case, 'classic')
                stick_counter(case, 'pro')
            else:
                sd_counter(case)
            print(f'{device}: card swap ok ({case.count} runs)')
            for note in case.notes:
                print(f'  {note}')
        except (smoke.Fail, Exception) as error:  # noqa: BLE001 - report and go on to the next device
            failed = True
            print(f'{device}: FAIL {error}')
            for note in case.notes:
                print(f'  {note}')
    # Machine copies hold firmware RAM; only the logs stay after a pass.
    if not failed:
        for path in root.rglob('*'):
            if path.is_file() and path.suffix != '.log':
                path.unlink()
    print(f'logs in {root}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
