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

TX sleep is still incomplete. The power manager currently starts sleep-exit reset
immediately, without waiting for a programmed wake event. Successful display
restoration does not establish that the device stayed asleep.

Patch 0030 fixes an earlier failure in the ROM's resume path. The first fault was
an instruction prefetch translation fault immediately after enabling the MMU;
the later SRAM stack access fault occurred during exception handling. The patch
models XScale's two 32-byte instruction fetch buffers, including hits across an
MMU transition, replacement and invalidation. It does not add an SRAM mapping,
change the MMU-enable delay or implement TLB lockdown. The existing instruction
cache and CP15 control-bit limitations remain.

The hardware reference is the Intel XScale Core Developer's Manual, sections
4.2.2–4.2.3 and 4.3.3
([manual](https://www.ece.ucdavis.edu/~akella/282S05/XScale%20Core%20Developer%20Manual.pdf)).
CP15 chunk version 1 persists the fetched instructions, including a snapshot
between MMU enable and the branch to its virtual continuation. Version 0 sessions
load with empty fetch buffers. No firmware or guest page tables are changed.

`make test-xscale-prefetch` checks real ARM/Thumb execution, bounded replacement,
invalidation, first-level translation faults, fresh-CPU restore, legacy chunks
and malformed state rejection. It is included in `make test`.

For user-owned firmware, build the optional test and run either form:

```sh
heavy build make test-tx-power-rom
heavy emulator ./build/test-tx-power-rom /private/tx.session
heavy emulator ./build/test-tx-power-rom /private/TX-nor.bin /private/TX-nand.bin
```

The test works on in-memory copies, performs two power-key transitions and
round-trips full sessions into fresh SoCs with the display off and on. Exit 1
means boot, display, resume or persistence failed. Exit 2 means resume and
persistence worked but the device woke without a wake event. Only exit 0 is a
complete sleep/wake pass. The fixture must be in a UI that handles the power key;
booting raw firmware waits 60 seconds of guest time before testing it.

For a local diagnostic build, apply `tools/debug/tx-power-trace.patch` to the
prepared source with `git apply --directory=build/source`. Set
`FROND_POWER_TRACE=1` to record CP15 operations, TTBR, power requests, CPU resets,
sleep/wake events and the instruction history at the first fault.
`FROND_POWER_STOP_ON_FAULT=1` terminates the diagnostic process with exit 86 on a
fault. The observer adds instruction-loop overhead and is excluded from normal
builds. Keep its logs and any firmware/session captures outside Git; reverse the
observer patch and rebuild before performance measurements or packaging.
