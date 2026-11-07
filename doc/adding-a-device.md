# Adding a device

How a device is described to Frond and what to build to make a new one run. The table of devices is `deviceProfiles5` in `src/common/device_profile5.h` of the patched core (`build/source` after `make prepare`). Everything else reads from it: the core, the desktop app, the Android app and the Python tools. See [developer-reference.md](developer-reference.md) for the command line and socket, [testing.md](testing.md) for the test targets and [performance.md](performance.md) for the engines.

A 68k device (the Palm IIIx is the example) runs on a different runner, `frond-68k`, so some steps below differ. See [68k devices](#68k-devices).

Core changes go in a new patch after the last one in `patches/`, or in an unpushed patch regenerated in place. Check them with `make check-reconstruction`. Keep the upstream licence headers and credit.

## Steps

First check the existing profiles, board dispatch and ROM detection. A device may already have an inherited model with no frontend. Keep its fixed name and extend that model. If it shares a board with a supported device, start from that board's working paths and list the differences in firmware, display, controls and peripherals.

1. Reserve the fixed name and write the profile (below). Leave `frontend.available` false.
2. Add detection and rejection, and check wrong files are refused before a machine is made.
3. Add the board and get a cold boot, setup and launcher, then soft and hard reset.
4. Add the PACE and API bridge, so install, launch and export work.
5. Check the Fast engine runs the device (Fast engine requirements).
6. Add each peripheral and set its capability flag when it works.
7. Add the button bar and silkscreen art.
8. Fill in the frontend fields, run `make generate-device-data` and set `available`.
9. Add a smoke entry and a setup recipe.

Hardware that exists on the real device but isn't done yet stays in the profile and is greyed out in the frontends. Whether a peripheral is present and whether Frond supports it are separate fields, so don't work out one from the other.

For a shared board, audit checks for the existing device type and name in the core, frontends and tools. Share hardware selection through the board model, and use capabilities for supported host interfaces. Keep ROM signatures, firmware-specific patches and qualified audio hooks separate. Test the existing device too when changing a shared path.

## Profile fields

A `DeviceProfile5` has these fields. The name is what sessions, settings, the control socket, `--device` and the logs use. `DeviceType5` is internal and never saved. Don't change a name or a saved display ID once it's out.

| Field | Meaning |
|---|---|
| `deviceType` | Internal type. `deviceTypeInvalid` for a name reserved with no board |
| `name`, `displayName` | Fixed name (31 characters at most) and the name shown in menus |
| `cohort`, `support` | Group and status, shown in the exported JSON |
| `detect`, `companyId`, `halId` | How firmware is recognised: `cardHeader` (the Palm card header's company and HAL IDs, company 0 matches any), `norSize`, or `none` |
| `board` | Which board model runs it. `none` means no runner can start it |
| `art` | Which button bar and silkscreen it gets |
| `capabilities` | `network`, `keyboard`, `serial`, `usb`, `fastAudio`, `infrared`, `bluetooth`, `sound`, `microphone`, `backgroundPanel` (see Peripherals) |
| `norSize`, `nandSize` | Exact NOR size and raw NAND size including spare bytes. Zero for no fixed size or no NAND |
| `minRam`, `maxRam` | Powers of two, in bytes |
| `defaultDisplay`, `displayModes` | Default `DisplayMode` and a mask with a bit per allowed mode. The numbers are saved in sessions |
| `requiresExplicitNand` | A new machine has to be given a NAND image |
| `norLayout`, `headerOffset`, `bigCardOffset` | `any`, `headerOffset` (card header at an offset), or `smallAndBigCards` (small card at 0, big card at `bigCardOffset` running to the end) |
| `legacySessionNor` | Old sessions may hold another NOR size or layout and still load |
| `sdMin`, `sdMax` | SD image sizes the CLI takes, in bytes |
| `msSizes`, `msProMaxMib` | Memory Stick sizes in MB (ORed together) and the largest Memory Stick Pro in MiB (1886 for "2 GB"). Zero for no slot |

`frontend` holds what the apps need:

| Field | Meaning |
|---|---|
| `available` | The frontends offer the device only when this is true |
| `setup`, `launcher` | Names of the setup recipe and the launcher frame check in `tools/palm_boot.py` |
| `width`, `height`, `silkscreenHeight`, `stripHeight`, `sideTabWidth` | Screen, silkscreen, button bar and side panel sizes in pixels |
| `appNames` | The four hard key labels, separated by `\|` |
| `buttons` | Controller button IDs, separated by `\|` |
| `homeX`, `homeY`, `menuX`, `menuY` | Pen taps for Home and Menu where they are silkscreen or status bar taps. -1 for none |
| `rotatingStatusBar` | Menu is a status bar tap that depends on rotation |
| `sdSlot`, `camera`, `storageSlot`, `memoryStick` | A slot or camera is present. `storageSlot` is `none`, `sd` or `memoryStick`, and the SD slot implies `sd` |
| `fastAudioNorMd5` | MD5 of the NOR qualified for audio preview. Empty for the queue setting only |
| `homeKey` | SDL keycode of a physical Home key. 0 for none |
| `holdPresent`, `holdSupported` | A Hold switch is present, and it works |

### The devices

| | Palm TX | Tungsten E2 | Zire 72 | Sony CLIÉ TG50 | Palm IIIx |
|---|---|---|---|---|---|
| `name` | `palm-tx` | `palm-e2` | `palm-zire-72` | `sony-clie-tg50` | `palm-iiix` |
| `board` | `pxa270Tx` | `pxa255E2` | `pxa270Zire72` | `pxa250SonyTG50` | `ez328` |
| `art` | `tx` | `e2` | `zire72` | `tg50` | `iiix` |
| `detect` | NOR size | card header, Palm `hspr` | card header, Palm `aZ72` | card header, `sony` `vrna` | `romHeader68k` |
| NOR | 16 MiB | 8 MiB, header at 0x20000 | 14,942,208 bytes, small and big cards | 13,041,664 bytes, small and big cards | one ROM, up to 2 MiB, header at 0 |
| NAND (raw) | 138,412,032, given explicitly | 34,603,008 | none | none | none |
| RAM | 32 MiB | 16 or 32 MiB | 32 MiB | 16 MiB | 4 MiB |
| Screen | 320x480 | 320x320 | 320x320 | 320x320 | 160x160 |
| Silkscreen, bar | 0, 84 | 120, 80 | 120, 96 | 0, 251, side panel 38 | 60, 44 |
| Cards | SD 32 MiB to 2 GiB | SD 512 bytes to 1 GiB | SD 32 MiB to 2 GiB | Memory Stick 4 to 128 MB, Pro up to 1886 MiB | none |
| `network` | yes | no | no | no | no |
| `serial` | yes | no | no | yes | yes |
| `keyboard`, `usb`, `bluetooth` | yes | yes | yes | yes | no |
| `infrared`, `sound` | yes | yes | yes | yes | yes |
| `fastAudio` | yes | yes | no | no | no |
| `microphone`, `backgroundPanel` | no | no | no | yes | no |
| Camera, Hold | no, no | no, no | camera present (no sensor, stays off), no | no, Hold switch works | no, no |
| Home and Menu taps | Menu 54,466 | 27,351 and 27,405 | 23,352 and 23,412 | none (Home key F6) | none |
| `setup`, `launcher` | `tx`, `wallpaper` | `e2`, `wallpaper-upper` | `zire`, `wallpaper` | `tg50`, `tg50-clie` | `iiix`, `iiix` |
| Runner | `frond` | `frond` | `frond` | `frond` | `frond-68k` |

The IIIx also has `cpuHz` (16,580,608, the speed setting's Original) and `runs68k`.

`repalm` is an inherited model with no frontend. `palm-m515` is a reserved name with button bar art and no board, so the core refuses it.

### E2 variants

`palm-e2-franken` identifies the Tungsten E3 by company `Palm` and HAL `DmGR`. It shares the E2's `pxa255E2` board, NAND, SD controller, codec, GPIO keys, Bluetooth model and button bar. Its 320x480 display includes a guest-drawn input area, with no physical silkscreen.

The E3 profile enables both frontends and requires an 8 MiB NOR with its header at 0x20000 for new imports. RAM is 16 or 32 MiB, with 32 MiB as the default. Status-bar Home (20,466) and Menu (88,466) work with the input area expanded or collapsed. The modified ROM's NVFS and no-sleep handling stay separate from the shared hardware paths. The E3 has no Power control. Low-latency audio can use the shorter queue without ROM hooks. The qualified E3 preview shares the E2 HAL layout and uses the ARM stream-start verifier, with the Thumb queue hooks disabled. Its fingerprint is separate from the E2's.

## Detection and rejection

The detection fields tell the desktop, the core and the Python tools which device a NOR file belongs to. `deviceProfileInspectNor5` looks for a Palm card header every 32 KiB and matches its company and HAL IDs against the profiles. For `smallAndBigCards` it also checks that both cards are there, agree and run to the end of the file. A TX NOR has an IPL and no card header, so TX firmware is picked by `--device palm-tx` and the explicit NAND.

Checks only cover size, layout and identity where a header exists. They can't tell two dumps of the same size apart and don't show a file boots. Unknown names, firmware for another device, wrong sizes, a missing required NAND, bad RAM and display values, and damaged sessions are all refused before the machine is built, with no fallback to another device. Check these with sparse synthetic files in the tests; the repo never holds firmware.

`tests/device-profiles.cpp` (`make test-device-profiles`) and `tests/firmware-input.cpp` cover the table. Rejection without firmware:

```
python3 tests/device-rom.py --device sony-clie-tg50 --synthetic-rejection
```

## Board bring-up

New devices are ARM boards on the `SocPXA` base (`soc_PXA.cpp`) with a device file for the board's chips, like `devicePalmTungstenE2.cpp` and `deviceSonyTG50.cpp`. Port the smallest change that gives an ordinary cold boot, then check display, touch, setup and launcher on real firmware, then soft and hard reset. Measure screen predicates and tap coordinates, don't rely on fixed delays.

Then check save and reload in a fresh process (identity, display, guest state), and that a bad load into a running machine leaves the old machine working. Keep old sessions loading.

Each board also has things to settle:

- RAM probe: some ROMs write a marker and expect it not to read back, or expect a mirror. Say which in the board.
- USB cable detect: the GPIO and its polarity differ per device.
- Devices on chip selects the ROM touches but the real board doesn't answer on need a bus that takes writes and reads zero.

On the ARM boards 68k code runs inside PACE, through the interpreter in `uarm/uae`. A device with a real 68k CPU has its own runner instead, see [68k devices](#68k-devices).

### PACE and the API bridge

Install, launch, database export and the other app commands need a verified point where the guest is waiting in an application, found by patching the PACE entry points (`pace_patch.cpp`). Each board does this its own way:

| Device | Where the patch lives |
|---|---|
| TX | OS image loads from NAND into RAM, so the patch is found and applied there, after the OS loads |
| E2 | Patched from boot, with E2-specific pen, NVFS and UI hooks |
| Zire 72 | NOR, applied from boot. 5.2.8 has the E2's PACE layout |
| TG50 | Found in the original NOR and applied to an instruction shadow, so the original stays intact. Also hooks the automatic library Sleep/Wake call |

Signatures are checked before anything is patched. A missing or ambiguous match leaves memory unchanged. A signature match alone doesn't mean the device is ready. The `guest-ready` command (and every command that needs the guest) repeats a preflight at a supported boundary and returns `not-ready` if there isn't one. Each operation then takes a checkpoint of RAM, flash, devices and the card, runs inside bounded recovery, and rolls back on any fault, so a failed install, launch or export leaves the guest as it was. Plan on the checkpoint needing about the size of guest RAM, flash and card again in memory.

## 68k devices

The Palm IIIx (DragonBall EZ, 68k) runs in `frond-68k`, built from the vendored CloudpilotEmu core (UAE 68000 plus the DragonBall register models) and Frond's runner in `src/cloudpilot/frond/`. The two runners share the art, the window layout, the control socket framing (`src/common/DesktopChannel.h`) and the session container, and have separate main loops and command handling. Patches 0211 to 0229 add it.

- Profile: `board` is `ez328`, `detect` is `romHeader68k`, and `runs68k` is true. `deviceProfileRunnerEligible5` means the ARM runner and is false for it, `deviceProfileRuns68k5` tells the frontends to start `frond-68k`. `deviceProfileInspect68kRom5` reads the big-endian card header (`PalmCard`, `0xfeedbeef`, header version 3 to 5, EZ flag, reset vector in the ROM) and, on version 5, company `palm` and HAL `sumo`. The core then checks the ROM with `EmROMReader` and `EmDevice::SupportsROM`, so a Palm V 3.x or m100 ROM passes the header check and is refused by the core.
- ROMs: OS 3.1 retail and debug, generic OS 3.5 EZ retail and debug, IIIx/V OS 4.1. The ROM goes in the session's NOR slot, with the RAM and the Cloudpilot savestate. Frond stores its own clock marker next to the savestate (a session without `clock 1` has its cycle counters doubled on load).
- Cohort `ez68k`, no Fast engine (the setting is hidden), no PACE. A device with a 68k CPU has no PACE patch step, install and launch go through the Cloudpilot installer (`DbInstaller`) once `gSystemState.IsUIInitialized()` is true, which `guest-ready` reports.
- Speed: Cloudpilot's timers run from emulated cycles. Original runs them at `cpuHz`, Uncapped raises the cycle budget (8 times) while timers stay on real time. Cycles are the 68000 manual's clocks plus the bank wait states (ROM 1, RAM 2 on the IIIx) plus the LCD DMA and DRAM refresh time the guest programmed, see [performance.md](performance.md#68k-timing).
- Networking is PPP over the serial port to a host PPP server. Frond never turns on the NetLib redirect for a 68k device.
- Android: `libmain68k.so` runs the same `frond/main.cpp` in `Frond68kActivity`, in its own `:emulator68k` process, so the two UAE copies never share a process. `DeviceInventory.kt` lists 68k devices with `runs68k`.
- Another EZ or 68328/VZ Palm needs a profile, art and a setup recipe, and Cloudpilot already has the device model. A VZ board's bus timing is only unit-tested so far.
- Checks: `make test-frond-68k`, `tests/device-rom.py --device palm-iiix --rom FILE --launcher --resets`, `make test-core-control`, see [testing.md](testing.md#per-device-tests). The IIIx has no `SMOKE` entry yet.

## Fast engine requirements

The Fast engine is a JIT (Dynarmic). A new board runs in it when these hold:

- The JIT keeps copies of code. Anything that changes guest code from the host (PACE patching, peephole patches, reset, session load) has to clear it through `jitClear`. Guest writes to translated RAM pages drop the code by themselves.
- Instructions the JIT can't run, the PCs of hooks and watched sites go to the interpreter (`cpuJitPlain`). A new hook that changes instructions needs the same rule.
- `capabilities.backgroundPanel` (the TG50) keeps the machine running without drawing while the device menu is open in the background, with its audio output fenced. `capabilities.microphone` also fences the microphone and audio output during checkpointed guest work. Other boards leave both off.
- Run `make test-jit-differential` after any CPU change. It compares the JIT with the interpreter on random machine states and blocks. `make test-jit-overshoot` checks how far a run can pass its cycle budget.
- Smoke the device in Fast (`FROND_CORE=build/frond-jit tests/smoke.py run DEVICE`, see Smoke and setup).

## Peripherals and capability flags

List every control and port the real device has: keyboard, jog controls, storage, sound, power and sleep, USB, serial, infrared, Bluetooth and radios. Qualify each by itself, including reset, sleep and wake, and save and reload where state persists. Set its capability flag only when it works. The frontends grey out the rest.

- `capabilities` gates settings in the desktop (`capabilities()` in `desktop/FirmwareInput.h`), Android, the CLI and the core.
- `sound` is a separate fact from the storage kind and from `fastAudio`.
- The storage slot kind comes from `deviceProfileStorageSlot5` (SD, Memory Stick or none). Sizes are checked by `deviceProfileSdSizeValid5`, `deviceProfileMsSizeValid5` and `deviceProfileMsProSizeValid5`.
- Add a presence field to the frontend part when a peripheral needs one (the camera and SD slot have one).

When a peripheral needs its own guest procedure, add a scenario behind `tests/device-rom.py` and reuse the shared console, frame, setup and probe helpers in `tools/`.

## Button bar and silkscreen art

The bar under the screen and the printed silkscreen are drawn at run time as original vector art by the native core, in `src/uarm/native/DeviceControls.{h,cpp}` and `Silkscreen.{h,cpp}` of the patched source. There are no image files. The desktop and Android apps only host the SDL runner, so a new bar shows up in both with no frontend drawing code.

The bar should be a recognisable approximation of the real one, not a close copy: the right button count, shapes, arrangement, colours and bezel, with original glyphs. No logos, brand names, another emulator's skin or bundled reference photos.

### Profiles (DeviceControls.h)

- `DeviceControlProfile` is one device's bar: size in strip coordinates, its `DeviceButton`s, the five-way ring and select pill, a `DeviceControlStyle`, the app key corner radius, `unit` (strip units per screen pixel, 2 for 160-wide screens), and for a keyboard or side panel `keyboardHeight`, `sideTab` and `sideTabWidth`.
- Strips are authored 320 units wide. The TX is 320x84, the E2 320x80, the Zire 72 320x96, the TG50 320x224 and the IIIx 320x44.
- `DeviceButton` is a key (`keyIdHard1` to `keyIdHard4`, `keyIdUp` and so on), its rectangle, a label for tooltips, a `DeviceControlIcon`, an optional corner and the `section` it belongs to (`main`, `keyboard` or `sideTab`). A key that is half of a bigger shape, like a rocker, gets a `shape` to hit against.
- `deviceControls(DeviceArt)` returns the profile for an art value. Add the new one there, and a value in `DeviceArt` in `device_profile5.h`.
- Hit testing and drawing share the same geometry. App keys hit their whole rounded rectangle. The five-way hits the select pill first, then the ring split along its diagonals (`deviceNavigatorAt`). Moving a shape moves its hit area.
- The strip is centred under the screen (`deviceButtonRect`), so a narrower screen needs its own width. The TG50 scales its bar across the whole body and can hide its keyboard and side panel (`DeviceControlView`).
- The jog dial on the TG50 is the `keyIdJogUp`, `keyIdJogPress`, `keyIdJogDown` and `keyIdBack` keys on the side panel. `JogPlacement` puts it hidden, left of the LCD or left of the keyboard.

### Drawing (DeviceControls.cpp)

- Everything is drawn into a `Canvas` with signed distance functions (`box`, `ellipse`, `ring`, `segment`, `polygon`, `arc`), then shaded and anti-aliased at the real window scale, so it stays crisp at every display scale.
- Each style has its own painter (`paintTxControls`, `paintE2Controls` and so on). A new style adds a `DeviceControlStyle` value and a painter. `paintNavigator` and `iconDistance` (the glyphs) are shared.
- `drawDeviceControls` takes the pressed keys as a bit mask, so every key needs a visible pressed state. Keep a key's pressed state and release it on focus loss, hiding, closing and rotation.

### Silkscreen

- `DeviceSilkscreen` holds the size in LCD pixels (the E2, Zire 72 and IIIx are 320x120), the `SilkscreenButton`s (Home, Menu, Favorites, Find, clock, brightness, keyboard) and the two Graffiti areas.
- Take the rectangles from the ROM, not a photo. The E2's and Zire's come from the `'aslk'` 19000 resource, doubled from Palm OS coordinates to LCD pixels. Taps on the silkscreen go to the guest as pen input, so the art has to sit where Palm OS expects the buttons.
- `paintSilkscreen` draws the panel, discs with glyphs (`silkscreenDiscGlyph`), the clock and brightness glyphs and the abc/123 areas. `createDeviceSilkscreen` makes the texture.

### In the frontends

- Desktop: `capabilities()` in `desktop/FirmwareInput.h` greys out what a device doesn't have. The window size comes from the frontend fields.
- Android: `PalmButtons.kt` maps on-screen and controller buttons to keys, and `tapPoint()` gives the pen positions for Home and Menu where they are silkscreen or status bar taps (from `homeX`, `homeY`, `menuX`, `menuY`). `Controller.kt` lists the buttons per device. `FrondActivity.BACKGROUND_HEX` is the dark end of the bar art and fills around the screen.

### Making one

1. Write the real layout down first: screen size, every hardware button and its place, the navigator (five-way, rocker, scroll keys, jog dial), silkscreen icons and Graffiti areas, colours and materials. Read the silkscreen rectangles out of the ROM.
2. Add the profile and, if it doesn't look like an existing one, a new style and painter.
3. Render the bar and silkscreen at 1x and 2x and compare them to reference photos kept outside the repo. Adjust until it reads as the device at a glance.
4. Extend `tests/device-controls.cpp` with every key, the five-way parts and the gaps between keys, and run `make test-device-controls`.
5. Map the keys on Android and set the capabilities on the desktop.

## Frontend fields and generated data

Desktop menus, firmware detection and the core read the profile table directly. Android and the Python tools read exports of the same table. JSON includes every profile; the generated Android inventory only includes profiles with `frontend.available` set. After changing a profile:

```
make prepare
make generate-device-data
```

This compiles `tools/device-profiles.cpp` into `build/device-data/export` and rewrites `tools/device-profiles.json` and `android/app/src/main/kotlin/com/rm/frond/DeviceInventory.kt`. The exporter never opens firmware. Commit both generated files with the profile patch. `make check-device-data` and the Android `preBuild` task fail if they're stale. The JSON also carries `runner_eligible` for ARM boards and `runs_68k` for the separate 68k runner. Python runners use these fields, so inventory-only and art-only profiles stay out of them.

Android's `Device.kt` has the fields in Kotlin. The `TX`, `E2` and `ZIRE72` names there and the desktop `Device` enum names are compatibility aliases. HotSync button presence comes from `deviceProfileHasHotSyncButton5`, exported as `hot_sync_button` and generated as Android's `hotSyncButton`. Check remaining device-specific actions in both frontends. Enable `available` once the board has passed the generic firmware run and the desktop and Android import, launch, controls, settings and session workflows work. A core screenshot alone doesn't show the frontend works.

## Smoke and setup recipe

`tools/palm_boot.py` holds the measured recipes. The profile's `setup` names one (`SETUP_RECIPES`, which also picks the reset recipe in `RESET_RECIPES`) and `launcher` names a frame predicate (`LAUNCHER_CHECKS`). Reuse a recipe only when the device shows the same screens. Otherwise add a small recipe and predicate and name them in the profile. An unknown recipe fails before any input is sent.

The generic firmware check:

```
python3 tests/device-rom.py --device DEVICE --nor nor.bin --launcher
```

Add `--nand nand.bin` where needed, `--sha256 HASH` to pin the image, `--resets` for the measured soft and hard reset flows and `--evidence build/NEW-DIRECTORY` to keep a named run (an existing directory is refused). Without `--launcher` it only checks a short real boot. Missing firmware is a skip. The Zire 72 default runs its own suite, including the `FROND_ZIRE_*` opt-ins.

Memory-failure diagnostics fail the check unless the image's guest recovery was seen first. `--expected-faults expected.json` needs `--sha256`, `--launcher`, `--resets` and `device-session-probe`. The file has `device`, `nor_sha256` and `phases` (`boot`, `setup`, `reload`, `rollback`, `reset-soft`, `reset-hard`), each a list of exact `access`, `size`, hexadecimal `va` and `pc`, integer `fsr` and positive `count`. Extra, missing or unparseable faults fail.

For the quick check after shared changes, add an entry to `SMOKE` in `tests/smoke.py`: the firmware files, the `setup` function, an `app` to launch with `taps` and the screen `area` they change, and optionally `media`, `audio` and a one-time `reminder`. `make smoke-sessions` makes the sessions and `make smoke` runs per device: app and tap, save and reload, supported card media and sound. `FROND_CORE` picks another core binary (`build/frond-jit` for the Fast engine) and `FROND_DEVICE_IMAGES` another firmware folder. See [testing.md](testing.md) for the rest.

Keep firmware, sessions, installed Palm applications, raw PCM and captures out of git. Public tests build synthetic headers, geometry and sessions.

## Hardware notes per SoC

### PXA270 (Palm TX, Zire 72)

- CP15 reports the XScale CPU as `0x69054114`. With alignment checking off, ARMv5 word reads align the access and rotate single-word loads, and stores and multiple transfers use aligned addresses. With it on, a misaligned word access takes a precise abort.
- OSCR0 runs at the PXA27x 3.25 MHz rate. Scheduler periods use integer nanoseconds. The OS timer batches up to 32 ticks between events, counter reads account for progress in the current batch, and during interrupt-driven idle the timer can jump to its next interrupt.
- The instruction fetch model keeps two 32-byte fetch buffers across MMU transitions, and they are saved in the CP15 chunk. TLB lockdown isn't implemented.
- Power sleep (`PWRMODE=3`) is separate from idle. It stops the CPU and peripheral clocks until a qualified GPIO edge, keypad level or RTC alarm; ordinary IRQs can't wake it. On exit the CPU, CP15 and non-retained peripheral registers reset from the power-on snapshot, while RTC, wake setup, scratchpad, clocks, external devices and SDRAM stay. PSLR sets timer and SRAM retention, and GPIO outputs stay held through PGSR until PSSR[PH] is cleared. The reset is applied at the scheduler wake boundary, not inside input or IRQ callbacks. Keypad wake needs manual scans (column select bit position, last column, row mask, write-one-to-clear PKSR). Standby, deep sleep, USB, MSL and power-fault wake and extended timers aren't modelled, and the RTC uses host wall time. `make test-pxa-power` covers it.
- Session loading releases stale host keys without making a wake event.

### Palm TX

- NAND command, address and data mappings, ready GPIO, device ID, RAM probe marker, keypad matrix and GPIO defaults are in the board. The TX NOR holds an IPL, not a card header.
- The input path is a WM9712L codec on the AC97 link. Its IRQ goes to PXA GPIO27, and the driver uses the codec's sticky virtual GPIO13 pen detector. The E2 keeps its own GPIO3 PENDOWN line.
- Slot-mode ADC conversions update the AC-link stream and the readable result register, and a coordinate poll stays busy until its last conversion. Accepted register writes report as done, and power-ready status is read-only, which lets the guest leave its initialisation loops.
- The AC97 controller honours interrupt masks, updates FIFO and global status, and completes mapped FIFO reads and writes on underrun or overflow. An empty receive read returns stale FIFO storage without moving the pointer and sets FIFOE. A transmit underrun repeats the last sample. Halfword reads write only two bytes.
- The virtual panel maps all 320x480 pixels onto ADC range 320 to 3800, both axes in the direction the driver expects. Mouse pressure is a fixed 100, as the TX driver rejects 320 and above.
- The USB cable detect is GPIO13, active low. The PACE patch lives in RAM.

### PXA255 (Tungsten E2)

- The E2 detects its USB cable on GPIO35, active low, and its pen on GPIO3. The NOR has a 128 KiB boot block, then the card header at 0x20000.

### Zire 72

- Its small ROM clears RTC registers at the mistyped address 0x04090000, where nothing answers on chip select 1, so the bus takes the writes and reads zero. The USB cable detect is GPIO15, active high. The 5.2.8 ROM runs unpatched, and its PACE patch stays in NOR.

### DragonBall EZ (Palm IIIx)

- 16.58 MHz 68000. The IIIx 3.1 and 3.5 ROMs program ROM with 1 wait state and RAM with 2 (`bus-wait`). `bus-timing` shows the LCD DMA and refresh times the guest set: the 3.1 ROM runs the LCD at about 246 Hz, 3.5 and 4.1 at 82 Hz in 1 bpp.
- Holding power for about two seconds turns the green backlight on.
- The HotSync button is on the cradle, so it is a command (`hotsync`) and a running-page button, not a device key.

### PXA250 (Sony CLIÉ TG50)

- The board adds the Sony TG50 support chip (`mmiodev_TG50uc`) with the keyboard matrix, jog and Hold. Audio is the Sony mono path on the shared ring-buffer queue, and storage is a Memory Stick controller. RAM is 16 MiB, plus an extra MiB and a mirror that reproduce the RAM probe workaround.
- Its firmware has no SndStream.
