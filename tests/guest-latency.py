#!/usr/bin/env python3
"""Ignore silent buffers and attribute each pipeline stage to the right press."""
import array
import importlib.util
from pathlib import Path
import sys
import tempfile

spec = importlib.util.spec_from_file_location('guest_latency', Path(__file__).resolve().parents[1] / 'tools/analyze-guest-latency.py')
guest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guest)
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    samples = array.array('h', [0] * 2048)
    samples[1024] = 1000
    if sys.byteorder != 'little':
        samples.byteswap()
    (root / 'pcm').write_bytes(samples.tobytes())
    (root / 'log').write_text('LAT AUDIO 800000 0 512 1000\n'
                             'LAT INPUT 1000000 1 108 151\n'
                             'LAT TOUCH 1010000 1 108 151\n'
                             'STAGE EVTRET 1020000 1000 1 1 55 77\n'
                             'STAGE RENDEROUT 1025000 2000 1024 0\n'
                             'STAGE RENDEROUT 1030000 2000 1024 500\n'
                             'STAGE DMA 1040000 3000 3000 2000 9023d000 0\n'
                             'STAGE DMA 1140000 3000 3000 2000 9023d000 250\n'
                             'LAT GUEST 1140100 1536\n'
                             'LAT AUDIO 1200000 512 512 1000\n'
                             'LAT INPUT 1500000 1 108 151\n')
    result = guest.analyze(root / 'log', root / 'pcm')
    first, missing = result['trials']
    t = first['stages_ms_from_press']
    assert t['touch'] == 10 and t['palm_event'] == 20 and t['rendered'] == 30
    assert t['dma'] == 140 and abs(t['codec'] - 140.1) < 0.001
    assert first['render_to_dma_ms'] == 110 and t['sdl'] == 200
    assert missing['stages_ms_from_press']['palm_event'] is None
    (root / 'log').write_text('LAT AUDIO 800000 0 512 1000\n'
                             'LAT INPUT 1000000 1 108 151\n'
                             'LAT INPUT 1100000 1 192 151\n'
                             'STAGE EVTRET 1200000 1000 1 1 96 77\n'
                             'LAT AUDIO 1200000 512 512 1000\n')
    result = guest.analyze(root / 'log', root / 'pcm')
    assert result['trials'][0]['stages_ms_from_press']['palm_event'] is None
print('PASS: guest latency stages reject silent buffers and later presses')
