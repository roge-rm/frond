# Tungsten E2 qualification workflow

The Tungsten E2 is available in the Qt desktop and its control protocol
(`e2-desktop` in the profile inventory). It passed the gates below on the dumped
ROM: cold boot with and without NAND, calibration, editor content and continued
input after a fresh-process restore, reset, sleep and wake, SD, audio and six
representative apps. The other inherited cohorts stay unavailable.

Known limits. The E2's VFS returns `vfsErrFileEOF` at the end of a file, where
the TX returns no error, so the SD test script doesn't accept E2 logs as they
are. Networking remains unavailable. Bluetooth has separate original-stack
qualification as described below. USB HotSync is supported as described below. Physical-device timing
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
IrDA beaming and virtual Bluetooth are qualified as described below; networking
remains unavailable. Power-key sleep
and wake work on the dumped ROM, including a second
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

## USB HotSync

The E2 uses the PXA255 USB controller with fixed bulk endpoints. The shared
host and PTY/TCP bridge support pilot-link HotSync from the Qt desktop or
`--usb pty` / `--usb tcp:PORT`. Its cradle serial port remains unavailable.
See [USB HotSync](peripherals.md#usb-hotsync) for usage and transport limits.

Qualification covers enumeration, database listing, installing and fetching a
small record database with identical record payloads, and fetching it again over
TCP after a fresh-process session restore. With the USB bridge active, the E2
also passed true sleep and wake, restoring a sleeping session in a fresh process,
checkpoint preservation, rejected-load rollback and normal-load reattachment.
The tests do not establish physical USB timing or arbitrary USB accessory support.

`test-pxa255-udc` covers fixed FIFO addresses and directions, byte-wide FIFO
accesses, EP0 hardware requests and configuration acknowledgement, IRQ masks and
acknowledgements, backpressure, partial-packet/legacy restore and corrupt-state
refusal. `test-usb-bridge-soc` checks shared host lifecycle behavior on both boards.

## Low-latency audio

The desktop and `--fast-audio` support the qualified E2 firmware
with a separate runtime driver profile. It previews one signed 16-bit stereo
44.1 kHz stream before the firmware queue, retaining the driver's volume and
pan calculation. Qualified basic ARM callbacks can use the earlier render-to-writer
handoff after a witnessed stream start; extended/decoder streams remain on HAL.
Other firmware is refused; unsupported streams or playback starvation fall back
to normal audio. See [fast audio](fast-audio.md).

Qualification includes exact PCM agreement with the normal codec path and
isolated SunVox note responses, plus preview reactivation after session restore,
audio off/on, sleep/wake and restoration of a sleeping session. Reset clears
queued preview audio. Firmware-free profile and ARM interception tests cover
both E2 and TX. Host callback measurements exclude sound-server and physical
output delay; the smaller queue remains sensitive to host scheduling.

## Infrared keyboard

The E2 supports the same Palm Universal Wireless Keyboard accessory as the TX.
The original Palm 1.13 driver receives typed lower/upper-case characters through
the E2 STUART. Qualification also checks reset, attachment persistence, fresh
restore with Shift held, detachment and reattachment. The saved enabled driver
programs a 9600-baud 8N1 STUART receiver with infrared enabled. The accessory
model retains its 8N2 transmit spacing; this does not assert that the E2 driver
programs two stop bits.

`make test-keyboard` checks both boards through their real UART MMIO, including
receiver gating, exact accessory packets, partial-packet restore and releases.
The optional `tests/desktop-keyboard.py` acceptance workflow accepts an E2
session too. The driver must be supplied separately; see
[keyboard setup](peripherals.md#computer-keyboard). This is the accessory byte
protocol; IrDA beaming and Bluetooth are separate peripherals.

## IrDA beaming

TX/E2 beaming passes through the original guest stacks in all four directions.
The SDK probes verify exact 4096-byte payloads and a serialized two-record Palm
database imported in both cross-device directions by the original `ExgDBRead`
API, preserving records and IDs.
Cancellation during payload transmission returns the original user-cancel error;
a fresh peer and retry delivers the complete payload without changing prior logs.
Peer interruption, checkpoint/refused-load rollback, accepted replacement, power,
sleep/wake and fresh sleeping-session restore are also checked. The desktop
enables beaming and prevents simultaneous infrared keyboard attachment.

See [infrared setup, tests and timing limits](infrared.md). This decoded SIR
transport does not qualify optical timing or host IrDA-stack interoperability.

## Virtual Bluetooth

The original E2 driver and unchanged uploaded image operate through the optional
[local virtual controller provider](bluetooth-provider.md). Original Exchange
Manager send/receive, real mid-payload Cancel and fresh retry, raw BtLib RFCOMM
with credit advancement and L2CAP echo all pass exact 4096-byte checks. Original
PIN entry, authentication/encryption state queries and trusted-device
persistent flags are checked separately.

Read-only checkpoint and refused-load rollback preserve the live transfer and
exact model/RAM/NAND state. Idle accepted replacement, reset/reinitialization,
true sleep/wake and fresh sleeping-session restore are followed by a fresh
successful transfer. External radio connections are not serialized. Loading
mid-transfer closes the old peer without claiming success; the original progress
dialog can remain blocked and require Reset before radio reinitialization.

The desktop exposes the controller for E2, default off. This qualifies the listed
virtual profiles and original APIs, without claiming interoperability with a
physical adapter, arbitrary profiles or physical radio/encryption timing.
