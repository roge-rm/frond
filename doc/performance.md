# Performance

How to measure speed, latency and audio, and which diagnostic patches exist. Command details are in [developer-reference.md](developer-reference.md). Sessions, apps and captures are private and stay out of Git. Compare runs only on the same binary, session, replay and host conditions, and write down the host load. Results depend on the host, so apart from the comparison with CloudpilotEmu they are not listed. Measure on your machine.

## Engines

Each machine runs on the Exact engine (the interpreter) or the Fast engine (the Dynarmic JIT, built in on x86-64 and arm64 unless `FROND_FAST=0`). `--engine exact|fast` picks one, default exact. `FROND_JIT=1` or `0` in the environment overrides both. A debugger attached to a machine runs it on Exact. The Palm IIIx, Dana and Dana Wireless (`frond-68k`) only have the Exact engine, there is no JIT for 68k and the setting is hidden for them.

Fast interprets code until a block has been reached a few times, then compiles it on a background thread. Tuning variables, for measuring and tests only:

| Variable | Meaning |
| --- | --- |
| `FROND_JIT` | `1` Fast, `0` Exact. Overrides `--engine`. |
| `FROND_JIT_HOT` | Times a block is reached before it is compiled. Default 4, `1` compiles everything at once. |
| `FROND_JIT_THREAD` | `0` compiles in place instead of on the thread. |
| `FROND_JIT_SYNC` | `1` waits for each compile where it was asked for, so runs stop at the same places every time. The benchmark tools and tests turn this on themselves. |
| `FROND_JIT_AHEAD` | How many links ahead of a requested block the thread also compiles. Default 1, `0` is off. |
| `FROND_JIT_MAX_UNCHECKED` | Longest run of guest instructions between checks of the cycle budget. Default 64, `0` no limit. |
| `FROND_JIT_STATS` | `1` prints compile and exit counters when the process ends. |

Correctness against the interpreter: `make test-jit-differential` and `make test-jit-overshoot` (see [testing.md](testing.md)). The benchmark below runs Exact unless `FROND_JIT=1` is set, so run it both ways and compare speed and hashes.

`make frond-jit` copies `out/frond` to `build/frond-jit`, kept for older scripts. `make benchmark-replay-jit` does the same for the benchmark.

## 68k timing

`frond-68k` runs the Palm IIIx's 68000 with the clocks the MC68000 manual gives (section 8) instead of a flat cost per instruction. Each opcode has a clock count and a count of program reads. Data-dependent parts are worked out as the code runs: taken or not taken branches, shift counts, MOVEM register counts, MULU, MULS, DIVU and DIVS operands, and exceptions with their frame writes.

- Wait states: each address bank has byte and word waits, set from the DragonBall EZ chip selects (CSA for ROM, CSD or DRAMC for RAM) when the guest writes them. Operand accesses add the wait, instruction fetches add the program reads times the wait. `bus-wait` shows them.
- Bus time: LCD DMA and DRAM refresh take bus cycles from the CPU, worked out from the registers the guest programmed (line length from LRRA, LXMAX, PCD and the PLL dividers, burst length, refresh period). `bus-timing` shows the figures and the LCD frame rate.
- The 328 and VZ chips use the same cycle table. The EZ has the full bus model; the VZ one (chip select waits, EDO, fast page and SDRAM page hits) is only unit-tested against the manual. The 328 gets no bus timing.
- Not modelled: collisions between DRAM refresh and a single access.
- `Original` runs the machine at the IIIx's real 16.58 MHz (cycles and timers, `speed-state` says 100%). `Uncapped` runs the CPU 8 times faster, or as fast as the host allows, with timers and the clock on real time. Set it in Edit settings, the Display panel or sheet, `--speed original|uncapped` or the `speed` console and socket command.
- Original is worked out from the clock and the manual's tables, there was no real IIIx to time it on. The timing probe (`tests/palm/timing68k-probe.c`) runs a dozen short loops (branches, calls, reads, MULU, DIVU, DBRA, shifts, MOVEM, ROM and register work) and `python3 tests/timing68k.py --rom ROM --output DIR` compares each loop's measured clocks with the manual's count plus the wait states and bus time. They agree within 1%, allowing for the 100 Hz tick interrupt. `make test-frond-68k` runs the table test.
- Host cost: Original is mostly asleep after boot (a few percent of one core); Uncapped is bound by the host. A debugger costs about 3% host time while attached and almost nothing while no gdb is connected. FrondBench runs on the OS 3.5 and 4.1 ROMs (it needs 3.5), at Original and Uncapped; it is Frond's own, so there are no real IIIx scores to compare against.

