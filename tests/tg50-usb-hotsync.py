#!/usr/bin/env python3
"""Sony CLIE TG50 USB HotSync with pilot-link: list, fetch, install, list again and dlpsh.

Skipped unless out/frond, pilot-xfer, pilot-dlpsh and a ready TG50 session (build/smoke/machines/sony-clie-tg50,
from make smoke-sessions, or FROND_TG50_SESSION) exist. The session is only read. Takes about two minutes.
"""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'out/frond'
SESSION = Path(os.environ.get('FROND_TG50_SESSION', (ROOT / 'build/smoke/machines/sony-clie-tg50'
                                                             if (ROOT / 'build/smoke/machines/sony-clie-tg50').exists()
                                                             else ROOT / 'build/smoke/sony-clie-tg50.session')))
PILOT = shutil.which('pilot-xfer') and shutil.which('pilot-dlpsh')


def small_pdb(path, name):
    """An empty record database to install over HotSync."""
    header = name.encode().ljust(32, b'\0') + struct.pack('>HHIIIIII4s4sI', 0, 1, 0xb6000000, 0xb6000000, 0, 0, 0, 0,
                                                           b'DATA', b'FrUs', 0)
    path.write_bytes(header + struct.pack('>IH', 0, 0) + b'\0\0')
    return path


@unittest.skipUnless(CORE.is_file() and SESSION.exists() and PILOT,
                     'needs out/frond, a ready TG50 session (make smoke-sessions) and pilot-link')
class Tg50UsbHotSyncTest(unittest.TestCase):
    def hotsync(self, endpoint):
        with tempfile.TemporaryDirectory(prefix='tg50-usb-', dir=ROOT / 'build') as work:
            work = Path(work)
            pdb = small_pdb(work / 'FrondUsbTest.pdb', 'FrondUsbTest')
            result = subprocess.run(
                [sys.executable, str(ROOT / 'tools/probe-tx-usb-hotsync.py'), '--session', str(SESSION),
                 '--install', str(pdb), '--endpoint', endpoint, '--button', '161,170', '--step-seconds', '12',
                 '--output', str(work / 'out')], capture_output=True, text=True, timeout=600)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads((work / 'out/usb-hotsync.json').read_text())
            self.assertEqual(report['status'], 'ok', result.stdout + result.stderr)
            self.assertEqual([s['name'] for s in report['steps']],
                             ['list', 'fetch', 'install', 'list-after-install', 'dlpsh'])
            self.assertTrue(any('enumerated 054c:00da' in line for line in report['usb_log']))

    def test_pty(self):
        self.hotsync('pty')


if __name__ == '__main__':
    unittest.main()
