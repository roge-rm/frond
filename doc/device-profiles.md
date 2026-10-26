# Device profiles and staged support

The desktop still starts Palm TX only. The native core inherits three other
models from the pinned Cloudpilot source. Listing a model does not establish
that a particular firmware, peripheral or application works in Frond.

Build the public inventory without any firmware:

```sh
heavy build make device-profiles
./out/frond-device-profiles
```

The JSON contains model names, stable session IDs, geometry and support status.
It never scans firmware, sessions or a private ROM collection. `nor_bytes: null`
means that the profile has no established fixed NOR size. It is not an invitation
to load arbitrary firmware. `tx-desktop` names the existing experimental TX path;
`inherited-unverified` does not enable another desktop device.

## Current inventory

- `palm-tx`, session ID 3: existing TX desktop path, 16 MiB NOR, 138,412,032-byte
  raw NAND (132 MiB, including spare bytes), 32 MiB RAM and 320×480 display.
- `tungsten-e2`, ID 0: inherited E2 CLI model, 34,603,008-byte raw NAND,
  16 or 32 MiB RAM and 320×320 display. Desktop qualification is pending.
- `franken-e2`, ID 1: inherited patched E2 / Frankenpilot model, the same NAND
  geometry and RAM choices, with a 320×480 display. This is a patched model,
  not evidence of support for another retail Palm. Desktop qualification is pending.
- `repalm`, ID 2: inherited paravirtualized model, no NAND, power-of-two RAM
  sizes starting at 8 MiB and the eight inherited display modes. It requires
  matching paravirtualized firmware. Desktop qualification is pending.

Other Palm OS devices have no native profile here. Sharing an ARM CPU, screen
size or Palm OS version does not establish matching board wiring, flash layout,
ROM patches, input or persistence. The separate upstream 68k emulator is outside
this native inventory.

## Implementation and evidence

Patch `0041-device-profiles.patch` adds `src/common/device_profile5.h` to the
prepared source. It is shared by native device configuration, core firmware
validation and the desktop's TX file checks. The four numeric device IDs and
eight display IDs keep their existing session values. The catalogue contains
only constraints already in `device_configuration.cpp`, TX bring-up patch 0001
and the inherited `RomInfo5` model recognition. No private dump metadata or
firmware hashes were used to populate it.

The inherited CLI paths remain available. Their existing ROM header recognition
and recommended RAM rules still apply; an E2 path can initialise blank NAND
when no image is supplied. TX raw boot continues to require an explicit NAND.
There is no new flag to force an unverified device into the TX desktop path.

Validation checks readable files, model identity where the inherited ROM parser
recognises it, RAM/display choices and NOR/NAND geometry. Unknown session IDs,
recognised ROM/profile mismatches, corrupt session envelopes and overflowing
RAM arguments are rejected. Truncated ROM header strings are bounded. Size
checks cannot distinguish same-sized firmware dumps, authenticate a TX dump or
prove that it boots. TX continues to use its explicit raw-image path because the
inherited header parser does not identify its IPL/NAND layout.

The 0.2.0 v4 envelope and v5 envelope with embedded SD retain their formats and
IDs. Valid TX sessions need no migration. Synthetic tests exercise both readers
and writers; they do not establish guest boot or a complete hardware restore.

## Qualifying another cohort

Keep a proposed model unavailable to the desktop until it has evidence for:

1. Firmware layout, device identification and the board/SoC model, with rejection
   tests for incompatible input and no fallback to another device.
2. Cold boot, display, touchscreen, buttons, reset and relevant peripherals using
   user-owned firmware. Check audio and storage separately.
3. Save/restore into a fresh process, including RAM, peripheral state and SD,
   with corrupt/truncated-state rollback and existing TX session regressions.
4. Representative Palm applications and repeatable acceptance results. Record
   unresolved hardware and timing limits before changing its support status.

Keep private ROMs, metadata, identifying dump hashes, sessions, screenshots and
capture logs outside Git. Public tests should build synthetic headers, flash
geometry and session envelopes. The shared-profile inventory itself establishes
no non-TX boot or peripheral results. The [E2 qualification workflow](e2-qualification.md) adds synthetic board
and session regressions plus an optional private firmware runner; the cohort
remains unavailable to the desktop.

## Tests

```sh
heavy build make -j4 test-device-profiles test-desktop
```

The cohort tests cover all four existing models, invalid IDs, zero and
non-power-of-two RAM, overflow, display ranges, raw NAND with and without spare
bytes, mismatched profile tags, truncated ROM headers and v4/v5 sessions. The Qt
file validator uses sparse synthetic files. CLI rejection tests stop before
SoC construction or SDL startup. No firmware or proprietary app is needed.
