# Experimental TX sessions and reset

The native console accepts `save-session /path/to/tx.session` and `reset`.
Restore a complete session with `./out/frond /path/to/tx.session`; the session
selects the TX device automatically. Raw firmware startup still needs
`--experimental-tx NOR --nand NAND`. Original firmware inputs are never rewritten.
A session contains firmware, flash, RAM and CPU/peripheral state and stays outside Git.

Patch 0006 adds serialization for PXA270 IMC, keypad, USB controller and WMMX,
all seven LCD DMA channels, PXA270 power registers and the TX RAM probe marker.
Existing E2 LCD/power chunks retain version zero. Restore reconstructs the checked
RAM PACE metadata, invalidates host frame buffers and releases pending host input
and audio. Invalid or incomplete peripheral state fails instead of silently
continuing. Normal reset reloads captured power-on controller state while retaining
RAM and flash for the firmware's normal reset handling.

`make test-tx-state` uses synthetic ROM/RAM/NAND, no proprietary inputs. It checks
MMIO register reload, all 16 WMMX data registers with real instructions, reset
without RAM erasure, audio queue clearing, truncated and missing-chunk rollback,
and LCD rendering after loading into a fresh SoC. It also checks E2 timer and
version-zero LCD/power session compatibility. The PACE suite checks reconstruction
from patched RAM and rejection of a damaged remaining signature.

TX SD images are supported at startup and in the console; card-aware
snapshots embed the image in session format v5. See [peripherals](peripherals.md). Reset without
extensions and hard reset have not received the same firmware-level coverage as
normal reset. This is experimental support, not exhaustive hardware state
validation. Older incomplete TX snapshots are rejected.

