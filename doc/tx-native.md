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
These operations need a verified guest API boundary. The 68k path stops before
an `EvtGetEvent` trap. For the qualified TX firmware, a native path can also stop
at a natural return from the UI event manager's internal wait. This supports
commands while a native application waits without returning to PACE.

Native preflight checks the firmware instructions, saved caller chain, running
and selected UI task, scheduler lock state, event-manager flags and memory
mappings. It makes no guest API calls or explicit guest writes. It never sends
input, changes an application's wait timeout, or reuses a consumed PACE frame.
The native path rejects different firmware, busy tasks and unqualified call
chains. The existing 68k path remains available where supported; if neither path
is ready, the command returns `not-ready`. A successful signature scan alone does
not establish readiness.

Preflight advances the live guest and can change memory, flash, devices or
network state. That progress remains on success and failure. After preflight,
export immediately checkpoints the prepared guest. Database calls, temporary
allocations and callbacks run inside bounded recovery. Native calls use owned
scratch memory and a separate stack, then restore the interrupted CPU/task
context. Export rolls back guest memory, storage and saved device state before
publishing its ZIP. No archive is published after a guest-operation failure.

Install and launch use transactions too: successful results commit, while
failed replacement or a call fault/timeout restores the previous guest. Queued
and held host inputs survive temporary rollback. Ordinary session restore still
discards stale host input. The checkpoint requires extra memory roughly equal
to guest RAM, flash and the optional SD image.

`guest-ready` is a point-in-time observation. Normal execution between socket
requests can consume the boundary, so each operation repeats preflight before
its checkpoint. Calls and callbacks have count, memory and execution bounds;
native sessions also have aggregate guest-time and wall-time limits. A blocked
or unsupported guest operation returns an error rather than manufacturing
readiness.

Ordinary injected calls have a 50-million-cycle limit. The database-image importer
alone can continue through ten such slices, checking deadlines between slices
without restarting the call. This accommodates larger databases while preserving
the 20-accounted-guest-second and 30-wall-second session limits and rollback on
fault or exhaustion.

`make test-export-checkpoint`, `make test-checkpoint-input` and the optional
real-guest workflow acceptance cover preflight, rollback, failed replacement,
input preservation, saved sessions and named log retrieval. Full RAM export
still fails if an included database cannot be exported; use named export for
an application's closed log database. Open applications cannot always be
replaced, even when the command boundary is available.

## Native performance

ARM and Thumb execution check decoded instruction-cache hits inline. Misses,
decoding, MMU/MPU checks and faults use the normal fetch path. Virtual cache tags
retain all ten bits to distinguish addresses separated by 2 GiB. The timer skips
redundant interrupt-controller updates when no status bit is set; register
writes and asserted interrupt levels retain their normal handling.

A fetch in the same 32-byte cache line as the previous one reuses that line,
still checking its tag, revision and decoded slot. Loads and stores of up to
8 bytes to primary RAM take an inlined path after MMU translation, with the same
permission, alignment, dirty-page and framebuffer handling. On local Bike and
SunVox guest-timed replays the core runs about 7% fewer host instructions, with
identical frame, PCM and snapshot hashes. An access that runs past
0xffffffff matches no RAM, ROM or device region and fails.

On native TX, the OS timer batches up to 32 ticks between events. Counter reads
account for instruction progress inside the current CPU batch; new matches and
watchdog deadlines shorten the next batch. This removes repeated scheduler work
without changing the timer frequency. During interrupt-driven idle the timer can jump
to its next interrupt. PXA27x power sleep stops the 3.25-MHz timer clock; see
[power lifecycle checks](session-testing.md#tx-power-transition-diagnostics). Saved timer state retains partially observed batches;
older timer chunks still load, and E2 keeps its version-zero timer format.

The frontend uses an adaptive governor with a 97% load ceiling. Native catch-up
reserves another ten percentage points and bounds each work request to one
frontend slice; events are checked between batches. Normal SDL playback uses
eight/six callback high/low water marks, six callbacks at startup and three during
underrun recovery. The extra steady headroom costs about 12 ms at 512-frame
callbacks. The experimental low-latency path retains its seven/five water marks.
The queue can grow when SDL negotiates a larger callback period. Backpressure
uses the current PCM-output state, including sound enabled over desktop RPC or
the console after a `--no-sound` launch.

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
