# Tungsten E2 qualification workflow

The Tungsten E2 is available in the Qt desktop and its control protocol
(`e2-desktop` in the profile inventory). It passed the gates below on the dumped
ROM: cold boot with and without NAND, calibration, editor content and continued
input after a fresh-process restore, reset, sleep and wake, SD, audio and six
representative apps. The other inherited cohorts stay unavailable.

Known limits. The E2's VFS returns `vfsErrFileEOF` at the end of a file, where
the TX returns no error, so the SD test script doesn't accept E2 logs as they
are. IR keyboard drivers, Bluetooth, USB and HotSync, and networking aren't
covered; the desktop turns those controls off for the E2. Physical-device timing
isn't checked. Patch `0044-e2-cohort.patch` adds physical app-button replay
names and clears obsolete queued host input on E2 restore.

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
runner refuses other headers, a NOR that isn't 8,388,608 bytes and a NOR whose
card header isn't at 0x20000, after the 128 KiB boot block. E2 sessions saved
by earlier builds skip these two checks. Size and layout still
don't identify firmware. A matching raw NAND has 528-byte pages, including spare bytes, for
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

## Qualification gates

The desktop was enabled once these were met. Separate cold boot with existing and blank NAND, complete calibration,
editor content persistence and continued input after fresh-process restore.
Check normal reset and representative apps independently. Codec/SD synthetic
passes do not establish guest audio or storage.
IR keyboard drivers, Bluetooth, USB/HotSync and networking need their own
evidence. Power-key sleep and wake work on the dumped ROM, including a second
cycle and a session saved asleep and woken in a fresh process. A soft reset opens
Preferences on Date & Time, as the ROM does on hardware. Physical-device timing and behavior require available hardware.

On the dumped ROM the SD probe (`tests/palm/sd-probe.c`) reads, writes and
deletes files on a 32 MiB card, after card insert, eject and reinsert, after
sleep and wake with the card in, and after a fresh-process restore with the card
embedded in the session. A file held open and written through a sleep is
complete afterwards. The E2's VFS returns `vfsErrFileEOF` on a read that reaches
the end of the file, where the TX returns no error, so `tests/sd_results.py`
doesn't accept E2 logs as they are. The codec runs at 44.1 kHz with variable
rate on. Taps, the alarm preview in Sounds & Alerts and SunVox notes give guest
PCM at the right pitch, it stops in sleep and comes back after wake. Edited
memos survive leaving Memos and a fresh-process restore. Damaged session files
are refused, and a refused savestate leaves the running guest as it was.

Record exact fixture identities, source hashes, observations and unresolved
failures only in a private local report. Do not commit NOR/NAND, metadata,
identifying dump hashes, sessions, captures, logs or exported guest databases.
