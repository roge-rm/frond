# Adding a device

Add devices in four milestones, with a fresh diff review after each. Keep the device plan and qualification notes locally. Version changes can wait until the final feature milestone. Publish only when Dan asks for a release.

## Shared data and tools

The patched core's `src/common/device_profile5.h` owns the public model inventory, `deviceProfiles5`. Keep fixed device names and saved display IDs stable. A reserved name can have art without a runnable board. Frontends offer only profiles whose `frontend.available` is true.

Put firmware geometry, detection IDs and card layout, session exceptions, storage limits, qualified capability flags, screen and silkscreen sizes, key labels and controller taps in the profile. Hardware that exists but has no qualified implementation stays visible and greyed out. Physical presence and qualified support are separate facts. Do not infer one from the other. Add a presence field when a peripheral needs it; the camera and SD slot already have one.

Native runner eligibility is separate from frontend availability. The exporter sets `runner_eligible` when a profile has a concrete board; Python runners use that field. Inventory-only profiles have an internal type but no board, and art reservations have no type. Both stay out of runners. Add the concrete board during boot bring-up, then qualify it through the generic runner before enabling desktop or Android.

Sound qualification is `capabilities.sound`. Physical storage kind is `deviceProfileStorageSlot5`: SD, Memory Stick or none. These fields do not qualify a transport. Unimplemented real hardware stays disabled when the frontend is enabled.

Desktop menus and firmware detection read the profiles directly. Android and Python use exports from the same table. After changing a profile:

```
heavy build make prepare
heavy build make generate-device-data
```

Commit the generated `tools/device-profiles.json` and Android `DeviceInventory.kt` with the profile patch. `make test` and Android's `preBuild` check them for drift. Android's TX, E2 and ZIRE72 names and the desktop enum names are compatibility aliases; new devices enter through the inventory without adding aliases.

Setup and launcher names select measured recipes in `tools/palm_boot.py`. Reuse a recipe only when the device follows the same screens. Otherwise add a small recipe and frame predicate there, then name them in the profile. Unknown recipes must fail before sending input. Keep firmware paths, hashes, sessions and acceptance captures out of these public descriptors.

Use the generic firmware entry point:

```
python3 tests/device-rom.py --device DEVICE --nor /private/nor.bin --launcher
```

Add `--nand /private/nand.bin` when needed, `--sha256 HASH` to pin the inventoried original and `--evidence build/NEW-DIRECTORY` to retain a named run. Existing evidence directories are refused. Without `--launcher` the generic run checks a short real boot; it does not claim setup, peripherals or frontend acceptance. Missing private firmware is a skip. The Zire 72 default delegates to its existing suite, including its `FROND_ZIRE_*` opt-ins. Synthetic header and export tests establish rejection and metadata behaviour only. Inventory-only rejection can be checked without private firmware:

```
python3 tests/device-rom.py --device sony-clie-tg50 --synthetic-rejection
```

This checks CLI and session rejection before SoC construction. It does not boot a guest.

Memory-failure diagnostics fail the generic check by default. For an original image
with independently observed guest recovery, `--expected-faults /private/expected.json`
requires `--sha256`, `--launcher`, `--resets`, the built `device-session-probe` and all functional
postconditions. The private version 1 JSON has `device`, `nor_sha256` and `phases`.
Each phase is a list of exact `access` (`read` or `write`), `size`, hexadecimal
`va` and `pc`, integer `fsr` and positive `count` records. Empty lists mean no
faults. Required phases are `boot`, `setup`, `reload`, `rollback`, `reset-soft` and
`reset-hard`, using a measured reset recipe. Extra,
missing or unparseable faults fail. Keep expectations and their prior evidence
private, preserve earlier strict failures and describe observed handled writes.

Core changes go into new ordered patches after the last published patch. Preserve upstream licence headers and credit. Never edit a published patch. Check reconstruction against the current working source before review.

## 1. Inventory

Record the original firmware's SHA-256, geometry and source in a private inventory. Read originals without modifying them. Reconcile the board, CPU, memory map and every physical peripheral against the manufacturer's material and the upstream model. Distinguish advertised hardware RAM from any internal backing or mirrored probe region.

Reserve the fixed name, describe firmware detection and rejection rules and list hardware and capability gates. Check wrong devices, sizes, missing companion images and malformed card layouts before creating a machine. Do not enable a frontend entry before its runnable path exists. Review the inventory and profile changes.

## 2. Boot

Port the smallest board and chip changes needed for an ordinary cold boot. Follow the upstream model's licence. Qualify display, touch, setup and launcher against real firmware, then soft and hard reset. Measure screen predicates and tap coordinates instead of relying on fixed host delays.

Save a session, quit and reload it in a fresh process. Check device identity, display and guest state. Try malformed loads against an already running machine and confirm the old machine still works. Keep earlier session compatibility and IDs. Retain logs, hashes and captures, and review the changes.

## 3. Peripherals

Inventory all real controls and ports, including keyboard, jog controls, storage, sound, power and sleep, USB, serial, infrared, Bluetooth and radios. Qualify each independently, including reset, sleep/wake and save/reload where state persists. Enable its profile gate only when the retained evidence supports it. Leave unfinished real peripherals greyed out and describe the exact limit.

Use device-specific scenarios behind the generic test entry point when a peripheral needs its own guest procedure. Reuse shared console, frame, setup and probe helpers. Keep probe binaries, installed Palm applications, raw PCM, firmware and captures private. Review this milestone separately.

## 4. Art and release preparation

Use existing device art where available. New button bars should be recognisable approximations of the hardware; never bundle reference photos. Wire the profile's art and input descriptors through the ordinary frontend paths. Qualify actual desktop and Android import, launch, controls, settings and session workflows. Core boot screenshots alone do not establish frontend acceptance.

Update public support and limitations, prepare the next unreleased version and run the checks below. Review the complete change. Leave signing, tagging, repository publication and deployment for an explicit release request.

## Checks and evidence

Select real-firmware smoke coverage by the code paths changed. A board-only change needs that board and nearby shared models. A shared frontend, input, storage or session change needs the devices using that path. There is no requirement to smoke every device on every release. Record the selection and any untested paths.

Run builds one at a time through `heavy`:

```
heavy build make -j4 test
heavy build make check-reconstruction
heavy build make test-desktop
cd android
heavy build ./gradlew assembleDebug testDebugUnitTest
```

Run guest cores through `heavy emulator`. Claim shared AVDs in the machine resource registry before use; run headless with `-gpu swiftshader_indirect`. Use the private test GDB through `FROND_GDB` when required. Check free space before large builds.

Evidence goes into a new directory under `build/`; files Dan should inspect go under `debug/`. Keep `build/step7-peripherals/` mode 0700. Do not overwrite earlier binaries or results and do not call a failed or skipped check a pass. Append short milestone notes to the local plan, including checks, evidence paths and remaining limits. Plan and handoff files stay local.
