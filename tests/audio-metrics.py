#!/usr/bin/env python3
"""Check PCM verdicts independently of emulator implementation."""
import importlib.util
from pathlib import Path
import struct
import tempfile

spec = importlib.util.spec_from_file_location('audio_check', 'tools/check-native-audio.py')
audio = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audio)
with tempfile.TemporaryDirectory(dir='build') as directory:
    p = Path(directory) / 'capture.raw'
    p.write_bytes(bytes(3 * 44100 * 4) + struct.pack('<hh', 1000, -1000) * (44100 - 441)
                  + bytes(441 * 4))
    m = audio.pcm_metrics(p)
    assert m['examined_frames'] == 44100 and m['peak'] == 1000
    assert m['silent_10ms_blocks'] == 1 and m['complete_10ms_blocks'] == 100
    assert m['zero_frames'] == m['longest_zero_frames'] == 441 and not m['clipped_samples']
    p.write_bytes(struct.pack('<hh', -32768, 32767))
    assert audio.pcm_metrics(p, 0)['clipped_samples'] == 2
    p.write_bytes(b'\0')
    try:
        audio.pcm_metrics(p)
    except ValueError:
        pass
    else:
        raise AssertionError('truncated stereo frame accepted')
print('PASS: PCM warm-up exclusion, silence duration, clipping and truncation checks')
