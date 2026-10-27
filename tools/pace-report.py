#!/usr/bin/env python3
"""Combine PACE context with explicit program identity and byte-bound symbols."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import re
import subprocess
import tempfile


def program_identity(path):
    # Never deserialize guest pointers, execute Palm calls, or infer a load base.
    # Parse and hash one bounded immutable snapshot: concurrent PRC rebuilds must
    # not combine an old directory with a new file hash.
    with path.open('rb') as source:
        data = source.read(64 * 1024 * 1024 + 1)
    size = len(data)
    if size > 64 * 1024 * 1024:
        raise ValueError('program exceeds 64 MiB')
    header = data[:78]
    if len(header) != 78 or b'\0' not in header[:32]:
        raise ValueError('not a Palm database header with a terminated name')
    attributes, version = struct.unpack_from('>HH', header, 32)
    count = struct.unpack_from('>H', header, 76)[0]
    resource_db = bool(attributes & 1)
    entry_size = 10 if resource_db else 8
    table = data[78:78 + count * entry_size]
    if len(table) != count * entry_size or struct.unpack_from('>I', header, 72)[0]:
        raise ValueError('truncated or chained Palm database directory')
    digest = hashlib.sha256(data)
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
    with path.open('rb') as source:
        data = source.read(16385)
    if len(data) > 16384:
        raise ValueError('context exceeds 16 KiB; supply one monitor JSON object')
    return validate_context(json.loads(data))


def validate_context(value):
    if (not isinstance(value, dict) or type(value.get('version')) is not int or
            value['version'] != 1 or type(value.get('available')) is not bool):
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


def resource_bytes(path, identity):
    """Read bounded resource payloads; file offsets never imply guest addresses."""
    if identity['size'] > 64 * 1024 * 1024:
        raise ValueError('program exceeds 64 MiB symbol attribution limit')
    with path.open('rb') as source:
        data = source.read(64 * 1024 * 1024 + 1)
    if len(data) != identity['size'] or hashlib.sha256(data).hexdigest() != identity['sha256']:
        raise ValueError('program changed while building report')
    offsets = sorted(set(r['file_offset'] for r in identity['resources']) | {len(data)})
    result = {}
    for resource in identity['resources']:
        key = (resource['type_hex'], resource['id'])
        if key in result:
            raise ValueError('duplicate Palm resource identity')
        start = resource['file_offset']
        end = next((o for o in offsets if o > start), len(data))
        result[key] = data[start:end]
    return result


def executable_symbols(executable, program, identity, prefix):
    # A concurrent build must not mix verified text with symbols from another file.
    with tempfile.TemporaryDirectory(prefix='frond-executable-') as tmp:
        snapshot = Path(tmp) / 'executable'
        digest, size = hashlib.sha256(), 0
        with executable.open('rb') as source, snapshot.open('wb') as output:
            while chunk := source.read(1024 * 1024):
                size += len(chunk)
                if size > 256 * 1024 * 1024:
                    raise ValueError('executable exceeds 256 MiB')
                output.write(chunk)
                digest.update(chunk)
        result = snapshot_symbols(snapshot, program, identity, prefix)
        result['executable_sha256'] = digest.hexdigest()
        return result


def snapshot_symbols(executable, program, identity, prefix):
    """Bind compiler .text to one unique PRC payload by exact bytes."""
    description = subprocess.check_output([prefix + 'objdump', '-f', str(executable)], text=True)
    if not re.search(r'architecture:\s*m68k(?:\b|:)', description):
        raise ValueError('PACE symbols require a 68k executable')
    sections = subprocess.check_output([prefix + 'objdump', '-h', str(executable)], text=True)
    matches = re.findall(r'^\s*\d+\s+\.text\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s', sections, re.M)
    if len(matches) != 1:
        raise ValueError('executable must have exactly one .text section')
    size, base = (int(v, 16) for v in matches[0])
    if not 0 < size <= 64 * 1024 * 1024:
        raise ValueError('invalid executable .text size')
    with tempfile.TemporaryDirectory(prefix='frond-symbols-') as tmp:
        output = Path(tmp) / 'text.bin'
        subprocess.run([prefix + 'objcopy', '-O', 'binary', '-j', '.text', str(executable), str(output)], check=True)
        text = output.read_bytes()
    if len(text) != size:
        raise ValueError('extracted .text size mismatch')
    locations = []
    for (kind, ident), payload in resource_bytes(program, identity).items():
        if kind != '636f6465':  # PACE executes 68k code, not native armc.
            continue
        start = payload.find(text)
        if start >= 0:
            locations.append((kind, ident, start))
            if payload.find(text, start + 1) >= 0:
                raise ValueError('ambiguous executable bytes within code resource')
    if len(locations) != 1:
        raise ValueError('executable .text must match exactly one PRC code resource')
    kind, ident, offset = locations[0]
    listing = subprocess.check_output([prefix + 'nm', '-n', '--defined-only', str(executable)], text=True)
    symbols = []
    for line in listing.splitlines():
        match = re.fullmatch(r'([0-9a-fA-F]+)\s+[Tt]\s+(.+)', line)
        if not match:
            continue
        address, name = int(match[1], 16), match[2]
        if name.startswith('.') or name in ('gcc2_compiled.', '__gnu_compiled_c'):
            continue
        if base <= address < base + size:
            symbols.append({'name': name, 'resource_offset': offset + address - base})
    return {'source': 'exact-executable-text-match', 'program_sha256': identity['sha256'],
            'text_sha256': hashlib.sha256(text).hexdigest(), 'type_hex': kind, 'id': ident,
            'text_resource_offset': offset, 'text_size': size, 'symbols': symbols}


def resolve_pc(context, mapping, identity, payloads, symbols):
    """Validate a separately captured live map; never treat it as active-app proof."""
    validate_context(context)
    if not isinstance(mapping, dict):
        raise ValueError('invalid runtime map')
    validate_context(mapping.get('context'))
    if (type(mapping.get('version')) is not int or mapping['version'] != 1 or
            type(mapping.get('context_pc')) is not int or
            mapping.get('program_sha256') != identity['sha256'] or
            mapping.get('context') != context or mapping.get('context_pc') != context.get('pc') or
            not context['available']):
        raise ValueError('runtime map does not match this program and exact PACE context')
    resources = mapping.get('resources')
    if not isinstance(resources, list) or len(resources) > 65535:
        raise ValueError('invalid runtime resource map')
    intervals = []
    for resource in resources:
        if not isinstance(resource, dict):
            raise ValueError('invalid runtime resource entry')
        kind, ident = resource.get('type_hex'), resource.get('id')
        if not isinstance(kind, str) or not re.fullmatch('[0-9a-f]{8}', kind) or type(ident) is not int or not 0 <= ident <= 65535:
            raise ValueError('invalid runtime resource identity')
        address, size = resource.get('address'), resource.get('size')
        payload = payloads.get((kind, ident))
        if (type(address) is not int or type(size) is not int or not 0 <= address < 2**32 or
                not 0 < size <= 2**32 - address or payload is None or len(payload) != size or
                hashlib.sha256(payload).hexdigest() != resource.get('sha256')):
            raise ValueError('runtime resource range or live hash does not match PRC bytes')
        if any(address < end and start < address + size for start, end in intervals):
            raise ValueError('overlapping runtime resource ranges')
        intervals.append((address, address + size))
    found = [r for r in resources if r['address'] <= context['pc'] < r['address'] + r['size']]
    if not found:
        return {'available': False, 'reason': 'pc-outside-captured-resources'}
    resource = found[0]
    offset = context['pc'] - resource['address']
    result = {'available': True, 'source': 'user-supplied-runtime-map',
              'capture_source': mapping.get('source'), 'active_app_verified': False,
              'type_hex': resource['type_hex'], 'id': resource['id'], 'resource_offset': offset,
              'nearest_text_symbol': None}
    if (symbols and (symbols['type_hex'], symbols['id']) == (resource['type_hex'], resource['id']) and
            symbols['text_resource_offset'] <= offset < symbols['text_resource_offset'] + symbols['text_size']):
        preceding = [s for s in symbols['symbols'] if s['resource_offset'] <= offset]
        if preceding:
            symbol = max(preceding, key=lambda s: s['resource_offset'])
            result['nearest_text_symbol'] = dict(symbol, displacement=offset - symbol['resource_offset'])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--context', type=Path, required=True,
                        help='one JSON object from monitor pace or monitor pace-last')
    parser.add_argument('--program', type=Path,
                        help='optional local PRC/PDB; identity is NOT verified against the running guest')
    parser.add_argument('--executable', type=Path, help='compiler executable; .text must uniquely match PRC code bytes')
    parser.add_argument('--tool-prefix', default='m68k-palmos-', help='objdump/objcopy/nm prefix for executable')
    parser.add_argument('--runtime-map', type=Path, help='resource ranges and live SHA-256 hashes captured at this exact context')
    args = parser.parse_args()
    try:
        result = {'version': 1, 'context': read_context(args.context),
                  'declared_program': program_identity(args.program) if args.program else None}
        if (args.executable or args.runtime_map) and not args.program:
            raise ValueError('--executable and --runtime-map require --program')
        if args.executable:
            result['symbols'] = executable_symbols(args.executable, args.program, result['declared_program'], args.tool_prefix)
        if args.runtime_map:
            with args.runtime_map.open('rb') as source:
                mapping = source.read(1024 * 1024 + 1)
            if len(mapping) > 1024 * 1024:
                raise ValueError('runtime map exceeds 1 MiB')
            result['code_location'] = resolve_pc(result['context'], json.loads(mapping),
                result['declared_program'], resource_bytes(args.program, result['declared_program']), result.get('symbols'))
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        parser.exit(1, f'pace-report: {exc}\n')
    print(json.dumps(result, indent=2, ensure_ascii=True))


if __name__ == '__main__':
    main()
