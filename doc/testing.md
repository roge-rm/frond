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

`make test` builds each test against the real core with synthetic ROM, RAM, NAND and card data, so it needs no firmware. It covers CPU alignment and conditions, instruction cache, MMU and memory regions, PACE and the GDB stub, session v9 and state round trips, audio queues and lifecycle, SD, Memory Stick, USB, serial and IrDA, Bluetooth, networking, keyboard, device profiles and controls, checkpoints and database export. `make -n test` lists the targets, and `make test-NAME` runs one.

- `make test-desktop` builds the Qt desktop with tests and runs ctest: the control-socket client and the save, export and settings workflows.
- `make test-build-tools` tests staging safety and package policy with synthetic data. It does not package Frond.
- `make check-reconstruction` prepares the core twice in separate temporary directories from the pinned upstream and `patches/`, compares file hashes and modes, and checks them against `build/source`. A matching prepare stamp alone doesn't show that `build/source` is unmodified, so run it before judging a build. Core changes go in a new patch after the last one, then run this.
- `make test-jit-overshoot` and `make test-jit-differential` run with `make test` when the Fast engine is built. The differential test runs random ARM and Thumb blocks on both engines and compares them, plus directed cases: seeds 1 to 30000, then two further ranges of 10000. For a long run use `make test-jit-differential JIT_DIFF_ARGS="--seeds 1: --duration 3600"`, or `--seed N` for one case.
- `make test-zodiac-lcdc-contrast` checks the explicit contrast-only LCDC policy, native MMIO sequence, PERCLK2 counter and guarded panel/DMA in Exact and Fast.
- `make test-zodiac-ssi-startup` checks SSI1 pin mux and idle configuration, empty-state readback, guarded transfers and Exact/Fast startup.
- `make test-zodiac-uart-startup` checks disabled UART1/2 control, independent reset/readback, guarded effects and Exact/Fast startup.
- `make test-zodiac-dma-channel` checks disabled channel allocation/configuration, independent state, reset, guarded transfers and Exact/Fast execution.
- `make test-zodiac-dma-startup` checks idle DMA global state, reset pulse timing, guarded transfers and Exact/Fast startup.
- `make test-zodiac-normal-runtime` checks the Zodiac's quarter turn of the panel, pen samples, stick offsets and the EEPROM identity read from a made-up ROM. No firmware.
- `make test-zodiac-graphics-commands` checks bounded native ring packets, delayed solid RGB565 paint, pointer and busy state, rejected-batch atomicity, clipping and byte order.
- `make test-zodiac-graphics-gpu` checks the Zodiac's host GPU fills (`FROND_ZODIAC_GPU`) against the CPU model, with CPU accesses between them, the differential mode and recovery from a GL error. It's skipped where there's no usable GPU.
- `make test-zodiac-graphics-acquisition` checks independent virtual raw-axis and mode-1 inputs, guest-time acquisition, reset/cancellation, tagged FIFO, GPIO source selection and real timed helper readback/identity transfers on Exact and Fast.
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
- `make appimage` and `make appimage-arm64` build `out/Frond-VERSION-x86_64.AppImage` and `-aarch64.AppImage` in Docker on Debian 12, so they run on glibc 2.36 and newer. The arm64 one is cross built and can only be tried on arm64 hardware.
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
| `alphasmart-dana`, `alphasmart-dana-wireless` | no | `test-68k-dana`, `test-68k-vz-registers`, `test-68k-sd`, `test-dana-input`, `test-dana-view`, `test-dana-card-inventory`, `test-dana-wifi-module`, `test-usb-printer-runtime`, `test-usb-wifi-runtime` | `device-rom.py --device alphasmart-dana`, `dana-session.py`, `dana-input.py`, `dana-power.py`, `dana-sound.py`, `dana-irda.py`, `dana-cards.py` |

The IIIx tests, with the ROMs from `FROND_DEVICE_IMAGES` (`Palm/IIIx/` and `Palm/Generic-OS-3.5-EZ/`) or `--rom`. Without a ROM they skip.

