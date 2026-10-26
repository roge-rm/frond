#!/usr/bin/env python3
"""Combine PACE monitor JSON with explicit, unverified local Palm program identity."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def program_identity(path):
    # Never deserialize guest pointers, execute Palm calls, or infer a load base.
    # Bound header/table reads; hash the file in chunks instead of loading it all.
    with path.open('rb') as source:
        header = source.read(78)
        if len(header) != 78 or b'\0' not in header[:32]:
            raise ValueError('not a Palm database header with a terminated name')
        attributes, version = struct.unpack_from('>HH', header, 32)
        count = struct.unpack_from('>H', header, 76)[0]
        resource_db = bool(attributes & 1)
        entry_size = 10 if resource_db else 8
        table = source.read(count * entry_size)
        if len(table) != count * entry_size or struct.unpack_from('>I', header, 72)[0]:
            raise ValueError('truncated or chained Palm database directory')
        source.seek(0)
        digest = hashlib.sha256()
        size = 0
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
            size += len(chunk)
    resources = []
    for i in range(count):
        entry = table[i * entry_size:(i + 1) * entry_size]
        offset = struct.unpack_from('>I', entry, 6 if resource_db else 0)[0]
        if not 78 + len(table) <= offset <= size:
            raise ValueError('Palm database entry offset is outside its data area')
        if resource_db:
            resources.append({'type_hex': entry[:4].hex(), 'id': int.from_bytes(entry[4:6], 'big'),
                              'file_offset': offset})
    return {'source': 'user-supplied-file', 'active_app_verified': False,
            'sha256': digest.hexdigest(), 'size': size,
            'database_name': header[:32].split(b'\0', 1)[0].decode('latin-1'),
            'database_name_hex': header[:32].split(b'\0', 1)[0].hex(),
            'type_hex': header[60:64].hex(), 'creator_hex': header[64:68].hex(),
            'attributes': attributes, 'version': version, 'resource_database': resource_db,
            'entry_count': count, 'resources': resources,
            'guest_load_address': None}


def read_context(path):
    if path.stat().st_size > 16384:
        raise ValueError('context exceeds 16 KiB; supply one monitor JSON object')
    value = json.loads(path.read_text())
    if not isinstance(value, dict) or value.get('version') != 1 or type(value.get('available')) is not bool:
        raise ValueError('expected a version 1 PACE monitor object')
    if value['available']:
        for name in ('pc', 'state_pointer', 'sr'):
            if type(value.get(name)) is not int or not 0 <= value[name] <= (0xffff if name == 'sr' else 0xffffffff):
                raise ValueError('invalid register in PACE monitor object')
        for name in ('a', 'd'):
            registers = value.get(name)
            if not isinstance(registers, list) or len(registers) != 8 or any(
                    type(r) is not int or not 0 <= r <= 0xffffffff for r in registers):
                raise ValueError('invalid register bank in PACE monitor object')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--context', type=Path, required=True,
                        help='one JSON object from monitor pace or monitor pace-last')
    parser.add_argument('--program', type=Path,
                        help='optional local PRC/PDB; identity is NOT verified against the running guest')
    args = parser.parse_args()
    try:
        result = {'version': 1, 'context': read_context(args.context),
                  'declared_program': program_identity(args.program) if args.program else None}
    except (OSError, ValueError) as exc:
        parser.exit(1, f'pace-report: {exc}\n')
    print(json.dumps(result, indent=2, ensure_ascii=True))


if __name__ == '__main__':
    main()
