# Testing

Command details (options, console and socket commands, the runner) are in [developer-reference.md](developer-reference.md). Firmware, sessions and installed apps are private: tests read copies, put results in a new directory under `build/` (mode 0700) and skip when an input is missing. Never commit any of it.

## Test levels

| Level | Command | Needs |
| --- | --- | --- |
| Reconstruction | `make test-build-tools check-reconstruction` | nothing |
| Core | `make -j4 test` | nothing |
| Desktop | `make test-desktop` | nothing (set `QT_QPA_PLATFORM=offscreen` without a display) |
| Smoke | `make smoke` | firmware, ready sessions |
| Release only | the real-firmware and real-app checks below | firmware, SDK, apps |

Quick checks after a change: the focused `make test-NAME` target for what you touched, `make check-reconstruction` if you changed `patches/`, and the build or compile. Put wider checks, smoke and device checks on the project's before-release list and run that list once before release.

The first `make` fetches Dynarmic (needs network, Ninja and Boost). `FROND_FAST=0` skips it, and the Fast engine tests with it.

## Firmware-free tests

`make test` builds each test against the real core with synthetic ROM, RAM, NAND and card data, so it needs no firmware. It covers CPU alignment and conditions, instruction cache, MMU and memory regions, PACE and the GDB stub, session v7 and state round trips, audio queues and lifecycle, SD, Memory Stick, USB, serial and IrDA, Bluetooth, networking, keyboard, device profiles and controls, checkpoints and database export. `make -n test` lists the targets, and `make test-NAME` runs one.

- `make test-desktop` builds the Qt desktop with tests and runs ctest: the control-socket client and the save, export and settings workflows.
- `make test-build-tools` tests staging safety and package policy with synthetic data. It does not package Frond.
- `make check-reconstruction` prepares the core twice in separate temporary directories from the pinned upstream and `patches/`, compares file hashes and modes, and checks them against `build/source`. A matching prepare stamp alone doesn't show that `build/source` is unmodified, so run it before judging a build. Core changes go in a new patch after the last one, then run this.
- `make test-jit-overshoot` and `make test-jit-differential` run with `make test` when the Fast engine is built. The differential test runs random ARM and Thumb blocks on both engines and compares them, plus directed cases: seeds 1 to 30000, then two further ranges of 10000. For a long run use `make test-jit-differential JIT_DIFF_ARGS="--seeds 1: --duration 3600"`, or `--seed N` for one case.
- `make test-zodiac-uart-startup` checks disabled UART1/2 control, independent reset/readback, guarded effects and Exact/Fast startup.
- `make test-zodiac-graphics-commands` checks bounded native ring packets, delayed solid RGB565 paint, pointer and busy state, rejected-batch atomicity, clipping and byte order.
- `make test-zodiac-graphics-acquisition` checks the virtual raw-axis input, guest-time acquisition, reset/cancellation, tagged FIFO, GPIO source selection and real timed helper readback/identity transfers on Exact and Fast.
- `make check-device-data` checks the generated per-device data against the profiles.
- `make test-compatibility` runs synthetic 68k and ARM/Thumb programs through the core: mixed-width memory, branches, returns, traps, faults and an ARM to Thumb to ARM call chain. These are bounded checks, not ISA conformance tests.

## Real-firmware checks

All of these use your own firmware and a ready session, and say what they skipped. Palm OS SDK, `pilrc` and the m68k and ARM toolchains are needed to build the probe apps (`python3 tools/build-palm-probe.py --probe NAME --sdk DIR`, probes are listed in its `--help`). Probe apps and sessions stay under `build/` and are never committed.

