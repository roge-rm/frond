#!/usr/bin/env python3
"""Check measured onset, missed input and rejection of background audio."""
import array
import importlib.util
from pathlib import Path
import tempfile
import sys

spec = importlib.util.spec_from_file_location('latency', Path(__file__).resolve().parents[1] / 'tools/analyze-input-latency.py')
latency = importlib.util.module_from_spec(spec)
spec.loader.exec_module(latency)
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    pcm = array.array('h', [0] * 2048)
    for i in range(512 + 441, 1024):
        pcm[2 * i] = 1000
    if sys.byteorder != 'little':
        pcm.byteswap()
    (root / 'pcm').write_bytes(pcm.tobytes())
    (root / 'log').write_text('LAT AUDIO 800000 0 512 1000\n'
                             'LAT INPUT 1000000 1 108 230\n'
                             'LAT TOUCH 1020000 1 108 230\n'
                             'LAT INPUT 1100000 0 -1 -1\n'
                             'LAT AUDIO 1200000 512 512 1000\n'
                             'LAT INPUT 1300000 1 108 230\n'
                             'LAT INPUT 4000000 1 108 230\n'
                             'LAT AUDIO 3900000 0 512 1000\n')
    result = latency.analyze(root / 'log', root / 'pcm', 108, 230)
    first, background, missed = result['trials']
    assert abs(first['pcm_ms'] - 210) < 0.001
    assert first['touch_ms'] == 20
    assert background['status'] == 'background_audio'
    assert missed['status'] == 'no_response'
    (root / 'log').write_text('LAT AUDIO 800000 0 512 1000\n'
                             'LAT INPUT 1000000 1 108 230\n'
                             'LAT INPUT 1100000 1 108 230\n'
                             'LAT AUDIO 1200000 512 512 1000\n')
    result = latency.analyze(root / 'log', root / 'pcm', 108, 230)
    assert result['trials'][0]['status'] == 'no_response'
print('PASS: input latency onset, background rejection and missing response')
