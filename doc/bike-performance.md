# Bike or Die 2 performance investigation

Bike or Die 2 v2.1b (database `BikeOrDie`, creator `BiKD`) is a real TX workload
for native rendering, guest OS work, input and audio. The supplied PRC and TX
session are private prerequisites. No firmware, PRC, session, screenshot, audio
capture, registration information or extracted guest code belongs in Git.

## Prepare a private gameplay fixture

Use the [developer runner](developer-run.md) and a completed, calibrated TX
session. Build with `heavy build make -j2 out/frond`. Install the supplied app
into a new private output directory:

```sh
python3 tools/frond-run.py --session /private/tx-ready.session \
  --install /private/BikeOrDie.prc --app BikeOrDie --headless \
  --settle 5 --screenshot 0 --screenshot 15000 --run-for 16 \
  --output /private/bike-install
```

Wait for a successful `result.json` with `session_saved: true` before using
`working.frond` as the next input. An unsuccessful or still-running operation
may leave that file containing the original session.

Inspect the captured screen. In this investigation the app showed About with
**Continue in trial mode** and **Unlock**, without a license agreement. Choosing
trial mode entered the animated intro; no unlock, registration, purchase or new
license acceptance was performed. If another version presents a license, obtain
authorization before accepting it.

Continue from the saved copy with `--resume-app`, rather than relaunching the
app: relaunching shows About again. On the supplied calibrated TX, trial mode
was reached by a pen-down at `(160,295)`, followed by pen-up 200 ms later. Allow
the intro to advance, then tap `(160,200)` and release 200 ms later. The resulting
fixture showed level 1, **PAUSED**, and the stationary bike at
its starting position. Inspect every transition; do not blindly replay these
setup taps over an unexpected screen. The provided TX preferences session did
not need recalibration in this run.

Save this state privately as `level1-paused.frond`. The committed
[gameplay replay](../tests/fixtures/bike-level1.replay) presses Up, then Right,
then releases them. It exercises motion, flag collection and additional hardware-key
input, rather than completing the tutorial. Screenshots confirmed those actions
through guest input handling. A successful runner exit alone is not gameplay
evidence.

## Fixed guest-time replay

```sh
heavy build make -j2 benchmark-replay
sha256sum /private/level1-paused.frond tests/fixtures/bike-level1.replay \
  build/benchmark-replay > /private/bike-inputs.sha256
heavy emulator ./build/benchmark-replay /private/level1-paused.frond \
  tests/fixtures/bike-level1.replay 10000 100 on /private/bike-before
```

Arguments are session, replay, guest milliseconds, virtual MIPS budget,
`on|off` for PCM scheduling, and a **new** output directory in an existing parent.
An optional final integer sets the Palm-epoch seconds at replay start; the default
is 2026-10-26 00:00:00. The benchmark replaces `timeutil.o` only in this executable
so RTC advances with restored guest time. Production RTC behavior is unchanged.

The benchmark uses fixed 1 ms work requests and dispatches each event at the
first boundary at or after its guest-time deadline. Actual dispatch times are
recorded in `inputs.csv`; small cycle rounding/overshoot is included in results.
Unlike the SDL frontend's host-time replay, it does not use an adaptive governor.
The 100 MIPS setting is an experimental virtual budget, **not** a TX hardware
clock or a target inferred from browser/E2 reports.

The benchmark requires a complete TX session with networking disabled and no SD
image/inserted card. It restores the saved foreground state directly, without
Palm API injection or relaunch. Pen and hardware-key replay commands use normal
SoC queues; infrared-keyboard commands are unsupported. `snapshot` destinations
are redirected to numbered PNGs inside the new mode-0700 result directory.

`summary.json` records process CPU/wall time after restore, guest time/cycles,
LCD frame counts, a frame-sequence hash including guest timestamps, and PCM
count/hash/peak/silence. `frames.csv` records each observed pending LCD frame.
Observation, hashing and requested PNG writes are included in measured cost;
use the same tool and replay for both variants. Hashes are non-cryptographic
regression checks, not proofs that every guest state is identical. Keep all
output private.