Smoke (`tests/smoke.py`):
- `make smoke-sessions` cold boots each device through setup and saves a ready machine folder in `build/smoke/machines/DEVICE/` (about 2 minutes per device).
- `make smoke` per device restores that session at the launcher, opens an app and taps it (the screen must change), saves and loads it in a new process, reads a file from an inserted SD card or Memory Stick with the probe app, and checks that the audio probe plays non-silent PCM. Devices without a session or firmware are skipped with the command that makes the session. Frames and logs are in `build/smoke/work/DEVICE/`.
- `FROND_CORE` picks another core binary, `FROND_DEVICE_IMAGES` is the firmware folder; without it the firmware steps skip. Sessions saved before the folder layout (`build/smoke/DEVICE.session`) still load. `tests/zire72-rom.py` also needs `FROND_DEVICE_IMAGES`.
- Zire 72: a setup reminder comes due when an old session loads (the guest clock follows the host). Smoke taps OK on it first.

Other checks:
- Boot check: run `./out/frond` with the device's firmware, `--no-sound --run-for 60 --framebuffer-output FILE` and `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`, then look at the picture. A zero exit status only means the run finished, so check that the guest reached a usable screen (a fresh TX shows Date & Time setup). Boot time depends on the host.
- `tests/device-rom.py --device NAME --nor FILE [--nand FILE] [--launcher] [--resets]` checks detection, rejection, boot and (with `--launcher`) setup and a saved-session restore in a new process. `--synthetic-rejection` needs no firmware. `tests/zire72-rom.py` does the Zire 72, and `tests/e2-rom.py --rom NOR --nand NAND --output NEWDIR` boots an E2 for 60 seconds, saves, restores in a new process and saves again, with optional `--input-replay`.
- Developer-runner checks (`tests/developer-run.py` is firmware-free, the runner itself is `tools/frond-run.py`): install, launch, replay, screenshot and named log export in a private session copy.
- SDK compatibility probe: `make test-compatibility` then `python3 tools/build-palm-probe.py --probe compatibility --sdk DIR` and `python3 tools/check-compatibility.py --session READY --output NEWDIR`. It runs twice, the second time from the first run's saved session, and checks memory, databases, preferences, ticks, drawing, native ARM and Thumb calls, the event queue and a real pen tap on a control. Logs carry a unique token so an old log can't pass. `--nor` and `--nand` cold boot the first run.
- App matrix: `python3 tools/app-compatibility.py --device NAME --manifest PRIVATE.json --output NEWDIR`. The manifest pins the core, fixtures, installs and replays by SHA-256 (`--help` shows the format). Stages are cold-boot, launch, interaction, persistence and restore, each with assertions (`database-records`, exact record hashes) or `visual-review` (always ends as `needs-review`). `execution_status` and `coverage_status` are separate, and skipped or blocked stages are never passes.
- Lifecycle and power ROM tests, built by `make test-tx-lifecycle-rom` and `make test-tx-power-rom` and run by hand with `--plain-session` / `--audio-session` / `--output` or a session path. They restore awake and sleeping sessions into fresh SoCs and check the CPU stays still in sleep, no new PCM and no card changes. Exit 0 is a pass, and for the power test exit 2 means it woke without a wake event.
- PCM continuity: `tools/check-native-audio.py SESSION NEWDIR [--seconds N] [--continuous] [--replay FILE] [--mips N] [--cpu N] [--binary FILE]` captures the emulator's own PulseAudio stream (needs `pactl` and `parec`; PipeWire works) and records SDL underruns, host load and thread scheduling. `--continuous` is only for a known continuous session: it requires nonzero, unclipped PCM and no silent 10 ms blocks after 3 seconds. Look at the screenshots to confirm menus responded, and listen to the recording. A monitor capture excludes output device latency. `tools/check-audio-probe.py LOG.zip` checks the audio probe's guest log.
- Input replay parsing: `python3 tools/test-input-replay.py ./out/frond NOR NAND` (TX) checks that bad replays fail before the guest runs. Replay format: see the developer reference.
- `python3 tools/tx-baseline.py --session S --nor N --nand D --output NEWDIR` (TX) runs the synthetic suite, a cold boot, the desktop-control check and optional SD, network, Wi-Fi and audio probes. Omitted inputs are skipped.
- Desktop and peripheral acceptance with the TX or E2 and a probe app: `tests/desktop-control.py`, `desktop-sd.py`, `desktop-network.py`, `desktop-host-network.py`, `desktop-keyboard.py` and `desktop-idle.py` (each has the build and run lines in its header). Large cards: `tests/large-card-rom.py DEVICE MIB...`. TG50 Memory Stick: `tests/ms-classic-rom.py MB...` and `tests/ms-pro-rom.py MIB...`. TG50 USB HotSync with pilot-link: `make test-tg50-usb-hotsync`. These need `make smoke-sessions` first.

