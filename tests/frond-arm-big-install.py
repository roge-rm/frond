#!/usr/bin/env python3
"""A 10 MB record database installs on the T|X and exports back the same, through frond-run.

The guest stores a database this size for far longer than the native call's base deadline, so it
checks the deadline grows with the data moved. Sessions: --session PATH, else
FROND_ARM_SESSIONS/palm-tx, else build/smoke/machines/palm-tx; with none it prints SKIPPED, and
fails with FROND_REQUIRE_SESSIONS=1."""
import argparse
import json
import os
import random
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NAME = b'FrondBigInstall'
COUNT = 2400  # records of 8 to 8192 bytes, about 9.8 MB, like The Quest's TheQuestRes.pdb


def build_pdb():
    rng = random.Random(4038)
    records = [rng.randbytes(rng.randint(8, 8192)) for _ in range(COUNT)]
    header = NAME.ljust(32, b'\0') + struct.pack('>HHIIIIII4s4sIIH', 0, 1, 0xc0000000, 0xc0000000,
                                                    0, 0, 0, 0, b'bigD', b'FrBI', 0, 0, COUNT)
    offset = len(header) + COUNT * 8 + 2
    table = b''
    for index, data in enumerate(records):
        table += struct.pack('>II', offset, 0x10000 + index)
        offset += len(data)
    return header + table + b'\0\0' + b''.join(records), records


def records_of(pdb):
    attributes, count = struct.unpack('>H', pdb[32:34])[0], struct.unpack('>H', pdb[76:78])[0]
    if attributes & 1:
        raise ValueError('exported database is a resource database')
    offsets = [struct.unpack('>I', pdb[78 + i * 8:82 + i * 8])[0] for i in range(count)] + [len(pdb)]
    return [pdb[offsets[i]:offsets[i + 1]] for i in range(count)]


def session(args):
    if args.session:
        return Path(args.session)
    folder = os.environ.get('FROND_ARM_SESSIONS')
    for path in ([Path(folder) / 'palm-tx'] if folder else []) + [ROOT / 'build/smoke/machines/palm-tx']:
        if path.exists():
            return path
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', help='T|X machine folder or .frond file')
    parser.add_argument('--core', type=Path, default=ROOT / 'out/frond')
    args = parser.parse_args()
    path = session(args)
    if not path:
        print('SKIPPED: no palm-tx session')
        return 1 if os.environ.get('FROND_REQUIRE_SESSIONS') == '1' else 0
    pdb, records = build_pdb()
    with tempfile.TemporaryDirectory(dir=ROOT / 'build') as work:
        work = Path(work)
        source = work / 'FrondBigInstall.pdb'
        source.write_bytes(pdb)
        command = [sys.executable, str(ROOT / 'tools/frond-run.py'), '--core', str(args.core),
                   '--session', str(path), '--install', str(source), '--app', 'Memo Pad',
                   '--export-db', NAME.decode(), '--run-for', '1', '--headless', '--timeout', '900',
                   '--ready-timeout', '400', '--command-timeout', '400', '--output', str(work / 'run')]
        run = subprocess.run(command, capture_output=True, text=True)
        try:
            result = json.loads(run.stdout)
        except json.JSONDecodeError:
            print(run.stdout[-2000:], run.stderr[-2000:])
            raise
        steps = [step for step in result['steps'] if step['command'] == 'install']
        if result['status'] != 'ok' or not steps or not steps[0]['ok']:
            print('FAIL:', result['status'], result['stage'], result.get('error'), steps)
            return 1
        with zipfile.ZipFile(result['artifacts']['exports'][0]['path']) as archive:
            exported = archive.read(archive.infolist()[0])
        if exported[:32].rstrip(b'\0') != NAME or records_of(exported) != records:
            print('FAIL: the exported database differs from the one installed')
            return 1
    print(f'PASS: {len(pdb)} byte database installed and exported the same')
    return 0


if __name__ == '__main__':
    sys.exit(main())