Patch 0007 corrects OSCR0 from the PXA25x 3.6864 MHz rate to the PXA27x 3.25 MHz
rate. Intel PXA27x Developer's Manual section 22.4.3 explicitly distinguishes the
two clocks ([manual](https://docs.toradex.com/100211-colibri-arm-som-pxa270-technical-reference-manual.pdf)).
The scheduler uses integer nanosecond periods, so small quantization error remains.
Restored TX schedules are migrated to the corrected timer rate. Timer frequency alone does not establish correct audio timing or continuity.

## TX power transition diagnostics

Patch 0033 separates PXA27x power sleep (`PWRMODE=3`) from interrupt-driven
idle. Power sleep stops CPU and peripheral-clock work until a qualified GPIO
edge, keypad input level, or RTC alarm match. Wake enable, edge, direction and
keypad alternate-function settings are honored; ordinary IRQs cannot wake it.
RTC alarm wake does not require its CPU interrupt enable. Sleep and pending wake
state persist in power chunk version 2; versions 0/1 load without power sleep.
GPIO chunk version 1 preserves sleep output holds; version 0 still loads and E2
continues writing version 0.

Sleep exit resets CPU/CP15 and the nonretained peripheral register domains using
the existing power-on register snapshot. It preserves RTC, wake configuration,
scratchpad, clock settings, external device state and SDRAM. PSLR controls timer
and SRAM retention; unretained SRAM banks become zero (hardware contents are
undefined). GPIO outputs use PGSR and remain held until PSSR[PH] is cleared.
Reset is applied at the scheduler wake boundary, outside input and IRQ callbacks.
Session loading releases stale host keys without generating a new wake event.

The ROM confirms a keypad wake with manual scans. This also required correcting
the manual column-select bit position, inclusion of the last column, and valid
row mask, plus PKSR write-one-to-clear handling. `make test-pxa-power` exercises
wake qualification, clock stopping, retained/reset state, manual keypad scans,
legacy chunks and fresh-SoC sleep restoration without firmware.

References: Intel PXA27x Developer's Manual, Table 3-2 and sections 3.6.9,
3.8.1.4–3.8.1.16, 18.4.1, 18.4.5 and 21.4.6.2
([manual](https://courses.cs.washington.edu/courses/cse466/08wi/labs/l5/pxa27x_developers_manual.pdf)).
This models the implemented TX wake sources, not all PXA power modes. Standby,
deep sleep, USB/MSL/power-fault wake, extended OS timers, oscillator/regulator
delays, and complete electrical pad/pull-up behavior remain unsupported. The RTC
still uses host wall time, and physical-device timing has not been compared.

The XScale instruction-fetch model preserves two 32-byte fetch buffers across
MMU transitions. It does not implement TLB lockdown or change guest page tables.

The hardware reference is the Intel XScale Core Developer's Manual, sections
4.2.2–4.2.3 and 4.3.3
([manual](https://www.ece.ucdavis.edu/~akella/282S05/XScale%20Core%20Developer%20Manual.pdf)).
CP15 chunk version 1 persists the fetched instructions, including a snapshot
between MMU enable and the branch to its virtual continuation. Version 0 sessions
load with empty fetch buffers. No firmware or guest page tables are changed.

`make test-xscale-prefetch` checks real ARM/Thumb execution, bounded replacement,
invalidation, first-level translation faults, fresh-CPU restore, legacy chunks
and malformed state rejection. It is included in `make test`.

## Audio, card and host I/O lifecycle

True power sleep clears queued host PCM and fast-audio preview state. Ordinary
interrupt idle retains them. A queue clear marks a stream discontinuity; SDL
discards its previous buffering/backpressure decision and primes the replacement
stream before playback. A concurrent clear cannot make an old callback snapshot
consume replacement PCM. Audio already copied to SDL or the host sound backend
cannot be retracted. Reset and nonempty load attempts also clear queued output; a rejected
load restores guest state but does not retain external audio, sockets or peers.

SD snapshots preserve partial controller transfers. Restore performs no backing
I/O, and successful remount reconnects the controller without resetting its FIFO
or adding a card-detect edge. Explicit removal, including failed remount after a
fresh-SoC restore, discards pending transfer data and withdraws DMA requests.
Invalid restored transfer lengths, FIFO bounds and card states are rejected.
Close guest files before ejecting: aborting a transfer cannot flush an app's files.

Memory-copy DMA descriptor rings yield after bounded work per channel so guest
execution, other channels, reset and checkpoint requests remain reachable. UART
peer callbacks stop during true power sleep; wake resets the UART domain and
detaches its external owner. Nonempty normal and rejected loads close host socket/peer
ownership; apps may need to reconnect. Temporary database checkpoints preserve
ownership while freezing external effects. These checks do not add host serial
transport, UART DMA, USB or HotSync support.

`make test-lifecycle` is firmware-free and included in `make test`. It checks
audio discontinuities and concurrent queue replacement, whole-guest clock pause
under audio backpressure, fast-audio sleep/restart, cyclic DMA fairness, local
socket/peer checkpoint and failure paths, partial SD transfers and rejected-load
recovery. The existing power, session, network and UART suites remain in use.

For whole-session firmware checks, including embedded SD backing, build the
optional driver and use new private output directories:

```sh
heavy build make test-tx-lifecycle-rom
timeout 600s heavy emulator ./build/test-tx-lifecycle-rom \
  --plain-session /private/tx-ready.session \
  --output /private/results/tx-plain-lifecycle
timeout 600s heavy emulator ./build/test-tx-lifecycle-rom \
  --audio-session /private/tx-playing-with-card.session \
  --output /private/results/tx-audio-lifecycle
```

The driver accepts complete v4/v5 TX sessions and embedded 32/64/128 MiB cards.
It copies inputs, preserves card attachment and backing bytes, records per-phase
PCM, and round-trips awake and sleeping state into fresh SoCs. A plain fixture
must enter true power sleep; an audio fixture requires nonzero PCM before/after
awake restore and characterizes the app's power-key policy. LCD-off alone is not
power sleep. When sleep occurs, it checks stationary CPU, no new PCM and unchanged
card bytes for ten guest seconds, then wake and restored output. Reset checks
bounded LCD/controller recovery and whole-session restore; it does not assert
that the app or playback automatically restarts. The separate
[SD VFS acceptance](peripherals.md#validation) requires fresh post-reset app and
file-I/O results. A nonblank display alone does not establish UI recovery.

Qualified TX fixtures have passed these phases with continuous audio and both
inserted and ejected 32 MiB cards. This establishes playback restart, not gapless
wake audio. PCM capture excludes SDL/backend and physical output latency. Guest
run budgets and host deadlines are separate measurements; audio backpressure
pauses scheduled CPU/peripheral work together, while RTC remains host-wall-time
based. Physical TX comparisons and deterministic RTC timing remain unvalidated.
Keep all snapshots, PCM and logs private. The driver needs an external host
deadline, including time spent waiting for a shared emulator slot.

For user-owned firmware, build the optional test and run either form:

```sh
heavy build make test-tx-power-rom
heavy emulator ./build/test-tx-power-rom /private/tx.session
heavy emulator ./build/test-tx-power-rom /private/TX-nor.bin /private/TX-nand.bin
```

The test works on in-memory copies, allows up to twenty guest seconds for ROM
sleep entry, performs two power-key transitions and round-trips full sessions into fresh SoCs after confirmed sleep entry and while
awake. It checks that the CPU remains stationary for ten seconds of emulated
time without input. Exit 1 means boot, display, resume or persistence failed. Exit 2 means resume and
persistence worked but the device woke without a wake event. Only exit 0 is a
complete sleep/wake pass. The fixture must be in a UI that handles the power key;
booting raw firmware waits 60 seconds of guest time before testing it.
This older helper rejects embedded SD sessions; use `test-tx-lifecycle-rom` for
those. The fixed-budget `benchmark-session` helper also rejects embedded cards
rather than benchmarking a silently detached device.

For a local diagnostic build, apply `tools/debug/tx-power-trace.patch` to the
prepared source with `git apply --directory=build/source`. Set
`FROND_POWER_TRACE=1` to record CP15 operations, TTBR, power requests, CPU resets,
sleep/wake events and the instruction history at the first fault.
`FROND_POWER_STOP_ON_FAULT=1` terminates the diagnostic process with exit 86 on a
fault. The observer adds instruction-loop overhead and is excluded from normal
builds. Keep its logs and any firmware/session captures outside Git; reverse the
observer patch and rebuild before performance measurements or packaging.