Peripheral qualification (`tools/qualify-peripherals.py --plan PLAN.json --output NEWDIR [--timeout N]`) runs staged JSON plans against private copies of sessions, for transfers between two machines (IrDA, Bluetooth, serial relays).
- A plan has `nodes` (name to core and session), optional `inputs` (files hashed before and after) and ordered `steps`. `$OUT` expands to the result directory.
- Each step has a unique `id` and one action: `command` (a control-socket command, `retry_ready`, `expect_code`, `expect_message`), `replay` (inline replay text), `service` (a relay or peer process, logged to `ID.log`, `allow_exit`), `wait_log` (a marker in a log), `wait` (seconds), `stop` (a service) or `check` (a checker that exits 0 and prints JSON with `status: "pass"`).
- Step timeout defaults to 30 s, node `startup_timeout` to 60 s and `command_timeout` to 10 s. Raise them for slow operations such as database export.
- Export the sender and receiver logs before each attempt and give them to the checker (`tools/check-irda-probe.py`, `tools/check-bluetooth-probe.py`) so an old success can't pass a new attempt.
- `result.json` has the failing step, replies, checker verdicts, timings, input hashes and artifact hashes. On failure it tries to save each live guest. `make test-peripheral-qualification` tests the runner without firmware.
- A runner pass is not a device qualification: a new device still needs its own firmware check.

## Real-app checks

For a heavier workload than the probes, install the app with `install FILE` and `launch DatabaseName` (the Palm database name, case included) or use the runner, once the guest is at its event loop. If install reports `not currently possible`, let startup finish and retry.
- A 68k game (SimCity): create a city, place a zone, and check the simulation advances and menus respond.
- A native ARM synthesizer (SunVox): launch it, pick a theme, select Generator and play two keys an octave apart. Open the input area first, since it moves the controls.
- Sustained playback: load the shared fixtures in `tests/fixtures/` (see its README): the Echo project for continuous sound, the dry one for note onsets. Test Play, Stop, replay and menus.
- Native sound apps (Acidulite): play after Stop and after Bench, with `tests/fixtures/acidulite-controls.replay` and `check-native-audio.py --seconds 60 --export-databases`, without `--continuous`.
- Record the app version, firmware, settings and session so later runs use the same workload. Keep RAM exports and recordings out of Git, they hold every installed database.

## Packages and CI

- `make deb` builds the amd64 package, `make deb-arm64` the arm64 one (Docker only, built from a fresh core). `python3 tools/check-package.py [--architecture arm64] FILE.deb` checks version, architecture, dependencies, ownership, modes and the full file list without installing it.
- `.github/workflows/validate.yml` runs by hand (`workflow_dispatch`) on Debian trixie: `make test-build-tools check-reconstruction check-docs`, then `make -j2 test test-desktop`, then `check-reconstruction` again. It uploads nothing and uses no firmware.
- The Qt tests run offscreen. They don't replace trying the desktop on a real display, installing a package on a clean system, or the firmware and audio checks above.

## Per-device tests

A new device adds a row here. "Smoke" means an entry in `SMOKE` in `tests/smoke.py`.