- `make test-frond-68k` (in `make test`): the 68000 timing table against the manual and the bus timing (`build/test-cpu68k-timing`), the piezo tone renderer (`build/test-frond-68k-sound`) and `tests/frond-68k.py` for ROM acceptance and refusal with synthetic headers (wrong size, little-endian ARM header, truncated). `--rom` adds the real ROMs by hash.
- `make test-frond-68k-peripherals`: tones and their frequencies, backlight, sleep and wake with an alarm, serial HotSync with pilot-link over a PTY and TCP, database install, launch, list and export. `--only ppp` also dials out over serial PPP to slirp and fetches a page, `--only modemsync` does a Modem Sync to the HotSync number and a Network HotSync over the modem (OS 3.5, pilot-link). Serial needs pilot-link. `FROND_68K_ROMS="--rom A --rom B"` picks ROMs.
- `make test-frond-68k-gdb`: prc-tools' `m68k-palmos-gdb` 5.3 and `gdb-multiarch` on the debug Hello app in `build/hello-debug`, on the OS 3.5 ROM. Skips without them.
- `make test-core-control`: the same control socket requests against `out/frond` (an ARM smoke session, `--arm-session` or `FROND_ARM_SESSION`) and `out/frond-68k`, checking replies, pushed events, save and reload, and that what a device lacks answers `unsupported`.
- `tests/device-rom.py --device palm-iiix --rom FILE --launcher --resets`: detection, cold boot to the launcher, saved session restore in a new process, soft and hard reset, damaged sessions refused into a running machine, saved contrast. Use `--sha256` to pin the ROM.
- `tests/timing68k.py` and `tests/frond-68k-old-session.py` are by hand, see [performance.md](performance.md#68k-timing).
- Desktop: `make test-desktop` has the IIIx import, settings and launch cases; `iiixRealRom` is opt-in (`FROND_IIIX_ROM`, `FROND_IIIX_CORE`, optional `FROND_IIIX_SHOTS`).

Tests that cover every device (profiles, controls, sessions, lifecycle, SD, networking) run in `make test` and need no row.

`make test-zodiac-store-pc` checks the ARM920T stored PC value, word/block address
modes, writeback, aborts and focused Exact/Fast probe admission.

`make test-68k-frame-bounds` checks VZ frame row, destination and guest-address bounds without firmware.

`make test-68k-vz-registers` checks VZ Port D polarity and register-driven LCD copy bounds, including a partial dirty row. It needs no firmware.

Dana staged geometry checks: `heavy build make test-68k-vz-registers` covers the final panned pixel at 560x160, malformed LCD rows and depth, classic 160x160 decoding and VZ level IRQ pending/status polarity, enable, direction and masking.

The same VZ target checks ADS784x partial commands, pending conversions and partial replies across save/load, with malformed saved bit counters rejected. These are generic converter tests, not Dana analogue qualification.

`heavy build make test-68k-dana` uses no firmware. It checks the Dana ST version parser at every split, reply continuation, controller reset and ready polling, held Power/Up packets, D4 and board IRQ1 level/edge polarity and acknowledgement. It also checks the disconnected ISP register bank: nonzero HC/DC partial words, partial interrupt-status reads, interrupted writes, guest-cycle frame counting, HCR completion, buffer allocation limits and malformed state rejection. Parser-only ST snapshots isolate its state fields; they do not replace whole-session rollback tests.

Dana's virtual analogue inputs use unsigned 12-bit ADC counts (0–4095). A4/channel 2 samples main battery, E4/channel 6 samples supply/alkaline detection, G1 is a separate pack-detect input and G2 selects the ADC. Defaults are main 3072 counts, supply 50 counts and pack-detect high, representing a virtual battery with no external supply. These are simulator choices, without a voltage or divider calibration. Tests select both channels, check zero/midscale/full-scale and reject out-of-range inputs, retaining the shared converter's serial conversion pipeline. The native ROM pen bridge is supported separately; ADC pen electrical accuracy is unqualified.

The ISP bank has no transfer buffers, attached endpoints or board interrupt output wiring. Cards, sound and IrDA have separate checks below. The ST board handler qualifies IRQ1 only, retaining the base acknowledgement for other interrupts; generic VZ INT0–3 edge capture remains unqualified. Normal runner boot, reset and session checks pass with the disconnected controller.

Dana's VZ backend runs in `frond-68k`. Its
virtual ADC defaults are main 3072, supply 50 raw counts and pack-detect high.
Supply 50 avoids unsigned underflow in the ROM's `(supply >> 1) - 25` comparison;
these are deterministic simulator inputs, with no measured voltage implied.
`test-68k-dana` covers that classification boundary through actual ADC exchanges.

With private firmware and the built `tests/palm/sync68k-probe.c` application:

```
python3 tests/dana-session.py --rom FILE --probe FrondSync68k.prc --evidence NEW_DIRECTORY
```

This uses normal backend dispatch for cold setup/launcher, soft reset, Power/Up
hard reset, erased app/database checks, install/launch/export and fresh-process
PDB equality. Eleven malformed production ADC/ST/ISP chunks must roll back with
identical PNG/PDB bytes. Parser fixtures remain separate. Saved controller checks
require no enabled pending HC event, disabled transfer-buffer sources and no DC
interrupt enables. USB GPIO outputs, attached transfers and electrical ADC/pen
accuracy remain unqualified. `tests/device-rom.py --device alphasmart-dana --rom
FILE --launcher --resets` uses the same setup, launcher and reset recipes.

Dana input: `heavy build make test-dana-input` checks typed source ownership and
refusal without firmware. `heavy build make test-68k-dana` also checks ST pacing,
partial replies and v5 migration with a later dirty snapshot. With private inputs,
run `python3 tests/dana-input.py --session READY.frond --probe FrondKeyboardProbe.prc
--evidence NEW-DIRECTORY --core out/frond-68k` under the build/timing lock. It checks
actual guest text, modifiers, chords, repeat, refusal and pending-input lifecycle.

The same Dana guest check is available through `tests/device-rom.py --device
alphasmart-dana --keyboard-session READY.frond --keyboard-probe
FrondKeyboardProbe.prc --evidence NEW-DIRECTORY` with the built `out/frond-68k`.

`tests/dana-input.py --skip-basics` runs the chord, ownership and lifecycle portion
while developing input state. The full check also covers every ordinary key group
and enables Sticky Keys through the guest Keyboard application.
`--fresh-only --session pending.frond` starts a new process on the pending.frond
an earlier run saved and checks only that its queued text arrives.
`--replay-only --session pending.frond` starts it with an `--input-replay` and checks
the replay's key arrives through the window's first hide.

Dana ordinary power development: `python3 tests/dana-power.py --session READY.frond
--core out/frond-68k --evidence NEW-DIRECTORY` checks Power long holds, ordinary
sleep/wake and production saved-power state. Use a private ready session with
On key selected, under the build/timing lock. Two-button EB mode remains blocked;
this test does not qualify it or the USB mode commands.

Dana sound development uses a private ready session and the `dana-sound` Palm
probe. Build it with `tools/build-palm-probe.py --probe dana-sound --sdk SDK`.
On the test host, source `lab/frond-work/tools/pilrc/env.sh` first. Run
`tests/dana-sound.py --core CORE --session SESSION --probe PRC --evidence NEW_DIR`
under the build slot and speed lock. It measures captured PCM for Sound Manager
calls, guest mute, quiet, reset, pending-tone reload/rollback and ordinary sleep.
The separate `alarm68k` probe checks guest alarm wake. Alternate ST power modes
are outside this sound check. Wider frontend and listening checks wait for release.

Dana IrDA checks use private inputs and a new evidence directory. Build `irda`,
`irda-db` and `dana-irda-fifo` with `tools/build-palm-probe.py` and the SDK.
`tests/dana-irda.py` takes `--core`, `--peer-core`, `--session`, `--peer-session`,
`--probe` and `--evidence`; the peer is a ready T|X session. It checks both
4096-byte guest beams and a connection cut during another transfer. Use
`--database --sender 0` and then a new directory with `--sender 1` for the
Exchange Manager database writer/importer probe.

`tests/dana-irda-state.py` takes `--core`, `--session`, `--probe` (the `irda`
probe) and `--evidence`. It checks guest receive controls, shutdown gating,
active local save/load, malformed UART rollback, no-peer cancellation and reset.
`tests/dana-irda-fifo.py` takes the same arguments with the `dana-irda-fifo`
probe. This separate raw UART diagnostic masks the receive IRQ so 32 incoming
bytes stay in the hardware FIFO, then checks local rollback and fresh-process
readback. It does not qualify IrLAP or OBEX by itself.

Dana's UART2 FIFO chunk is version 1, separate from ST version 9. It stores
receive/transmit bytes in order and the local receive-in-progress flag. An
older Dana snapshot starts with empty UART2 FIFOs. Host endpoint queues, socket
state and external peers are outside the checkpoint. A local load cannot undo
bytes already sent to a peer or restore its transaction. A load can replay local
sends; cancel or reset and
start another guest transfer after reconnecting.

`make test-68k-sd` checks the inherited SPI SD model without firmware. It covers
command arguments, partial read/write cursors, malformed cursor and metadata
chunks, end-of-image transfers, two independent instances and legacy one-slot
metadata. It does not qualify Dana's board wiring, guest VFS, MMC, read-only
media or session-container persistence.

Dana runtime card development checks use `make test-68k-sd`,
`make test-68k-dana`, `make test-card-backing` and
`make test-68k-card-provider`. The provider test covers failed reads,
failed write rollback and refusal of a foreign committed backing root.
`tests/dana-cards.py` takes explicit `--core`, `--session`, `--probe`,
`--make-card` and a new private `--evidence` directory. Build the fixture
helper from `tests/dana-card-fixture.cpp` with the native headers, and the
`dana-card` probe with `tools/build-palm-probe.py` and the installed SDK/PilRC.
It uses mtools and the public runtime commands for two-card native guest SD
read/write, readonly, host equality and independent removal/swap/reinsertion.
These development checks do not qualify MMC or dual-card session persistence.

Dana linked-session restore: `tests/dana-card-restore.py --core CORE --session
PRIVATE_READY_SESSION --probe FrondDanaCards.prc --make-card FIXTURE_HELPER
--evidence NEW_PRIVATE_DIRECTORY`. Use the probe from `tests/palm/dana-card-probe.c`.
This checks normal save/load, fresh-process dual guest reads, file hashes,
protected/ejected restore, owned-lock reuse, staged second-card lock/root failure,
malformed protocol refusal and whole-machine rollback. Full in-flight SPI,
sleep/reset and portable session acceptance remain separate gates.

`tests/dana-card-portable.py` uses the Dana card probe and the native console to
check portable dual-card folders. It moves a copy away from the source paths,
loads it in a fresh process, reads both cards through guest VFS and increments
`COPY.TXT` to prove new writes survive another process without changing the
source backings. It also checks duplicate/import/peek, protected/ejected media,
malformed manifests/payloads/roots/protocol state, rollback and staged-lock cleanup.
Use new private evidence folders and a frozen candidate binary. Run on the test host
under its shared speed lock; this is a development gate, not release acceptance.

Dana D3c native card inventory uses stable HAL slots SD0=0 and SD2=2 (labels
1 and 2), each with representable SDSC capacities from 32 MiB up to 1 GiB
minus 256 KiB and legacy MMC SPI at 32/64/128/256 MiB. Guest-qualified sizes are
32 MiB FAT for each family and slot, paired and both mixed orders. Runtime
bounds do not claim guest qualification at every capacity. The backend is
complete for native CLI/runtime/session use.
`make test-dana-card-inventory` checks per-slot/family geometry and metadata
without firmware. Frontend/card export acceptance waits for the next release.
The native guard check uses `tests/dana-card-geometry-fixture.cpp` to build and
validate v2 files through the production header and payload readers before
testing an unrepresentable capacity and a representable control on both slots.