Repeat identical inputs for each binary and each sound setting:

```sh
python3 tools/analyze-gameplay.py /private/bike-before
python3 tools/analyze-gameplay.py /private/bike-before --compare /private/bike-after
```

The comparison fails if guest progress, frame sequence or PCM results differ.
Compare sound on against sound on, and off against off: disabling sound changes
codec scheduling and the guest's executed workload. `off` still models a slower
codec and can produce PCM internally; it does not mean every audio device or
interrupt is disabled.

## Frontend pacing and diagnostics

For normal adaptive pacing, apply the optional diagnostic patch to a prepared
source tree and rebuild:

```sh
git apply --directory=build/source tools/debug/gameplay-timing.patch
heavy build make -j2 out/frond
FROND_GAMEPLAY_TRACE=1 python3 tools/frond-run.py \
  --session /private/level1-paused.frond --resume-app --settle 0 \
  --replay tests/fixtures/bike-level1.replay --run-for 10 --headless --sound \
  --output /private/bike-wall
python3 tools/analyze-gameplay.py /private/bike-wall
git apply --reverse --directory=build/source tools/debug/gameplay-timing.patch
heavy build make -j2 out/frond
```

The runner takes its own `heavy emulator` slot. Do not wrap it in another slot.
The analyzer measures from the first non-snapshot event through the final replay
event, excluding the subsequent session save/export/shutdown. A final snapshot
is a useful explicit end marker. Changed-frame intervals describe LCD observations,
not necessarily the game's internal frame rate. First frame change after input
is a visual-response proxy; inspect screenshots to establish causality. Frontend
runs have different adaptive instruction budgets and host-clock RTC values, so
frame/PCM hashes are not expected to match across them.

`--headless --sound` uses SDL's dummy audio callback. It measures callback pacing
and reported underruns, and the fixed benchmark checks generated PCM, but neither
establishes sound-server latency or audible quality on a real desktop.

For guest execution profiling, apply `tools/debug/guest-execution-profile.patch`
and rebuild the benchmark. Run with `FROND_EXECUTION_PROFILE=1`. It counts ARM,
Thumb and host PACE instruction attempts and samples 256-byte PC bins once per
1021 attempts per instruction set. `EXEC_TRAP` counts dispatched PACE system
calls. PACE's modeled cycle charge differs from ARM/Thumb: do not interpret
instruction-count shares as host CPU shares. This diagnostic patch adds hot-loop
overhead; never use its timings as production performance evidence. Reverse it
and rebuild after profiling.

`FROND_HOST_PROFILE=1` enables a Linux/x86-64 SIGPROF sampler in the standalone
benchmark. It writes `host-pc.csv` and private process mappings. Resolve the
`executable_relative_pc` column with `addr2line -f -C -e` and the exact profiled
binary; library addresses will not resolve against the executable. Profile runs
are separate from comparative timings.

## Findings and validation

The baseline is Frond commit `1c03bf2`. The host was an Intel Xeon E5-2620 v4
with 24 online logical CPUs and the `performance` frequency governor. Physical
TX comparison remains deferred because no device is available.