| Device | Smoke | State and board tests | Firmware tests |
| --- | --- | --- | --- |
| `palm-tx` | yes | `test-tx-state`, `test-pxa-power`, `test-tx-serial`, `test-hotsync-line`, `test-tx-usb-rom` (built, run by hand) | `device-rom.py`, `tx-baseline.py`, `app-compatibility.py`, lifecycle and power ROM tests, `desktop-*.py`, `large-card-rom.py` |
| `palm-e2` | yes | `test-e2-state`, `test-hotsync-line`, `test-pxa255-udc`, `test-pxa255-udc-dma` | `e2-rom.py`, `device-rom.py`, `app-compatibility.py`, `large-card-rom.py`, `desktop-keyboard.py` |
| `palm-e2-franken` (E3) | yes, optional | `test-device-profiles` (runs `e3-boot.py`) | `device-rom.py`, `FROND_REQUIRE_E3=1` makes a missing session fail |
| `palm-e2` de-NVFS image | yes (`palm-e2-denvfs`), optional | `test-device-profiles` (runs `e2-denvfs-boot.py`), `test-fast-audio` | `device-rom.py`, `FROND_TEST_E2_DENVFS_NOR` is the NOR, `FROND_REQUIRE_E2_DENVFS=1` makes a missing session fail. It never sleeps, so the sleep step of `frond-arm-install.py` is skipped |
| `palm-zire-72` | yes | `test-device-profiles` | `zire72-rom.py` (`FROND_ZIRE_LAUNCHER=1`, `_STORAGE=1`, `_PERIPHERALS=1` add more), `app-compatibility.py`, `large-card-rom.py` |
| `palm-iiix` | no | `test-frond-68k` (68000 timing table, piezo, ROM header checks), `test-core-control` | `device-rom.py --device palm-iiix --rom FILE --launcher --resets`, `test-frond-68k-peripherals`, `test-frond-68k-gdb`, `timing68k.py`, `frond-68k-old-session.py` |
| `sony-clie-tg50` | yes | `test-tg50-state`, `-pace`, `-audio-replay`, `-audio-queue`, `-pwm-audio`, `-microphone-*`, `-serial-board`, `-serial-configuration`, `-call-budget`, `test-memory-stick-*` | `device-rom.py`, `app-compatibility.py`, `ms-classic-rom.py`, `ms-pro-rom.py`, `test-tg50-usb-hotsync` |

The IIIx tests, with the ROMs from `FROND_DEVICE_IMAGES` (`Palm/IIIx/` and `Palm/Generic-OS-3.5-EZ/`) or `--rom`. Without a ROM they skip.

- `make test-frond-68k` (in `make test`): the 68000 timing table against the manual and the bus timing (`build/test-cpu68k-timing`), the piezo tone renderer (`build/test-frond-68k-sound`) and `tests/frond-68k.py` for ROM acceptance and refusal with synthetic headers (wrong size, little-endian ARM header, truncated). `--rom` adds the real ROMs by hash.
- `make test-frond-68k-peripherals`: tones and their frequencies, backlight, sleep and wake with an alarm, serial HotSync with pilot-link over a PTY and TCP, database install, launch, list and export. `--only ppp` also dials out over serial PPP to slirp and fetches a page. Serial needs pilot-link. `FROND_68K_ROMS="--rom A --rom B"` picks ROMs.
- `make test-frond-68k-gdb`: prc-tools' `m68k-palmos-gdb` 5.3 and `gdb-multiarch` on the debug Hello app in `build/hello-debug`, on the OS 3.5 ROM. Skips without them.
- `make test-core-control`: the same control socket requests against `out/frond` (an ARM smoke session, `--arm-session` or `FROND_ARM_SESSION`) and `out/frond-68k`, checking replies, pushed events, save and reload, and that what a device lacks answers `unsupported`.
- `tests/device-rom.py --device palm-iiix --rom FILE --launcher --resets`: detection, cold boot to the launcher, saved session restore in a new process, soft and hard reset, damaged sessions refused into a running machine, saved contrast. Use `--sha256` to pin the ROM.
- `tests/timing68k.py` and `tests/frond-68k-old-session.py` are by hand, see [performance.md](performance.md#68k-timing).
- Desktop: `make test-desktop` has the IIIx import, settings and launch cases; `iiixRealRom` is opt-in (`FROND_IIIX_ROM`, `FROND_IIIX_CORE`, optional `FROND_IIIX_SHOTS`).

Tests that cover every device (profiles, controls, sessions, lifecycle, SD, networking) run in `make test` and need no row.

`make test-zodiac-store-pc` checks the ARM920T stored PC value, word/block address
modes, writeback, aborts and focused Exact/Fast probe admission.
