#!/usr/bin/env python3
"""Cards an older Frond kept in dana-cards/ move into Cards: a Dana save with both cards linked
there (with saved changes beside them), a Palm m125 save with one and a CLIE T415 save with its
stick. The desktop's move (test-desktop-dana-cards migrateFixture) takes each card into its machine
folder with its changes, and each old save then loads with its cards in place; the cards also go
back in by path with the argument the desktop gives.

Needs FROND_DEVICE_IMAGES, out/frond, out/frond-68k and build/out/desktop/test-desktop-dana-cards.
Evidence goes in build/dana-card-migrate/<time>/."""
import json, os, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
OBJ = Path(os.environ.get('FROND_OBJ') or ROOT / 'build/out')
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot

images = os.environ.get('FROND_DEVICE_IMAGES')
DEVICES = [('alphasmart-dana', 'AlphaSmart/Dana/AlphaSmart-Dana-1.5.rom', 'Dana', ['sd', 'sd2']),
           ('palm-m125', 'Palm/m125/Palm-m125-4.0.1.rom', 'm125', ['sd']),
           ('sony-clie-t415', 'Sony/CLIE-T400/Sony-Clie-T400.rom', 'T415', ['ms'])]
if not images or not all((Path(images) / rom).is_file() for _, rom, _, _ in DEVICES):
    print('dana-card-migrate skipped: no FROND_DEVICE_IMAGES ROMs')
    sys.exit(0)
palm_boot.CORE_68K = ROOT / 'out' / 'frond-68k'
frond = ROOT / 'out' / 'frond'
fixture = OBJ / 'desktop' / 'test-desktop-dana-cards'
work = ROOT / 'build' / 'dana-card-migrate' / time.strftime('%Y%m%d-%H%M%S')
old, sessions = work / 'Frond' / 'dana-cards', work / 'sessions'
old.mkdir(mode=0o700, parents=True)


def tool(*words):
    out = subprocess.run([str(frond), '--card', *map(str, words)], capture_output=True, text=True).stdout.strip().splitlines()
    return out[-1] if out else ''


def state(core, slot):
    return core.send(f'{slot}-state', 'card:').split('card:', 1)[1].strip()


for number, (device, rom, name, slots) in enumerate(DEVICES):
    machine = sessions / name
    machine.mkdir(parents=True)
    session = machine / f'{name}.frond'
    keys = [f'{number}{n}'.ljust(8, str(n + 1)) + 'c' * 24 for n in range(len(slots))]
    cards = []
    for slot, key in zip(slots, keys):
        stick = slot == 'ms'
        card = old / f'{key}.{"frondms" if stick else "frondsd"}'
        assert tool('make', 'ms' if stick else 'sd', 16 if stick else 32, card).startswith('ok '), card
        (old / f'{key}.json').write_text(json.dumps({'protocol': 'sd', 'name': f'{name} {slot}'}))
        cards.append(card)
    core = palm_boot.Core([Path(images) / rom, '--device', device], work / f'{name}-boot.log', '--speed', 'uncapped')
    try:
        time.sleep(8)
        for slot, key, card in zip(slots, keys, cards):
            reply = core.send(f'{slot}-attach "{card}" {key} writable{"" if slot == "ms" else " sd"}', 'card:')
            assert 'inserted' in reply, reply
        time.sleep(4)
        assert 'wrote session' in core.send(f'save-session "{session}"', ('wrote session', 'refused'), timeout=60)
    finally:
        core.close()
    history = sorted(p.name for p in old.glob('.gen-*'))
    print(name, 'saved changes beside the cards:', len(history))
    run = subprocess.run([str(fixture), 'migrateFixture'], capture_output=True, text=True, timeout=300,
                         env={**os.environ, 'QT_QPA_PLATFORM': 'offscreen', 'FROND_MIGRATE': f'{sessions}|{machine}|{old}'})
    (work / f'{name}-migrate.log').write_text(run.stdout + run.stderr)
    assert run.returncode == 0, run.stdout + run.stderr
    for card in cards:
        assert not card.exists(), card
    files = ['ms.frondms'] if slots == ['ms'] else ['sd.frondsd', 'sd2.frondsd'][:len(slots)]
    for f in files:
        assert (machine / f).is_file(), f
    moved = sorted(p.name for p in machine.glob('.gen-*'))
    assert len(moved) == len(history), (history, moved)
    log = (machine / 'card-moves.log').read_text().splitlines()
    assert len(log) == len(slots) and all(' moved ' in line for line in log), log
    attach = {int(line.split(' ', 2)[1]): line.split(' ', 2)[2] for line in run.stdout.splitlines() if line.startswith('attach ')}
    assert sorted(attach) == list(range(len(slots))), run.stdout

    # The old save loads with its cards found in the machine folder.
    core = palm_boot.Core([session, '--device', device], work / f'{name}-load.log')
    try:
        for slot, key in zip(slots, keys):
            assert state(core, slot).startswith(f'inserted {key}'), (slot, state(core, slot))
        time.sleep(2)
        assert 'wrote session' in core.send(f'save-session "{session}"', ('wrote session', 'refused'), timeout=60)
    finally:
        core.close()
    # Taken out while stopped (settled into the file, as the desktop does), then back in by path
    # with --card-swap and the attach the desktop sends.
    for slot, key, f in zip(slots, keys, files):
        settled = tool('settle-linked', machine / f, key)
        assert settled.startswith('ok settled'), settled
    core = palm_boot.Core([session, '--device', device, '--card-swap'], work / f'{name}-swap.log')
    try:
        for n, slot in enumerate(slots):
            now = state(core, slot)
            if now == 'none':
                reply = core.send(f'{slot}-attach {attach[n]}', 'card:')
                assert 'inserted' in reply, reply
            assert state(core, slot).startswith('inserted '), state(core, slot)
    finally:
        core.close()
    print(name, 'ok')
print('dana-card-migrate ok')
