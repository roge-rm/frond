"""Validate a fresh, completed Palm SD probe invocation."""
import re
import struct
import zipfile


class ControlError(AssertionError):
    def __init__(self, code, message):
        super().__init__(f'{code}: {message}')
        self.code = code


def export_if_ready(request, path):
    try:
        request('export', path)
    except ControlError as error:
        if error.code != 'not-ready':
            raise
        return False
    return True


def read_log(path):
    with zipfile.ZipFile(path) as archive:
        names = [n for n in archive.namelist() if n.rsplit('/', 1)[-1].startswith('FrondSDLog.')]
        assert len(names) == 1, 'Expected exactly one FrondSDLog database'
        raw = archive.read(names[0])
    assert len(raw) >= 78, 'Truncated Palm log header'
    count = struct.unpack_from('>H', raw, 76)[0]
    assert 78 + count * 8 <= len(raw), 'Truncated Palm record list'
    offsets = [struct.unpack_from('>I', raw, 78 + i * 8)[0] for i in range(count)] + [len(raw)]
    assert all(78 + count * 8 <= x <= y <= len(raw) for x, y in zip(offsets, offsets[1:])), 'Invalid Palm record offsets'
    return [raw[x:y].rstrip(b'\0').decode() for x, y in zip(offsets, offsets[1:])]


def check_records(lines, size, *, previous=(), prior=False, host=True):
    assert lines[:len(previous)] == list(previous), 'Previous probe records were lost or changed'
    starts = [i for i, line in enumerate(lines) if line.startswith('begin ')]
    assert starts and starts[-1] >= len(previous), 'No fresh SD probe invocation; old results are insufficient'
    invocation = lines[starts[-1]:]
    entries = {}
    for line in invocation:
        match = re.fullmatch(r'([a-z-]+) err=([0-9A-Fa-f]{4}) value=([0-9]+)', line)
        assert match, f'Invalid probe record: {line}'
        label, error, value = match.groups()
        assert label not in entries, f'Duplicate probe result: {label}'
        entries[label] = int(error, 16), int(value)
    assert invocation[-1].startswith('complete '), 'Latest SD probe invocation is incomplete'
    assert entries['begin'][0] == 0 and entries['complete'] == entries['begin'], 'Probe did not complete successfully'

    def expect(label, value=0, error=0):
        assert entries.get(label) == (error, value), f'{label}: expected {(error, value)}, got {entries.get(label)}'

    assert entries.get('enumerate', (1, 0))[0] == 0, 'Volume enumeration failed'
    error, total = entries.get('volume-size', (1, 0))
    assert error == 0 and (size << 20) * .95 <= total <= (size << 20), f'Wrong mounted capacity: {total} bytes, expected {size} MiB'
    if host:
        expect('host-open'); expect('host-read', 16); expect('host-match'); expect('host-close')
    else:
        expect('host-open', error=0x2a08)
    expect('prior-open', error=0 if prior else 0x2a08)
    if prior:
        expect('prior-read', 19); expect('prior-match'); expect('prior-close')
    expect('guest-open'); expect('guest-write', 19); expect('guest-close')
    expect('guest-reopen'); expect('guest-read', 19); expect('guest-match'); expect('guest-read-close')
    return lines
