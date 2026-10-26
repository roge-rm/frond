#!/usr/bin/env python3
"""Describe the optional TX audio-buffer observer; never issue a continuity verdict."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import statistics


def analyze(text):
    counts = Counter(dict.fromkeys((
        'lifecycle_boundaries', 'clock_resets', 'unreadable_guest_records',
        'dma_completed_records', 'dma_partial_or_unknown_records',
        'dma_complete_blocks', 'dma_words', 'dma_zero_words', 'dma_all_zero_blocks',
        'mixed_blocks', 'mixed_all_zero_blocks', 'mixer_acquires',
        'mixer_failed_acquires', 'mixer_empty_queue_failures', 'mixer_short_queue_failures',
        'codec_fifo_empty_samples'), 0))
    durations = {name: [] for name in ('callback', 'variable_callback', 'mixer', 'filtered_accumulate')}
    pending = {}
    previous_time = None
    profile = None
    # Entry/return pairs in the signature-checked TX firmware. Durations include
    # preemption and scheduling: these are guest elapsed times, not CPU costs.
    starts = {'2004b69c': ('callback', 'r4'), '2004b6b0': ('variable_callback', 'r4'),
              '20087764': ('mixer', 'obj'), '20087884': ('filtered_accumulate', 'obj')}
    ends = {'2004b6a0': ('callback', 'r4'), '2004b6b4': ('variable_callback', 'r4'),
            '20087980': ('mixer', 'obj'), '20087888': ('filtered_accumulate', 'obj')}
    for line in text.splitlines():
        kind = line.split(' ', 1)[0]
        f = dict(re.findall(r'(\w+)=([0-9a-f]+)', line))
        if kind == 'GUESTPROFILE':
            profile = bool(int(f['supported']))
        elif kind == 'LIFECYCLE':
            counts['lifecycle_boundaries'] += 1
            pending.clear()
        elif kind in ('GUEST', 'MIXOUT', 'DMAPCM'):
            now = int(f['time'])
            if previous_time is not None and now < previous_time:
                counts['clock_resets'] += 1
                pending.clear()
            previous_time = now
            if int(f.get('failed', '0')):
                counts['unreadable_guest_records'] += 1
                pending.clear()
                continue
            if kind == 'DMAPCM':
                counts['dma_completed_records'] += 1
                if f.get('complete') != '1':
                    counts['dma_partial_or_unknown_records'] += 1
                    continue
                counts['dma_complete_blocks'] += 1
                counts['dma_words'] += int(f['words'])
                counts['dma_zero_words'] += int(f['zeros'])
                counts['dma_all_zero_blocks'] += f['words'] == f['zeros']
            elif kind == 'MIXOUT':
                counts['mixed_blocks'] += 1
                counts['mixed_all_zero_blocks'] += int(f['zeros']) == int(f.get('frames', '1024'))
            else:
                pc = f['pc']
                if pc in starts:
                    name, key = starts[pc]
                    pending[name, f[key]] = now
                if pc in ends:
                    name, key = ends[pc]
                    start = pending.pop((name, f[key]), None)
                    if start is not None:
                        durations[name].append((now - start) / 1e6)
                if pc == '200877b0':
                    counts['mixer_acquires'] += 1
                    if int(f['r0'], 16) == 0:
                        counts['mixer_failed_acquires'] += 1
                        counts['mixer_empty_queue_failures'] += f['free'] == f['size']
                        counts['mixer_short_queue_failures'] += int(f['size']) - int(f['free']) < 4 * int(f['need'])
        elif kind == 'GUESTPCM':
            counts['codec_fifo_empty_samples'] += int(f['fifo_empty'])
    return {
        'measurement': 'diagnostic guest elapsed time; includes preemption; not physical latency or continuity acceptance',
        'firmware_profile_supported': profile,
        'counts': dict(sorted(counts.items())),
        'elapsed_guest_ms': {name: {'pairs': len(values),
                                  'median': statistics.median(values) if values else None,
                                  'max': max(values, default=None)}
                             for name, values in durations.items()},
        'unfinished_intervals': len(pending),
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.log.read_text()), indent=2))
