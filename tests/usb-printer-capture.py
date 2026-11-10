import importlib.util
from pathlib import Path
import unittest
spec = importlib.util.spec_from_file_location('checker', Path(__file__).resolve().parents[1] / 'tools/check-usb-printer-capture.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

def packet(toggle, payload, result=0):
    return f'CALL port=0 addr=2 ep=1 pid=1 toggle={toggle} count={len(payload)//2} bytes={payload}\nREPLY result={result} toggle={toggle} bytes=\n'

class Capture(unittest.TestCase):
    def test_nak_and_duplicate(self):
        trace = packet(0, 'ff', 1) + 'printer: attached collecting bytes=0\n'
        trace += packet(0, '61', 1) + packet(0, '61') + packet(0, '61') + packet(1, '0c')
        self.assertEqual(checker.accepted(trace), b'a\x0c')
    def test_bad_length(self):
        with self.assertRaises(ValueError):
            checker.accepted('printer: collecting bytes=0\n' + packet(0, '61').replace('count=1', 'count=2'))
    def test_missing_job(self):
        with self.assertRaises(ValueError):
            checker.accepted(packet(0, '61'))
    def test_repeated_begin(self):
        with self.assertRaises(ValueError):
            checker.accepted('printer: collecting bytes=0\n' * 2)

unittest.main()
