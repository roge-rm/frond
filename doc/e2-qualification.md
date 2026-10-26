# Tungsten E2 qualification workflow

The E2 remains `inherited-unverified` and unavailable to the Qt desktop and its
control protocol. Patch `0044-e2-cohort.patch` adds physical app-button replay
names and clears obsolete queued host input on E2 restore. It does not enable a
new desktop device or change TX key aliases or restore behavior.

## Firmware-free regressions

```sh
heavy build make -j2 test-e2-state test-device-profiles test-tx-state test-device-controls
heavy build make check-reconstruction
```

`test-e2-state` exercises the inherited PXA25x board with synthetic ROM, NAND,
RAM and card data, at both 16 and 32 MiB RAM. It checks queued GPIO app/navigation/
power keys, release of obsolete queued host input after restore, direct GPIO50
pen detection including the silkscreen area, the RAM
probe mirror, actual LCD DMA rendering, v4/v5 envelope round trips, fresh-SoC
RAM/LCD/card restoration, reset with retained RAM/card detect, audio queue
clearing, and atomic rejection of truncated or missing-codec peripheral state.
It does not execute Palm firmware or establish usable guest applications.

`test-device-profiles` checks header recognition and rejection before SoC/SDL
construction, including E2 replay surface bounds and physical app-button names.
`test-tx-state` retains TX state/reset and E2 legacy peripheral-chunk coverage.
The new board test is also included in `make test`.

## Optional private firmware evidence

Supply a legally held E2 NOR with the inherited recognized Palm/E2 header. The
runner refuses other headers; neither file size nor screen geometry identifies
firmware. A matching raw NAND has 528-byte pages, including spare bytes, for
34,603,008 bytes total. That geometry alone cannot identify its device, language,
provenance or compatibility with a NOR. Verify the pairing privately. Omitting
NAND deliberately exercises the inherited erased-flash path, which may still
require guest calibration/setup and is not a boot guarantee.

```sh
heavy build make -j2 out/frond
heavy emulator python3 tests/e2-rom.py \
  --rom /private/e2-nor.bin --nand /private/e2-nand.bin \
  --output /private/new-e2-results
```

The runner uses dummy SDL video/audio and disables sound. It reads inputs, runs
the native CLI for 60 host seconds, captures the LCD, saves a full session, exits,
and restores that session in a fresh native process for five seconds. It then
captures and saves again. Inputs are hashed before/after and never written.
The binary hash, source hashes, logs, sessions and captures stay in the new
mode-0700 result directory. Keep that directory outside version control.

`--boot-seconds` adjusts the host-time boot allowance. Optional `--input-replay`
and `--replay-seconds` run a locally authored replay after boot and before saving;
allow enough time for every event. Snapshot commands are redirected into the
private result directory. Replay uses host time and normal SoC pen/key queues.
Use `hard1`–`hard4` for physical app-button positions, plus navigation and power
names; the older app names describe TX assignments. E2's LCD is 320×320 and its
full touch surface including silkscreen is 320×440. Captures contain only the LCD.

Successful execution records evidence for inspection, not an automatic guest UI
pass. Inspect both cold and restored captures. For calibration, confirm each
target advances and setup finishes. Open an editor, enter new content using
normal guest input, save, exit and restore in a fresh process, then verify that
content and subsequent input work. Check normal reset separately. Do not infer
text entry from a changed frame, or persistence from an unchanged setup screen.
Do not accept application licences or fetch fixtures as part of this workflow.

## Qualification gates still required

Keep the cohort unavailable to the desktop until the device-profile gates are
met. Separate cold boot with existing and blank NAND, complete calibration,
editor content persistence and continued input after fresh-process restore.
Check normal reset, audio, guest storage/SD operations, and representative apps
independently. Codec/SD synthetic passes do not establish guest audio or storage.
IR keyboard drivers, Bluetooth, USB/HotSync, networking and sleep/wake need their
own evidence. Physical-device timing and behavior require available hardware.

Record exact fixture identities, source hashes, observations and unresolved
failures only in a private local report. Do not commit NOR/NAND, metadata,
identifying dump hashes, sessions, captures, logs or exported guest databases.
