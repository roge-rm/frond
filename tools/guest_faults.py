"""Exact, private expectations for observed guest permission-fault diagnostics."""
import json
import re
from collections import Counter
from typing import NamedTuple


PHASES = {'boot', 'setup', 'reload', 'rollback', 'reset-soft', 'reset-hard'}
FAULT = re.compile(r'([RW]) of (\d+) bytes to (0x[0-9a-fA-F]+) failed '
                   r'\(pc=(0x[0-9a-fA-F]+), fsr=(\d+)\)!')
SHA256 = re.compile(r'[0-9a-f]{64}')
ADDRESS = re.compile(r'0x[0-9a-fA-F]{1,8}')


class Signature(NamedTuple):
    access: str
    size: int
    va: int
    pc: int
    fsr: int


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON key: ' + key)
        result[key] = value
    return result


def integer(value, low, high, name):
    if type(value) is not int or not low <= value <= high:
        raise ValueError('invalid ' + name)
    return value


def address(value, name):
    if not isinstance(value, str) or not ADDRESS.fullmatch(value):
        raise ValueError('invalid ' + name)
    return int(value, 16)


def parse_contract(payload, *, device, actual_sha256, pinned_sha256, launcher, resets):
    if not launcher or not resets or not pinned_sha256:
        raise ValueError('--expected-faults requires --launcher, --resets and --sha256')
    if not SHA256.fullmatch(pinned_sha256) or actual_sha256 != pinned_sha256:
        raise ValueError('expected-faults requires the pinned original NOR SHA-256')
    try:
        data = json.loads(payload, object_pairs_hook=unique_object)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError('cannot read expected-faults JSON: ' + str(error)) from error
    if not isinstance(data, dict) or set(data) != {'version', 'device', 'nor_sha256', 'phases'}:
        raise ValueError('invalid expected-faults fields')
    if type(data['version']) is not int or data['version'] != 1:
        raise ValueError('unsupported expected-faults version')
    if data['device'] != device or data['nor_sha256'] != actual_sha256:
        raise ValueError('expected-faults device or NOR identity does not match')
    phases = data['phases']
    if not isinstance(phases, dict) or set(phases) - PHASES or set(phases) != PHASES:
        raise ValueError('missing or unknown expected-faults phase')
    result = {}
    for phase, entries in phases.items():
        if not isinstance(entries, list):
            raise ValueError('expected-faults phase must contain a list')
        result[phase] = Counter()
        for entry in entries:
            if not isinstance(entry, dict) or set(entry) != {'access', 'size', 'va', 'pc', 'fsr', 'count'}:
                raise ValueError('invalid expected-faults signature fields')
            if entry['access'] not in ('read', 'write'):
                raise ValueError('invalid fault access')
            size = integer(entry['size'], 1, 64, 'fault size')
            if size not in (1, 2, 4, 8, 16, 32, 64):
                raise ValueError('invalid fault size')
            signature = Signature(entry['access'], size, address(entry['va'], 'fault VA'),
                                  address(entry['pc'], 'fault PC'), integer(entry['fsr'], 0, 255, 'fault FSR'))
            if signature in result[phase]:
                raise ValueError('duplicate expected-faults signature')
            result[phase][signature] = integer(entry['count'], 1, 1000000, 'fault count')
    return result


def observed_faults(text):
    result = Counter()
    for line in text.splitlines():
        if 'failed (pc=' not in line and not re.search(r'\b(?:[RW] of .*failed|memory fault)\b', line, re.I):
            continue
        match = FAULT.fullmatch(line.lstrip('> '))
        if not match:
            raise ValueError('unrecognised guest fault diagnostic: ' + line)
        access, size, va, pc, fsr = match.groups()
        result[Signature('read' if access == 'R' else 'write', int(size), int(va, 16),
                         int(pc, 16), int(fsr))] += 1
    return result


def describe(faults):
    return [dict(access=s.access, size=s.size, va=f'0x{s.va:08x}', pc=f'0x{s.pc:08x}',
                 fsr=s.fsr, count=count) for s, count in sorted(faults.items())]


def check_diagnostics(text, phase, contract=None):
    for failure in ('Invalid instr', 'framebuffer not in RAM'):
        if failure in text:
            raise ValueError(f'{phase}: {failure}')
    observed = observed_faults(text)
    expected = Counter() if contract is None else contract.get(phase)
    if expected is None:
        raise ValueError('missing expected-faults phase: ' + phase)
    if observed != expected:
        raise ValueError(f'{phase}: guest faults differ; expected {describe(expected)}, observed {describe(observed)}')
    return observed