## Fixed guest-time benchmarks

`make benchmark-replay` builds `build/benchmark-replay`, which restores a session and runs a replay for a fixed amount of guest time with no wall-clock pacing:

```sh
./build/benchmark-replay SESSION REPLAY GUEST_MS MIPS on|off NEWDIR [PALM_EPOCH_SECONDS]
```

- `MIPS` is the virtual CPU budget, not a hardware clock. `on|off` is PCM scheduling: compare on with on and off with off, because off changes what the guest runs.
- It needs a complete TX session with networking off and no SD card. Events are sent through the normal pen and key queues at the first 1 ms boundary after their time. The RTC follows guest time so runs repeat.
- `NEWDIR` gets `summary.json` (CPU and wall time after restore, guest time and cycles, LCD frame counts, frame-sequence hash, PCM count, hash, peak and silence), `frames.csv`, `inputs.csv` and numbered PNGs for `snapshot` lines. Observation and PNG writing are in the measured cost.
- `BENCH_DUMP_RAM=1` also writes guest RAM at start and end (`ram-start.bin`, `ram-end.bin`).
- `python3 tools/analyze-gameplay.py DIR [--compare OTHERDIR]` summarises a run and fails if guest progress, frames or PCM differ. Hashes catch regressions, they don't prove identical state.
- `FROND_HOST_PROFILE=1` (Linux x86-64) samples the host PC into `host-pc.csv`. Resolve `executable_relative_pc` with `addr2line -f -C -e` on the same binary. Profile in separate runs from timings.

Workloads:
- A game level: `tests/fixtures/bike-level1.replay` (10000 guest ms) on a paused level-one session you make with the runner (install the app, resume it with `--resume-app`, save). Check the screenshots to see the replay really plays.
- `make frondbench` builds `build/frondbench/FrondBench.prc`, a Palm benchmark with Integer, Memory, Drawing and Database tests (needs `m68k-palmos-gcc`, `pilrc`, `build-prc` and `PALM_SDK`). Run it once in the guest, save the session, and replay `tests/fixtures/frondbench-run-tx0414.replay` (40000 guest ms) from it. Its Run tap is placed for the reference session, which was calibrated by hand; `frondbench-run.replay` is the same tap for cores before 0414.
- `make benchmark-menu` builds `build/benchmark-menu`; `./build/benchmark-menu SESSION [MIPS]` runs 24 guest seconds of a playing SunVox session with the menu closed and reports CPU cost, guest latency and the first menu region's hash.
- `make benchmark-session` builds `build/benchmark-session`; `./build/benchmark-session SESSION SECONDS MIPS [NEW.raw]` runs a playing session without pacing and drains the codec directly, to compare execution cost apart from SDL starvation. Silence statistics skip the first 132300 frames. It rejects sessions with an embedded card.

`make speed-check` runs both workloads through `benchmark-replay` and prints host time per guest second, host MIPS, frame and PCM hashes and the FrondBench times per test. JSON goes to `build/speed-check/<timestamp>/speed-check.json`. Set `BIKE_SESSION` and `FRONDBENCH_SESSION` to your session files (no defaults; a workload without one is skipped), `SPEED_MIPS` (default 100) and `SPEED_BENCH` (another benchmark binary). `SPEED_HOST=user@host` runs it on another machine over ssh in `~/frond-speed/` (`SPEED_SSH_KEY` picks a key). `SPEED_REAL_MS=Integer=1000,Memory=500,...` adds your real device's FrondBench times and a ratio column. Host time per guest second under 1 means faster than real time at that MIPS.

## Input and menu latency

These tools measure from emulator input enqueue to PCM callback or screen presentation. They leave out the stylus, the sound server, the output device and display scanout. They need debug patches, see below. Two are in `tools/debug/`, the others in git history (see below).

