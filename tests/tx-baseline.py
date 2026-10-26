#!/usr/bin/env python3
"""Exercise baseline process handling, input protection and framebuffer checks."""
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

spec = importlib.util.spec_from_file_location('tx_baseline', 'tools/tx-baseline.py')
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)
with tempfile.TemporaryDirectory(dir='build') as directory:
    root = Path(directory).resolve()
    image = root / 'frame.png'
    def chunk(kind, payload):
        return (struct.pack('>I', len(payload)) + kind + payload +
                struct.pack('>I', zlib.crc32(kind + payload) & 0xffffffff))
    pixels = (b'\0' + bytes(320 * 4)) * 480
    valid = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 320, 480, 8, 6, 0, 0, 0)) +
             chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))
    image.write_bytes(valid)
    baseline.check_png(image)
    for invalid in (b'not a PNG', valid[:24], valid[:-1], valid[:20] + b'\x01' + valid[21:]):
        image.write_bytes(invalid)
        try:
            baseline.check_png(image)
            raise AssertionError('bad framebuffer accepted')
        except ValueError:
            pass
    code, timed_out = baseline.run_command([sys.executable, '-c', 'print("done")'], root / 'pass.log', 5, os.environ)
    assert code == 0 and not timed_out and (root / 'pass.log').read_text().strip() == 'done'
    code, timed_out = baseline.run_command([sys.executable, '-c', 'import time;time.sleep(30)'], root / 'timeout.log', .2, os.environ)
    assert code != 0 and timed_out
    session = root / 'source.frond'; session.write_bytes(b'untouched')
    output = root / 'result'; output.mkdir(); (output / 'keep').write_bytes(b'untouched')
    command = [sys.executable, 'tools/tx-baseline.py', '--session', str(session), '--output', str(output), '--skip-synthetic']
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 2 and (output / 'keep').read_bytes() == b'untouched'
    assert session.read_bytes() == b'untouched'
    result = subprocess.run([*command, '--nor', str(root / 'nor')], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 2 and b'must be supplied together' in result.stderr
    result = subprocess.run([*command, '--audio-continuous'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 2 and b'requires --audio-session' in result.stderr
print('PASS: baseline command results, timeout cleanup, framebuffer validation and input/output protection')
