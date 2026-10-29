#!/usr/bin/env python3
"""Validate original Bluetooth Exchange Manager results and exact probe payload bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile

PAYLOAD_SHA256 = 'f3d193f0b904a2bbaaa972f2ef80bdeb19266e8340dbcb3626c26f2f98c4327c'
LOG_NAME = 'FrondBTExgLog'
PAYLOAD_NAME = 'FrondBTReceived'


def database_records(raw, name):
    if len(raw) < 78 or raw[:32].rstrip(b'\0') != name.encode('ascii'):
        raise ValueError('wrong Palm database identity')
    if struct.unpack_from('>H', raw, 32)[0] & 1 or raw[60:68] != b'DATAFrBO':
        raise ValueError('expected a FrBO record database')
    count = struct.unpack_from('>H', raw, 76)[0]
    end = 78 + count * 8
    if end > len(raw):
        raise ValueError('truncated Palm record table')
    offsets = [struct.unpack_from('>I', raw, 78 + i * 8)[0] for i in range(count)]
    if offsets != sorted(set(offsets)) or any(n < end or n >= len(raw) for n in offsets):
        raise ValueError('invalid Palm record offsets')
    return [raw[a:b] for a, b in zip(offsets, offsets[1:] + [len(raw)])]


def read_records(path, name):
    with zipfile.ZipFile(path) as archive:
        matches = []
        for entry in archive.infolist():
            if entry.is_dir() or entry.file_size < 78:
                continue
            with archive.open(entry) as source:
                if source.read(32).rstrip(b'\0') == name.encode('ascii'):
                    matches.append(entry)
        if len(matches) != 1 or matches[0].file_size > 1024 * 1024:
            raise ValueError('need exactly one bounded ' + name + ' database')
        return database_records(archive.read(matches[0]), name)


def log_records(path):
    result = []
    for record in read_records(path, LOG_NAME):
        if not record.endswith(b'\0') or b'\0' in record[:-1]:
            raise ValueError('invalid probe log string')
        result.append(record[:-1].decode('ascii'))
    return result


def check(role, log, payload, before=None):
    rows = log_records(log)
    start = 0
    if before:
        prior = log_records(before)
        if len(rows) <= len(prior) or rows[:len(prior)] != prior:
            raise ValueError('probe log did not append a fresh unchanged-history attempt')
        start = len(prior)
    begin = role + '-begin'
    attempts = [i for i in range(start, len(rows)) if rows[i] == begin]
    if not attempts:
        raise ValueError('no fresh ' + role + ' attempt')
    index = attempts[-1]
    registrations = [row for row in rows[:index] if row.startswith('register ')]
    if not registrations or registrations[-1] != 'register err=0000 bytes=0':
        raise ValueError('probe datatype registration failed')
    if role == 'send':
        expected = ['send-begin', 'put err=0000 bytes=0', 'send err=0000 bytes=4096',
                    'disconnect err=0000 bytes=4096', 'send-complete']
        prefixes = ('send-', 'put ', 'send ', 'disconnect ')
    else:
        expected = ['receive-begin', 'accept err=0000 bytes=0', 'receive err=0000 bytes=4096',
                    'receive-disconnect err=0000 bytes=4096', 'receive-complete']
        prefixes = ('receive-', 'accept ', 'receive ')
    actual = [row for row in rows[index:] if row.startswith(prefixes)]
    if actual != expected:
        raise ValueError('latest original ' + role + ' APIs did not complete successfully')
    if zipfile.is_zipfile(payload):
        records = read_records(payload, PAYLOAD_NAME)
        if len(records) != 1:
            raise ValueError('expected exactly one received payload record')
        data = records[0]
    else:
        data = Path(payload).read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != 4096 or digest != PAYLOAD_SHA256:
        raise ValueError('payload differs in length or bytes')
    return {'status': 'pass', 'role': role, 'scope': 'original Exchange Manager completion and exact payload',
            'bytes': len(data), 'payload_sha256': digest}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role', choices=['send', 'receive'], required=True)
    parser.add_argument('--log', type=Path, required=True, help='original exported FrondBTExgLog ZIP')
    parser.add_argument('--payload', type=Path, required=True, help='received FrondBTReceived ZIP or independent owned peer binary')
    parser.add_argument('--before', type=Path, help='prior log ZIP; requires unchanged history and a fresh attempt')
    args = parser.parse_args()
    try:
        result = check(args.role, args.log, args.payload, args.before)
    except (ValueError, KeyError, OSError, UnicodeError, struct.error, zipfile.BadZipFile) as error:
        parser.exit(1, str(error) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
