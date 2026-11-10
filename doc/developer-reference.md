# Developer reference

Everything here is for running the core and the tools without the desktop or Android app. `make check-docs` fails if an option, command, environment variable, tool or make target in the sources is missing from this file.

The core is `out/frond` (built by `make out/frond`). The desktop app runs it as a child and talks to it over the control socket.

The Palm IIIx has its own runner, `out/frond-68k` (`make out/frond-68k`, installed as `/usr/lib/frond/frond-68k-core`). It takes the same desktop arguments and speaks the same control socket, but has its own options and console, see [Palm IIIx (frond-68k)](#palm-iiix-frond-68k). Everything outside that section is about `out/frond`.

## Command line

`frond [options] nor_or_session`. The positional argument is a firmware NOR image or a saved session. `frond --help` prints the same list.

| Option | Meaning |
| --- | --- |
| `-h`, `--help` | Show the help and exit. |
| `-v`, `--version` | Print the version and exit. |
| `-n`, `--nand FILE` | NAND image, for devices that have one. |
| `--device NAME` | Device for raw firmware: `palm-tx`, `palm-e2`, `palm-zire-72` or `sony-clie-tg50`. The E2 is also found from its NOR header. |
| `--engine exact\|fast` | CPU engine: `exact` is the interpreter, `fast` is the JIT where it's built in. Default `exact`. `FROND_JIT` overrides it. |
| `--run-for SECONDS` | Exit after this many wall-clock seconds and print execution diagnostics. Not with `--script` or `--desktop-socket`. |
| `--framebuffer-output FILE` | Save the unrotated LCD as a PNG at the end of `--run-for`. Needs `--run-for`. |
| `--input-replay FILE` | Replay timed pen and key events and snapshots, see [Input replay](#input-replay). |
| `-s`, `--sd FILE` | SD card file. |
| `--work-folder DIR` | Run a session from elsewhere, in this empty machine folder. Its card files are copied there. |
| `--card-swap` | Load a session whose card files the launcher changed while it was off. A slot with another card, the same card changed elsewhere, or none comes up ejected, and the card there now goes in once the Palm has run 500 ms awake (5 s at most), with a `card` event. |
| `--card-identity HEX` | With `--card-swap`, the identity of a card put in, as 64 hex digits (its 32 descriptor bytes, as in `card.json`). |
| `--card-protected` | With `--card-swap`, a card put in is write protected. |
| `--ram-size MB` | RAM size, 16 or 32. |
| `--mips N` | Target speed in MIPS. Default: the device's Original speed (150 on the T|X and Zire 72, 96 on the E2 and TG50). |
| `--audio-period FRAMES` | Host audio period, 64 to 8192. Default 512. Low-latency audio sizes its limits from it; Android passes 1024 or more. |
| `-q`, `--no-sound` | Start with audio off. |
| `--fast-audio`, `--experimental-fast-audio` | Preview one qualified TX, E2, E3 or Zire 72 stereo stream before the firmware audio queue. The E3 and E2 de-NVFS image use the HAL path with an ARM stream-start verifier and no Thumb queue hooks. |
| `--low-latency-queue` | The `--fast-audio` queue limits without the preview, on any device and firmware: low startup thresholds, and the 120 Hz slice and pending caps of 6 and 4 host periods (3072/2048 frames at the default 512) while non-silent frames were queued in the last 100 ms. The frontends pass it for Low-latency audio on ARM devices with sound when the firmware is not qualified for the preview, including modified E3 images and the TG50. |
| `--gdb PORT` | Wait for GDB on 127.0.0.1:PORT before running, see [GDB and PACE monitor](#gdb-and-pace-monitor). |
| `--serial off\|pty\|tcp:PORT\|unix:PATH` | Connect the serial port to a host PTY, a loopback TCP listener or a Unix socket, see [Connections](#connections) (one client at a time). |
| `--usb off\|pty\|tcp:PORT\|unix:PATH` | Same for the USB HotSync port, through a built-in USB host. |
| `--irda off\|pty\|tcp:PORT\|unix:PATH\|phone` | Same for infrared SIR bytes, or `phone` for the core's mobile phone (the `irPhone` capability). Not with the infrared keyboard. |
| `--bluetooth off\|SOCKET` | Connect the virtual radio to a local H4 controller Unix socket (such as `btvirt`). |
| `--desktop-socket PATH` | Connect to the desktop launcher's control socket, see [Control socket](#control-socket). Not with `--run-for` or `--script`. |
| `--save-on-disconnect FILE` | Save the session to this absolute path if the control socket closes without a `quit`, then exit, 1 if that save failed. Holds `FILE.core` locked for the run. Needs `--desktop-socket`. |
| `--script FILE` | Run console commands from a file at start. One per line, `#` starts a comment. |
| `--display WxH` | Display mode, if the device supports it: 160x160, 160x240, 240x240, 240x320, 320x320, 320x480, 480x480 or 480x720. |
| `--window-scale N` | Window scale: 1, 1.25, 1.5, 1.75 or 2. Default is the largest that fits. |
| `--small-window` | Same as `--window-scale 1`. |
| `--window-fit MODE` | Fit the device to a resizable window: `fit`, `whole` or `fill`. |
| `--window-background RRGGBB` | Colour around a fitted device. Default 000000. |
| `--smooth-lcd` | Smooth pixels instead of sharp nearest-neighbour. |
| `--hide-keyboard` | Hide the TG50 keyboard. |
| `--jog-controls hidden\|screen\|keyboard` | TG50 jog controls: hidden, left of the LCD, or left of the keyboard. Default `screen`. |
| `--hide-jog-controls` | Same as `--jog-controls hidden`. |

For a headless run set `SDL_VIDEODRIVER=dummy` and `SDL_AUDIODRIVER=dummy`.

`frond --card` makes, imports, exports, settles or checks a card file without starting a machine:

```sh
frond --card make sd|ms|mspro SIZE OUTPUT
frond --card import IMAGE sd|ms|mspro OUTPUT
frond --card export CARD IMAGE
frond --card settle CARD
frond --card discard CARD
frond --card verify CARD
```

SIZE is in MiB for an SD card or a Memory Stick Pro and in MB for a classic stick, within what some device takes. Import takes a raw SD or Pro image, a `.frondms`, or a raw classic stick image (its logical blocks in order, which get fresh boot blocks). The output is a settled card file (`.frondsd`, `.frondmsp` or `.frondms`) or, for export, a raw image; a classic stick exports its logical blocks, or a version 2 `.frondms` when IMAGE ends in `.frondms`. It's written beside OUTPUT and renamed into place, so a failed or cancelled run leaves nothing and an existing file stays. Settle finishes a change cut off beside a card file (below), discard first drops a standalone journal that doesn't fit the card, with its pending list, then settles, and verify checks every byte of one against its CRCs without changing it. Settle and verify also take a machine's `nand.frondnand`. Standard output has `progress DONE/TOTAL` in bytes at each whole percent, then `ok KIND BYTES` (`ok image BYTES` for export, `ok settled GENERATION` for settle and discard, `ok verified GENERATION` for verify) or `error CODE MESSAGE`. The exit status is 0, 1 on failure or 2 on bad use. SIGTERM or SIGINT cancels. Codes: `usage`, `size`, `read`, `image`, `same`, `unsettled` (a machine's save journal beside the card, or a change that doesn't fit it), `busy` (the card is open in Files), `space` (the folder needs the output plus 16 MiB free), `write`, `cancelled`.

The card files, journals, settle rules and session media are in [Storage formats](#storage-formats).

The desktop launcher `frond-desktop [session]` takes `--core PATH` (the emulator to run, found beside it or in `/usr/lib/frond` by default), `--core-68k PATH` (the same for 68k devices like the Palm IIIx, default `frond-68k` beside the ARM core), `-h` and `-v`.

## Storage formats

Every medium a machine keeps outside its `.frond` is a container file in the machine's folder, written and settled by one library (`src/common/storage/`) that the core, `frond --card`, the desktop app and Android (through `libfrondcard.so`) all use.

| Medium | File | Magic | Side region |
|---|---|---|---|
| SD card | `sd.frondsd` | `FRONDSD\0` | none |
| Memory Stick Pro | `ms.frondmsp` | `FRONDSD\0` | none |
| Classic Memory Stick | `ms.frondms` | `FRONDMS\0` | boot blocks, extra bytes, location map, spare slots |
| Internal NAND (T\|X, E2) | `nand.frondnand` | `FRONDNA\0` | spare bytes of every page |

### Container

A 64-byte header, little endian, then the stored bytes, then for version 2 a CRC table.

| Bytes | Version 1 | Version 2 |
|---|---|---|
| 0-7 | magic | magic |
| 8-11 | version 1 | version 2 |
| 12-15 | sector size, 512 | sector size, 512 |
| 16-23 | payload bytes | payload bytes |
| 24-27 | CRC32 of the payload | header CRC: the header with this field zeroed, then the table |
| 28-35 | zero | content generation |
| 36-43 | zero | table offset, 64 + stored bytes |
| 44-47 | zero | chunk size, 1 MiB |
| 48-55 | zero | side region bytes |
| 56-63 | zero | zero |

The payload is the medium's logical sectors. With a side region the stored bytes are the payload, zeros to the next 1 MiB boundary, then the side region; without one they're just the payload. The table has one CRC32 per 1 MiB chunk of the stored bytes, the last one short, so the padding and side region are checked too. Only SD and Pro files can be version 1, and the classic stick and NAND magics need version 2 with a side region. Payloads are at most 2 GiB. A load checks the header CRC and every chunk, and refuses the file if one is wrong. The generation goes up by one at every save and every Files visit that changes something.

### Journals

A change is never written straight over a container. `CARD.journal-N` takes the file to generation N:

- Header, 64 bytes: `FRONDJNL`, version u32 (2), flags u32 (bit 0 standalone), stored bytes u64, from generation u64, to generation u64, record count u64, table count u64, 8 zero bytes.
- Records: stored-bytes offset u64, length u32, the bytes.
- The whole new chunk table.
- Trailer, 16 bytes: `FRONDEND`, CRC32 of everything before it, zero u32.

A journal is written as a temp and renamed, so one under its own name is complete. A session journal comes from a machine's save and is replayed only by a load of the `.frond` at N; version 1 journals from older saves are read as session journals. A standalone journal comes from Files and is replayed whenever the card is settled. A save that writes the whole medium (its first save, or a conversion) writes `CARD.journal-N.full` instead, a whole copy at N, which a load of that `.frond` renames into place.

Files writes each change in three steps, holding an open file description lock on the card (which Android's `FileChannel` lock also sees):

1. Before data goes into a chunk, its number goes into `CARD.journal-N.pending` (`FRONDJP1`, stored bytes u64, generation u64, then a u32 per chunk), fsynced. Data only goes into clusters the FAT calls free.
2. The data is fsynced, then the FAT and folder sectors and the new table go into a standalone journal, written as `.frondsd.tmp-edit-CARD-XXXXXX` and renamed.
3. The journal is applied in place with the header, then the journal and pending list go.

Earlier Android versions left `CARD.files-intent` and `CARD.files-commit` instead; those are still settled.

### Settle

Settle finishes whatever a crash left beside a container, and runs before every load, move, export, Files visit and `frond --card` command. In order:

1. Old Android `.files-commit` and `.files-intent`.
2. Standalone journals, replayed at their from or to generation.
3. Pending lists with no journal: the listed chunks' CRCs and the header are rewritten at N (only free space changed, so the change isn't there). Leftover standalone temps go.
4. With a `.frond` to load, its save: a session journal or `.full` copy to the generation the `.frond` names. Other journals go once the card is at that generation.

Steps 1-3 take the card's lock, so a card open in Files is left alone (`busy`). Each step is safe to repeat after a crash in it. Settle refuses rather than guesses, and leaves the files as they are:

| Found | Code | Message |
|---|---|---|
| A standalone journal of another size or generation | `mismatch` | The card has an unfinished change that doesn't match it. |
| A pending list of another size | `mismatch` | The same |
| A pending list or journal beside a header that doesn't parse | `image` | The card file is damaged. |
| A session journal or `.full` with no `.frond` loading it | `unsettled` | The card has an unfinished save. Open its machine once to finish it. |

Discard unfinished change (Cards and Files in both apps) and `frond --card discard` drop the mismatched journals and pending lists, then settle. A journal name whose generation doesn't fit in 64 bits isn't taken as a journal; a load at the card's generation removes it with the other stale files.

### Classic Memory Stick

A version 2 `ms.frondms` keeps every byte the Sony driver can see. The payload is the logical blocks in order, 494 in the first 512-block segment and 496 in each later one, so logical sector = block x pages per block + page. The side region holds the data of physical blocks 0 and 1 (the boot blocks), the 9 extra bytes of every physical page, a u32 location per physical block (erased, boot, a logical slot or a spare slot) and the spare slots, one block each. The block the driver uses for a logical block, by its holder rule (overwrite bit clear wins, then the higher block), always sits in that logical block's slot. Other written blocks go in the spare slots, then in unused logical slots.

| Size | Blocks | Pages per block | Files formats it as |
|---|---|---|---|
| 4 MB | 512 | 16 | FAT12 |
| 8 MB | 1024 | 16 | FAT12 |
| 16 MB | 1024 | 32 | FAT12 |
| 32 MB | 2048 | 32 | FAT12 |
| 64 MB | 4096 | 32 | FAT12 |
| 128 MB | 8192 | 32 | FAT16 |

A 128 MB stick takes 136.6 MB. A version 1 `ms.frondms` (the physical pages with their extra bytes, and a change counter at bytes 32-39) still loads, and its next save writes version 2 at the counter plus one.

Files writes a stick as the driver does:

- It opens only a settled stick: each logical block claimed by at most one block, and that block in its slot. Otherwise it asks for the stick to go in the TG50 once.
- A held logical block is written in place. One nobody holds gets a free block in its segment, searched forward from its home block, with `f8 ff Lhi Llo ff ff ff ff ff` in the extra bytes of every page and zeros in the pages it doesn't write. Blocks in the disabled-block table are never used.
- A stick with no partition table or boot sector, whose held blocks are all zeros or all 0xff (made by Frond, or mounted once in a TG50 and never formatted), is blank. Files formats it the first time it opens it writable, with the driver's layout: one MBR entry, the boot sector, 1 reserved sector, 2 FATs, 512 root entries, media byte f8, a hidden read-only `MEMSTICK.IND` in the root and the rest of the root zeroed. Read only, it says the stick is blank.

### Internal NAND

The T|X and E2 keep their NAND in `nand.frondnand`: the 512 main bytes of each 528-byte unit in page order as the payload, then the 16 spare bytes of each in the side region (a 2112-byte T|X page is 2048 main and 64 spare, the same split). A NAND is at most 256 MiB raw. Saves journal it like a card, and settle and verify take it. It can't be opened in Files: databases go in and out through Internal. `--nand` and `save-nand` keep the raw 528-byte unit format.

### Session media

From version 7 a `.frond` holds bounded state and a media section describing each container beside it, one 100-byte descriptor per medium (92 bytes in version 7):

| Bytes | Field |
|---|---|
| 0-3 | kind: 1 SD, 2 classic stick, 3 Pro, 4 NAND |
| 4-35 | file name, zero padded |
| 36-39 | inserted, 0 or 1 |
| 40-43 | write protected, 0 or 1 |
| 44-75 | identity, the card's 32 bytes (unused for NAND) |
| 76-83 | logical bytes (NAND: main bytes) |
| 84-91 | physical bytes (NAND: raw bytes with spare) |
| 92-99 | content generation (version 8 on) |

A load settles each file to its descriptor's generation and checks its size and CRCs, and refuses the session if one doesn't match. Version 9 adds kind 4, so the NAND leaves the `.frond`. Version 8 and older sessions load their NAND from the `.frond`, and the next save writes version 9. Every device saves version 9.

### Exported sessions

The desktop's Export writes the machine folder with the NAND packed as `nand.frondnand.gz`: a gzip file (deflate, no name or time) of the whole container. The other files go as they are, settled. A T|X folder of about 160 MB exports at about 43 MB, an E2 at about 9 MB. Android exports a zip, which deflates every file, so its NAND stays `nand.frondnand` inside. Both apps import either.

Unpacking checks the gzip CRC and size, stops at the size the container header describes (a NAND at 256 MiB), then checks every container CRC, writing a temp that is renamed only when all of it is right. A load of a folder with only `nand.frondnand.gz` unpacks it the same way, removing the `.gz` once the plain file is in place.

## Console commands

The core reads these from the terminal and from `--script`. A command with no argument takes none.

| Command | Meaning |
| --- | --- |
| `help` | List the commands. |
| `quit`, `exit` | Quit. |
| `set-mips N` | Set the target MIPS. |
| `input-replay FILE` | Load an input replay; its times start now and it replaces any pending replay. A bad file leaves the old one. |
| `hold-state` | Show the Hold switch. |
| `hold on\|off` | Set the Hold switch (TG50). |
| `audio-on`, `audio-off` | Turn audio on or off. |
| `mount IMAGE` | Insert an SD card image. The built-in help says "Unmount SD card." |
| `sd-take-out`, `ms-take-out` | Leave the ejected card or stick settled in the machine folder's card file and empty the slot. Prints its identity and `protected` or `writable`. |
| `sd-put-in IDENTITY [protected\|writable]`, `ms-put-in ...` | Insert the card file in the machine folder as a new card with that identity. |
| `unmount` | Eject the SD card. |
| `reset` | Soft reset. |
| `reset-no-ext` | Reset with Up held, so extensions don't load. |
| `reset-hard` | Hard reset. Erases the device. |
| `rotate` | Rotate the view 90 degrees. |
| `save-session FILE [CARD]` | Save the session, and the SD card image as well if CARD is given. |
| `save-sd IMAGE` | Save the SD card image. |
| `save-nand IMAGE` | Export the raw NAND with its spare bytes. |
| `dump-mmu` | Print the MMU mappings. |
| `os-version` | Ask the Palm for its OS version. |
| `set-auto-off SECONDS` | Set the auto-off time. Not on the TG50. The built-in usage line leaves out the argument. |
| `install FILE` | Install a PRC or PDB. |
| `launch NAME` | Launch an app by its database name. |
| `db-list` | List the RAM databases. |
| `db-export FILE` | Export the RAM databases to a ZIP. |
| `db-export-rom FILE` | Export RAM and ROM databases (not the PACE cache). |
| `db-export-all FILE` | Export every database. |

`install` (here and on the control socket) runs where the app in front next asks for an event. On the T|X and Zire 72 an app waiting for input with no timeout, like Memo Pad, is woken first through EvtWakeup, as an interrupt would, and gets a nilEvent; `launch` and the `db-` commands do the same. After it, as after a HotSync, the app with the database's creator gets `sysAppLaunchCmdSyncNotify` unless it's in front, and an app installed while the launcher shows makes the launcher reopen with it listed. Another app in front stays, and so does a launch already under way.

## Control socket

The desktop app listens on a Unix socket and the core connects to it with `--desktop-socket`. Only the emulation thread serves requests, one at a time.

When the socket closes the core exits. The desktop passes its working session with `--save-on-disconnect`, so if Frond is killed or crashes the core saves there first, on the emulation thread as a `save` request would. It doesn't save after a `quit`, one still waiting to be read included, or when a bad request broke the protocol. A failed save exits 1. Both cores ignore SIGPIPE, so a dead Frond's output pipes don't stop the save.

For the whole run the core holds an `flock` on `FILE.core` (`<name>.frond.core` beside the session), which the kernel frees when it exits. A second core on the same session is refused. Frond counts a session whose `.core` is held as in use, like one whose `.lock` is held: starting it says it is still saving, and reconcile, the stale temp and work cleanup, rename, delete and duplicate leave it alone. On xcb the embedded Palm window is in Frond's X save set, so it goes back to the root window rather than being destroyed when Frond dies.

Lines are tab separated and end in a newline.

| Direction | Line |
| --- | --- |
| Request | `ID TAB command TAB HEX` |
| Reply | `ID TAB ok\|error TAB code TAB HEX` |
| Event | `0 TAB event TAB name TAB HEX` |

`ID` is 1 to 20 digits and not 0. `HEX` is the UTF-8 argument or message as lowercase hex, empty for none. A zero byte is refused. The input queue is 32 KB. The next request is taken once the last reply is sent. Replies with several lines (`db-list`, `ms-files`) put them in the message. A path is always absolute. A command that takes no argument fails with `argument` if given one.

Two requests with ID 0 get no reply: `cancel-files` (argument is the job ID of a running file job) and `panel-visible` (`on` or `off`).

Commands that touch files or the guest pause audio while they run and don't catch up the lost time.

Error codes seen across commands: `argument`, `unsupported` (the device lacks it), `not-ready` (the Palm isn't at a point where it can take the call, try again), `busy`, `empty`, `cancelled`, `timeout`, `command` (unknown) and `failed`.

### Events

| Event | Message |
| --- | --- |
| `ready` | The core has connected. |
| `close` | The user closed the window. The launcher decides what to do. |
| `sd-progress` | `done total` in bytes, at each whole percent of an SD or Memory Stick create, import or export. |
| `card` | `inserted`, when a card that `--card-swap` left ejected goes in. |
| `shortcut` | A key pressed in the Palm window that belongs to the app, as `ctrl+s`, `ctrl+shift+s`, `ctrl+1` or `f12`: F11, F12, and Ctrl (with Shift) and a letter or digit. They never reach the Palm. With the infrared keyboard attached, Ctrl keys go to the Palm and only `f11` and `f12` are sent. |
| `serial`, `usb`, `infrared`, `bluetooth`, `microphone` | The new state, sent when it changes, for example a peer connecting. |

### State and guest commands

| Command | Argument | Success reply |
| --- | --- | --- |
| `device-state` | | `device`: profile name |
| `window-id` | | `window`: `x11 ID`, the decimal X11 window id the app can embed. `unsupported` when the window isn't an X11 window (Wayland, the dummy video driver). `--window-fit whole` makes the Palm follow the window's size. |
| `power-state` | | `power`: `asleep` or `awake`, then `wakes=N`, the wakes from sleep since the core started. Not on `frond-68k`. |
| `guest-ready` | | `guest-ready`; `not-ready` until Palm OS is at an application-call boundary |
| `speed-state` | | `speed`: `N MIPS, limit N MIPS, N%, N underruns` |
| `guest-orientation` | | `orientation`: `portrait`, `landscape`, `reverse-portrait` or `reverse-landscape` (TX only; `unknown` if unreadable) |
| `save` | file | `saved` |
| `install` | file (PRC or PDB) | `installed`, or `needs-reset` (reset to finish). Errors: `corrupt`, `memory`, `open`, `overwrite`, `unsupported`, `not-ready`, `read` |
| `launch-app` | database name, up to 31 bytes | `launched`; errors `launch`, `not-ready` |
| `delete-db` | database name, up to 31 bytes | `deleted`; errors `missing`, `rom`, `running` (the open application), `open`, `delete`, `not-ready` |
| `export` | ZIP file | `exported`: all RAM databases |
| `export-db` | ZIP file, newline, database name | `exported`: that database |
| `db-list` | | `databases`: a line per RAM database, `type creator attributes` as hex, then the hex name |
| `input-replay` | file | `loaded`; error `replay` with the line |
| `replay-state` | | `running` or `finished`, message is the last event time in ms |
| `replay-snapshots` | | `snapshots`: comma list of `ok`, `no-frame`, `lcd-off` or `failed`, one per snapshot so far |
| `reset`, `hard-reset`, `rotate`, `audio-on`, `audio-off`, `quit` | | `done` |

### Device commands

State commands take no argument. Mode commands change the setting and reply with the new state.

| Command | Argument | Reply |
| --- | --- | --- |
| `hold-state` | | `hold`: `on`, `off` or `unavailable` |
| `hold` | `on` or `off` | `hold` |
| `hotsync` | | `hotsync`: pulls the connector's HotSync line low for 500 ms (T|X GPIO10, E2 GPIO4); `unsupported` elsewhere, or while another key is held |
| `keyboard-state` | | `keyboard`: `attached` or `detached` |
| `keyboard-attach` | `on` or `off` | `keyboard`; `infrared` error if an infrared peer owns the UART |
| `keyboard-key` | hex scan code, space, `d` or `u` | `keyboard` |
| `sound-state` | | `sound`: `on`, `muted` or `off` |
| `sound-mode` | `on` or `off` | `sound` |
| `microphone-state`, `microphone-epoch` | | `microphone` |
| `microphone-mode` | `on`, `off` or `on-return:N` (arm when the Palm app returns to the foreground, N from `microphone-epoch`) | `microphone` |
| `display` | words `fit=`(`none` `fit` `whole` `fill`), `smooth=`(`on` `off`), `keyboard=`(`on` `off`), `jog=`(`hidden` `screen` `keyboard`) | `display`: `queued`, applied once the app is in the foreground |
| `serial-state`, `usb-state` | | `serial`, `usb`: `off`, `pty PATH waiting\|connected`, `tcp PORT waiting\|connected`, `unix PATH waiting\|connected`, or `off` with an error; frond-68k's modem gives `modem idle\|dialling\|online 33600` |
| `serial-mode`, `usb-mode`, `irda-mode` | `off`, `pty`, `tcp:PORT` or `unix:PATH`, and `modem` for `serial-mode` on a device with a snap-on modem (frond-68k, `unsupported` elsewhere there), `phone` for `irda-mode` on a device with the `irPhone` capability (`unsupported` elsewhere, and on frond-68k before Palm OS 3.3) | the state |
| `irda-state` | | `infrared`; the phone gives `phone idle\|linked\|dialling\|online 33600` |
| `irprinter` | `status`, `save ABSOLUTE_PATH` or `clear` | `irprinter`: any error text, then `attached\|off receiving\|ready bytes=N receiving=R last=LAST idle=MS` for the infrared printer (`irda-mode printer`): N is the held output `save` writes, R the bytes of a job still coming in, LAST the last save's result and MS the milliseconds since a byte arrived or a job ended (-1 with none) |
| `irda-stats` | | `infrared`: counters `rx= tx= rx_drop= tx_drop= pending= baud= rx_on= tx_on= power= sleeping=`; frond-68k gives `rx= tx= port=`; the phone gives `frames_in= frames_out= bad= connects= resent= timeouts=` (frond-68k with `port=` first) |
| `serial-stats` | | frond-68k: `serial`: the modem's `power=on\|off dtr=on\|off`, or `off` outside modem mode |
| `modem-sync` | none, or `off`, `pty`, `tcp:PORT` or `unix:PATH` | frond-68k: `modem-sync`: the modem's HotSync link, as `serial-state` shows a link; `unsupported` without a modem |
| `bluetooth-state` | | `bluetooth` |
| `bluetooth-mode` | `off` or an absolute Unix socket path | `bluetooth` |
| `network-state` | | `network`: mode name |
| `network-mode` | `disabled`, `connected`, `timeout`, `disconnected`, `internet` or `wireless` | `network`; on the Dana (`usbWifi` capability) only `wireless` and `disabled`, which pick the Wi-Fi adapter or no device on the USB port; on the Dana Wireless (`builtInWifi`) only `wireless` and `disabled`, which turn the built-in module's radio on and off (on by default) |
| `wifi-module-state` | | `wifi-module`: `off`, or `powered\|unpowered link=up\|down ip=ADDRESS mac= tx= rx= dropped=`. `off` while the radio is off, `unpowered` while the Dana hasn't raised PK0 to power the module. Console: `wifi-module [on\|off]` |
| `usb-port-state` | | `usb-port`: `off`, `printer ` and the `printer status` line, or `wifi powered\|unpowered link=up\|down ip=ADDRESS mac= tx= rx= dropped=`. Every device answers `off` except the Dana |
| `usb-port` | `off`, `printer` or `wifi` | `usb-port`: the new state. The Dana's one external USB port holds a printer, the Wi-Fi adapter (a Prism 2.5 stick, 045e:006e) or nothing. Choosing what is already there does nothing. `printer attach on` is `usb-port printer`. Other devices answer `unsupported` except for `off` |

### Card commands

| Command | Argument | Reply |
| --- | --- | --- |
| `sd-state` | | `card`: `none`, `inserted` or `ejected` |
| `sd-create` | size in MB | `inserted`; a `size` error lists the sizes the device takes |
| `sd-insert` | image file | `inserted` |
| `sd-eject`, `sd-reinsert` | | `ejected`, `inserted` |
| `sd-export` | file (card must be ejected) | `exported` |
| `sd-take-out`, `ms-take-out` | | `taken-out`: `IDENTITY protected\|writable`. The ejected card is settled in the machine folder's card file and the slot is empty, so the launcher can move the file |
| `sd-put-in`, `ms-put-in` | `IDENTITY` (64 hex digits), then `protected` or `writable` | `inserted`: the card file now in the machine folder goes in as a new card |
| `ms-state` | | `card`: `none`, `inserted` or `ejected` |
| `ms-type` | | `type`: `none`, `classic MB` or `pro MiB` |
| `ms-protection` | | `protection`: `read-only` or `writable` |
| `ms-read-only` | `on` or `off` | `protection` |
| `ms-create` | classic 4 to 128 MB, or Pro 256, 512, 1024 or 1886 MiB | `inserted` |
| `ms-insert` | image file | `inserted` |
| `ms-eject`, `ms-reinsert` | | `ejected`, `inserted` |
| `ms-export` | file (stick must be ejected) | `exported`: a version 2 `.frondms` for a classic stick, a raw image for Pro |
| `ms-files` | card folder path | `files`: first line `space USED TOTAL`, then `d\|f SIZE HEXNAME` per entry |
| `ms-get` | hex host file, newline, hex card path | `exported` |
| `ms-put` | hex host file, newline, hex card path | `changed` (host file up to 8 MB) |
| `ms-mkdir`, `ms-delete` | card path | `changed` |
| `ms-rename` | hex card path, newline, hex new name | `changed` |

The `ms-*` and `sd-*` commands exist on devices with that slot; the other gets `unsupported`. The file commands on a Memory Stick go through Palm OS and need `guest-ready`; the apps use Files on the card file instead. A classic stick, a Pro stick and an SD card each use their own image format.

## Input replay

`--input-replay FILE` and the `input-replay` command send events through the same queues as the SDL window. Each line is `MS COMMAND ARGS`, with MS the milliseconds since the replay started (since the main loop started for `--input-replay`), in nondecreasing order. Empty lines and `#` comments are allowed. Replay uses host time, so it isn't deterministic in guest instructions.

| Command | Meaning |
| --- | --- |
| `pen-down X Y`, `pen-move X Y` | Touch at unscaled guest pixels. The range is the device surface, TX 0..319 by 0..479, E2 320 by 440 with its silkscreen. |
| `pen-up` | Lift the pen. |
| `key-down KEY`, `key-up KEY` | A key by name or by number (1 up to the highest key id). |
| `keyboard-attach`, `keyboard-detach` | Plug the infrared keyboard in or out. |
| `keyboard-down NAME`, `keyboard-up NAME` | Infrared keyboard key by SDL scancode name, quoted if it has a space, such as `"Left Shift"`. |
| `snapshot PATH` | Save the Palm screen as a PNG. The folder must exist. The path may be quoted. A failed snapshot is logged and the run goes on. |

```text
100 pen-down 100 140
200 pen-up
1500 key-down center
1600 key-up center
2000 snapshot "after-tap.png"
```

Key names:

| Name | Key | Devices |
| --- | --- | --- |
| `hard1`, `hard2`, `hard3`, `hard4` | The four application buttons by position | all |
| `home`, `calendar`, `contacts`, `web` | Aliases for `hard1`, `hard2`, `hard3`, `hard4` in that order | all |
| `up`, `down` | Navigator up and down | all |
| `left`, `right`, `center` | Navigator left, right and select | TX, E2, Zire 72 |
| `power` | Power; ignored on E2 de-NVFS | supported firmware |
| `voice` | Side voice button | Zire 72, TG50 |
| `home-key` | Separate physical Home key | TG50 |
| `graffiti` | Graffiti button | TG50 |
| `back` | Back | TG50 |
| `jog-up`, `jog-down`, `jog-press` | Jog dial | TG50 |
| `hold` | Hold switch, held while down | TG50 |
| `hotsync` | The connector's HotSync line, low while down | TX, E2 |

The TG50 keyboard keys are numbers 17 to 58, in rows from the top left.

`tools/test-input-replay.py` checks that invalid replays fail before the guest runs.

## Connections

Connections link Frond installs on a network, see [using-frond.md](using-frond.md). The desktop runs them in a per-user hub, `frond --link-service` (same binary, no window). Windows start it when they need it and it exits 30 seconds after its last client.

### Unix endpoints

`unix:PATH` for `--serial`, `--irda` and `--usb` (and `serial-mode`, `irda-mode`, `usb-mode`, in both cores) listens on a Unix socket and takes one client, otherwise like `tcp:`. PATH is absolute and under 108 bytes, and its folder must exist, be a directory you own and have mode 0700, or the core refuses it. A stale socket file is removed, a live one is refused ("is in use") and a file that isn't a socket is left alone. The socket is removed when the endpoint closes. A core that is killed leaves it behind until the next start.

### Hub socket

The hub listens on `$XDG_RUNTIME_DIR/frond/link.sock` (folder mode 0700, `FROND_LINK_SOCKET` picks another). The cores' link sockets are `ir-PID.sock`, `serial-PID.sock` and `bt-PID.sock` in the same folder. Messages are JSON lines. A client sends `{"op": ...}` and the hub answers with `{"type": ...}` events.

| Op | Fields |
| --- | --- |
| `watch` | `on`. Sends `state` now and on every change. |
| `state` | One `state` event. |
| `rename` | `name`, this install's name. |
| `peer-rename`, `peer-address`, `forget` | `id`, then `name` or `address`. |
| `peer-radio` | `id`, `radio` (`here` or `there`). |
| `pair`, `pair-code`, `pair-cancel`, `pair-dismiss` | `id` (from Nearby) or `address`, then the `code`. |
| `ir-attach`, `serial-attach` | `machine`, `device`, `link` (`local` or a connection id), `socket` (the core's `unix:` path). One of each per client. |
| `ir-detach`, `serial-detach` | |
| `bt-attach` | `machine`, `device`, `link` (`radio` or a connection id), `socket` (the path the hub listens on for the core, which is then given to `bluetooth-mode`). |
| `bt-detach` | |

Attaches end when the client disconnects. A client that wants one to survive a hub restart sends it again.

Events: `hello` (`id`, `name`, `port`, `pid`), `state` (name, port, connections with `peerName`, `rtt`, `seen`, `up`, `keyChanged`, `forgotten`, `bluetoothRadio` and `channels`, nearby installs, and `local`, the channels between machines on this computer), `pair-request`, `pair-request-update`, `pair-request-end`, `pair-peer`, `pair-result` (`paired`, `wrongcode`, `lockedout`, `expired`, `notpairing`, `unreachable`, `version`, `failed`), and `ir`, `serial` and `bt` with `state` (`joined`, `waiting`, `busy`, `notfound`, `connecting`, `keychanged`, `forgotten`, `unknown`; `bt` also `radio`, `noradio`, `here`, `there`), `peer`, `machine` and `device` (the far machine), `far` (`asleep`, `paused`, `reset`, `running`), `busyBy` and `core` (the core's socket is up; `bt` has `listening` and `controllers` instead).

### Store

`<data>/frond/connections/` (0700, files 0600, the Android app keeps its own copy of the same format under its files folder). Only the hub writes it, behind a `.lock` file.

- `identity.json`: `version`, `id`, `name`, `secret` (X25519), `port`.
- `connections.json`: `version` 1 and `entries`, each with `id`, `name`, `peerName`, `key`, `address`, `manual`, `paired`, `lastSeen`, `lastHost`, `lastPort`, `bluetoothRadio` (`here` or `there`), `bluetoothRadioAt` and `keyChanged`.

### Network

TCP 5035 carries the links (Noise KK against the pinned keys, or a pairing request while a pair dialog is open) and UDP 5035 the beacons, sent while Connections is open, a pairing is under way or any machine has Infrared, Serial or Bluetooth on a connection, even once its link is up. Incoming links and beacons are only taken from loopback, private (10/8, 172.16/12, 192.168/16), link-local and ULA (fc00::/7) addresses. Frond dials any address, so a connection added by address can be on a VPN, but the far side still has to see this one at an address in those ranges.

### Dana USB Wi-Fi

The Dana's external USB port holds one device: nothing, the printer or the Wi-Fi adapter. The `usbWifi` capability (`usb_wifi` in `tools/device-profiles.json`, Dana only) says a device has the adapter. The adapter is an emulated Microsoft MN-510 (Prism 2.5, 045e:006e) on the ISP1161's external root port (`src/common/UsbPrism2Adapter.h`), its Ethernet frames go to libslirp through `frondNet::WifiRouter` (`src/common/net`), and `UsbWifiRuntime` plugs it in. It shows one open network, "Frond" on channel 6, so any WEP key connects and ad hoc finds no peers. The guest is 10.0.2.15, the router 10.0.2.2 and DNS 10.0.2.3, as on the T|X's Virtual Wi-Fi. The choice is made with `usb-port`, kept by reset and by a session load, and `usb-port-state` reads it back with the link state. A session saved with the adapter keeps its MAC, settings and what was part way over USB (a request half sent, a reply not yet read), but a load makes a fresh slirp, so connections drop. Frontends send the machine's USB port choice at every start, Off included, and then read `usb-port-state`, so the setting wins over the device a session was saved with.

The Dana Wireless (`alphasmart-dana-wireless`, device type 8, `builtInWifi` / `built_in_wifi`) runs the Dana's ROM with 16 MiB and an MN-510 leaf on the ISP1161's second root port, attached while PK0 is high (`UsbWifiRuntime moduleWifi`), with its own MAC and router. The core sets the ROM's `snum` token to read as a Wireless one and refuses a ROM with no such token. `usb-port wifi` is refused there, and the USB port holds Off or Printer. Its session keys `module-mac`, `module-adapter` and `module-bus` are the external adapter's `wifi-mac`, `wifi-adapter` and `wifi-bus` for the module (the last two only while its radio is on). A `.frond` session carries its device name, so a Dana session doesn't load as a Wireless or the other way round.

Desktop: the Connections panel has a USB port list (Off, Printer, Wi-Fi adapter) with a readout, and polls `usb-port-state` once a second while Wi-Fi is chosen. Android: the Connections sheet has a USB port row with the same three choices. Both save the choice as `usbPort` in the machine settings and send it at every start. On the Dana Wireless the list is Off and Printer, and a Wi-Fi row (On, Off and a readout from `wifi-module-state`, polled once a second) sends `network-mode wireless|disabled`, saved as `wifiModule`.

### Machine settings

The per-machine settings file `<name>.frond.settings.json` has `infrared`, `serial`, `usb`, `bluetooth` and, on the Dana, `usbPort` (and `wifiModule` on the Wireless). Values:

| Key | Values |
| --- | --- |
| `infrared`, `serial` | `off`, `pty`, `tcp:PORT`, `link:local` (This computer, desktop), `link:ID` |
| `infrared` | also `phone` on a device with the `irPhone` capability: the desktop passes `--irda phone`, Android sends `irda-mode phone` once the core answers and opens no link |
| `infrared` | also `printer` on any device with infrared: the desktop passes `--irda printer`, Android sends `irda-mode printer` once the core answers and opens no link. Both poll `irprinter status` every second while it is the choice |
| `serial` | also `modem` on a device with the `modem` capability (the IIIx): the desktop passes `--serial modem`, Android sends `serial-mode modem` once the core answers and opens no link |
| `usb` | `off`, `pty`, `tcp:PORT` |
| `usbPort` | Dana: `off`, `printer` or `wifi`, the USB port choice. The desktop sends `printer attach on` or `usb-port wifi` once the core answers, Android does the same on connect. Older settings with `usbPrinter` true count as `printer`; the file still writes `usbPrinter` when the choice is the printer |
| `wifiModule` | Dana Wireless: `false` when the built-in Wi-Fi is off, left out when on. Sent as `network-mode disabled` or `wireless` at every start, before the USB port choice |
| `bluetooth` | `off`, `radio` (This Frond), `phone` (This Frond with the mobile phone), `socket:/absolute/path`, `link:ID` |

A `link:ID` that isn't in the store acts as `off` and shows as Unknown connection. The Android metadata `settings` holds `infrared`, `serial` and `bluetooth` the same way, with only `off`, `radio` and `link:ID` (and `modem` for `serial`, `phone` and `printer` for `infrared`). Nothing goes into the `.frond` session. With a link or `radio` choice the desktop starts the core with `--irda`, `--serial` or `bluetooth-mode` pointing at the hub's sockets.

### btvirt

`make deb` builds `btvirt` with `tools/build-bluetooth-provider.py` and installs it as `/usr/lib/frond/btvirt`. For This Frond's radio the hub looks for `FROND_BTVIRT`, `../lib/frond/btvirt` beside the binary, `btvirt` beside the binary, then the build tree, and runs it in `<runtime>/frond/radio/`. It stops 5 seconds after nothing uses it.

## Developer runner

`tools/frond-run.py` installs PRC and PDB files into a private copy of a session, launches an app, runs a replay, takes screenshots, exports databases, and prints one JSON result. It needs Python 3.11 or later, a built core, and your own session or firmware.

```sh
python3 tools/frond-run.py --session ready.frond \
  --install MyApp.prc --install TestData.pdb --app 'My App' \
  --replay test.replay --screenshot 0 --screenshot 2000 \
  --export-db MyAppLog --run-for 6 --headless --output results/run-01
```

| Option | Meaning |
| --- | --- |
| `--core PATH` | The core to run. Default `out/frond`. |
| `--device NAME` | `palm-tx`, `palm-e2`, `palm-zire-72` or `sony-clie-tg50`. |
| `--session PATH` | Restore a saved session: a machine folder (`sessions/NAME/`) or a `.frond` file. A file brings the card files (`sd.frondsd`, `ms.frondms`, `ms.frondmsp`) and the NAND file (`nand.frondnand`) beside it along. The run works on a copy. |
| `--nor FILE`, `--nand FILE` | Cold boot from firmware instead of a session. NAND is needed for the TX, optional for the E2 and absent on the Zire. The 6 MiB E2 de-NVFS image uses `--device palm-e2` and needs no external NAND. |
| `--output DIR` | New result folder, mode 0700. It must not exist. |
| `--install FILE` | PRC or PDB. Repeat; installed in order. A reset request resets and waits again before the next one. |
| `--app NAME` | Exact Palm database name to launch (case matters). |
| `--resume-app` | Continue the saved app without launching it. Use one of `--app` or `--resume-app`. |
| `--replay FILE` | Input replay. Its `snapshot` paths are replaced by `replay-snapshot-NNN.png` in the output folder. |
| `--screenshot MS` | Capture at this many ms after the replay starts. Repeat. Files are `screenshot-NNN.png`. |
| `--export-db NAME` | Export one database to `database-NNN.zip`. Repeat. |
| `--run-for SECONDS` | Minimum replay time. Later events extend it. |
| `--settle SECONDS` | Wait after launch before the readiness check and replay. Default 1. |
| `--timeout`, `--startup-timeout`, `--ready-timeout`, `--command-timeout` | Wall-clock limits in seconds: overall 180, startup (socket and ready event) 60, each readiness phase 30, each socket command 30. |
| `--headless` | SDL dummy video and audio. |
| `--sound` | Sound on (off by default). |
| `--serial ENDPOINT`, `--usb ENDPOINT` | `off`, `pty`, `tcp:PORT` or `unix:PATH`, passed to the core. |

The JSON has `version`, `status` (`ok` or `error`), `stage`, `error`, the socket `steps`, input hashes, `elapsed_seconds`, `core_exit_code` and `artifacts`. The exit status is 0 for ok, 1 for a failed run and 2 for bad arguments. The core log is `core.log`. The machine runs from a copy in `working/` in the output folder and the saved session is `working/working.frond` (with its card files beside it); `artifacts.machine` and `artifacts.session` give both paths, and `working/` can be passed to `--session` for a following run.

The runner waits for `guest-ready` before installs and launches, and retries only `not-ready`. Replay done means the events were sent, not that the app handled them. Input files are never written, and their hashes are checked again at the end. Results hold firmware and personal data, so keep the folder private.

## GDB and PACE monitor

`--gdb PORT` starts a GDB Remote Serial Protocol server on 127.0.0.1 only, with no password. The guest doesn't start until GDB connects. Use an ARM GDB that reads XML target descriptions.

```gdb
target remote 127.0.0.1:3333
info registers
x/8i $pc
break *0xADDRESS
continue
detach
```

Scope:

- One ARM CPU, all-stop mode. r0 to r15 and CPSR (register 25), virtual memory reads and writes through the MMU, step, continue, Ctrl-C, up to 16 address breakpoints (`Z0`, `Z1`).
- Watchpoints (`Z2` to `Z4`), signals, kill, binary writes, VFP and multiple clients aren't supported.
- Detach, EOF or reset clears the breakpoints and resumes the guest. The listener stays open for the next client.
- The debugger runs on the emulation thread, so the desktop socket doesn't answer while it's stopped. The Fast engine isn't used while attached.
- At a PACE (68k) stop the ARM registers show the suspended native context. Native step, register writes and memory writes return `E16` there. Use the monitor commands for 68k state, not `si`.

Monitor commands:

| Command | Meaning |
| --- | --- |
| `monitor pace` | JSON with D0 to D7, A0 to A7, PC, SR and the state pointer, or `available: false` outside PACE. |
| `monitor pace-break ADDR` | Break before the 68k instruction at this hex address (no `0x`, even). Up to 16. |
| `monitor pace-clear` | Remove all PACE breakpoints. |
| `monitor pace-stop on\|off` | Stop right after a PACE exception, before its handler runs. |
| `monitor pace-last` | JSON for the latest PACE exception seen while attached. |
| `monitor pace-step` | Arm one 68k instruction. The next plain `continue` runs it and stops. `monitor pace-step off` cancels. |
| `monitor pace-last-step` | JSON for the last stepped instruction. |
| `monitor attribution` | Foreground app metadata at the TX native UI wait. Names and creators are hex. |

Breakpoints and snapshots aren't saved in sessions. On Android, Settings > Debugger > gdb server opens the port on all network addresses, so only use it on a network you trust.

`tools/pace-report.py` combines a `monitor pace` object with the PRC you ran and prints a private report with resource offsets and, given the matching executable, symbols (`--context`, `--program`, `--executable`, `--runtime-map`, `--tool-prefix`).

## Palm IIIx (frond-68k)

`frond-68k` runs the Palm IIIx (DragonBall EZ, Palm OS 3.1 to 4.1) on the CloudpilotEmu 68k core. It is a separate program from `frond`, built by `make out/frond-68k` (the cloudpilot sources with `FROND_68K=1`, no network proxy, readline or Fast engine) and shipped in the Debian package as `/usr/lib/frond/frond-68k-core` beside `frond-core`. The desktop and Android apps start it for 68k devices. Run it by hand like this:

```sh
out/frond-68k --rom palm-os-3.5-ez.rom --run-for 10 --framebuffer-output out.png
out/frond-68k iiix.frond --serial pty
```

### Command line

`frond-68k [--rom] FILE [options]`. FILE is a ROM or a saved session. `--version` (as the only argument) prints `frond-68k VERSION`. A wrong option prints the usage and exits 2.

| Option | Meaning |
| --- | --- |
| `--rom FILE` | The ROM or session. A plain positional file works the same. |
| `--device NAME` | Expect this device, `palm-iiix`. |
| `--check-rom` | Check the ROM (header, then Cloudpilot's own check) and print what it found, without starting. Not with a session. |
| `--run-for SECONDS` | Run headless for this many guest seconds, as fast as the host allows, 0.001 to 3600. Not with `--desktop-socket`. |
| `--framebuffer-output FILE` | Save the LCD as a PNG at the end of `--run-for`. |
| `--input-replay FILE` | Replay timed pen and key events and snapshots, the same format as in [Input replay](#input-replay). |
| `--speed original\|uncapped` | Original runs at the real 16.58 MHz, Uncapped at 8 times that with timers on real time. Default original. |
| `--contrast N` | Screen contrast 0 to 100. Default 50, or the session's. |
| `--window-scale N` | Screen pixels per Palm pixel, 1 to 4. Default 2. The desktop passes twice its own scale. |
| `--window-fit MODE`, `--window-background RRGGBB` | Fit the device to a resizable window (`fit`, `whole` or `fill`) and the colour around it, as for `frond`. |
| `--smooth-lcd` | Smooth pixels instead of nearest-neighbour. |
| `-q`, `--no-sound` | Start with sound off. |
| `--audio-output FILE` | Write the piezo sound to a WAV file. |
| `--audio-period FRAMES` | Host audio period, 64 to 8192. Default 512. |
| `--tone-log` | Print each tone change (`tone: HZ Hz duty D at S s`). |
| `--modem-sync off\|pty\|tcp:PORT\|unix:PATH` | frond-68k: the modem's HotSync link. A call to the HotSync number (4687962) is bridged to it raw, for pilot-link. Devices with the `modem` capability. |
| `--serial off\|pty\|tcp:PORT\|unix:PATH\|modem`, `--irda off\|pty\|tcp:PORT\|unix:PATH\|phone` | Connect the serial port or infrared to a host PTY, a loopback TCP listener or a Unix socket. The PTY path is printed. `modem` puts the snap-on modem on the serial port (devices with the `modem` capability): any number dialled connects at 33600 to a PPP server with slirp behind it, guest 10.0.2.15, DNS 10.0.2.3. `phone` puts a mobile phone with the same modem on the IR port (devices with the `irPhone` capability, Palm OS 3.3 or later), reached over IrCOMM. |
| `--gdb PORT` | Start the GDB stub on 127.0.0.1:PORT, see below. |
| `--desktop-socket PATH` | Connect to the desktop's control socket, see [Control socket](#control-socket). The console is off then. |
| `--save-on-disconnect FILE` | As on the ARM core. |

`--work-folder`, `--nand`, `--engine`, `--jog-controls`, `--usb`, `--bluetooth`, `--fast-audio`, `--hide-keyboard` and `--hide-jog-controls` are taken and ignored, as the desktop passes them to every core. `--card-swap`, `--card-identity` and `--card-protected` are refused with a one-line message and exit 2, as a IIIx has no card slot.

In the window F1 to F4 are the app buttons, the up and down arrows the rocker and Escape power. Typed text goes into the Palm.

68k sessions save bus timing in optional chunk `0x210`, version 1: LCD and refresh
deadlines, fractional charges, the partial usage window and open SDRAM rows.
Restoring this history preserves subsequent cycle charges. Older sessions still
load, with a deterministic starting phase and closed SDRAM rows; they cannot
reproduce bus history that was never saved. Invalid timing state is refused.

### Console commands

Read from standard input, one per line, unless `--desktop-socket` is used. Paths may be quoted.

| Command | Meaning |
| --- | --- |
| `quit` | Quit. |
| `save-session FILE` | Save the session. |
| `load-session FILE` | Load a session. The whole file is checked first and the machine is kept if it is refused. |
| `reset`, `reset-hard`, `reset-noext` | Soft reset, hard reset (erases the Palm), or a reset with extensions not loaded. |
| `input-replay FILE` | Load an input replay. |
| `snapshot FILE` | Save the Palm screen as a PNG. |
| `window-snapshot FILE` | Save the whole window, with silkscreen and buttons, as a PNG. |
| `audio-on`, `audio-off`, `sound-state` | Turn sound on or off, or show it. |
| `serial-mode off\|pty\|tcp:PORT\|unix:PATH\|modem`, `irda-mode off\|pty\|tcp:PORT\|unix:PATH\|phone` | Set the serial or infrared endpoint and show its state. |
| `modem-sync off\|pty\|tcp:PORT\|unix:PATH` | Set the modem's HotSync link and show it. |
| `serial-state`, `irda-state` | Show the endpoint, `off` or `pty\|tcp\|unix ENDPOINT connected\|waiting`, for the modem `modem idle\|dialling\|online 33600` (then `; hotsync` and the HotSync link while it's on), or for the phone `phone idle\|linked\|dialling\|online 33600`. |
| `hotsync` | Press and release the cradle's HotSync button. |
| `backlight-state` | `backlight: on` or `off`. |
| `speed original\|uncapped` | Set the speed. |
| `speed-state` | Measured MHz, the limit, the percentage and the mode. |
| `bus-wait` | The wait states per byte and word cycle for ROM and RAM, as the guest set the chip selects. |
| `bus-timing` | The LCD line and DMA clocks, the DRAM refresh period, the LCD frame rate and the DragonBall registers they come from. |
| `display contrast=N smooth=on\|off` | Change the contrast or smoothing. |
| `launch NAME` | Launch an app by its database name. |
| `hidden [on\|off]` | Test only: set or show the state SDL's background event sets on Android, in which the guest stands still unless a host call is waiting. |
| `cycles` | Test only: `cycles: N`, the guest's system cycle count. |
| `install FILE` | Install a PRC or PDB. |
| `db-list` | List the RAM databases. |
| `db-export FILE`, `db-export-all FILE` | Export the RAM databases, or all of them, to a ZIP. |
| `db-export-db NAME FILE` | Export one database as a PDB or PRC. |
| `gdb-start [PORT]` | Start the GDB stub on 127.0.0.1, port 2159 by default. |
| `gdb-stop` | Stop it and go back to breaking everywhere. |
| `gdb-state` | `off`, or `listening` or `connected`, then `running` or `stopped`. |
| `gdb-app FILE [NAME]` | Tell the stub which executable (COFF or ELF) the app came from, and its database name, see below. |

`launch`, `install` and the `db-` commands call into Palm OS, so they wait for the running app's next event loop call (EvtGetEvent) and run there, and the console takes no other command meanwhile. If the app is busy for 30 guest seconds, the Palm is asleep or the debugger has stopped it, the command fails with the reason (`export failed: Palm OS is busy, ...`) and the session runs on. After an install, as after a HotSync, the app with the database's creator gets `sysAppLaunchCmdSyncNotify` unless it's in front, and an app installed while the launcher shows makes the launcher reopen with it listed. Another app in front stays.

### Control socket

The framing, the replies and the events are the same as for `frond`. `frond-68k` answers these: `device-state`, `guest-ready`, `replay-state`, `input-replay`, `save`, `install`, `export`, `export-db`, `db-list`, `launch-app`, `reset`, `hard-reset`, `quit`, `serial-state`, `serial-mode`, `irda-state`, `irda-mode`, `irda-stats`, `serial-stats`, `modem-sync`, `speed-state`, `display` (`contrast=` and `smooth=`), `sound-state`, `sound-mode`, `audio-on`, `audio-off`, and the IIIx's own `hotsync`, `speed` (`original` or `uncapped`), `backlight-state` and `gdb-start`, `gdb-stop`, `gdb-state`. `guest-ready` and the commands that need Palm OS answer `not-ready` until the UI is up. `install`, `export`, `export-db`, `db-list` and `launch-app` wait for the app's event loop as on the console, holding back later requests, and answer `not-ready` with the reason if it doesn't come. `window-id` is answered as on `frond`. Events are `ready`, `close`, `shortcut`, `serial` and `infrared`.

What a IIIx doesn't have answers as absent to the state queries the desktop asks every device (`network-state` `disabled`, `hold-state` `unavailable`, `keyboard-state` `detached`, `usb-state` `off`, `sd-state` `none`, `microphone-state` and `microphone-epoch` `off` and `0`) and with `unsupported` to the rest: `usb-mode`, `bluetooth-state`, `bluetooth-mode`, `hold`, `keyboard-attach`, `keyboard-key`, `microphone-mode`, `network-mode` (other than `disabled`), `guest-orientation`, `rotate`, `replay-snapshots`, `sd-*` and `ms-*`. `make test-core-control` runs the same requests against both cores.

### Debugging 68k apps with GDB

The 68k runner has its own GDB stub on 127.0.0.1, off by default. Start it with `--gdb PORT` or the console command `gdb-start [PORT]` (2159 by default); `gdb-stop` and `gdb-state` stop it and show it. The monitor commands of the ARM stub don't exist here.

To debug an app built with prc-tools, install its `.prc`, then tell the stub which executable it came from so it can find the code and relocate the symbols:

```sh
gdb-app path/to/hello Hello      # the COFF or ELF file, then the database name
```

Then connect with prc-tools' gdb:

```sh
m68k-palmos-gdb hello
(gdb) set remotetimeout 120
(gdb) target remote 127.0.0.1:2159
```

With an app set, gdb waits at `target remote` until the app starts and stops at its first instruction (gdb 5.3 crashes if it first stops outside the app). Launch the app on the Palm. From there `break`, `continue`, `next`, `step`, `stepi`, `finish`, `info registers`, `bt` and `detach` work. A `next` over a call that waits for input, like an alert, only comes back once you answer it on the Palm. An app built for a newer Palm OS than the ROM (for example a 3.5 app on the 3.1 ROM) shows its "needs Palm OS 3.5" alert first.

gdb-multiarch can't read prc-tools' COFF files. Use it with `set architecture m68k`, `set endian big` and breakpoints at addresses: `gdb-app` prints how far the code moved, add that to the symbol's address from `m68k-palmos-nm`. On Android the gdb server is on in Settings, as for ARM.

The debugger costs almost nothing while no gdb is connected. `make test-frond-68k-gdb` runs both gdbs on the debug Hello app in `build/hello-debug` on an OS 3.5 ROM.

### Serial PPP

`tools/palm-pppd.sh` connects the serial port to slirp or pppd, see [Palm IIIx serial and PPP](using-frond.md#palm-iiix-serial-and-ppp).

### Modem and mobile phone

The snap-on modem (`modem` capability, IIIx) and the infrared phone (`irPhone` capability: IIIx, T|X, TG50, E2, E3, Zire 72) are the core's own, with no link or socket. Both run one engine from `src/common` on guest time: `HayesModem`, `PppServer` and slirp (`SlirpIp`), and for the phone IrLAP, IrLMP, IAS, Tiny TP and IrCOMM (`src/common/ir`). A call isn't saved, only that one was up: the session keeps the modem's settings and S registers in a `modem-call` or `phone-call` metadata line, and on load the modem is back in command mode with them and says NO CARRIER. The phone says it once the guest's IrLAP has timed out and IrCOMM opens again, with a CD drop in the first DCE lines. Palm PPP doesn't notice either (the EZ serial port has no DCD, PPP ignores the text and sends no LCP echo), so the guest needs Disconnect and Connect in Network.

| What | Value |
|---|---|
| Flags | `--serial modem`, `--irda phone`, `--modem-sync` (the desktop passes `pty` for every `modem` device) |
| Console and control | `serial-mode modem`, `irda-mode phone`; `unsupported` on other devices, and for the phone on a 68k ROM before Palm OS 3.3 |
| Settings values | `serial` `modem`, `infrared` `phone` |
| `serial-state` | `modem idle\|dialling\|online 33600`, then `; hotsync pty PATH connected\|waiting` (or tcp, unix) with a HotSync link |
| `irda-state` | `phone idle\|linked\|dialling\|online 33600` |
| Desktop text | "Modem: idle", "Modem: dialling", "Modem: online at 33600", each with ", HotSync on PATH", and the same with "Mobile phone:" ("linked" too) |
| Exclusive | The phone and the infrared keyboard refuse each other; the desktop greys the keyboard while the phone is on |

The line is 33600. The guest is 10.0.2.15, the router 10.0.2.2 and DNS 10.0.2.3, with slirp behind it. PPP is async HDLC with LCP, PAP accepted if sent, and IPCP with DNS. A `login:` and `Password:` prompt comes first that takes any line, and the first 0x7e starts PPP, so the Unix template and Windows RAS both work. `login:` repeats every 3 s until something is typed, since the Palm throws away what comes in the first seconds after CONNECT, and the call hangs up after 60 s with no login. On Android DNS comes from `HostDns`, started by `FrondActivity`, which `Frond68kActivity` extends.

AT commands (`HayesModem`):

| Command | Does |
|---|---|
| `AT`, `A/` | Prefix (case folds); `A/` repeats the last line |
| `D`, `DT`, `DP` | Dial. Digits, `*`, `#`, `A` to `D`, `,` (S8), `W`, `@`, `!`, `;`, `L` redials. Dial time is S6 plus 100 ms a tone digit (pulse: 700 ms plus 100 ms a pulse), then 6 s of handshake, capped by S7 |
| `A` | NO CARRIER, there is never a call to answer |
| `E`, `Q`, `V` | Echo, quiet and verbose results (numeric CONNECT is always 1) |
| `X0` to `X4` | Result set. X0 and X1 can't give NO DIALTONE or BUSY, X2 not BUSY; above 4 is ERROR |
| `H`, `O`, `Z`, `&F` | Hang up, back online after `+++`, reset, factory settings |
| `I` | 0 gives the line speed, others "Frond data modem" |
| `Sn=v`, `Sn?` | S-registers, `?` prints three digits. S0, S2 (escape), S3, S5, S6, S7, S8, S12 are used |
| `&Dn` | A DTR drop does nothing (0), goes to command mode (1), hangs up (2) or resets (3) |
| `&C`, `&K`, `+...`, `*NCnn` and other vendor commands | Taken and ignored |
| `B`, `C`, `L`, `M`, `N`, `W` and other letters | Taken and ignored |
| `+++` | Escape after a guard time (S12 in 20 ms steps) on guest time, then `ATO` or `ATH0` |

Results: OK, CONNECT 33600, NO CARRIER, ERROR, NO DIALTONE, BUSY, NO ANSWER. Unknown letters give OK. Turning the serial line driver off powers the modem down: it hangs up and resets.

HotSync by modem: each dial picks the far end in `LineSwitch` (`src/common/modem/LineSwitch.h`). A number whose digits end in 4687962 (`kHotSyncNumber`, HOTSYNC on a keypad, so a dial prefix still works) goes to a `PipeLine`, whose raw bytes the transport passes to the `--modem-sync` link both ways; with that link off it gives NO ANSWER. When a TCP or Unix client, or a PTY's reader, that was there during the call goes away, the modem gives NO CARRIER once the bytes it sent last have reached the guest, or after a second with none taken (in online command mode they're dropped). Every other number goes to the PPP server, where Network HotSync reaches `pilot-xfer -p net:127.0.0.1` through slirp at 10.0.2.2 (TCP 14238). The Palm first sends a UDP wakeup (0xfade, type 1) to 14237 and connects only once it comes back as type 2; pilot-link has no UDP side, so `SlirpIp` answers a wakeup to 10.0.2.2 itself and doesn't pass it on.

Dial plan: the default plan is empty, so every number connects. `HayesModem::Options::dialPlan` maps dialled digit strings to BUSY, NO ANSWER, NO DIALTONE or NO CARRIER for tests. An empty number gives NO CARRIER.

IR phone details: it answers discovery in a random slot as "IrCOMM" (hints PDA and IrCOMM), takes SNRM with up to 115200 baud, window 1 and 512 byte frames, answers at the guest's own minimum turnaround, and serves `IrDA:IrCOMM` and `IrDA:IrLMP` in IAS. IrCOMM is 9-wire cooked, 3-wire cooked or raw, and the modem is powered while IrCOMM is open. `irda-stats` gives `frames_in= frames_out= bad= connects= resent= timeouts=`. Tests: `make test-modem`, `make test-ir-peer`.

IR printer details: `irda-mode printer` (or `--irda printer`) on any device with infrared puts an IrDA printer on the port, "Frond Printer" with the printer and IrCOMM hints. It serves IrLPT (IAS class `IrLPT`, IrLMP LSAP 4, what PrintBoy uses) and IrCOMM. Each connection is a job, ended by the guest's LM disconnect, IrLAP DISC or its disconnect time; finished jobs are held until `irprinter save` or `clear`. A load or reset drops a job still coming in. Tests: `make test-ir-printer`, and `make test-printer-control` for the socket commands. An overflowing job gets an LM disconnect on IrLPT or IrCOMM. The desktop and Android reuse the USB printer's job handling for it (`PrinterPanel` and `PrintJob`, `PrintJobs` and `PrintStore`) with the `irprinter` command; unsaved or arriving output is confirmed before the choice changes. On Android one notice and one notification cover both printers on a Dana: "2 prints ready" with Show, which opens the Connections sheet's prints.

Bluetooth phone (desktop only, any `bluetooth` device): the settings value `bluetooth` `phone` is This Frond's radio with `"phone": true` in the hub's `bt-attach`. While any machine asks for it the hub runs `bluetooth-opp-peer RADIO-SOCKET --dun` beside `btvirt` (`FROND_BT_PHONE`, else `../lib/frond/bluetooth-phone`, `bluetooth-phone` beside the binary, then the build tree; installed by `make deb` as `/usr/lib/frond/bluetooth-phone`). The hub's state is `nophone` when it's missing. The peer prints `DUN_STATE idle|dialling|online SPEED|off` lines; the hub passes the last one on as `call` in the attach status (`off` as `idle`). The peer is "Frond Phone" (class of device 0x52020C, address 02:46:52:4E:44:50 written with 0xfc01 at start, so a pairing holds whatever order radios join) with PIN 1234 and an SDP record for Dial-up Networking (0x1103, 0x1201) on RFCOMM channel 9. It takes one call at a time: another link's connect is ignored while a call is up, and the call ends on an RFCOMM DISC, L2CAP close or the link going. The channel's bytes go to `tools/bluetooth-dun.cpp`, a C shim over the same `HayesModem`, `PppServer` and `SlirpIp` on monotonic time, powered while the channel is open. `make test-bluetooth-dun` runs a scripted client through a private `btvirt`: the phone's address, then SDP, RFCOMM, `ATDT*99#`, CONNECT 33600, LCP and IPCP up, twice on the same handle (hung up by dropping the link, then by DISC).

## Using an IDE

Frond has no IDE plugin. An IDE runs your build, then the [developer runner](#developer-runner) to install and try the app, and attaches GDB to a core started with `--gdb`.

What each device gives you:

- T|X, E2, Zire 72 and TG50: `tools/frond-run.py` for runs, and the ARM GDB stub with the PACE monitor. A 68k app on these is debugged at addresses with `monitor pace-break`, not at source lines.
- IIIx: no runner, use `frond-68k` and its console. Source-level debugging works with prc-tools' `m68k-palmos-gdb` (gdb 5.3), which is too old for the GDB front ends of VS Code and Eclipse, so run it in the IDE's terminal. `gdb-multiarch` works in the IDE at addresses, see [Debugging 68k apps with GDB](#debugging-68k-apps-with-gdb).

The examples below use a TX session in `sessions/tx/`, an app `MyApp.prc` named `My App`, and Frond built in `~/frond`. Change the paths to yours.

### VS Code

`.vscode/tasks.json`:

```json
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "Build",
      "type": "shell",
      "command": "make",
      "group": "build"
    },
    {
      "label": "Run on Frond",
      "type": "shell",
      "command": "rm -rf build/frond-run && python3 ~/frond/tools/frond-run.py --core ~/frond/out/frond --session sessions/tx --install MyApp.prc --app 'My App' --run-for 10 --screenshot 5000 --output build/frond-run",
      "dependsOn": "Build",
      "problemMatcher": []
    },
    {
      "label": "Frond with GDB",
      "type": "shell",
      "command": "~/frond/out/frond build/frond-run/working --gdb 3333",
      "isBackground": true,
      "problemMatcher": []
    }
  ]
}
```

"Run on Frond" installs the app into a copy of the session, runs it, and leaves the result, screenshots and the saved session in `build/frond-run/`. Drop `--run-for` and add `--headless` for a quick check without a window. "Frond with GDB" starts a window on that saved session, with the app installed, and waits for GDB.

`.vscode/launch.json`, with the Native Debug extension (`webfreak.debug`):

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Attach to Frond",
      "type": "gdb",
      "request": "attach",
      "remote": true,
      "target": "127.0.0.1:3333",
      "gdbpath": "gdb-multiarch",
      "cwd": "${workspaceFolder}",
      "autorun": ["set architecture arm", "set endian little"]
    }
  ]
}
```

Start "Frond with GDB", then "Attach to Frond". Type monitor commands in the debug console, for example `monitor pace-break 1234`. `tools/pace-report.py` turns a stop into resource offsets and symbols.

For the IIIx, start `frond-68k` in a terminal (`~/frond/out/frond-68k iiix.frond --gdb 2159`), then type `install MyApp.prc`, `gdb-app build/MyApp 'My App'` and `launch My App` at its console, and run `m68k-palmos-gdb` in a second terminal as shown above.

### Eclipse

With CDT and its GDB Hardware Debugging feature:

- Run > External Tools > External Tools Configurations, a Program with Location `/usr/bin/python3` and the `frond-run.py` arguments from the VS Code task. Make a second one for `out/frond build/frond-run/working --gdb 3333`.
- Run > Debug Configurations > GDB Hardware Debugging, a new configuration. On Debugger set the GDB command to `gdb-multiarch` and the remote target to localhost, port 3333. On Startup clear "Reset and Delay", "Halt" and "Load image", and add `set architecture arm` to the initialisation commands. A C/C++ Application is needed but only gives symbols, so any build output works.
- Start the second external tool, then the debug configuration. Monitor commands go in the Debugger Console as `monitor pace-break 1234`.

The IIIx works as in VS Code, in Eclipse's Terminal view.

## Environment variables

### Core

| Variable | Meaning |
| --- | --- |
| `FROND_JIT` | `1` or `0` picks the Fast or Exact engine at run time, over `--engine`. |
| `FROND_JIT_SYNC` | `1` waits for each compile where it was asked for, so runs repeat exactly. `0` uses the compile thread. Tests and replays default to sync. |
| `FROND_JIT_THREAD` | `0` compiles in place instead of on the compile thread. |
| `FROND_JIT_AHEAD` | How many links on from a requested block to compile ahead. `0` is off. Default 1 with the thread. |
| `FROND_JIT_HOT` | Times a block is reached before it's compiled. `1` compiles at once. Default 4. |
| `FROND_JIT_MAX_UNCHECKED` | Most cycles generated code runs without checking its budget. `0` means unchecked. |
| `FROND_JIT_STATS` | Print JIT counters at exit. |
| `FROND_PACE_FAULT_DIAGNOSTICS` | Log guest permission faults seen from PACE. |
| `FROND_TG50_ORIGINAL_PACE_DIAGNOSTIC` | TG50 PACE entry diagnostics. |
| `FROND_TG50_AUDIO_TRACE` | Trace the TG50 audio codec to stderr. |
| `FROND_MS_TRACE` | File to write a Memory Stick register trace to. |
| `FROND_MS_SUMMARY` | With `FROND_MS_TRACE`, write failures only. |
| `FROND_VFS_TRACE` | Trace guest file calls for Memory Stick file access to stderr. |
| `FROND_TRACE_WAKE` | Log `native: host wake` to stderr each time the core wakes an idle UI task for a host call (T|X and Zire 72), for `tests/host-call-wake.py`. |
| `FROND_WIFI_TRACE` | Trace the virtual Wi-Fi card. |
| `FROND_HOST_PROFILE` | Print host timing figures from the benchmark tools. |
| `SDL_VIDEODRIVER`, `SDL_AUDIODRIVER` | Standard SDL; `dummy` for headless. |

### Tests and tools

| Variable | Meaning |
| --- | --- |
| `FROND_DANA_SPI_ARM`, `FROND_DANA_SPI_TRACE`, `FROND_DANA_SPI_REPLAY` | Files and replay data that `tests/dana-card-spi.py` hands to its GDB script. |
| `FROND_DANA_USB_LOAD_ONLY` | Set for `tests/dana-usb-session-gdb.py` to load the session and skip the rest of the check. |
| `ISP_FRAME_AREA`, `ISP_HOST_AREA`, `ISP_ATL_AREA` | Scratch folders of `test-isp-frame`, `test-isp-host-pio` and `test-isp-host-atl`. |
| `RAW_PRINT_BUILD`, `RAW_PRINT_SOURCE`, `RAW_PRINT_FILE_BUILD` | Scratch folders of `test-raw-print-job` and `test-raw-print-file`. |
| `USB_PRINTER_AREA`, `USB_RUNTIME_AREA`, `USB_PRISM2_AREA`, `USB_WIFI_AREA` | Scratch folders of `test-usb-raw-printer`, `test-usb-printer-runtime`, `test-usb-prism2-adapter` and `test-usb-wifi-runtime`. |
| `FROND_CORE` | Core binary for `tests/smoke.py` and similar, such as `build/frond-jit`. |
| `FROND_DEVICE_IMAGES` | Firmware folder (laid out as `Palm/` with the device images) for `tests/smoke.py` and `tests/zire72-rom.py`. No default; those tests skip without it. |
| `FROND_EMULATOR_CMD` | Command that launches the core in `tools/qualify-peripherals.py`, such as `heavy emulator`. Default `emulator`. |
| `FROND_GDB` | ARM-capable GDB for the debugger tests. Otherwise `gdb-multiarch` is used if found. |
| `FROND_GDBSTUB_TARGET` | Test target binary for `tests/gdbstub.py`. Default `build/test-gdbstub-target`. |
| `FROND_TX_DEBUG_CONFIG` | JSON config handed to the debugger probe script by `tests/pace-debug-tx.py`. |
| `FROND_BTVIRT` | `btvirt` binary for the Zire 72 Bluetooth test. |
| `FROND_BT_PHONE` | The Bluetooth phone the hub runs (the desktop tests use a stand-in). |
| `FROND_HARD_RESET` | `1` adds the TX and E2 hard-reset run to `tests/zire72-rom.py`. |
| `FROND_ZIRE_LAUNCHER`, `FROND_ZIRE_STORAGE`, `FROND_ZIRE_PERIPHERALS` | `1` adds the launcher and key runs, the storage runs, or the peripheral runs to the Zire 72 ROM tests. |
| `FROND_ZIRE_EVIDENCE` | Folder those runs write screenshots to. |
| `FROND_ARM_SESSION` | ARM session for `tests/core-control.py`. Default the palm-tx smoke machine. |
| `FROND_LINK_PRINT` | `1` makes `make test-link-protocol` print its pinned values instead of only checking them. |
| `FROND_LINK_SOCKET`, `FROND_LINK_PORT`, `FROND_LINK_DISCOVERY_PORT`, `FROND_LINK_DISCOVERY_BIND`, `FROND_LINK_BEACON_TO`, `FROND_LINK_IDLE_EXIT_MS`, `FROND_LINK_PAIR_CODE_MS`, `FROND_LINK_HUB` | Link hub hooks for tests: hub socket, TCP port (0 for an ephemeral one), UDP discovery port (0 turns it off), the address the beacon socket binds, host:port beacon targets (comma list) instead of broadcast, idle exit in ms (default 30000), the pairing code's life in ms (default and most 120000) and the hub program a window starts. |
| `XDG_RUNTIME_DIR` | Holds `frond/` (mode 0700) with the link hub's socket and the cores' link sockets. |
| `FROND_DESKTOP`, `FROND_IR_PROBE` | `frond-desktop` and the IR probe PRC for `tests/link-irda-cores.py`. Defaults `out/frond-desktop` and a probe built with pilrc and the SDK. |
| `FROND_ARM_SESSIONS` | Folder of ARM machine folders, one per device name, for `tests/frond-arm-install.py`, `tests/host-call-wake.py`, `tests/patched-traps.py`, `tests/card-swap-rom.py`, `tests/card-reinsert-rom.py` and `tests/link-irda-cores.py` (palm-tx only). Default `build/smoke/machines`. |
| `FROND_REQUIRE_SESSIONS` | `1` makes those two tests fail instead of skipping when a session or the probes are missing. |
| `FROND_REQUIRE_E3` | `1` makes `tests/smoke.py` and `tests/frond-arm-install.py` fail instead of skipping the Tungsten E3 when its firmware or session is missing. |
| `FROND_REQUIRE_E2_DENVFS` | The same for the E2 de-NVFS image (`palm-e2-denvfs`), whose NOR comes from `FROND_TEST_E2_DENVFS_NOR`. |
| `FROND_IIIX_ROM`, `FROND_IIIX_CORE` | IIIx ROM and `frond-68k` binary for the opt-in IIIx run in the desktop workflow test (`iiixRealRom`). |
| `FROND_IIIX_SHOTS` | Folder that run saves pictures of the Palm window to (needs ImageMagick's `import` and a display). |
| `Z72_SESSION`, `FROND_TEST_MIC_LEVEL` | Zire 72 session for `make test-z72-voice-memo` (skipped when empty), and the fake 1 kHz microphone level that test's core build reads. |
| `FROND_TG50_SESSION` | TG50 session for `tests/tg50-usb-hotsync.py`. Default `build/smoke/machines/sony-clie-tg50` (or the older `build/smoke/sony-clie-tg50.session`). |
| `FROND_TG50_SCREENSHOT_DIR` | Folder `tests/device-controls.cpp` saves TG50 screenshots to. |
| `FROND_TEST_CORE`, `FROND_TEST_SESSION` | Core and TX session for the desktop workflow test. |
| `FROND_CARD_TOOL` | The program the desktop runs as the card tool (`--card`), instead of its own core. The workflow test sets it to the ARM core. |
| `FROND_CARD_OUT` | Folder the card file tests (`tests/card-file.cpp`, Android's `CardFileTest`) write edited cards to, with the files each should hold in `NAME.files/`, for loading them in the core. |
| `FROND_FIXTURE_OUT` | Folder the card file tests (`tests/card-file.cpp`, Android's `CardFileTest`) write that side's cut-off card fixture to, for `tests/fixtures/card-leftovers/desktop` or `android`. |
| `FROND_SCREENSHOTS` | Folder the desktop workflow test saves its Cards window, New card and picker screenshots in. |
| `FROND_TEST_SCREENSHOT`, `FROND_TEST_KEYBOARD`, `FROND_TEST_NETWORK` | Extra checks in that test: expected screenshot, keyboard and network state. |
| `FROND_TEST_SHOTS` | Folder to save pictures of the home window and settings to. |
| `FROND_TEST_SHOT` | File the desktop workflow test saves the running window with the Applications panel open to. |
| `FROND_TEST_E2_NOR`, `FROND_TEST_E2_NAND` | E2 firmware for the running-page picture. |
| `FROND_TEST_QUALIFIED_NOR` | TX NOR image for the firmware import test. |
| `FROND_TEST_E2_DENVFS_NOR` | Qualified 6 MiB E2 de-NVFS NOR for desktop and Android raw/session audio checks, and for the `palm-e2-denvfs` entry in `tests/smoke.py`. Read only; changed-image checks use copies. |
| `FROND_TEST_TG50_CORE`, `FROND_TEST_TG50_NOR`, `FROND_TEST_TG50_SESSION`, `FROND_TEST_TG50_HOST_SESSION`, `FROND_TEST_TG50_TONE_SESSION`, `FROND_TEST_TG50_SHOTS`, `FROND_TEST_TG50_BT_SOCKET` | TG50 core, firmware, sessions, picture folder and Bluetooth socket for the TG50 desktop tests. |
| `BENCH_DUMP_RAM` | `1` makes `benchmark-replay` write guest RAM at the start and end. |
| `BIKE_SESSION`, `FRONDBENCH_SESSION` | Session files for `make speed-check`. No default; a workload without one is skipped. |
| `SPEED_BENCH` | `benchmark-replay` binary for `tools/speed-check.py`. |
| `SPEED_MIPS` | MIPS setting for `make speed-check`. Default 100. |
| `SPEED_HOST` | Run `make speed-check` on another machine: an ssh destination (`user@host`), working in `~/frond-speed/`. |
| `SPEED_SSH_KEY` | Optional ssh key for `SPEED_HOST`. |
| `SPEED_REAL_MS` | Optional real-device FrondBench times, like `Integer=1000,Memory=500`, for a ratio column. |

### Build

| Variable | Meaning |
| --- | --- |
| `FROND_FAST` | `0` leaves the Fast engine (Dynarmic) out. Default on for x86-64 and arm64. |
| `BUILD_JOBS` | Parallel jobs for the desktop and Dynarmic builds. Default 4. |
| `ARM64_JOBS` | Jobs for the arm64 Docker build. |
| `FROND_DYNARMIC_SRC`, `FROND_DYNARMIC_LIBS` | Where the core's makefile finds the Dynarmic source and libraries. `make` sets them from `DYNARMIC_SRC`. |
| `DYNARMIC_SRC` | Where the patched Dynarmic source goes. Default `build/dynarmic-trial/src`. |
| `DYNARMIC_DONOR` | A Dynarmic clone to export the pinned source from. Default `build/dynarmic-donor`, fetched on first use. |
| `DYNARMIC_MIRROR` | Where `tools/dynarmic-source.py` fetches Dynarmic from. |
| `JIT_DIFF_ARGS` | Arguments for `make test-jit-differential`, such as `--seeds 1: --duration 3600` or `--seed N`. Replaces the default runs. |
| `PALM_SDK` | Palm OS SDK for `make frondbench` and the Palm probes. |
| `SLIRP_RESOLV_CONF` | `resolv.conf` the Android build gives libslirp. |

## Tools

All in `tools/`. Most take `--help`.

| Tool | What it does |
| --- | --- |
| `frond-run.py` | Developer runner, see above. |
| `pace-report.py` | PACE report with program identity and symbols. |
| `prepare.py` | Rebuilds `build/source` from the upstream tree and `patches/` (`make prepare`). |
| `check-reconstruction.py` | Rebuilds the core twice independently and compares them. |
| `check-docs.py` | Checks this file against the sources (`make check-docs`). |
| `check-usb-printer-capture.py` | Compares a private USB packet trace with one finished raw print job (`doc/usb-printer-runtime.md`). |
| `generate-device-data.py` | Writes the device profile data for Python and Android. `--check` fails if it's stale. |
| `firmware_variants.py` | Helper module: finds the firmware variant (such as the E2 de-NVFS image) of a NOR file for `frond-run.py`. |
| `device-profiles.cpp`, `device-profiles.json` | Dumps and holds the device profiles. |
| `dynarmic-source.py` | Exports the pinned Dynarmic source and applies its patches. |
| `package-deb.py`, `package_policy.py`, `check-package.py` | Build a Debian package from a fixed file list, and check one. |
| `speed-check.py` | Emulation speed against fixed guest work (`make speed-check`). |
| `benchmark-replay.cpp`, `benchmark-session.cpp`, `benchmark-menu.cpp` | Benchmark programs that run a replay or session for a fixed guest time. |
| `frondbench/` | FrondBench, a Palm benchmark app (`make frondbench`). |
| `host-profile.h` | Host timing used by the benchmarks. |
| `analyze-pcm.py` | Signal levels and silent gaps in 16-bit little-endian PCM. |
| `analyze-audio-buffers.py`, `analyze-audio-timing.py`, `analyze-gameplay.py`, `analyze-guest-latency.py`, `analyze-input-latency.py`, `analyze-menu-response.py` | Read the traces from the optional debug patches (two in `tools/debug/`, the rest in git history, see performance.md). |
| `check-native-audio.py` | Captures a native audio stream and measures its PCM and load. |
| `check-audio-probe.py`, `check-bluetooth-probe.py`, `check-irda-probe.py` | Check the logs of the Palm probe apps. |
| `check-compatibility.py`, `compatibility-results.py`, `app-compatibility.py` | App and system compatibility runs and their result decoding. |
| `build-palm-probe.py` | Builds a Palm probe app from `tests/palm/` with a local SDK (`--probe NAME`, `--sdk PATH`). |
| `build-bluetooth-provider.py`, `build-bluetooth-opp-peer.py` | Build the BlueZ H4 provider (`btvirt`, also installed by `make deb` as `/usr/lib/frond/btvirt`) and the virtual peer (OPP and SPP test peer, and the Bluetooth phone, installed as `/usr/lib/frond/bluetooth-phone`). |
| `bluetooth-opp-peer.c`, `bluetooth-dun.cpp`, `bluez-bdaddr.patch`, `bluez-bthost-config.patch` | Sources and patches for those. |
| `palm-pppd.sh DEVICE` | Runs slirp (or pppd with `--pppd`) on the IIIx's serial PTY or `tcp:PORT`, see Palm IIIx. `slirp-select-fix.c` is the shim it preloads. |
| `link-pty.py ENDPOINT` | Puts a PTY in front of a TCP endpoint (`PORT`, `HOST:PORT` or `listen:PORT`) for programs that only open serial devices. `--link PATH`, `--seconds N`. |
| `link-irda.py FIRST SECOND` | Joins two infrared endpoints. `--seconds N`. |
| `link-bluetooth.py serve\|join A B` | Carries Bluetooth controller connections over TCP. `--seconds N`. |
| `probe-tx-serial.py`, `probe-tx-serial-host.py`, `probe-tx-usb-hotsync.py`, `probe-serial-hotsync.py` | Serial and USB HotSync probes on a copy of a TX session. `probe-serial-hotsync.py` runs pilot-link over the serial port of a TX or E2 session, pressing the HotSync button with a `key-down hotsync` replay. |
| `qualify-peripherals.py` | Staged peripheral run against your own sessions. |
| `tx-baseline.py` | TX acceptance checks from your own images. |
| `test-input-replay.py` | Invalid input replays must fail before the guest runs. |
| `palm_boot.py`, `guest_process.py`, `guest_faults.py` | Helpers the tests and tools import. |
| `arm64/` | Dockerfile and build script for the arm64 package. |
| `debug/` | Optional trace patches for the core. |

## Make targets

`make` builds `out/frond` and the desktop app. Test targets build a small program and run it. They need no firmware unless noted.

| Target | What it does |
| --- | --- |
| `all` | Core and desktop app. |
| `prepare` | Rebuild `build/source` from upstream and the patches. |
| `out/frond` | The core only. |
| `out/frond-68k` | The Palm IIIx runner. |
| `frond-jit` | Copy of `out/frond` for runs that name the JIT. |
| `desktop` | The Qt app, `out/frond-desktop`. |
| `deb`, `deb-arm64` | Debian packages. The arm64 one is built in Docker. |
| `clean` | Remove build output. |
| `dynarmic-source` | Export the pinned Dynarmic source again. |
| `device-profiles` | Build the device profile dump. |
| `generate-device-data`, `check-device-data` | Write, or check, the generated profile data. |
| `check-reconstruction` | The patches rebuild to the same source twice. |
| `check-docs` | This reference matches the sources. |
| `test` | Everything in the groups below marked "in `make test`". |
| `test-desktop` | The Qt tests. Needs `mtools`. |
| `smoke`, `smoke-sessions` | Real-firmware check per device, and making its sessions. Skips a device without firmware. |
| `speed-check` | Speed against fixed guest work. |
| `benchmark-replay`, `benchmark-replay-jit`, `benchmark-session`, `benchmark-menu` | Build and run the benchmark programs. |
| `frondbench` | Build FrondBench with m68k-palmos-gcc. |
| `device-session-probe` | Build the device session probe. |
| `bluetooth-opp-peer` | Build the OPP test peer and Bluetooth phone. |

Tests in `make test`, by area:

| Area | Targets |
| --- | --- |
| CPU and engines | `test-cpu`, `test-arm-conditions`, `test-pxa270-wmmx`, `test-instruction-cache`, `test-mmu-fast-path`, `test-mem-region-bounds`, `test-xscale-prefetch`, `test-jit-overshoot`, `test-jit-differential`, `test-jit-compile-thread` |
| SoC and timing | `test-pxa-timer`, `test-pxa-power`, `test-pxa255-udc`, `test-pxa255-udc-clock`, `test-pxa255-udc-dma`, `test-pxa255-udc-dma-state`, `test-pxa270-udc`, `test-scheduler`, `test-microwire64x16` |
| Device state and sessions | `test-tx-state`, `test-e2-state`, `test-tg50-state`, `test-device-profiles`, `test-device-controls`, `test-e2-family-peripherals`, `test-multi-touch`, `test-68k-frame-bounds`, `test-68k-bus-state`, `test-session-v7`, `test-frond-68k`, `test-checkpoint-input`, `test-export-checkpoint`, `test-replay-snapshot`, `test-lifecycle`, `test-shortcut-keys` |
| Palm databases | `test-native-database`, `test-native-primitives`, `test-native-governor`, `test-db-list`, `test-db-backup` |
| Storage | `test-sd-image`, `test-sd`, `test-card-backing`, `test-memory-stick-controller`, `test-memory-stick-media`, `test-ms-logical-equivalence` (the version 2 stick layout against the flat physical model, `build/test-ms-logical-equivalence SEQUENCES OPERATIONS` for a longer run), `test-memory-stick-pro`, `test-card-tool`, `test-storage-settle` (needs mtools), `test-storage-fat` (needs mtools), `test-storage-ms` (classic stick writes, the settled check and blank stick formats, needs mtools), `test-card-swap` (its firmware part on whichever smoke sessions are in `FROND_ARM_SESSIONS`), `test-card-reinsert` (the same sessions) |
| Audio | `test-sdl-audio`, `test-fast-audio`, `test-earlier-audio`, `test-audio-results`, `test-pxa-i2s-audio`, `test-tg50-pwm-audio`, `test-tg50-microphone-adc`, `test-tg50-microphone-dma`, `test-tg50-microphone-state`, `test-z72-microphone`, `test-z72-voice-memo`, `test-tg50-call-budget`, `test-tg50-pace`, `test-tg50-audio-replay`, `test-tg50-serial-board`, `test-tg50-serial-configuration` |
| Serial, USB, infrared | `test-uart-lifecycle`, `test-uart-checkpoint`, `test-uart-flow`, `test-uart-dma-peer`, `test-tx-serial`, `test-hotsync-line`, `test-host-serial`, `test-pty-link`, `test-usb-enumeration`, `test-usb-host-fixture`, `test-usb-bridge`, `test-usb-bridge-soc`, `test-irda-endpoint`, `test-irda-link`, `test-irda-results`, `test-keyboard` |
| Dana USB, printing and Wi-Fi | `test-68k-card-provider`, `test-68k-dana`, `test-68k-sd`, `test-68k-vz-registers`, `test-dana-card-inventory`, `test-isp-frame`, `test-isp-host-atl`, `test-isp-host-pio`, `test-print-text`, `test-raw-print-file`, `test-raw-print-job`, `test-usb-raw-printer`, `test-usb-printer-runtime`, `test-usb-prism2-adapter` (the adapter leaf), `test-net-wifi-router` (the shared Ethernet router), `test-usb-wifi-runtime` (the Dana's adapter on the ISP1161 port: enumeration, slirp ARP, supply, port reset, swap with the printer, reset, load, a load mid frame and a rejected load), `test-dana-wifi-module` (the Dana Wireless's built-in Wi-Fi module). `desktop-print` in `make test-desktop` covers the USB port row |
| Link protocol | `test-link-protocol` |
| Bluetooth | `test-bluetooth`, `test-bluetooth-link`, `test-bluetooth-results`, `test-csr-bcsp`, `test-csr-controller-lifecycle`, `test-csr-peer-services` |
| Network | `test-network`, `test-network-reliability`, `test-host-network`, `test-wifi`, `test-modem`, `test-ir-peer`, `test-ir-printer` |
| Debugger and tools | `test-gdbstub`, `test-pace-debug`, `test-pace-step`, `test-debugger-inspect`, `test-developer-run`, `test-compatibility`, `test-gameplay-metrics`, `test-build-tools`, `test-render-link`, `test-peripheral-qualification` |

Not in `make test`. The first group builds a program and runs it with a private session, the rest are firmware-free and can be run by name:

| Target | Needs |
| --- | --- |
| `test-tx-lifecycle-rom`, `test-tx-power-rom`, `test-tx-usb-rom` | Build only. Run the program with a TX session. |
| `test-frond-68k-peripherals` | IIIx ROM. Piezo, backlight, sleep, serial HotSync (pilot-link) and databases. |
| `test-frond-68k-gdb` | IIIx ROM, `build/hello-debug`, `m68k-palmos-gdb` and gdb-multiarch. |
| `test-frond-68k-boot-save` | IIIx ROM. A session saved over the GDB stub mid-boot after a soft reset keeps its databases on reload. |
| `test-frond-68k-busy` | IIIx 3.1 ROM, `m68k-palmos-gcc` and the 3.5 SDK in `../palm-os-sdk`. Database commands while an app is busy for 45 s. |
| `test-frond-68k-install` | IIIx 3.1 ROM, `m68k-palmos-gcc` and the 3.5 SDK in `../palm-os-sdk`. Installs at the launcher and with another app in front, and install and export while hidden. |
| `test-frond-arm-install` | T\|X, E2, Zire 72 and TG50 smoke sessions (`FROND_ARM_SESSIONS`, else `build/smoke/machines`), `m68k-palmos-gcc` and the 3.5 SDK. The same installs on each ARM device, and host calls while the Palm sleeps. |
| `test-link-irda-cores` | T\|X smoke session (`FROND_ARM_SESSIONS`, else `build/smoke/machines`) and the IR probe (`FROND_IR_PROBE`, else pilrc and the SDK). Two T\|X cores beam to each other over two link hubs on loopback, also after one sleeps and wakes. |
| `test-host-call-wake` | T\|X and Zire 72 smoke sessions as above. A GDB breakpoint on EvtWakeup: `guest-ready` wakes nothing, a host call wakes at most once. |
| `test-patched-traps` | T\|X smoke session as above. Hooks in the native trap table like UDMH's, written over the GDB stub: the database list, an install, an export and a launch still work, and the sync probe still gets its notify. |
| `test-session-folder-card` | Nothing, plus the T\|X smoke session as above for its last case. A session folder that doesn't load is refused with one line, not a crash, and names the card file when one is there. |
| `test-core-control` | ARM smoke session (`FROND_ARM_SESSION`) and IIIx ROM. The control socket on both runners, and the save when it closes without a quit. |
| `test-printer-control` | ARM smoke session (`FROND_ARM_SESSION`) and the Dana ROM (`FROND_DEVICE_IMAGES`). `irprinter` and `printer` with their arguments through both runners' control sockets. |
| `test-tg50-usb-hotsync` | TG50 session (`make smoke-sessions`) and pilot-link. |
| `test-tg50-audio-lifecycle`, `test-tg50-audio-queue` | Nothing. TG50 audio unit tests. |
| `test-host-microphone`, `test-host-microphone-state` | Nothing. Host microphone unit tests. |
| `test-csr-bcsp-lifetime`, `test-bluetooth-opp-peer` | Nothing. Bluetooth unit tests. |
| `test-bluetooth-dun` | `btvirt` (`BLUETOOTH_PROVIDER`, default `build/bluetooth-provider`). The Bluetooth phone end to end. |

Dana core keyboard commands are `dana-down NAME`, `dana-up NAME` and
`dana-text TEXT`, in the console and control socket. Names come from
`dana_keyboard5.h`; replay uses `key-down dana.NAME` and `key-up dana.NAME`.
Input can be refused while the bounded keyboard queue is busy; retry after the
machine runs. A text request holds at most 24 scan transitions (12 unshifted or
six shifted characters), accepts ASCII and uses US key positions. Palm OS applies
caps, sticky modifiers and repeat. SDL physical keys use Ctrl, GUI for command
and Alt for function. The built-in keyboard is separate from infrared keyboard
attachment.

The native Dana keyboard art shares these physical names. `danaKeyboardArt()`
supplies key bounds and legends; `danaKeyAt()` returns an art index, not a scan or
button ID. `DanaKeysHeld` stores pressed states by name and `drawDanaKeyboard()`
renders them. `danaLayout()` fits the wide LCD above the keyboard in an inset-safe
viewport, with the keyboard optionally hidden. Callers apply the viewport offset.
The art does not dispatch input or enable the frontend.
`make test-dana-keyboard-art` checks key inventory, hit areas, rendering and layouts.
`DANA_ART_SOURCE` and `DANA_ART_BUILD` select isolated source and build directories.

The adaptive native view accepts `--dana-view "keyboard=auto zoom=2"`, the
`dana-view` console/socket setter and `dana-view-state` query. Setter fields are
`keyboard=auto|compact|full|hidden`, `zoom=fit|1|1.5|2|3`, `pointer=touch|pan`,
`pan-x=0..560`, `pan-y=0..160`, `density=0.25..16`, `physical=on|off` and
`page=abc|symbols|more|fn`.
Pan is in guest pixels; density is renderer output pixels per logical unit.
The complete argument is validated before changing the view or cancelling input.
Other devices refuse these commands.

Replies and `dana-view` notifications share the `dana-view:` prefix and include
the desired fields, `resolved` keyboard mode and `page=abc|symbols|more|fn`.
Frontends save the user's view choices; density and physical-keyboard presence
remain live host state. Desired pan survives resizing while the rendered offset
is clamped to the visible region. `make test-dana-view` checks layout, coordinate
mapping and touch ownership; `DANA_VIEW_SOURCE` and `DANA_VIEW_BUILD` select
isolated directories.

`make test-dana-input` checks the typed Dana host route without firmware.
`tests/device-rom.py --keyboard-session READY.frond --keyboard-probe PROBE.prc`
selects its guest keyboard check, with `--device alphasmart-dana` and a new
`--evidence` directory.

Dana IrDA uses UART2 SIR on PJ4/PJ5, with active-low PG4 shutdown and the guest
pin mux. It uses the existing `irda-mode`, `irda-state` and `irda-stats` commands
and decoded SIR byte links. External serial stays unsupported. UART2 local FIFO
state is saved separately from the host endpoint; loading one machine does not
rewind a remote peer or bytes already sent to it. See the Dana checks in
[testing.md](testing.md).

Dana's staged localhost USB host uses a cooperative buffer-space query before
submitting a bulk OUT token. A full device FIFO holds the pending request until
space returns; only an actual ACK removes bytes from that request. Control and
IN transfers continue independently. Other adapters keep their existing request
scheduling. This host policy leaves the controller's full-FIFO NAK and transaction
status behaviour unchanged. Receive-pressure checks use Palm OS's event loop
while pausing application reads. Physical-host behaviour remains unqualified.

The inherited 68k SD model reserves slot IDs 0 for SD and 1 for Memory Stick;
a second SD slot uses ID 2. The default SD instance keeps chunk 0x1d0.
The second instance uses chunk 0x201, with its own command, buffer and block
cursor. SD uses version 1; MMC uses version 2 with a trailing protocol field.
ExternalStorage writes version 1 with the two
original image keys when the second SD is empty, or version 2 with a third
key when it is mounted. Version 1 loads with the second SD empty. These
chunks store media identity and protocol state, not card contents.

Dana routes SPI1 to separate SD instances. Slot 0 uses B5 power, active-low
J3 select and D2 detect; slot 1 uses K5 power, active-low F2 select and D3
detect. F7/F0 are low for write protection. D1/D0 stay high for SD memory cards;
SDIO interrupts are unsupported. PJ0/1/2 must select SPI1. Both selected cards
receive clocks; overlapping replies are combined with AND as a deterministic
contention model, without an electrical fidelity claim.

Power loss and explicit insertion/removal reset only that card. Deselection
ends its SPI transaction. Protocol restore keeps each cursor, including across
remount, and missing legacy Dana SD chunks reset their instances. No card
contents are rewound by these snapshots. Read-only CardImage policy is fixed
at construction and rejects sector and byte-range writes and formatting.

Dana's experimental frontend uses the native per-slot runtime attachments and
linked sessions. The native backend also supports portable folders. Frontend
transfer paths that cannot preserve both backings are refused.

Dana runtime cards (staged): `sd-attach` and `sd2-attach` take an absolute
`.frondsd` path, a distinct 32-character lowercase hexadecimal identity and
`writable` or `protected`, then optional `sd` or `mmc` (default `sd`).
Quote paths containing spaces. `--sd-attach` and
`--sd2-attach` accept the same argument as one quoted command-line value.
`sd-state`, `sd-eject`, `sd-insert` and `sd-take-out` address the first slot;
`sd2-state`, `sd2-eject`, `sd2-insert` and `sd2-take-out` address the second.
Socket commands use the same names and argument strings. `sd-export` and
`sd2-export` write the live card to a new absolute destination path. Eject before taking
out; taking out settles writable media into a portable card file.

The runner takes an exclusive nonblocking file lock, rejects duplicate keys
and CID serials. Its fixed-512-byte SDSC encoder accepts representable capacities
from 32 MiB up to 1 GiB minus 256 KiB. The legacy scalar inventory range still
ends at 2048 MiB; exact CSD validation rejects unrepresentable sizes within it.
These are runtime constraints; guest qualification covers 32 MiB.
MMC attachments accept 32, 64, 128 or 256 MiB.
Both physical slots accept these families independently. Stable ExternalStorage/HAL
IDs are SD0=0, MemoryStick=1 and SD2=2; Dana labels SD0 as slot 1 and SD2 as
slot 2. Dana has no Memory Stick slot. The additive `storage_slots` inventory
lists each slot, family rules and guest-qualified sizes. Legacy scalar SD fields
describe the primary slot. `deviceProfileCardSizeValid5` validates native media;
the SPI CSD encoder shares its exact representability helper. An integer MiB
size within the SDSC bounds can still fail CSD encoding. No SDHC or SDIO.

Guest qualification covers 32 MiB FAT SD and MMC on each slot, paired and in
both mixed orders, including read/write, protection, removal, restore and
lifecycle checks. Larger runtime capacities have synthetic geometry coverage.
Frontend selection, single-file card conversion and unsupported socket export
paths remain refused. ARM runner eligibility remains false; Dana uses frond-68k.

CardImage can read through a bounded CardBacking provider; raw constructors
remain writable by default. Provider images have no RawData pointer. Successful
runtime block writes commit file generations before the SD success response.
A commit error stops later writes. Completed external writes survive resets
and are not rewound by protocol state. Keep the backing and generation files
together until take-out succeeds. Protected attachment does not change payload
bytes. Linked sessions and portable folders retain inserted and ejected cards.

Native 68k linked SD sessions use `FRONDLINKSD1\n`, followed by three
little-endian uint32 values (manifest byte length, machine FNV-1a32, manifest
FNV-1a32), an at-most-8192-byte manifest and an ordinary SessionFile5 image.
New manifests start with `frond-linked-sd 3`. Each row contains HAL slot (0 or 2),
`sd`, quoted absolute backing path, 32-character lowercase hex identity, decimal
CID serial, payload bytes, protected and inserted flags (0 or 1), then the base
file's device, inode and nanosecond modification time, followed by `sd` or `mmc` and the committed root generation ID.
The row kind stays `sd` for the shared block container. Missing rows mean empty
slots. SD0/chunk01d0, MS1 and SD2/chunk0201 keep their existing IDs.

These saves refer to live external backings. Keep the base `.frondsd` and all
its `.gen-*` files together at their original paths. Completed external writes
remain current when an older machine snapshot loads. Taking out, replacing or
modifying the base invalidates earlier linked saves. This format is for the
native core; portable SessionFile5 readers refuse its magic. It is not a portable
export or a standalone machine copy. Card-free saves keep their existing format.
Unmanaged images and failed backings still refuse saves.

Load stages both backing handles and validates the descriptors and production SD
chunks before replacing attachments. Owned files reuse their locks only at the same normalised absolute path;
a protection change shares the owned backing with a new immutable image policy.
The paused load retains both old handles until success and restores the old
machine and attachments if another machine chunk fails. Provider bytes never
rewind with protocol state.

Strict Dana restore requires an existing root and a complete validated chain,
with a root ID at least as recent as the linked descriptor. Pristine attachments
and portable exports write a committed base marker before saving descriptors.
Missing roots or generations fail without deleting namespace entries. Dana
manifest versions 1 and 2 lack history proof and are refused, including pristine
copies. Generic FileCardBacking consumers retain missing-history refusal.

The uARM one-slot runtime explicitly opens a missing root from the last saved
base. Its ordinary writes are temporary overlays; saves adopt the updated base
without calling Commit. After a process crash, rootless overlays and indexes are
retained and ignored, and new generations use IDs above the retained files.
This policy cannot prove whether an absent root once represented a Commit;
callers that need committed-history proof must use the strict provider policy.
A present malformed root or a corrupt bound chain still refuses without cleanup.
Ordinary writes do not become durable commits.

Generation validation checks the full file and ordered unique page index before
staging succeeds. Each index contains at most one record per geometry page;
chains allow 256 files and validation visits at most sixteen full-card indexes.
Strict indexes are also capped at 524288 records, the 2 GiB runtime maximum
at 4096 bytes per page. Normal commits compact at eight generations; the larger
chain limit allows pinned histories while bounding validation work. A commit at
either validation limit must compact successfully before publishing its root.
Failed compaction leaves the previous committed root readable; retry or roll back.
Unknown namespace entries are preserved and prevent a strict pristine attach.
Temporary names that also belong to an existing neighbouring backing are retained.
Keep stale or crash files for diagnosis; do not delete hidden files to force attach.

Dana portable folders use the native console commands `export-portable <new-folder>`,
`copy-session <new-folder>` or `duplicate-session <new-folder>`. Each captures the
current machine and streams both owned committed cards, including ejected cards.
Open the folder as the runner's input, or use `import-portable <folder>` or
`load-session <folder>`. `peek-session <folder>` checks both slots and reports
insertion. Guest card IDs and CID serials stay with the saved protocol state;
the copies own separate files and generation roots. Ordinary saves remain linked.
Keep the whole folder, including hidden `.gen-*` files after use. Moving it on
the same filesystem keeps ownership. Cross-filesystem moves are refused; use a
runtime copy command before moving across filesystems. Use the copy commands for new copies; a
filesystem copy with different media inodes is conservatively refused. The new
`FRONDPORTSD1` wrapper is refused by the older common/frontend folder and container
readers; single-file conversion and frontend import are not supported yet.
Publication flushes the staging directory, renames without replacing an existing
output, then flushes its parent. A failed final flush reports output published
with uncertain durability and keeps the complete output. Only a completed parent
flush reports success.

Dana MMC media use legacy byte-addressed SPI with aligned 512-byte blocks,
CSD structure 1.2 / MMC spec field 3.3 and the virtual product name `FRNDMC`.
CMD1 initializes MMC; CMD55 and SD application commands are refused, and CMD8
has no R7 response. The six-character MMC CID carries the per-card serial.
The implementation covers initialization, identification, block reads/writes and
reset used by Dana. CRC enable, erase, card lock and programmable CSD/write
protection commands are unsupported. This does not claim MMCplus, high-speed
MMC, eMMC or SDHC support. Register fields and SPI behaviour follow the
[SanDisk MMC/RS-MMC manual 80-36-00320 v1.3](https://www.elde.cz/datasht/MMC%20-%20ProdManRS-MMCv1.3.pdf),
sections 3.5 and 5.

Portable manifests use `frond-portable-sd 3` with a trailing protocol
column and a committed base root in each new copy. Wrapper magic, HAL slots and chunk IDs stay the same.
SD protocol chunks retain v1. MMC chunks use v2 with a trailing uint32 protocol
value 1; MMC refuses ACMD state. Restore checks the decoded family against the
staged descriptor before installing either card. Ejected media retain the family.
A standalone `.frondsd` holds blocks only; specify the family again on attachment.
