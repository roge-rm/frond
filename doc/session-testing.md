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
