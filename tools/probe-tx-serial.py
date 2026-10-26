#!/usr/bin/env python3
"""Observe TX Palm Serial Manager support in private session copies; never claim HotSync."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import signal
import struct
import sys
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('frond_run', ROOT / 'tools/frond-run.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
APP, LOG = 'Frond serial probe', 'FrondSerialLog'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def config_database(token):
    require(re.fullmatch('[0-9a-f]{32}', token), 'Invalid run token')
    raw = bytearray(88)
    raw[:14] = b'FrondSerialCfg'
    raw[60:68] = b'DATAFrSr'
    struct.pack_into('>H', raw, 76, 1)
    struct.pack_into('>I', raw, 78, 88)
    raw[85] = 1
    return raw + token.encode() + b'\0'


def read_log(path):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        require(len(names) == 1 and names[0].rsplit('/', 1)[-1] == LOG + '.pdb',
                'Expected exactly one serial log database')
        require(archive.getinfo(names[0]).file_size <= 16384, 'Oversized serial log')
        raw = archive.read(names[0])
    require(len(raw) >= 78, 'Truncated Palm database')
    require(raw[:32].split(b'\0', 1)[0] == LOG.encode() and raw[60:68] == b'LOGSFrSr',
            'Wrong serial log identity')
    require(not struct.unpack_from('>H', raw, 32)[0] & 1, 'Expected record database')
    count = struct.unpack_from('>H', raw, 76)[0]
    end = 78 + count * 8
    require(3 <= count <= 48 and end <= len(raw), 'Invalid serial record count')
    offsets = [struct.unpack_from('>I', raw, 78 + i * 8)[0] for i in range(count)] + [len(raw)]
    require(all(end <= a < b <= len(raw) for a, b in zip(offsets, offsets[1:])),
            'Invalid serial record offsets')
    records = [raw[a:b] for a, b in zip(offsets, offsets[1:])]
    require(all(r.endswith(b'\0') and b'\0' not in r[:-1] for r in records), 'Invalid text record')
    return [r[:-1].decode('ascii') for r in records]


def check_records(lines, token):
    require(re.fullmatch('[0-9a-f]{32}', token), 'Invalid run token')
    require(len(lines) >= 3 and lines[0] == f'begin token={token} version=2', 'Stale serial log')
    require(lines[-1] == f'complete token={token} failures=0', 'Incomplete or failed serial invocation')
    observations, at = {}, 1

    def value(label):
        nonlocal at
        require(at < len(lines) - 1, 'Truncated serial observations')
        match = re.fullmatch(re.escape(label) + r' err=([0-9a-fA-F]{4}) value=([0-9]+)', lines[at])
        require(match is not None, 'Missing or reordered serial observation: ' + label)
        at += 1
        error, number = int(match[1], 16), int(match[2])
        require(number <= 0xffffffff, 'Value outside Palm UInt32 range')
        observations[label] = {'error': error, 'value': number}
        return error, number

    error, _ = value('manager')
    if not error:
        count_error, count = value('count')

        def info(label, port):
            error, _ = value(label)
            if not error:
                require(at < len(lines) - 1, 'Truncated device metadata')
                match = re.fullmatch(r'info port=([0-9]+) flags=([0-9]+) maxbaud=([0-9]+)', lines[at])
                require(match is not None and int(match[1]) == port, 'Invalid device metadata')
                require(all(int(x) <= 0xffffffff for x in match.groups()), 'Metadata overflow')
                observations[label].update(flags=int(match[2]), maxbaud=int(match[3]))
                return True
            return False

        if not count_error:
            for index in range(min(count, 16)):
                if info(f'device-{index}', index):
                    at += 1
        error, port = value('rs232-open')
        require(port <= 0xffff, 'Invalid open port ID')
        if not error:
            if info('rs232-info', port):
                at += 1
            error, count = value('rs232-send')
            require(count <= 8, 'Impossible serial send count')
            value('rs232-pending'); value('rs232-flush'); value('rs232-close')
    require(at == len(lines) - 1, 'Duplicate or extra serial observations')
    return observations


def check_trace(text):
    packet = [0x46, 0x72, 0x53, 0x72, 0, 0x7f, 0x80, 0xff]
    actual = [line for line in text.splitlines() if line.startswith('SERIAL_TX')]
    expected = [f'SERIAL_TX uart=40100000 char={byte:03x}' for byte in packet]
    require(actual == expected, 'FFUART trace does not contain exactly the eight-byte probe packet')
    return {'uart': '40100000', 'bytes': packet}


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--session', type=Path, required=True, help='user-supplied ready TX session')
    p.add_argument('--core', type=Path, default=ROOT / 'out/frond')
    p.add_argument('--probe', type=Path, default=ROOT / 'build/serial-probe/FrondSerialProbe.prc')
    p.add_argument('--output', type=Path, required=True, help='new private result directory')
    p.add_argument('--require-ffuart-trace', action='store_true',
                   help='require exact eight-byte FFUART TX in each phase (optional observer build)')
    p.add_argument('--timeout', type=runner.positive, default=180, help='wall seconds per phase including slot wait')
    return p


def run(args):
    report = {'version': 1, 'status': 'error', 'runs': [], 'error': None,
              'serial_delivery_validated': False, 'hotsync_validated': False,
              'scope': 'Serial Manager discovery and bounded RS232 send attempt; no host endpoint'}
    root = None
    try:
        require(not args.output.exists(), 'Use a new output directory')
        args.output.mkdir(parents=True, mode=0o700); root = args.output.resolve()
        session = args.session.resolve()
        report['input_sha256'] = runner.sha256(session)
        report['core_sha256'] = runner.sha256(args.core)
        report['probe_sha256'] = runner.sha256(args.probe)
        for phase in ('initial', 'restored'):
            token = uuid.uuid4().hex
            config = root / (phase + '-config.pdb')
            config.write_bytes(config_database(token)); config.chmod(0o600)
            argv = ['--core', str(args.core.resolve()), '--session', str(session),
                    '--output', str(root / phase), '--app', APP, '--install', str(config),
                    '--export-db', LOG, '--headless', '--run-for', '2', '--settle', '2',
                    '--timeout', str(args.timeout), '--startup-timeout', str(args.timeout),
                    '--ready-timeout', '45']
            if phase == 'initial':
                argv += ['--install', str(args.probe.resolve())]
            result = runner.run(runner.parser().parse_args(argv))
            report['runs'].append({'phase': phase, 'token': token, 'result': str(root / phase / 'result.json')})
            require(result['status'] == 'ok', f'{phase}: {result.get("stage")}: {result.get("error")}')
            archive = Path(result['artifacts']['exports'][0]['path'])
            report['runs'][-1]['observations'] = check_records(read_log(archive), token)
            if args.require_ffuart_trace:
                log = (root / phase / 'core.log').read_text(errors='replace')
                report['runs'][-1]['uart_tx_trace'] = check_trace(log)
            session = Path(result['artifacts']['session'])
        require(runner.sha256(args.session) == report['input_sha256'], 'Input session changed')
        report['status'] = 'observed'
    except KeyboardInterrupt:
        report['error'] = 'Interrupted'
    except (OSError, ValueError, IndexError, UnicodeError, zipfile.BadZipFile) as error:
        report['error'] = str(error)
    if root:
        (root / 'serial.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    result = run(parser().parse_args())
    print(json.dumps(result, indent=2))
    return 0 if result['status'] == 'observed' else 1


if __name__ == '__main__':
    sys.exit(main())
