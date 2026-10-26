#!/usr/bin/env python3
"""Report signal levels and silent intervals in signed 16-bit little-endian PCM."""
import argparse
import array
import json
import math
from pathlib import Path
import sys


def analyze(path, rate=44100, channels=2, block_ms=10):
    data = Path(path).read_bytes()
    if rate <= 0 or channels <= 0 or block_ms <= 0:
        raise ValueError('rate, channels and block duration must be positive')
    if not data or len(data) % (2 * channels):
        raise ValueError('PCM must contain complete, nonempty sample frames')
    samples = array.array('h')
    samples.frombytes(data)
    if sys.byteorder != 'little':
        samples.byteswap()
    frames = len(samples) // channels
    block = max(1, round(rate * block_ms / 1000))
    peaks, squares = [0] * channels, [0] * channels
    clipped = silent_frames = run = longest = silent_blocks = 0
    block_nonzero = False
    for frame in range(frames):
        silent = True
        for channel in range(channels):
            sample = samples[frame * channels + channel]
            peaks[channel] = max(peaks[channel], abs(sample))
            squares[channel] += sample * sample
            clipped += sample in (-32768, 32767)
            silent = silent and sample == 0
        if silent:
            silent_frames += 1
            run += 1
            longest = max(longest, run)
        else:
            run = 0
            block_nonzero = True
        if (frame + 1) % block == 0:
            silent_blocks += not block_nonzero
            block_nonzero = False
    return {
        'format': 's16le', 'rate': rate, 'channels': channels, 'frames': frames,
        'duration_seconds': frames / rate, 'peak_by_channel': peaks,
        'rms_by_channel': [math.sqrt(s / frames) for s in squares],
        'clipped_channel_samples': clipped, 'silent_frames': silent_frames,
        'silent_fraction': silent_frames / frames,
        'longest_silence_seconds': longest / rate,
        'block_ms': block_ms, 'complete_blocks': frames // block,
        'silent_blocks': silent_blocks,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pcm', type=Path)
    parser.add_argument('--rate', type=int, default=44100)
    parser.add_argument('--channels', type=int, default=2)
    parser.add_argument('--block-ms', type=int, default=10)
    args = parser.parse_args()
    try:
        result = analyze(args.pcm, args.rate, args.channels, args.block_ms)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
