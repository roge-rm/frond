"""Validate a fresh, completed Palm SD probe invocation."""
import re
from pathlib import Path
import json
import math
import shutil
import struct
import time
import zipfile


class ControlError(AssertionError):
    def __init__(self, code, message):
        super().__init__(f'{code}: {message}')
        self.code = code


class ProbeTimeout(TimeoutError):
    """The readiness deadline expired without corrupting the socket protocol."""


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


class ResetMountReadiness:
    """Accept only recorded enumeration-empty runs before a fresh full success."""
    def __init__(self, previous, size=32, *, prior=True, host=True, allow_empty=True):
        self.previous = list(previous)
        self.size, self.prior, self.host = size, prior, host
        self.allow_empty = allow_empty
        self.observations = []
        self.incomplete = False

    def observe(self, lines):
        self.incomplete = False
        if len(lines) <= len(self.previous):
            assert lines == self.previous[:len(lines)], 'Previous probe records were lost or changed'
            return None
        assert lines[:len(self.previous)] == self.previous, 'Previous probe records were lost or changed'
        cursor, mounted = len(self.previous), False
        while cursor < len(lines):
            assert lines[cursor].startswith('begin '), 'Missing fresh probe begin marker'
            end = next((i for i in range(cursor + 1, len(lines)) if lines[i].startswith('begin ')), len(lines))
            invocation = lines[cursor:end]
            if not invocation[-1].startswith('complete '):
                assert end == len(lines), 'An incomplete probe was followed by another invocation'
                self.incomplete = True
                return None
            begin = re.fullmatch(r'begin err=0000 value=([0-9]+)', invocation[0])
            pending = (len(invocation) == 3 and begin is not None
                       and invocation[1].lower() == 'enumerate err=290d value=0'
                       and invocation[2].lower() == f'complete err=290d value={begin[1]}')
            assert not any(x.startswith('mount-timeout ') for x in invocation), 'Palm mount readiness exceeded its guest deadline'
            if not pending or not self.allow_empty:
                check_records(lines[:end], self.size, previous=self.previous, prior=self.prior, host=self.host)
            self.observations.append({'state': 'mount-pending' if pending else 'mounted', 'records': invocation})
            self.previous = lines[:end]
            cursor, mounted = end, not pending
        return self.previous if mounted else None


def wait_for_probe(request, path, *, previous=(), size=32, prior=False, host=True,
                   reset_mount=False, timeout=60, now=time.monotonic, sleep=time.sleep):
    """Bound readiness, preserving each reset observation; actual I/O errors fail."""
    assert math.isfinite(timeout) and timeout > 0, 'Readiness timeout must be finite and positive'
    path = Path(path)
    started = now()
    deadline = started + timeout
    monitor = ResetMountReadiness(previous, size, prior=prior, host=host) if reset_mount else None
    strict = ResetMountReadiness(previous, size, prior=prior, host=host, allow_empty=False)
    report = {'timeout_seconds': timeout, 'status': 'waiting', 'observations': []}
    report_path = path.with_name(path.stem + '-mount-readiness.json')
    restart_probe = False

    def remaining():
        left = deadline - now()
        if left <= 0:
            raise ProbeTimeout(f'No fresh, complete successful probe within {timeout:g} seconds')
        return left

    def call(command, argument='', ok=True):
        remaining()
        return request(command, argument, ok=ok, deadline=deadline)

    def write_report():
        if monitor:
            report['elapsed_seconds'] = now() - started
            report_path.write_text(json.dumps(report, indent=2) + '\n')

    try:
        while True:
            remaining()
            if restart_probe:
                try:
                    call('launch-app', 'Frond SD probe')
                    restart_probe = False
                except ControlError as error:
                    if error.code != 'not-ready':
                        raise
                    sleep(min(1, remaining()))
                    continue
            ready, _ = call('guest-ready', ok=None)
            if ready and export_if_ready(call, path):
                entry = None
                if monitor:
                    archive = path.with_name(f'{path.stem}-observation-{len(report["observations"]) + 1:03}.zip')
                    shutil.copyfile(path, archive)
                    entry = {'archive': str(archive), 'elapsed_seconds': now() - started, 'state': 'reading'}
                    report['observations'].append(entry)
                try:
                    lines = read_log(path)
                    remaining()  # A late response cannot establish success.
                    if monitor:
                        entry['record_count'] = len(lines)
                        entry['history_preserved'] = lines[:len(monitor.previous)] == monitor.previous
                        entry['records'] = lines[len(monitor.previous):] if lines[:len(monitor.previous)] == monitor.previous else lines
                        print(f'reset observation {entry["archive"]} elapsed={entry["elapsed_seconds"]:.3f}s\n' + '\n'.join(entry['records']), flush=True)
                        before = len(monitor.observations)
                        result = monitor.observe(lines)
                        new = monitor.observations[before:]
                        entry['invocations'] = new
                        entry['state'] = 'mounted' if result is not None else 'waiting-for-fresh-probe'
                        for observation in new:
                            print(observation['state'], flush=True)
                        if new and new[-1]['state'] == 'mount-pending':
                            entry['state'] = 'mount-pending'
                        # Palm can stop the probe to open the card launcher when
                        # the volume mounts. Relaunch only the owned probe;
                        # acknowledgement alone never establishes freshness.
                        if result is None and monitor.observations and monitor.observations[-1]['state'] == 'mount-pending':
                            restart_probe = not monitor.incomplete
                        write_report()
                        if result is not None:
                            remaining()
                            report['status'] = 'mounted'
                            write_report()
                            return result
                    else:
                        result = strict.observe(lines)
                        if result is not None:
                            print('\n'.join(lines[len(previous):]), flush=True)
                            return result
                except Exception as error:
                    if entry is not None:
                        entry.update(state='failed', error=str(error))
                    raise
            sleep(min(1, remaining()))
    except Exception as error:
        report.update(status='timed-out' if isinstance(error, TimeoutError) else 'failed', error=str(error))
        write_report()
        raise
