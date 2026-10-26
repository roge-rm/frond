#!/usr/bin/env python3
"""Measure the known TX SunVox menu fixture from the optional frame observer."""
import argparse
import json
from pathlib import Path
import statistics


def analyze(text, threshold=900, timeout_ms=2500):
    events = []
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[0] == 'MENU':
            if fields[1] in ('INPUT', 'TOUCH', 'FRAME', 'PRESENT'):
                events.append((fields[1], int(fields[2]), fields[3:]))
    presses = [stamp for kind, stamp, data in events
               if kind == 'INPUT' and tuple(map(int, data[:2])) == (274, 8)]
    rows = []
    for index, start in enumerate(presses):
        end = min(start + timeout_ms * 1000,
                  presses[index + 1] if index + 1 < len(presses) else start + timeout_ms * 1000)
        trial = [(kind, stamp, data) for kind, stamp, data in events if start <= stamp < end]
        panel = next(((stamp, int(data[0])) for kind, stamp, data in trial
                      if kind == 'TOUCH' and tuple(map(int, data[1:4])) == (1, 274, 8)), None)
        frame = next(((stamp, int(data[0])) for kind, stamp, data in trial
                      if kind == 'FRAME' and int(data[5]) == 1 and int(data[4]) >= threshold), None)
        present = next((stamp for kind, stamp, data in trial
                        if kind == 'PRESENT' and frame and stamp >= frame[0]), None)
        rows.append({'press_us': start,
                     'panel_ms': (panel[0] - start) / 1000 if panel else None,
                     'menu_frame_ms': (frame[0] - start) / 1000 if frame else None,
                     'present_ms': (present - start) / 1000 if present else None,
                     'panel_to_frame_guest_ms': (frame[1] - panel[1]) / 1e6 if panel and frame else None,
                     'status': 'response' if present is not None else 'frame_only' if frame else 'no_response'})
    responses = [r['present_ms'] for r in rows if r['status'] == 'response']
    return {'boundary': 'host pen enqueue to SDL present return for lower-menu pixel threshold',
            'fixture': '320-pixel-wide TX SunVox editor; menu must be closed before each press',
            'white_threshold': threshold, 'timeout_ms': timeout_ms, 'trials': rows,
            'responses': len(responses), 'median_ms': statistics.median(responses) if responses else None,
            'max_ms': max(responses, default=None)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('--threshold', type=int, default=900)
    args = p.parse_args()
    if args.threshold < 1:
        p.error('threshold must be positive')
    try:
        print(json.dumps(analyze(args.log.read_text(), args.threshold), indent=2))
    except (OSError, ValueError, IndexError) as error:
        p.error(str(error))


if __name__ == '__main__':
    main()
