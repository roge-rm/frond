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
    assert m['trailing_zero_frames'] == 441 and m['longest_internal_zero_frames'] == 0
    p.write_bytes(struct.pack('<hh', 1000, -1000) * 441 + bytes(882 * 4)
                  + struct.pack('<hh', 1000, -1000) * 441)
    m = audio.pcm_metrics(p, 0)
    assert m['longest_internal_zero_frames'] == 882 and m['trailing_zero_frames'] == 0
    assert m['signal_span_seconds'] == 0.04
    p.write_bytes(bytes(441 * 4))
    m = audio.pcm_metrics(p, 0)
    assert m['signal_span_seconds'] == 0 and m['peak'] == 0
    assert m['zero_frames'] == m['trailing_zero_frames'] == 441
    p.write_bytes(struct.pack('<hh', -32768, 32767))
    assert audio.pcm_metrics(p, 0)['clipped_samples'] == 2
    p.write_bytes(b'\0')
    try:
        audio.pcm_metrics(p)
    except ValueError:
        pass
    else:
        raise AssertionError('truncated stereo frame accepted')
print('PASS: PCM warm-up exclusion, internal/trailing silence, clipping and truncation checks')
