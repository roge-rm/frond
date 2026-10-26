#!/usr/bin/env python3
"""Optional private E2 cold boot, replay, save and fresh-process restore evidence.

Run under heavy emulator. Captures require visual inspection; successful execution
is not a guest UI, firmware authenticity or hardware qualification verdict.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import struct
import subprocess
import time


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def positive(value):
    n = int(value)
    if not 1 <= n <= 600:
        raise argparse.ArgumentTypeError('expected 1..600 seconds')
    return n


def run(core, args, output, name, seconds, replay=None, replay_seconds=30):
    with (output / f'{name}.log').open('wb') as log:
        process = subprocess.Popen([str(core), *map(str, args), '--no-sound'],
                                   stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True,
                                   env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'))
        try:
            def command(line):
                if process.poll() is not None:
                    raise RuntimeError(f'{name} exited early; inspect private log')
                process.stdin.write((line + '\n').encode()); process.stdin.flush()
            time.sleep(seconds)
            if replay:
                command(f'input-replay "{replay}"')
                time.sleep(replay_seconds)
            capture = output / f'{name}.replay'
            capture.write_text(f'0 snapshot "{output / (name + ".png")}"\n')
            command(f'input-replay "{capture}"')
            time.sleep(2)
            saved = output / f'{name}.session'
            command(f'save-session "{saved}"')
            command('quit')
            if process.wait(timeout=60) != 0:
                raise RuntimeError(f'{name} failed; inspect private log')
            if not saved.is_file() or not (output / f'{name}.png').is_file():
                raise RuntimeError(f'{name} did not save/capture; inspect private log')
            # This runner must never route a different device into the E2 verdict.
            with saved.open('rb') as stream:
                magic, version, device = struct.unpack('<III', stream.read(12))
            if magic != 0x19800819 or version not in (4, 5) or device != 0:
                raise RuntimeError('saved file is not an E2 session')
            return saved
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL); process.wait()
            process.stdin.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, default=Path('out/frond'))
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--nand', type=Path, help='omit to exercise inherited blank NAND startup')
    parser.add_argument('--input-replay', type=Path)
    parser.add_argument('--boot-seconds', type=positive, default=60)
    parser.add_argument('--replay-seconds', type=positive, default=30)
    parser.add_argument('--output', type=Path, required=True, help='new private directory')
    args = parser.parse_args()
    core, rom, output = args.core.resolve(), args.rom.resolve(), args.output.resolve()
    # Names enter the native console, which has a deliberately small quoting grammar.
    if any(c in str(output) for c in '\n\r"\\'):
        parser.error('output path contains unsupported console characters')
    with rom.open('rb') as stream:
        found = False
        while data := stream.read(32768):
            if len(data) >= 0x88 and struct.unpack_from('<I', data, 8)[0] == 0xfeedbeef:
                found = struct.unpack_from('<II', data, 0x80) == (0x50616c6d, 0x68737072)
                break
    if not found:
        parser.error('recognized Palm/E2 header required; geometry alone is insufficient')
    sources = [rom] + ([args.nand.resolve()] if args.nand else [])
    if args.input_replay:
        sources.append(args.input_replay.resolve())
    originals = {str(p): digest(p) for p in sources}
    output.mkdir(mode=0o700) # exclusive: existing directories are refused
    replay = None
    replay_seconds = args.replay_seconds
    if args.input_replay:
        # Keep all captures in the private result directory, leaving replay input untouched.
        replay = output / 'input.replay'
        lines = []
        snapshots = 0
        for line in args.input_replay.read_text().splitlines():
            tokens = shlex.split(line, comments=True)
            if tokens and (not tokens[0].isdigit() or int(tokens[0]) >= replay_seconds * 1000):
                parser.error('every replay event must fall before --replay-seconds')
            if len(tokens) >= 2 and tokens[1] == 'snapshot':
                snapshots += 1
                line = f'{tokens[0]} snapshot "{output / ("input-" + str(snapshots) + ".png")}"'
            lines.append(line)
        replay.write_text('\n'.join(lines) + '\n')
    result = {'inputs_sha256': originals, 'core_sha256': digest(core),
              'visual_verdict': 'unverified: inspect cold.png, restore.png and replay captures',
              'audio': 'not tested (sound disabled)', 'physical_hardware': 'unavailable'}
    try:
        raw = [rom] + (['--nand', args.nand.resolve()] if args.nand else [])
        saved = run(core, raw, output, 'cold', args.boot_seconds, replay, replay_seconds)
        saved_hash = digest(saved)
        run(core, [saved], output, 'restore', 5)
        result['saved_session_unchanged'] = digest(saved) == saved_hash
        if not result['saved_session_unchanged']:
            raise RuntimeError('saved session changed during restore')
        result['execution'] = 'cold execution, session save and fresh-process restore completed'
    except Exception as error:
        result['execution'] = 'failed'
        result['error'] = str(error)
        raise
    finally:
        result['inputs_unchanged'] = all(digest(Path(p)) == h for p, h in originals.items())
        (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        if not result['inputs_unchanged']:
            raise RuntimeError('source input changed during run')
    print('E2 execution evidence saved privately; visual inspection and further qualification required')
