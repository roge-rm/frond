"""Strict compatibility log decoding and observable outcome validation (no firmware)."""
import re
import struct
import zipfile

APP = 'Frond compatibility'
LOG = 'FrondCompatLog'
CASES = ('memory', 'database', 'preferences', 'ticks', 'drawing',
         'native-arm', 'native-thumb', 'event-queue')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def checksum():
    value = 0x12345678
    for _ in range(37):
        value = ((value * 1664525 + 1013904223) ^ (value >> 7)) & 0xffffffff
    return value


def config_database(token, previous=''):
    require(re.fullmatch('[0-9a-f]{32}', token), 'Invalid run token')
    require(not previous or re.fullmatch('[0-9a-f]{32}', previous), 'Invalid previous token')
    raw = bytearray(88)
    raw[:14] = b'FrondCompatCfg'
    raw[60:68] = b'DATAFrCp'
    struct.pack_into('>H', raw, 76, 1)
    struct.pack_into('>I', raw, 78, len(raw))
    raw[85] = 1  # record unique ID
    raw.extend(token.encode() + b'\0' + previous.encode().ljust(33, b'\0'))
    return raw


def read_log(path):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        require(len(names) == 1 and names[0].rsplit('/', 1)[-1] == LOG + '.pdb',
                'Expected exactly one named compatibility log database')
        info = archive.getinfo(names[0])
        require(info.file_size <= 65536, 'Oversized compatibility log')
        raw = archive.read(names[0])
    require(len(raw) >= 78, 'Truncated Palm database header')
    require(raw[:32].split(b'\0', 1)[0] == LOG.encode() and raw[60:68] == b'LOGSFrCp',
            'Wrong Palm log database identity')
    require(not (struct.unpack_from('>H', raw, 32)[0] & 1), 'Log must be a record database')
    count = struct.unpack_from('>H', raw, 76)[0]
    end = 78 + count * 8
    require(count == len(CASES) + 2 and end <= len(raw), 'Wrong compatibility record count')
    offsets = [struct.unpack_from('>I', raw, 78 + i * 8)[0] for i in range(count)] + [len(raw)]
    require(all(end <= x < y <= len(raw) for x, y in zip(offsets, offsets[1:])),
            'Invalid Palm record offsets')
    records = [raw[x:y] for x, y in zip(offsets, offsets[1:])]
    require(all(r.endswith(b'\0') and b'\0' not in r[:-1] for r in records),
            'Malformed Palm text records')
    return [r[:-1].decode('ascii') for r in records]


def check_records(lines, token, *, resumed=False):
    require(len(lines) == len(CASES) + 2, 'Missing, duplicate or extra results')
    require(lines[0] == f'begin token={token} version=1', 'Stale or incompatible probe log')
    require(lines[-1] == f'complete token={token} failures=0', 'Incomplete or failed probe invocation')
    results = []
    for label, line in zip(CASES, lines[1:-1]):
        match = re.fullmatch(r'([a-z-]+) err=([0-9a-fA-F]{4}) value=([0-9]+)', line)
        require(match is not None and match[1] == label, f'Missing or reordered result: {label}')
        require(int(match[2], 16) == 0, f'Guest probe failed: {label}')
        value = int(match[3])
        expected = {'memory': 256, 'database': 2 if resumed else 1,
                    'preferences': 2 if resumed else 1, 'drawing': 3,
                    'native-arm': checksum(), 'native-thumb': checksum(), 'event-queue': 1}
        require(value >= 2 if label == 'ticks' else value == expected[label],
                f'Unexpected observable value for {label}: {value}')
        results.append({'case': label, 'status': 'pass', 'value': value})
    return results
