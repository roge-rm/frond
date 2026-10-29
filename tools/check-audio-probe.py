#!/usr/bin/env python3
"""Check the original Palm audio probe's callback/control log, not host playback.

A pass establishes callback production and API results only. It does not prove
that HAL, preview, SDL, or a physical audio device delivered those samples.
"""
import argparse
import collections
import json
from pathlib import Path
import re
import struct
import zipfile


def read_log(path):
    with zipfile.ZipFile(path) as archive:
        names = [name for name in archive.namelist()
                 if name.rsplit('/', 1)[-1] == 'FrondAudioLog.pdb']
        if len(names) != 1:
            raise ValueError('Expected exactly one FrondAudioLog.pdb')
        raw = archive.read(names[0])
    if len(raw) < 78:
        raise ValueError('Truncated database header')
    count = struct.unpack_from('>H', raw, 76)[0]
    end = 78 + count * 8
    if end > len(raw):
        raise ValueError('Truncated record table')
    offsets = [struct.unpack_from('>I', raw, 78 + i * 8)[0] for i in range(count)] + [len(raw)]
    if any(not end <= x < y <= len(raw) for x, y in zip(offsets, offsets[1:])):
        raise ValueError('Invalid record offsets')
    return [raw[x:y].rstrip(b'\0').decode('ascii') for x, y in zip(offsets, offsets[1:])]


def expected_pcm(seed, sequence, actual):
    samples = []
    for frame in range(actual // 4):
        sample = ((seed * 97 + sequence * 131 + frame * 17) & 4095) - 2048
        samples.extend((sample, -sample))
    return struct.pack('<' + 'h' * len(samples), *samples)


def check(lines):
    def require(condition, message):
        if not condition:
            raise ValueError(message)

    require(lines and lines[0] == 'audio-probe-v1 signed16le stereo44100 full-half-zero-full',
            'Unknown probe version')
    require(lines[-1] == 'complete', 'Probe did not complete')
    blocks = collections.defaultdict(list)
    controls = collections.defaultdict(dict)
    pair = collections.defaultdict(list)
    counts, current, in_pair = [], None, False
    for line in lines[1:-1]:
        if line in ('pair-begin', 'pair-end'):
            require(in_pair == (line == 'pair-end'), 'Invalid pair boundaries')
            in_pair = line == 'pair-begin'
            current = None
            continue
        match = re.fullmatch(r'block seed=(\d+) seq=(\d+) capacity=(\d+) actual=(\d+) checksum=(\d+) channel=(\d+)', line)
        if match:
            seed, sequence, capacity, actual, checksum, channel = map(int, match.groups())
            require(seed in range(1, 6), 'Unknown block seed')
            require(sequence == len(blocks[seed]), 'Missing, duplicated or reordered callback')
            require(0 < capacity <= 4096 and capacity % 4 == 0, 'Invalid callback capacity')
            expected = capacity
            if seed == 2:
                if sequence % 4 == 1:
                    expected = (capacity // 2) & ~3
                elif sequence % 4 == 2:
                    expected = 0
            require(actual == expected, 'Incorrect actual callback length')
            pcm = expected_pcm(seed, sequence, actual)
            words = struct.unpack('<' + 'H' * (len(pcm) // 2), pcm)
            require(checksum == sum(words) & 0xffffffff, 'Callback pattern checksum mismatch')
            require(channel != 0, 'Invalid callback channel')
            blocks[seed].append((capacity, actual, channel))
            continue
        match = re.fullmatch(r'([a-z-]+) err=([0-9a-fA-F]{4}) value=(\d+) tick=(\d+)', line)
        require(match is not None, 'Malformed record: ' + line)
        label, error, value, tick = match.groups()
        error, value, tick = int(error, 16), int(value), int(tick)
        require(error == 0, 'Guest API or callback overflow failed: ' + line)
        if label == 'case':
            require(not in_pair and value in (1, 2, 3) and value not in controls,
                    'Duplicate/unknown case')
            current = value
        if label == 'callback-count':
            require(in_pair or current is not None, 'Callback count outside a case')
            counts.append(value)
        elif in_pair:
            require(label in ('pair-create', 'pair-start', 'pair-delete'), 'Unknown pair operation')
            pair[label].append(value)
        elif current is not None:
            require(label not in controls[current], 'Duplicate case control: ' + label)
            controls[current][label] = value
        else:
            raise ValueError('Operation outside a case: ' + label)
    require(not in_pair and set(blocks) == {1, 2, 3, 4, 5}, 'Missing stream or pair cases')
    require(counts == [len(blocks[i]) for i in range(1, 6)], 'Callback count or bounded log mismatch')
    for seed in range(1, 6):
        require(len(blocks[seed]) >= 4, 'Insufficient callbacks')
        require(len({b[2] for b in blocks[seed]}) == 1, 'Channel changed within a stream')
    for seed in (1, 2, 3):
        c = controls[seed]
        required = {'case', 'create-extended' if seed == 2 else 'create-basic', 'volume-unity',
                    'start', 'pause', 'pause-return-calls', 'paused-calls', 'resume', 'volume-half',
                    'get-volume', 'pan-left', 'get-pan', 'pan-center', 'stop', 'stop-return-calls',
                    'stopped-calls', 'restart', 'delete'}
        require(set(c) == required, 'Missing/unexpected control result')
        require(c['pause-return-calls'] == c['paused-calls'], 'Callbacks continued after pause returned')
        require(c['stop-return-calls'] == c['stopped-calls'], 'Callbacks continued after stop returned')
        require(c['get-volume'] == 512 and c['get-pan'] == 0xfffffc00, 'Volume/pan readback failed')
    for label in ('pair-create', 'pair-start', 'pair-delete'):
        require(pair[label] == [0, 1], 'Missing/failed simultaneous-stream operation')
    require(blocks[4][0][2] != blocks[5][0][2], 'Simultaneous streams alias')
    require(any(actual == 0 for capacity, actual, channel in blocks[2])
            and any(0 < actual < capacity for capacity, actual, channel in blocks[2])
            and any(actual == capacity for capacity, actual, channel in blocks[2]),
            'Missing extended length variants')
    return {'status': 'pass', 'scope': 'guest callback production and API control results',
            'callbacks': {str(seed): len(blocks[seed]) for seed in range(1, 6)},
            'address_reused': blocks[1][0][2] == blocks[3][0][2]}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(check(read_log(args.archive)), indent=2))
    except (ValueError, OSError, zipfile.BadZipFile) as error:
        parser.exit(1, str(error) + '\n')