- Keyboard onset: install `tests/fixtures/frond-latency.pdb` in SunVox, load `A:/frond-latency.sunvox` (Generator straight to Output, short release), stop playback, open the input area so the keyboard sits at y=151, and save a session. Take the input latency patch (`input-latency-trace`) from git history, refresh it against the current source, apply it, then:

```sh
FROND_LATENCY_PCM=out/keyboard.raw ./out/frond IDLE_SESSION --run-for 27 \
  --input-replay tests/fixtures/frond-latency.replay > out/keyboard.log 2>&1
python3 tools/analyze-input-latency.py out/keyboard.log out/keyboard.raw --y 151
```

  Onset is the first sample of magnitude 100 or more after a quiet baseline, and the analyser stops at the next press. Expect about one audio callback of uncertainty.
- Guest stages: with `tools/debug/guest-audio-stages.patch` as well and `FROND_GUEST_TRACE=1`, `tools/analyze-guest-latency.py LOG RAW` splits the delay into render and DMA. The patch has addresses specific to one app and firmware, so check them first.
- Menu response: with the menu response patch (`menu-response-trace`, from git history) and `FROND_MENU_TRACE=1`, start with the SunVox menu closed and run `tests/fixtures/menu-response.replay` for 26 seconds, then `python3 tools/analyze-menu-response.py LOG`. It looks for a new menu framebuffer followed by presentation. Check the screenshots too.
- `--fast-audio` and `--low-latency-queue` take the same protocol, to compare with the normal path. A shorter queue lowers response time and makes playback more sensitive to host scheduling.
- Count missing responses as failures. `check-native-audio.py --cpu N` pins the emulation thread, and it records each thread's scheduling policy, priority and CPU time every second so you can see whether a real-time policy was granted. Pair latency with a PCM continuity check.

## Audio diagnostics

The qualified E3 preview uses the shared E2 HAL layout and its own ARM Sound Manager stream-start signature. It previews at HAL write, with the Thumb queue hooks off. Other E3 firmware uses the shorter queue without the preview.

The qualified 6 MiB E2 de-NVFS image also uses the E2 HAL layout with its own ARM stream-start signature and the Thumb queue hooks off. Qualification hashes the running NOR, including when it is embedded in a session. A changed image uses the shorter queue.

