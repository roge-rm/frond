# Device profiles and staged support

The desktop starts the Palm TX and the Tungsten E2. The native core inherits two
other models from the pinned Cloudpilot source. Listing a model does not establish
that a particular firmware, peripheral or application works in Frond.

Build the public inventory without any firmware:

```sh
heavy build make device-profiles
./out/frond-device-profiles
```

The JSON contains each device's fixed name, display name, geometry and support status.
It never scans firmware, sessions or a private ROM collection. `nor_bytes: null`
means that the profile has no established fixed NOR size. It is not an invitation
to load arbitrary firmware. `tx-desktop` names the experimental TX path and `e2-desktop` the Tungsten E2
path in the desktop. `inherited-unverified` does not enable another desktop device.

## Device names

Every device has a fixed name. Session files, desktop and Android settings, the
control socket's `device-state` reply, the core's `--device` option and log output
all use it. The core's internal device numbers are never saved or sent.

- `palm-tx`: existing TX desktop path, 16 MiB NOR, 138,412,032-byte
  raw NAND (132 MiB, including spare bytes), 32 MiB RAM and 320×480 display.
  Raw firmware needs `--device palm-tx`.
- `palm-e2`: inherited E2 model, 8 MiB NOR with its card header at
  0x20000, 34,603,008-byte raw NAND,
  16 or 32 MiB RAM and 320×320 display. Available in the desktop (`e2-desktop`).
  Found from its card header; `--device palm-e2` is optional.
- `palm-e2-franken`: inherited patched E2 / Frankenpilot model, the same NAND
  geometry and RAM choices, with a 320×480 display. This is a patched model,
  not evidence of support for another retail Palm. Desktop qualification is pending.
- `repalm`: inherited paravirtualized model, no NAND, power-of-two RAM
  sizes starting at 8 MiB and the eight inherited display modes. It requires
  matching paravirtualized firmware. Desktop qualification is pending.
- `palm-zire-72`, `sony-clie-tg50`, `palm-iiix` and `palm-m515`: reserved for
  coming devices. They have button bar art but no board, and the core refuses them.

Other Palm OS devices have no native profile here. Sharing an ARM CPU, screen
size or Palm OS version does not establish matching board wiring, flash layout,
ROM patches, input or persistence. The separate upstream 68k emulator is outside
this native inventory.

## Implementation and evidence

Patch `0041-device-profiles.patch` adds `src/common/device_profile5.h` to the
prepared source, and patch `0101-device-names.patch` makes it the one device
table: name, display name, detection, board, button bar art and capabilities.
Native device configuration, ROM detection, core firmware validation, the control
socket and the desktop's file checks and settings all look devices up there. The
catalogue contains
only constraints already in `device_configuration.cpp`, TX bring-up patch 0001
and the inherited `RomInfo5` model recognition. No private dump metadata or
firmware hashes were used to populate it.

The inherited CLI paths remain available. Their existing ROM header recognition
and recommended RAM rules still apply; an E2 path can initialise blank NAND
when no image is supplied. TX raw boot continues to require an explicit NAND.
There is no new flag to force an unverified device into the TX desktop path.

Validation checks readable files, model identity where the inherited ROM parser
recognises it, RAM/display choices and NOR/NAND geometry. Unknown device names,
recognised ROM/profile mismatches, corrupt session envelopes and overflowing
RAM arguments are rejected. Truncated ROM header strings are bounded. Size
checks cannot distinguish same-sized firmware dumps, authenticate a TX dump or
prove that it boots. TX continues to use its explicit raw-image path because the
inherited header parser does not identify its IPL/NAND layout.

Session files are version 6: the header holds the device name in a 32-byte
NUL-padded field. Sessions from earlier versions stored a number and are refused
with a short message; there's no migration. Synthetic tests exercise the reader
and writer; they do not establish guest boot or a complete hardware restore.

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
and session regressions plus an optional private firmware runner; the E2 is
available in the desktop and the other inherited cohorts are not.

## Tests

```sh
heavy build make -j4 test-device-profiles test-desktop
```

The tests cover every device name, name to device to name round trips, refused
unknown names and numbered sessions, invalid internal IDs, zero and
non-power-of-two RAM, overflow, display ranges, raw NAND with and without spare
bytes, mismatched profile tags, truncated ROM headers and named sessions with and
without an SD card. The Qt
file validator uses sparse synthetic files, including E2 header detection and
refusal of a NOR image for the other device. CLI rejection tests stop before
SoC construction or SDL startup. No firmware or proprietary app is needed.
