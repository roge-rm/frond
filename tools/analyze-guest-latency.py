#!/usr/bin/env python3
"""Split a known SunVox keyboard trace into Palm event, render, DMA and SDL stages."""
import argparse
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location('input_latency', Path(__file__).with_name('analyze-input-latency.py'))
input_latency = importlib.util.module_from_spec(spec)
spec.loader.exec_module(input_latency)


def analyze(log, pcm, x=108, y=151):
    result = input_latency.analyze(log, pcm, x, y)
    stages, all_presses = [], []
    for line in Path(log).read_text().splitlines():
        f = line.split()
        if len(f) < 3:
            continue
        if f[:2] == ['LAT', 'INPUT'] and f[3] == '1':
            all_presses.append(int(f[2]))
        if f[:2] == ['STAGE', 'EVTRET'] and int(f[4]) == 1:
            stages.append((int(f[2]), 'palm_event'))
        elif f[:2] == ['STAGE', 'RENDEROUT'] and int(f[-1]) >= 100:
            stages.append((int(f[2]), 'rendered'))
        elif f[:2] == ['STAGE', 'DMA'] and len(f) == 8 and int(f[-1]) >= 100:
            stages.append((int(f[2]), 'dma'))
        elif f[:2] == ['LAT', 'GUEST']:
            stages.append((int(f[2]), 'codec'))
    stages.sort()
    for row in result['trials']:
        at = row['press_us']
        end = min(at + 2000000, next((t for t in all_presses if t > at), at + 2000000))
        timeline = {'touch': row['touch_ms']}
        cursor = at
        for name in ['palm_event', 'rendered', 'dma', 'codec']:
            stamp = next((t for t, kind in stages if cursor <= t < end and kind == name), None)
            timeline[name] = None if stamp is None else (stamp-at)/1000
            if stamp is not None:
                cursor = stamp
        timeline['sdl'] = row['pcm_ms']
        row['stages_ms_from_press'] = timeline
        row['render_to_dma_ms'] = (timeline['dma'] - timeline['rendered']
                                   if timeline['dma'] is not None and timeline['rendered'] is not None else None)
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('pcm', type=Path)
    args = p.parse_args()
    print(json.dumps(analyze(args.log, args.pcm), indent=2))