Guest execution counts were approximately 99.1% ARM, 0.8–0.9% Thumb and
0.02% host PACE. About 65–67% of sampled instruction PCs mapped to the supplied
app's `armc` resource 2; matching private resource bytes established that mapping.
About 28–32% fell in the mapped TX OS image. Dominant app loops render pixels;
OS hotspots include a halfword/word copy loop and an event-wait loop. Thus PACE
acceleration is already active, and rendering plus OS activity both matter.
Host sampling identified instruction-cache lookup and MMU translation costs.
The [upstream developer discussion](https://www.reddit.com/r/Palm/comments/1pjfphg/cloudpilotemu_20_released_with_palmos_5_emulation/)
provided profiling hypotheses, not TX performance targets.

Patch `0037-bike-performance.patch` inlines native CPU hits in the existing MMU
TLB and shares the existing write-permission check. Misses retain the page walker.
It adds no translation state, no guest-code patch, no firmware/app signature and
no skipped guest work. PACE, Emscripten callers, invalidations and session formats
retain their existing paths and semantics.

The ten-guest-second replay at the fixed budget produced the same guest progress,
frame-sequence hash and PCM hash across baseline and changed builds for each
sound mode. Sound on produced 448 observed LCD frames (447 changed) and 443007
PCM frames; sound off produced 419 (417 changed) and 147001 PCM frames. This
sound-dependent difference is why cross-mode speed ratios are not valid.

Timing results from overlapping builds, SD acceptance, serial probes or simultaneous
frontend/fixed benchmarks are contended and excluded from speedup claims.
In a coordinated quiet window, three alternating pairs per sound mode used the
same logical CPU 7 (its SMT sibling 23 was offline). Both shared emulator slots
were held per pair, and build slots were monitored. The identical ten-guest-second
level-1 replay cost these process CPU seconds, excluding restore:

- Sound off: before 18.186, 16.777, 16.774; after 15.348, 15.270, 15.506.
  Median paired reduction **9.0%**, range 7.6–15.6%.
- Sound on: before 17.301, 16.915, 17.289; after 15.928, 15.213, 17.547.
  Median paired reduction **7.9%**; individual pairs ranged from a 1.5% regression
  to a 10.1% reduction. The regression is retained, not discarded as an outlier.

The coordinated window ran from 14:56:06 to 15:02:52 MDT on 2026-10-26.
All 20 runs used CPU 7; no concurrent heavy build was observed, and the emulator
locks prevented another emulator from running during a measured pair. Ordinary
browser, Node, AI harness and desktop services remained present. Across whole
runs, aggregate host CPU busy time was 6.6–15.9%, CPU 7 busy time 90.3–99.6%,
and one-minute load average 2.20–6.53. Per-run median sampled CPU frequency was
2.64–2.94 GHz. The sound-on third changed run coincided with the largest host
activity spike; its source was not identified by the process snapshots. It is
retained in the results. Thus the window isolated the measured heavy jobs, not
all host activity.

Two normal-frontend pairs per sound mode measured approximately eight wall
seconds from the Up press through the final snapshot:

- Sound off: changed LCD observations 253/258 before, 273/269 after. Median
  intervals were about 32.1 ms before and 31.8 ms after; p95 intervals were
  50.8–55.5 ms before and 43.3–47.2 ms after. First LCD change after Up stayed
  around 86 ms. Median governor budgets rose from 57/59 to 64/62 virtual MIPS.
- Sound on: changed observations 243/241 before, 273/275 after. Median intervals
  were 33.0–33.4 ms before and 31.8–32.0 ms after; p95 intervals were 51.7–52.2 ms
  before and 40.9–46.5 ms after. First LCD change was 100–110 ms before and
  86–97 ms after. Median governor budgets rose from 59/59 to 65/66 virtual MIPS.
- The frontend advanced about 7.87–7.97 guest seconds while using 7.48–7.59 CPU
  seconds in these windows. Adaptive pacing spends the available CPU headroom;
  these runs do not perform identical guest instruction workloads.
- Dummy SDL audio reported 10/12 underruns before and 14/10 after. There is no
  demonstrated sound-quality improvement. Actual sound-server output was not
  captured or listened to.

These are short workload measurements on a shared desktop, not a dedicated host
or a universal speedup. Frequency was not fixed, and background services remained
active. The fixed benchmark still takes substantially longer than ten host seconds
at this budget. Trial-level gameplay is demonstrated; sustained playability,
locked levels, physical TX equivalence and audible desktop output are not established.

Validation includes `heavy build make -j2 test`, `tests/developer-run.py`,
`tests/gameplay-metrics.py`, matching repeated private gameplay/PCM hashes, and
patch application checks. The MMU regression covers disabled translation,
section/page/tiny-page translations, write permissions and live domain changes,
faults, generation wrap and remapping. Existing alignment, cache, E2/TX state,
network, audio and other core tests pass. The optional GDB client smoke test was
skipped because `gdb-multiarch` was unavailable. The new analyzer regression excludes
session-save stalls from gameplay frame pacing.
