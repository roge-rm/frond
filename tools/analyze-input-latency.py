#!/usr/bin/env python3
"""Measure replay enqueue to touchscreen and SDL PCM onset from a latency trace."""
import argparse
import array
import json
from pathlib import Path
import statistics
import sys


def analyze(log, pcm, x, y, threshold=100, timeout_ms=2000, baseline_ms=250):
    samples = array.array('h', Path(pcm).read_bytes())
    if sys.byteorder != 'little':
        samples.byteswap()
    packets, inputs, touches = [], [], []
    for line in Path(log).read_text().splitlines():
        fields = line.split()
        if len(fields) < 2 or fields[0] != 'LAT':
            continue
        values = tuple(map(int, fields[2:]))
        if fields[1] == 'AUDIO':
            at, frame, count, pending = values
            if (frame + count) * 2 > len(samples):
                raise ValueError('truncated PCM capture')
            peak = max(map(abs, samples[frame * 2:(frame + count) * 2]), default=0)
            packets.append((at, frame, count, pending, peak))
        elif fields[1] == 'INPUT':
            inputs.append(values)
        elif fields[1] == 'TOUCH':
            touches.append(values)
    rows = []
    for at, down, px, py in inputs:
        if not down or (px, py) != (x, y):
            continue
        release = next((v[0] for v in inputs if v[0] > at and not v[1]), None)
        deadline = min(at + timeout_ms * 1000,
                       next((v[0] for v in inputs if v[0] > at and v[1]), at + timeout_ms * 1000))
        touch = next((v[0] for v in touches if v[0] >= at and v[1:] == (1, x, y)), None)
        before = max((p[4] for p in packets if at - baseline_ms * 1000 <= p[0] < at), default=None)
        onset = None
        if before is not None and before < threshold:
            for stamp, frame, count, pending, peak in packets:
                if stamp < at or stamp >= deadline or peak < threshold:
                    continue
                offset = next(i for i in range(count) if
                              max(abs(samples[2 * (frame + i)]),
                                  abs(samples[2 * (frame + i) + 1])) >= threshold)
                onset = stamp + offset * 1e6 / 44100
                break
        rows.append({'press_us': at,
                     'hold_ms': None if release is None else (release - at) / 1000,
                     'touch_ms': None if touch is None else (touch - at) / 1000,
                     'pcm_ms': None if onset is None else (onset - at) / 1000,
                     'baseline_peak': before,
                     'status': 'background_audio' if before is None or before >= threshold else
                               'response' if onset is not None else 'no_response'})
    latencies = [r['pcm_ms'] for r in rows if r['status'] == 'response']
    return {'boundary': 'host enqueue to SDL callback sample, excluding sound-server/device latency',
            'rate': 44100, 'threshold': threshold, 'timeout_ms': timeout_ms, 'baseline_ms': baseline_ms,
            'trials': rows, 'responses': len(latencies),
            'median_ms': statistics.median(latencies) if latencies else None,
            'max_ms': max(latencies, default=None)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('pcm', type=Path)
    parser.add_argument('--x', type=int, default=108)
    parser.add_argument('--y', type=int, default=230)
    parser.add_argument('--threshold', type=int, default=100)
    parser.add_argument('--baseline-ms', type=int, default=250)
    args = parser.parse_args()
    if args.threshold <= 0 or args.baseline_ms <= 0:
        parser.error('threshold and baseline duration must be positive')
    try:
        print(json.dumps(analyze(args.log, args.pcm, args.x, args.y, args.threshold, baseline_ms=args.baseline_ms), indent=2))
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
