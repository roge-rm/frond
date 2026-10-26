#!/usr/bin/env python3
"""Summarize private fixed-guest benchmarks or GAME_* frontend traces as JSON."""
import argparse
import csv
import json
from pathlib import Path
import statistics


def distribution(values):
    if not values:
        return None
    ordered = sorted(values)
    return {'count': len(values), 'median_ms': statistics.median(ordered),
            'p95_ms': ordered[int(.95 * (len(ordered) - 1))], 'max_ms': ordered[-1]}


def summarize_frames(frames, first_input):
    # Frame records: guest ns, host ns, hash. Initial paused frames are excluded.
    previous = None
    changes = []
    for guest, wall, hash_value in frames:
        if hash_value != previous and (first_input is None or wall >= first_input[1]):
            changes.append((guest, wall))
        previous = hash_value
    result = {'changed_frames_after_input': len(changes)}
    for i, clock in enumerate(('guest', 'wall')):
        result[clock + '_changed_frame_intervals'] = distribution(
            [(b[i] - a[i]) / 1e6 for a, b in zip(changes, changes[1:])])
    result['first_change_after_input'] = None
    if changes and first_input:
        result['first_change_after_input'] = {
            'guest_ms': (changes[0][0] - first_input[0]) / 1e6,
            'wall_ms': (changes[0][1] - first_input[1]) / 1e6}
    return result


def load(directory):
    directory = Path(directory)
    if (directory / 'summary.json').is_file():
        summary = json.loads((directory / 'summary.json').read_text())
        frames = [(int(r['guest_ns']), int(r['wall_ns']), r['hash'])
                  for r in csv.DictReader((directory / 'frames.csv').open())]
        events = [r for r in csv.DictReader((directory / 'inputs.csv').open())
                  if r['kind'] != 'snapshot']
        first_input = (int(events[0]['guest_ns']), int(events[0]['wall_ns'])) if events else None
        return {'kind': 'fixed-guest', 'summary': summary, **summarize_frames(frames, first_input)}
    frames, events, replay_times, runs = [], [], [], []
    for line in (directory / 'core.log').read_text().splitlines():
        f = line.split()
        if not f:
            continue
        if f[0] == 'GAME_FRAME':
            frames.append((int(f[2]), int(f[1]) * 1000, f[3]))
        elif f[0] == 'GAME_INPUT':
            replay_times.append(int(f[2]) * 1000)
            if int(f[4]) != 6:  # InputReplay::Kind::snapshot
                events.append((int(f[3]), int(f[2]) * 1000))
        elif f[0] == 'GAME_RUN':
            runs.append((int(f[1]), int(f[2]), int(f[3]), float(f[4]), int(f[5]), int(f[6])))
    if not frames or not runs:
        raise ValueError('no gameplay timing trace found')
    start = events[0][1] / 1000 if events else runs[0][0]
    # Saving a session can take seconds and emit a later redraw. Use the last
    # replay event as the explicit end marker, excluding save/export/shutdown.
    end = replay_times[-1] / 1000 if replay_times else runs[-1][1]
    frames = [r for r in frames if r[1] <= end * 1000]
    selected = [r for r in runs if r[0] >= start and r[1] <= end]
    span = (selected[-1][1] - selected[0][0]) / 1e6 if selected else 0
    return {'kind': 'frontend', **summarize_frames(frames, events[0] if events else None),
            'measured_wall_seconds': span,
            'guest_seconds': sum(r[2] for r in selected) / 1e9,
            'emulation_cpu_seconds': sum(r[3] for r in selected),
            'median_budget_mips': statistics.median(r[4] for r in selected) / 1e6 if selected else None,
            'audio_underruns_total': selected[-1][5] if selected else 0}


def compare(before, after):
    if before['kind'] != 'fixed-guest' or after['kind'] != 'fixed-guest':
        raise ValueError('determinism comparison requires fixed-guest results')
    fields = ('version', 'mips', 'sound', 'palm_epoch', 'guest_ns', 'cycles', 'lcd_frames',
              'changed_frames', 'pcm_frames', 'pcm_zero_frames', 'pcm_peak',
              'frame_sequence_hash', 'pcm_hash')
    mismatch = [key for key in fields if before['summary'][key] != after['summary'][key]]
    return {'matching_workload': not mismatch, 'mismatches': mismatch,
            'cpu_reduction_percent': 100 * (1 - after['summary']['cpu_seconds'] / before['summary']['cpu_seconds'])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--compare', type=Path, help='require matching guest/frame/PCM results')
    args = parser.parse_args()
    try:
        result = load(args.directory)
        if args.compare:
            result = compare(result, load(args.compare))
        print(json.dumps(result, indent=2))
        return 1 if result.get('matching_workload') is False else 0
    except (OSError, ValueError, KeyError, IndexError) as error:
        parser.exit(2, f'{error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
