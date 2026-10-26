# Native Palm TX core

Frond uses CloudpilotEmu commit
`064d5d578bcd1f9072de384d05aa217f83db2f7f`, with its core changes applied as an
ordered patch series. The upstream submodule stays unmodified.

For desktop setup, see the [desktop guide](desktop.md). To build just the SDL
emulator, run `make out/frond` after installing the build dependencies listed in
the [README](../README.md).

## Firmware and startup

```sh
./out/frond --experimental-tx /path/to/Palm-TX-5.4-nor.bin \
  --nand /path/to/Palm-TX-5.4-nand.bin
```

The TX configuration uses 32 MiB RAM, a PXA270 and a 320 × 480 display. It expects
16 MiB NOR and 132 MiB raw NAND: 65536 pages of 2048 data bytes plus 64 spare
bytes. NOR and NAND inputs are copied into process memory and are not overwritten.

The TX NOR contains an IPL rather than the Palm card header used by E2 detection,
so raw TX firmware needs the explicit flag. Saved sessions select their own
device type. See the [boot smoke test](boot-check.md) for bounded headless runs.

Firmware layout validation can be run separately:

```sh
python3 build/source/tools/test-tx-native.py ./out/frond \
  /path/to/Palm-TX-5.4-nor.bin /path/to/Palm-TX-5.4-nand.bin
```

## Board and CPU model

The TX board supplies NAND command/address/data mappings, ready GPIO, device ID,
RAM probe marker, keypad matrix, GPIO defaults and the WM9712L codec. The codec
IRQ is routed to PXA GPIO27; [input testing](input-testing.md) describes the ADC
and FIFO model.

CP15 identifies the XScale CPU as `0x69054114`. The alignment-control bit is
writable. With alignment checking disabled, ARMv5 word reads align the access
and rotate single-word loads; stores and multiple transfers use aligned
addresses without that rotation. With checking enabled, misaligned word accesses
raise a precise abort. Synthetic CPU tests cover both behaviours.

OSCR0 runs at the PXA27x 3.25 MHz rate rather than the PXA25x 3.6864 MHz rate.
Scheduler periods use integer nanoseconds, with small quantization error.
Peripheral serialization includes PXA270 IMC, keypad, USB controller, WMMX,
power registers and all seven LCD DMA channels. See
[sessions and reset](session-testing.md) and [peripherals](peripherals.md).

## PACE and Palm API injection

The TX loads its OS from NAND into RAM. Frond checks the active mappings of the
OS image at virtual `0x20000000` against physical RAM `0xa0200000`, then validates
the interpreter signature and callout destinations before patching three
instructions in process memory. Missing or ambiguous signatures leave memory
unchanged. The host decoded-instruction cache is invalidated after patching.

A scheduled probe activates the bridge during OS loading, before the first
interpreter invocation, and detects overwritten entry instructions after an OS
reload. E2-specific pen, NVFS and UI ROM hooks remain disabled for TX.

The bridge enables application installation, launch and RAM database export.
These operations need a suitable guest API boundary, so startup may still need
time after a signature match. Database export snapshots and restores the guest
around API injection, including memory, flash, card, peripheral and simulated
network state. A failed export must not leave a partial archive or mutate the
running guest. The checkpoint requires extra memory roughly equal to the guest
RAM, flash and optional SD image.

## Native performance

ARM and Thumb execution check decoded instruction-cache hits inline. Misses,
decoding, MMU/MPU checks and faults use the normal fetch path. Virtual cache tags
retain all ten bits to distinguish addresses separated by 2 GiB. The timer skips
redundant interrupt-controller updates when no status bit is set; register
writes and asserted interrupt levels retain their normal handling.

The frontend uses the baseline adaptive governor with a 97% load ceiling and
bounds each native catch-up request to one virtual frontend slice. Events are
checked between batches. Normal SDL playback uses seven/five callback high/low
water marks, six callbacks at startup and three during underrun recovery. The
queue can grow when SDL negotiates a larger callback period.

Use a saved, already-playing session to compare execution cost separately from
wall-clock pacing and SDL starvation:

```sh
make benchmark-session
./build/benchmark-session /path/to/playing-tx.session 10 50 out/benchmark.raw
```

Arguments are session, virtual seconds, virtual million cycles per second and an
optional new PCM output path. Existing output files are rejected. The benchmark
reports wall and process CPU time excluding restore, advances without wall-clock
pacing and drains codec PCM directly. Virtual cycles include sleep fast-forwarding;
they are not hardware-equivalent TX instruction counts. Silence statistics skip
the first 132300 frames.

Pair fixed-budget comparisons with [wall-time PCM checks](app-testing.md#pcm-capture-and-continuity).
Reliable playback depends on available host CPU time as well as guest execution
cost. Compare identical workloads and PCM, and record host conditions before
claiming a performance improvement.

## Optional pipeline diagnostics

Apply the combined audio trace to a prepared source tree:

```sh
git apply --directory=build/source tools/debug/performance-audio-trace.patch
make -j4
FROND_PCM_TRACE=1 FROND_HAL_AUDIO=1 FROND_LATENCY_PCM=out/pipeline.raw \
  ./out/frond /path/to/playing-tx.session --run-for 60
```

It records codec silence/FIFO state, HAL write timing and levels, input delivery
and PCM submitted to SDL. `FROND_FIXED_MIPS=50` fixes the diagnostic guest budget;
larger budgets can exceed host capacity. Use the combined patch alone, without
the overlapping input or guest-PCM traces. Its HAL hook observes the normal path.
Reverse the patch and rebuild before normal use.

For repeatable frontend pauses, apply `tools/debug/frontend-stall.patch` after
the combined trace and rebuild. `FROND_STALL_USEC=25000` injects a 25 ms pause
after drawing every five seconds. `FROND_FRONTEND_TRACE=1` records per-frame
work, and `FROND_MAX_LOAD` overrides the diagnostic load ceiling. Reverse the
frontend patch before the audio trace, then rebuild. These hooks are absent
from production builds.
