# Benchmarking Bike or Die 2

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

Inspect the captured screen and follow the game's setup prompts. Continue from
the saved copy with `--resume-app` to preserve the current screen. Save a paused
level-one session for repeatable comparisons.
Save this state privately as `level1-paused.frond`. The committed
[gameplay replay](../tests/fixtures/bike-level1.replay) presses Up, then Right,
then releases them. It exercises motion, flag collection and additional hardware-key
input, rather than completing the tutorial. Inspect screenshots to verify that the replay is exercising gameplay; a
successful runner exit alone does not establish that.

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
An optional final integer sets the Palm-epoch seconds at replay start. The
benchmark uses a controlled RTC so repeated runs advance with guest time;
production RTC behavior is unchanged.

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
