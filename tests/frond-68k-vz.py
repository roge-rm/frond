#!/usr/bin/env python3
"""A VZ Palm (m125, m500, WorkPad c500, m505, m515, m130, m520) boots, keeps a PRC, reads its SD card and refuses other devices' ROMs.

Runs tests/frond-68k-ez.py for the boot, install, save, reload and launch. Then, on the saved session,
attaches a FAT card image to the one slot, runs the card probe and checks the Palm side sees the
volume and the host's file, saves with the card in and checks a fresh process brings the card back
with the guest's file on it. Last, it starts the device on the other VZ ROMs and expects refusals.

Run: python3 tests/frond-68k-vz.py --device palm-m125 --rom PRIVATE.rom --other-rom OTHER.rom
     --prc FrondSync68k.prc --probe FrondDanaCards.prc --make-card build/dana-card-fixture --evidence DIR
--other-rom is a ROM the device must refuse (repeatable). Needs the private ROMs.
"""
import argparse
import hashlib
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

VZ = ('palm-m125', 'palm-m500', 'ibm-workpad-c500', 'palm-m505', 'palm-m515', 'palm-m130', 'palm-m520')


def fixture(root, make_card):
    """A 32 MiB FAT card with HOST.TXT, wrapped as a .frondsd."""
    raw = root / 'card.img'
    subprocess.run([str(make_card), str(raw)], check=True)
    host = root / 'host.txt'
    host.write_text('VZ host card\n')
    subprocess.run(['mlabel', '-i', f'{raw}@@32256', '::VZCARD'], check=True)
    subprocess.run(['mcopy', '-i', f'{raw}@@32256', str(host), '::HOST.TXT'], check=True)
    data = raw.read_bytes()
    header = bytearray(64)
    header[:8] = b'FRONDSD\0'
    struct.pack_into('<IIQI', header, 8, 1, 512, len(data), zlib.crc32(data))
    card = root / 'card.frondsd'
    card.write_bytes(header + data)
    return card


def extract(path, name):
    return subprocess.check_output(['mtype', '-i', f'{path}@@32320', '::' + name])


def command(core, op, args='', expected=None):
    reply = core.send(f'sd-{op} {args}'.rstrip(), 'card:').strip()
    print(reply, flush=True)
    if expected:
        assert expected in reply, reply
    else:
        assert 'error:' not in reply, reply
    return reply


def probe(core, root, tag, persisted):
    """Runs the card probe and returns its rows; the one volume must open HOST.TXT and write GUEST.TXT."""
    core.launch('Frond Dana cards')
    time.sleep(3)
    dest = root / f'{tag}.pdb'
    core.send(f'db-export-db FrondDanaCards "{dest}"', 'export written')
    data = dest.read_bytes()
    count = struct.unpack_from('>H', data, 76)[0]
    offsets = [struct.unpack_from('>I', data, 78 + i * 8)[0] for i in range(count)] + [len(data)]
    rows = [data[offsets[i]:offsets[i + 1]].rstrip(b'\0').decode() for i in range(count)]
    rows = rows[max(i for i, row in enumerate(rows) if row.startswith('begin ')):]
    print(tag, rows, flush=True)
    assert rows[-1] == 'complete err=0000 value=1', rows
    assert any(row.startswith('volume-info err=0000') for row in rows), rows
    assert any(row.startswith('host-open err=0000') for row in rows), rows
    assert any(row.startswith('guest-match err=0000') for row in rows), rows
    if persisted:
        assert any(row.startswith('persisted-match err=0000') for row in rows), rows
    core.frame(root / f'{tag}.png')
    return rows


def card_check(args, session):
    root = args.evidence / 'card'
    root.mkdir(mode=0o700)
    card = fixture(root, args.make_card.resolve())
    saved = root / 'card-in.frond'
    core = palm_boot.Core([session, '--device', args.device], root / 'card.log')
    try:
        core.until(f'install "{args.probe.resolve()}"', 'installation ', 'successful')
        command(core, 'attach', f'"{card}" {"1" * 32} writable sd', 'inserted')
        # A second slot isn't there.
        assert 'error:' in core.send('sd2-state', 'card:')
        time.sleep(6)
        probe(core, root, 'inserted', False)
        core.send(f'save-session "{saved}"', 'wrote session')
        assert saved.read_bytes().startswith(b'FRONDLINKSD1\n')
    finally:
        core.close()
    core = palm_boot.Core([saved, '--device', args.device], root / 'reload.log')
    try:
        time.sleep(3)
        command(core, 'state', expected='inserted ' + '1' * 32)
        probe(core, root, 'reloaded', True)
        command(core, 'eject', expected='ejected')
        time.sleep(1)
        command(core, 'take-out', expected='taken-out')
    finally:
        core.close()
    # The guest's file reached the card file on the host.
    guest = extract(card, 'GUEST.TXT')
    print('card guest file', guest, hashlib.sha256(guest).hexdigest(), flush=True)
    assert guest == b'Dana guest slot 1\n', guest
    assert extract(card, 'HOST.TXT') == b'VZ host card\n'
    print(f'{args.device}: card read and written, saved with the card in and reloaded', flush=True)


def check_rom(device, rom):
    run = subprocess.run([str(palm_boot.CORE_68K), '--rom', str(rom), '--device', device, '--check-rom'],
                         capture_output=True, text=True, errors='replace', timeout=60)
    return run.returncode, (run.stdout + run.stderr).strip()


def refusals(args):
    code, text = check_rom(args.device, args.rom)
    assert code == 0, text
    for index, rom in enumerate(args.other_rom):
        code, text = check_rom(args.device, rom)
        (args.evidence / f'refusal-{index}.log').write_text(text + '\n')
        if code == 0 or 'this is a' not in text:
            raise AssertionError(f'{args.device} did not refuse {rom.name}: {text[-200:]}')
        print(f'{args.device}: refused {rom.name}: {text.splitlines()[-1]}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True, choices=VZ)
    parser.add_argument('--rom', required=True, type=Path)
    parser.add_argument('--other-rom', action='append', default=[], type=Path)
    parser.add_argument('--prc', required=True, type=Path)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--make-card', required=True, type=Path)
    parser.add_argument('--evidence', required=True, type=Path, help='new directory')
    parser.add_argument('--skip-boot', action='store_true', help='reuse the boot step\'s saved session in --evidence')
    args = parser.parse_args()
    args.evidence = args.evidence.resolve()
    if not args.skip_boot:
        subprocess.run([sys.executable, str(ROOT / 'tests/frond-68k-ez.py'), '--device', args.device, '--rom',
                        str(args.rom), '--prc', str(args.prc), '--evidence', str(args.evidence)], check=True)
    args.evidence.chmod(0o700)
    card_check(args, args.evidence / 'installed.frond')
    refusals(args)
    print(f'PASS {args.device}: boot, PRC kept over a reload, one SD slot kept over a reload, other ROMs refused',
          flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
