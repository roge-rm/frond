#!/usr/bin/env python3
"""Summarize the optional audio-timing trace; does not decide PCM continuity."""
import argparse
import json
from pathlib import Path
import re
import statistics


def analyze(text):
    pressure = None
    previous_write = {}
    intervals, pauses, lifecycle, codec = [], [], [], []
    writes = silent_writes = failed_writes = dma = silent_dma = resets = 0
    for line in text.splitlines():
        fields = dict(re.findall(r'(\w+)=([0-9a-f]+)', line))
        if line.startswith('LIFECYCLE '):
            # Do not join intervals across starts, stops, format changes or closes.
            previous_write.clear()
            lifecycle.append(fields)
        elif line.startswith('HALPCM '):
            channel, guest = int(fields['channel']), int(fields['guest'])
            writes += 1
            failed = int(fields.get('failed', '0')) != 0
            failed_writes += failed
            silent_writes += not failed and int(fields['zeros']) == int(fields['frames'])
            if channel in previous_write:
                delta = guest - previous_write[channel]
                if delta >= 0:
                    intervals.append(delta / 1e6)
                else:
                    resets += 1
            previous_write[channel] = guest
        elif line.startswith('STAGE DMA '):
            dma += 1
            silent_dma += int(line.split()[-1]) == 0
        elif line.startswith('GUESTPCM '):
            codec.append({k: int(v) for k, v in fields.items()})
        elif line.startswith('PRESSURE '):
            host, guest = int(fields['host']), int(fields['guest'])
            if int(fields['active']):
                pressure = host, guest
            elif pressure is not None:
                dh, dg = host - pressure[0], guest - pressure[1]
                if dh >= 0 and dg >= 0:
                    pauses.append({'host_ms': dh / 1000, 'guest_ms': dg / 1e6})
                else:
                    resets += 1
                pressure = None
    return {
        'measurement': 'diagnostic emulator trace, not physical or PCM continuity acceptance',
        'hal_writes': writes, 'hal_all_zero_writes': silent_writes,
        'hal_unreadable_writes': failed_writes,
        'hal_guest_interval_ms_median': statistics.median(intervals) if intervals else None,
        'hal_guest_interval_ms_max': max(intervals, default=None),
        # DMA trace inspects every eighth frame at descriptor fetch, before transfer.
        'dma_descriptors': dma, 'dma_sampled_all_zero_descriptors': silent_dma,
        'codec_one_second_blocks': codec,
        'backpressure_intervals': pauses,
        'backpressure_guest_ms_total': sum(p['guest_ms'] for p in pauses),
        'unclosed_backpressure_interval': pressure is not None,
        'clock_resets': resets, 'lifecycle': lifecycle,
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.log.read_text()), indent=2))
