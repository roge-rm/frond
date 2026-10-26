#!/usr/bin/env python3
"""Freshness/negative regressions for serial discovery; no firmware required."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('serial_probe', ROOT / 'tools/probe-tx-serial.py')
probe = importlib.util.module_from_spec(spec); spec.loader.exec_module(probe)
TOKEN = 'a' * 32


def invocation(token=TOKEN, opened=False):
    lines = [f'begin token={token} version=2', 'manager err=0000 value=2', 'count err=0000 value=2']
    for label in ('device-0', 'device-1'):
        lines.append(label + ' err=0302 value=0')
    lines.append('rs232-open err=' + ('0000 value=2' if opened else '0302 value=0'))
    if opened:
        lines += ['rs232-info err=0302 value=0', 'rs232-send err=0000 value=8', 'rs232-pending err=0000 value=0',
                  'rs232-flush err=0000 value=0', 'rs232-close err=0000 value=0']
    return lines + [f'complete token={token} failures=0']


def archive(path, lines):
    raw = bytearray(80 + len(lines) * 8)
    raw[:len(probe.LOG)] = probe.LOG.encode(); raw[60:68] = b'LOGSFrSr'
    struct.pack_into('>H', raw, 76, len(lines))
    for i, line in enumerate(lines):
        struct.pack_into('>I', raw, 78 + i * 8, len(raw)); raw += line.encode() + b'\0'
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr(probe.LOG + '.pdb', raw)


class SerialProbe(unittest.TestCase):
    def test_observations_preserve_errors_and_do_not_prove_delivery(self):
        self.assertEqual(probe.check_records(invocation(), TOKEN)['rs232-open']['error'], 0x302)
        self.assertEqual(probe.check_records(invocation(opened=True), TOKEN)['rs232-send']['value'], 8)
        lines = invocation(); lines[3:4] = ['device-0 err=0000 value=1234',
                                         'info port=0 flags=65 maxbaud=115200']
        self.assertEqual(probe.check_records(lines, TOKEN)['device-0']['flags'], 65)

    def test_open_handle_metadata_and_enumeration_limit(self):
        lines = invocation(opened=True)
        at = lines.index('rs232-info err=0302 value=0')
        lines[at:at+1] = ['rs232-info err=0000 value=1234',
                           'info port=2 flags=3 maxbaud=115200']
        self.assertEqual(probe.check_records(lines, TOKEN)['rs232-info']['maxbaud'], 115200)
        lines[at+1] = 'info port=3 flags=3 maxbaud=115200'
        with self.assertRaises(ValueError): probe.check_records(lines, TOKEN)
        lines = invocation(); lines[2] = 'count err=0000 value=17'
        lines[3:5] = [f'device-{i} err=0302 value=0' for i in range(16)]
        self.assertEqual(probe.check_records(lines, TOKEN)['count']['value'], 17)

    def test_trace_requires_exact_packet_and_uart(self):
        lines = [f'SERIAL_TX uart=40100000 char={b:03x}' for b in (0x46,0x72,0x53,0x72,0,0x7f,0x80,0xff)]
        self.assertEqual(probe.check_trace('\n'.join(lines))['bytes'][-1], 0xff)
        for bad in ([], lines[:1] + lines[4:5], lines + lines, list(reversed(lines)),
                    [x.replace('40100000','40700000') for x in lines]):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                probe.check_trace('\n'.join(bad))

    def test_baud_trace_rejects_early_drift_missing_and_wrong_config(self):
        good = ['UART_TX_START uart=40100000 ticks=0 divisor=96 lcr=03']
        good += [f'UART_TX_CLOCK uart=40100000 ticks={tick} divisor=96 lcr=03'
                 for tick in (107,214,320,427,534,640,747,854)]
        self.assertEqual(len(probe.check_baud_trace('\n'.join(good))), 8)
        palm = ['UART_TX_START uart=40100000 ticks=0 divisor=96 lcr=07']
        palm += [f'UART_TX_CLOCK uart=40100000 ticks={tick} divisor=96 lcr=07'
                 for tick in (118,235,352,470,587,704,822,939)]
        self.assertEqual(len(probe.check_baud_trace('\n'.join(palm))), 8)
        broken = [good[1:], good[:-1], [s.replace('divisor=96', 'divisor=0') for s in good],
                  [s.replace('40100000','40700000') for s in good],
                  [s.replace('ticks=107 ', 'ticks=106 ') for s in good],
                  [s.replace('ticks=854 ', 'ticks=856 ') for s in good]]
        for bad in broken:
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                probe.check_baud_trace('\n'.join(bad))

    def test_stale_incomplete_duplicate_out_of_order_rejected(self):
        good = invocation(opened=True)
        cases = [invocation('b'*32), good[:-1], good + good, good[:3] + [good[2]] + good[3:],
                 list(reversed(good)), good[:-1] + [good[-1].replace('failures=0', 'failures=1')],
                 [x.replace('rs232-send err=0000 value=8', 'rs232-send err=0000 value=9') for x in good]]
        for bad in cases:
            with self.subTest(bad=bad), self.assertRaises((ValueError, IndexError)):
                probe.check_records(bad, TOKEN)

    def test_log_archive_and_invalid_identity(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'log.zip'; archive(path, invocation())
            self.assertEqual(probe.read_log(path), invocation())
            with zipfile.ZipFile(path, 'w') as z:
                z.writestr('other.pdb', b'bad')
            with self.assertRaises(ValueError): probe.read_log(path)

    def test_corrupt_record_boundaries_and_configuration_rejected(self):
        self.assertEqual(probe.config_database(TOKEN)[88:], TOKEN.encode() + b'\0')
        with self.assertRaises(ValueError): probe.config_database('stale')
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'log.zip'
            for corrupt in (lambda b: b.__setitem__(60, 0),
                            lambda b: struct.pack_into('>I', b, 78, 1),
                            lambda b: struct.pack_into('>I', b, 86, len(b) + 1),
                            lambda b: b.__setitem__(-1, 65),
                            lambda b: struct.pack_into('>H', b, 76, 65535)):
                archive(path, invocation())
                with zipfile.ZipFile(path) as z: raw = bytearray(z.read(probe.LOG + '.pdb'))
                corrupt(raw)
                with zipfile.ZipFile(path, 'w') as z: z.writestr(probe.LOG + '.pdb', raw)
                with self.assertRaises(ValueError): probe.read_log(path)

    def test_runner_restores_private_copy_and_reports_observation_only(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); session = root / 'source.session'; session.write_bytes(b'fixture')
            binary = root / 'binary'; binary.write_bytes(b'fixture')
            args = probe.parser().parse_args(['--session', str(session), '--core', str(binary),
                    '--probe', str(binary), '--output', str(root / 'results')])
            calls = []
            def run(a):
                calls.append(a); a.output.mkdir()
                token = a.install[0].read_bytes()[88:120].decode()
                exported = a.output / 'log.zip'; archive(exported, invocation(token, opened=True))
                copy = a.output / 'working.frond'; copy.write_bytes(b'private state')
                return {'status':'ok', 'artifacts':{'exports':[{'path':str(exported)}], 'session':str(copy)}}
            with patch.object(probe.runner, 'run', side_effect=run): result = probe.run(args)
            self.assertEqual(result['status'], 'observed', result)
            self.assertFalse(result['hotsync_validated']); self.assertFalse(result['serial_delivery_validated'])
            self.assertEqual(calls[1].session, calls[0].output / 'working.frond')
            self.assertNotEqual(result['runs'][0]['token'], result['runs'][1]['token'])
            self.assertEqual(len(calls[0].install), 2); self.assertEqual(len(calls[1].install), 1)
            self.assertEqual(session.read_bytes(), b'fixture')
            self.assertEqual(args.output.stat().st_mode & 0o777, 0o700)
            with patch.object(probe.runner, 'run') as run_again:
                self.assertEqual(probe.run(args)['status'], 'error'); run_again.assert_not_called()

    def test_stale_export_stops_before_restore(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); source = root / 'source'; source.write_bytes(b'fixture')
            args = probe.parser().parse_args(['--session', str(source), '--core', str(source),
                    '--probe', str(source), '--output', str(root / 'results')])
            def stale(a):
                a.output.mkdir(); exported = a.output / 'log.zip'; archive(exported, invocation())
                return {'status':'ok', 'artifacts':{'exports':[{'path':str(exported)}], 'session':str(source)}}
            with patch.object(probe.runner, 'run', side_effect=stale) as run:
                result = probe.run(args)
            self.assertEqual(result['status'], 'error'); self.assertIn('Stale', result['error'])
            self.assertEqual(run.call_count, 1); self.assertFalse(result['hotsync_validated'])
            self.assertEqual(source.read_bytes(), b'fixture')

    def test_interruption_reports_error(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); source = root / 'source'; source.write_bytes(b'fixture')
            args = probe.parser().parse_args(['--session', str(source), '--core', str(source),
                    '--probe', str(source), '--output', str(root / 'results')])
            with patch.object(probe.runner, 'run', side_effect=KeyboardInterrupt):
                result = probe.run(args)
            self.assertEqual(result['status'], 'error'); self.assertEqual(result['error'], 'Interrupted')
            self.assertTrue((args.output / 'serial.json').is_file())


if __name__ == '__main__': unittest.main()