Native audio backpressure pauses the whole guest (timers, DMA and codec) until SDL drains its queue, while input, drawing and the control socket keep running. The frontend runs 120 Hz slices, and with `--fast-audio` it pauses above 6 host periods of queued frames and resumes below 4 (3072 and 2048 at the desktop's 512). `--low-latency-queue` does the same without the preview, only while non-silent frames were queued in the last 100 ms, so a device that queues silence all the time keeps the normal limits until something plays.

- `tools/check-native-audio.py SESSION NEWDIR --seconds N [--continuous] [--mips N]` records PCM, SDL underruns, host load and the governor's `budget_mips_samples`. `--mips` is a ceiling, and the governor can still go lower under load, so compare the samples before blaming a change.
- With an audio trace patch (`audio-timing-trace` or `audio-buffer-trace`, from git history), `python3 tools/analyze-audio-timing.py LOG` reports HAL writes, sampled DMA contents, codec silence and FIFO starvation, sound-stream lifecycle and backpressure intervals (host and guest time; guest time should hold still while the frontend waits). `tools/analyze-audio-buffers.py LOG` adds callback and mixer durations and DMA transfer counts. Neither gives a continuity verdict. A nonzero HAL buffer followed by silence in DMA without FIFO starvation points upstream of SDL.
- Too little guest CPU budget can empty a stream queue or let DMA reach a cleared buffer before mixing finishes. A higher budget can fix one workload and is not a safe universal floor.
- Fast audio: the SDL underrun counter makes the core fall back to normal audio until reset or restore, with a brief gap. The guest probe `python3 tools/build-palm-probe.py --probe audio` plus `tools/check-audio-probe.py` checks callbacks and API results only, not host delivery.
- Guest execution profile (`guest-execution-profile` patch, from git history): `FROND_EXECUTION_PROFILE=1` counts ARM, Thumb and PACE instruction attempts and samples PC bins. PACE's cycle charge differs from ARM and Thumb, so counts are not host CPU shares.
- Instrumented builds change timing. Time uninstrumented binaries separately.

## Against CloudpilotEmu

Measured 2026-11-04 on a Ryzen 7 5700X, gcc 14.2, `-O2 -g` on both sides: Frond main at 1b9d14d and the IIIx at f550284 against unmodified CloudpilotEmu 2.3.0 (064d5d5, the pin in `vendor/cloudpilot`). Five runs of each, alternating sides, nothing else running. Upstream's runners open a window and pace to real time, so both sides ran under the same small headless driver linked against each tree's own core, using only its public API (boot, pen taps and keys, install and launch, a free-running loop, a RAM dump for FrondBench's results). Neither core was changed. The driver isn't in the repo.

Tungsten E2, FrondBench through PACE, 100 MIPS, guest 37 to 65 s. Fast is `FROND_JIT=1` with the compile thread on, as the app runs it.

| | Host MIPS best / median | Host s per guest s | FrondBench total |
|---|---|---|---|
| CloudpilotEmu uARM | 177.7 / 177.2 | 0.564 | 30030 ms |
| Frond Exact | 182.6 / 179.7 | 0.556 | 30020 ms |
| Frond Fast | 361.2 / 358.0 | 0.279 | 29960 ms |

Tungsten E2, Bike or Die 2 v2.1b in trial mode, level 1 with the TX's sustained key replay, 100 MIPS, guest 45 to 85 s. The game isn't frame-identical between cores, probably as the clock chip follows host time, but the level, section and input are the same.

| | Host MIPS best / median | Host s per guest s |
|---|---|---|
| CloudpilotEmu uARM | 115.6 / 113.9 | 0.878 |
| Frond Exact | 159.5 / 156.6 | 0.639 |
| Frond Fast | 323.9 / 313.3 | 0.319 |

Palm IIIx, FrondBench on the OS 3.5 EZ ROM, from the Run tap until the results are in RAM. The checksum is 1995F512 in every run on both cores.

| | Guest s | FrondBench total | Host s best / median | Host s per guest s |
|---|---|---|---|---|
| CloudpilotEmu (clock factor 0.5) | 270.0 | 269800 ms | 5.33 / 5.35 | 0.0198 |
| Frond Original | 510.8 | 510460 ms | 8.33 / 8.44 | 0.0165 |
| Frond Uncapped (8x) | 63.5 | 63170 ms | 8.04 / 8.31 | 0.131 |

- On the E2, Exact is about 1.5% faster than upstream on FrondBench and 1.37x on Bike or Die, and Fast about 2x and 2.75x. On the TX on the same machine Bike runs at 186 to 200 MIPS in Exact and about 575 in Fast.
- On the IIIx, Frond times the 68000 by the manual's clocks plus the EZ's wait states, LCD DMA and refresh (see [68k timing](#68k-timing)), so the same work takes 1.89x upstream's guest time; upstream runs guest code about 1.9x faster than a real IIIx. The same work takes about 1.58x upstream's host time, a few percent of it the GDB hooks. At real time a IIIx uses under 2% of one core, and Uncapped runs about 4x faster than upstream's default at about the same host cost.
- E2 only for ARM. CloudpilotEmu normally runs in a browser as WebAssembly, which wasn't measured and would be slower than its native build.

## Debug patches

`tools/debug/` keeps two optional tracing patches, never applied by the build. Apply one to the prepared source, rebuild, and reverse it before timing or packaging:

```sh
git apply --directory=build/source tools/debug/NAME.patch
make -j4 out/frond
git apply -R --directory=build/source tools/debug/NAME.patch
```

| Patch | Variables |
| --- | --- |
| `guest-audio-stages` | `FROND_GUEST_TRACE` |
| `guest-pcm-trace` | `FROND_PCM_TRACE` |

Older tracing patches (input-latency, menu-response, audio-timing, audio-buffer, performance-audio, frontend-stall, gameplay-timing, guest-execution-profile, tx-power, tx-serial, sunvox-abort) are in git history. To use one, take it from there (`git log --diff-filter=D --name-only -- tools/debug`, then `git show COMMIT^:tools/debug/NAME.patch`), refresh it against the current source and use it the same way. Check a patch with `git apply --check --directory=build/source`. Patches with overlapping hooks are not meant to be combined. Logs and captures stay out of Git.
