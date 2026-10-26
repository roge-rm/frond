#!/usr/bin/env python3
"""Trace attribution must respect lifecycle and restored guest clocks."""
import importlib.util

spec = importlib.util.spec_from_file_location('timing', 'tools/analyze-audio-timing.py')
timing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(timing)
report = timing.analyze('''HALPCM host=1 guest=1000000 channel=0 frames=1024 zeros=0
HALPCM host=2 guest=2000000 channel=0 frames=1024 zeros=1024
LIFECYCLE host=3 guest=3000000 trap=4318 r0=0 r1=0
HALPCM host=4 guest=8000000 channel=0 frames=1024 zeros=0
HALPCM host=5 guest=1000000 channel=0 frames=1024 zeros=0
PRESSURE host=1000 guest=1000000 active=1
PRESSURE host=34000 guest=34000000 active=0
PRESSURE host=35000 guest=34000000 active=1
PRESSURE host=68000 guest=34000000 active=0
STAGE DMA 68000 a00c32d0 a00c4320 a00c22b8 9023d000 0
GUESTPCM frames=44100 zeros=2048 longest=1024 fifo_empty=0 rate=44100
PRESSURE host=70000 guest=35000000 active=1
''')
assert report['hal_writes'] == 4 and report['hal_all_zero_writes'] == 1
assert report['hal_guest_interval_ms_max'] == 1
assert report['clock_resets'] == 1
assert report['backpressure_guest_ms_total'] == 33
assert report['backpressure_intervals'][1] == {'host_ms': 33, 'guest_ms': 0}
assert report['unclosed_backpressure_interval']
assert report['dma_sampled_all_zero_descriptors'] == 1
assert report['codec_one_second_blocks'][0]['fifo_empty'] == 0
assert timing.analyze('')['hal_guest_interval_ms_max'] is None
failed = timing.analyze('HALPCM host=1 guest=1 channel=0 frames=1 zeros=1 failed=1')
assert failed['hal_unreadable_writes'] == 1 and failed['hal_all_zero_writes'] == 0
print('PASS: audio trace lifecycle, clock resets, guest drift and unfinished pressure intervals')

spec = importlib.util.spec_from_file_location('buffers', 'tools/analyze-audio-buffers.py')
buffers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(buffers)
report = buffers.analyze('''GUESTPROFILE supported=1
GUEST time=1000000 pc=2004b69c r4=100
GUEST time=3000000 pc=2004b6a0 r4=100
GUEST time=4000000 pc=20087764 obj=200
GUEST time=4100000 pc=200877b0 obj=200 r0=0 free=8448 size=8448 need=1024
GUEST time=4200000 pc=20087980 obj=200
MIXOUT time=4300000 frames=1024 zeros=1024 failed=0
DMAPCM time=5000000 words=1024 zeros=1024 complete=1
DMAPCM time=6000000 words=20 zeros=20 complete=0
DMAPCM time=7000000 words=1024 zeros=1024
GUEST time=8000000 pc=2004b69c r4=100
LIFECYCLE guest=8500000 trap=4318
GUEST time=9000000 pc=2004b6a0 r4=100
GUEST time=10000000 pc=2004b69c r4=100
GUEST time=1000000 pc=2004b6a0 r4=100
MIXOUT time=2000000 frames=1024 zeros=1024 failed=1
GUESTPCM frames=44100 zeros=1024 fifo_empty=0
''')
assert report['firmware_profile_supported']
assert report['elapsed_guest_ms']['callback'] == {'pairs': 1, 'median': 2, 'max': 2}
assert report['elapsed_guest_ms']['mixer']['pairs'] == 1
c = report['counts']
assert c['dma_complete_blocks'] == c['dma_all_zero_blocks'] == 1
assert c['dma_partial_or_unknown_records'] == 2
assert c['mixer_empty_queue_failures'] == c['mixer_short_queue_failures'] == 1
assert c['mixed_all_zero_blocks'] == 1 and c['unreadable_guest_records'] == 1
assert c['clock_resets'] == 1 and c['codec_fifo_empty_samples'] == 0
assert buffers.analyze('')['firmware_profile_supported'] is None
print('PASS: buffer trace distinguishes actual/partial DMA, callback boundaries and unreadable data')
