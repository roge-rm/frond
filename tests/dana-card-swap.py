#!/usr/bin/env python3
"""frond-68k card swap on the Dana: a saved card moved into its machine folder (move-linked) still
loads, and under --card-swap a saved card that's gone leaves its slot empty for sd2-attach.

Needs FROND_DEVICE_IMAGES (AlphaSmart/Dana/AlphaSmart-Dana-1.5.rom), out/frond and out/frond-68k.
Evidence goes in build/dana-card-swap/<time>/."""
import os, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot

images = os.environ.get('FROND_DEVICE_IMAGES')
rom = Path(images) / 'AlphaSmart' / 'Dana' / 'AlphaSmart-Dana-1.5.rom' if images else None
if not rom or not rom.is_file():
    print('dana-card-swap skipped: no FROND_DEVICE_IMAGES Dana ROM')
    sys.exit(0)
palm_boot.CORE_68K = ROOT / 'out' / 'frond-68k'
frond = ROOT / 'out' / 'frond'
work = ROOT / 'build' / 'dana-card-swap' / time.strftime('%Y%m%d-%H%M%S')
old, machine, drawer = work / 'dana-cards', work / 'machine', work / 'drawer'
for d in (old, machine, drawer):
    d.mkdir(mode=0o700, parents=True)
keys = ['a' * 32, 'b' * 32]
cards = [old / f'{k}.frondsd' for k in keys]

def tool(*words):
    out = subprocess.run([str(frond), '--card', *map(str, words)], capture_output=True, text=True).stdout.strip().splitlines()
    return out[-1] if out else ''

for c in cards:
    assert tool('make', 'sd', 32, c).startswith('ok '), c
session = machine / 'session.frond'
core = palm_boot.Core([rom, '--device', 'alphasmart-dana'], work / 'boot.log', '--speed', 'uncapped')
try:
    time.sleep(8)
    for n, (k, c) in enumerate(zip(keys, cards)):
        reply = core.send(f'{"sd2" if n else "sd"}-attach "{c}" {k} writable sd', 'card:')
        assert 'inserted' in reply, reply
    time.sleep(3)
    assert 'wrote session' in core.send(f'save-session "{session}"', ('wrote session', 'refused'), timeout=60)
finally:
    core.close()

def start(*extra):
    return palm_boot.Core([session, '--device', 'alphasmart-dana'], work / f'load{len(list(work.glob("load*")))}.log', *extra)

def state(core, n):
    return core.send(f'{"sd2" if n else "sd"}-state', 'card:').split('card:', 1)[1].strip()

# Card 1 moves into the machine folder with its saved changes; the old save finds it there.
assert tool('move-linked', cards[0], machine / 'sd.frondsd') == 'ok moved'
core = start()
try:
    assert state(core, 0).startswith(f'inserted {keys[0]}'), state(core, 0)
    assert state(core, 1).startswith(f'inserted {keys[1]}')
finally:
    core.close()
# Card 2 goes to the drawer: without --card-swap the save is refused, with it slot 2 loads empty.
assert tool('move-linked', cards[1], drawer / 'sd.frondsd') == 'ok moved'
refused = subprocess.run([str(palm_boot.CORE_68K), str(session), '--device', 'alphasmart-dana', '--no-sound', '--run-for', '1'],
                         capture_output=True, text=True, env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'}, timeout=120)
assert refused.returncode != 0 and 'session refused' in refused.stderr, refused.stderr
core = start('--card-swap')
try:
    assert state(core, 1) == 'none', state(core, 1)
    assert state(core, 0).startswith(f'inserted {keys[0]}')
    # The frontend puts the slot's card in after start.
    assert tool('settle-linked', drawer / 'sd.frondsd', keys[1]).startswith('ok settled')
    assert tool('move-linked', drawer / 'sd.frondsd', machine / 'sd2.frondsd') == 'ok moved'
    reply = core.send(f'sd2-attach "{machine / "sd2.frondsd"}" {keys[1]} writable sd', 'card:')
    assert 'inserted' in reply, reply
    time.sleep(2)
    assert 'wrote session' in core.send(f'save-session "{session}"', ('wrote session', 'refused'), timeout=60)
finally:
    core.close()
# The new save links both cards from the machine folder.
core = start()
try:
    assert state(core, 0).startswith(f'inserted {keys[0]}') and state(core, 1).startswith(f'inserted {keys[1]}')
finally:
    core.close()
print('dana-card-swap ok')
