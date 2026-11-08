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
| `--mips N` | Target speed in MIPS. Default 100. |
| `--audio-period FRAMES` | Host audio period, 64 to 8192. Default 512. Low-latency audio sizes its limits from it; Android passes 1024 or more. |
| `-q`, `--no-sound` | Start with audio off. |
| `--fast-audio`, `--experimental-fast-audio` | Preview one qualified TX, E2, E3 or Zire 72 stereo stream before the firmware audio queue. The E3 and E2 de-NVFS image use the HAL path with an ARM stream-start verifier and no Thumb queue hooks. |
| `--low-latency-queue` | The `--fast-audio` queue limits without the preview, on any device and firmware: low startup thresholds, and the 120 Hz slice and pending caps of 6 and 4 host periods (3072/2048 frames at the default 512) while non-silent frames were queued in the last 100 ms. The frontends pass it for Low-latency audio on ARM devices with sound when the firmware is not qualified for the preview, including modified E3 images and the TG50. |
| `--gdb PORT` | Wait for GDB on 127.0.0.1:PORT before running, see [GDB and PACE monitor](#gdb-and-pace-monitor). |
| `--serial off\|pty\|tcp:PORT` | Connect the serial port to a host PTY or a loopback TCP listener (one client at a time). |
| `--usb off\|pty\|tcp:PORT` | Same for the USB HotSync port, through a built-in USB host. |
| `--irda off\|pty\|tcp:PORT` | Same for infrared SIR bytes. Not with the infrared keyboard. |
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
frond --card verify CARD
```

SIZE is in MiB for an SD card or a Memory Stick Pro and in MB for a classic stick, within what some device takes. Import takes a raw SD or Pro image, or a `.frondms`. The output is a settled card file (`.frondsd`, `.frondmsp` or `.frondms`) or, for export, a raw image (a `.frondms` for a classic stick). It's written beside OUTPUT and renamed into place, so a failed or cancelled run leaves nothing and an existing file stays. Settle finishes a change cut off beside an SD or Pro card file (below), and verify checks every byte of one against its CRCs without changing it. Standard output has `progress DONE/TOTAL` in bytes at each whole percent, then `ok KIND BYTES` (`ok image BYTES` for export, `ok settled GENERATION` and `ok verified GENERATION` for settle and verify) or `error CODE MESSAGE`. The exit status is 0, 1 on failure or 2 on bad use. SIGTERM or SIGINT cancels. Codes: `usage`, `size`, `read`, `image`, `same`, `unsettled` (a machine's save journal beside the card, or a change that doesn't fit it), `busy` (the card is open in Files), `space` (the folder needs the output plus 16 MiB free), `write`, `cancelled`.

Files (desktop and Android) edits a `.frondsd` or `.frondmsp` in place and keeps two files beside it while a change is being written: `CARD.files-intent` (the 1 MiB chunks that got data) and `CARD.files-commit` (the change's FAT and folder sectors, chunk CRCs and header). They're settled before the card is loaded, moved or exported, so the card comes out with the change done (a complete commit is replayed) or not (an intent alone gets its chunk CRCs and header rewritten). The apps, the core's card loads (a session folder, `--card-swap`, `sd-put-in`) and `frond --card export` and `settle` all settle them the same way. The card's generation moves by one per visit that changes something.

Card journals are `CARD.journal-N`, to generation N. A machine's save writes a session journal, replayed only by a load of the `.frond` at N. A standalone journal (journal version 2 with its standalone flag) is replayed whenever the card is settled. `CARD.journal-N.pending` lists the 1 MiB chunks a standalone change put data into before its journal; with no journal, settling rewrites those chunks' CRCs and the header at N. A v2 card file's header bytes 48-55 hold the size of a side region after the logical sectors, starting at the next 1 MiB boundary and covered by the chunk CRCs; it is 0 for every SD card and Memory Stick Pro.

The desktop launcher `frond-desktop [session]` takes `--core PATH` (the emulator to run, found beside it or in `/usr/lib/frond` by default), `--core-68k PATH` (the same for 68k devices like the Palm IIIx, default `frond-68k` beside the ARM core), `-h` and `-v`.

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
| `serial-state`, `usb-state` | | `serial`, `usb`: `off`, `pty PATH waiting\|connected`, `tcp PORT waiting\|connected`, or `off` with an error |
| `serial-mode`, `usb-mode`, `irda-mode` | `off`, `pty` or `tcp:PORT` | the state |
| `irda-state` | | `infrared` |
| `irda-stats` | | `infrared`: counters `rx= tx= rx_drop= tx_drop= pending= baud= rx_on= tx_on= power= sleeping=` |
| `bluetooth-state` | | `bluetooth` |
| `bluetooth-mode` | `off` or an absolute Unix socket path | `bluetooth` |
| `network-state` | | `network`: mode name |
| `network-mode` | `disabled`, `connected`, `timeout`, `disconnected`, `internet` or `wireless` | `network` |

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
| `ms-export` | file (stick must be ejected) | `exported` |
| `ms-files` | card folder path | `files`: first line `space USED TOTAL`, then `d\|f SIZE HEXNAME` per entry |
| `ms-get` | hex host file, newline, hex card path | `exported` |
| `ms-put` | hex host file, newline, hex card path | `changed` (host file up to 8 MB) |
| `ms-mkdir`, `ms-delete` | card path | `changed` |
| `ms-rename` | hex card path, newline, hex new name | `changed` |

The `ms-*` and `sd-*` commands exist on devices with that slot; the other gets `unsupported`. The file commands on a Memory Stick go through Palm OS and need `guest-ready`. A classic stick, a Pro stick and an SD card each use their own image format.

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
| `--session PATH` | Restore a saved session: a machine folder (`sessions/NAME/`) or a `.frond` file. A file brings the card files (`sd.frondsd`, `ms.frondms`, `ms.frondmsp`) beside it along. The run works on a copy. |
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
| `--serial ENDPOINT`, `--usb ENDPOINT` | `off`, `pty` or `tcp:PORT`, passed to the core. |

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
| `--serial off\|pty\|tcp:PORT`, `--irda off\|pty\|tcp:PORT` | Connect the serial port or infrared to a host PTY or a loopback TCP listener. The PTY path is printed. |
| `--gdb PORT` | Start the GDB stub on 127.0.0.1:PORT, see below. |
| `--desktop-socket PATH` | Connect to the desktop's control socket, see [Control socket](#control-socket). The console is off then. |
| `--save-on-disconnect FILE` | As on the ARM core. |

`--work-folder`, `--nand`, `--engine`, `--jog-controls`, `--usb`, `--bluetooth`, `--fast-audio`, `--hide-keyboard` and `--hide-jog-controls` are taken and ignored, as the desktop passes them to every core. `--card-swap`, `--card-identity` and `--card-protected` are refused with a one-line message and exit 2, as a IIIx has no card slot.

In the window F1 to F4 are the app buttons, the up and down arrows the rocker and Escape power. Typed text goes into the Palm.

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
| `serial-mode off\|pty\|tcp:PORT`, `irda-mode off\|pty\|tcp:PORT` | Set the serial or infrared endpoint and show its state. |
| `serial-state`, `irda-state` | Show the endpoint, `off` or `pty\|tcp ENDPOINT connected\|waiting`. |
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

The framing, the replies and the events are the same as for `frond`. `frond-68k` answers these: `device-state`, `guest-ready`, `replay-state`, `input-replay`, `save`, `install`, `export`, `export-db`, `db-list`, `launch-app`, `reset`, `hard-reset`, `quit`, `serial-state`, `serial-mode`, `irda-state`, `irda-mode`, `irda-stats`, `speed-state`, `display` (`contrast=` and `smooth=`), `sound-state`, `sound-mode`, `audio-on`, `audio-off`, and the IIIx's own `hotsync`, `speed` (`original` or `uncapped`), `backlight-state` and `gdb-start`, `gdb-stop`, `gdb-state`. `guest-ready` and the commands that need Palm OS answer `not-ready` until the UI is up. `install`, `export`, `export-db`, `db-list` and `launch-app` wait for the app's event loop as on the console, holding back later requests, and answer `not-ready` with the reason if it doesn't come. `window-id` is answered as on `frond`. Events are `ready`, `close`, `shortcut`, `serial` and `infrared`.

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
| `FROND_ENGINE_SETTING` | Desktop app: `1` shows the Engine setting. |
| `SDL_VIDEODRIVER`, `SDL_AUDIODRIVER` | Standard SDL; `dummy` for headless. |

### Tests and tools

| Variable | Meaning |
| --- | --- |
| `FROND_CORE` | Core binary for `tests/smoke.py` and similar, such as `build/frond-jit`. |
| `FROND_DEVICE_IMAGES` | Firmware folder (laid out as `Palm/` with the device images) for `tests/smoke.py` and `tests/zire72-rom.py`. No default; those tests skip without it. |
| `FROND_EMULATOR_CMD` | Command that launches the core in `tools/qualify-peripherals.py`, such as `heavy emulator`. Default `emulator`. |
| `FROND_GDB` | ARM-capable GDB for the debugger tests. Otherwise `gdb-multiarch` is used if found. |
| `FROND_GDBSTUB_TARGET` | Test target binary for `tests/gdbstub.py`. Default `build/test-gdbstub-target`. |
| `FROND_TX_DEBUG_CONFIG` | JSON config handed to the debugger probe script by `tests/pace-debug-tx.py`. |
| `FROND_BTVIRT` | `btvirt` binary for the Zire 72 Bluetooth test. |
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
| `build-bluetooth-provider.py`, `build-bluetooth-opp-peer.py` | Build the optional BlueZ H4 provider (`btvirt`) and the virtual OPP test peer. |
| `bluetooth-opp-peer.c`, `bluez-bdaddr.patch`, `bluez-bthost-config.patch` | Sources and patches for those. |
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
| `bluetooth-opp-peer` | Build the OPP test peer. |

Tests in `make test`, by area:

| Area | Targets |
| --- | --- |
| CPU and engines | `test-cpu`, `test-arm-conditions`, `test-instruction-cache`, `test-mmu-fast-path`, `test-mem-region-bounds`, `test-xscale-prefetch`, `test-jit-overshoot`, `test-jit-differential`, `test-jit-compile-thread` |
| SoC and timing | `test-pxa-timer`, `test-pxa-power`, `test-pxa255-udc`, `test-pxa255-udc-clock`, `test-pxa255-udc-dma`, `test-pxa255-udc-dma-state`, `test-pxa270-udc`, `test-scheduler`, `test-microwire64x16` |
| Device state and sessions | `test-tx-state`, `test-e2-state`, `test-tg50-state`, `test-device-profiles`, `test-device-controls`, `test-e2-family-peripherals`, `test-multi-touch`, `test-session-v7`, `test-frond-68k`, `test-checkpoint-input`, `test-export-checkpoint`, `test-replay-snapshot`, `test-lifecycle`, `test-shortcut-keys` |
| Palm databases | `test-native-database`, `test-native-primitives`, `test-native-governor`, `test-db-list`, `test-db-backup` |
| Storage | `test-sd-image`, `test-sd`, `test-card-backing`, `test-memory-stick-controller`, `test-memory-stick-media`, `test-memory-stick-pro`, `test-card-tool`, `test-storage-settle` (needs mtools), `test-card-swap` (its firmware part on whichever smoke sessions are in `FROND_ARM_SESSIONS`), `test-card-reinsert` (the same sessions) |
| Audio | `test-sdl-audio`, `test-fast-audio`, `test-earlier-audio`, `test-audio-results`, `test-pxa-i2s-audio`, `test-tg50-pwm-audio`, `test-tg50-microphone-adc`, `test-tg50-microphone-dma`, `test-tg50-microphone-state`, `test-z72-microphone`, `test-z72-voice-memo`, `test-tg50-call-budget`, `test-tg50-pace`, `test-tg50-audio-replay`, `test-tg50-serial-board`, `test-tg50-serial-configuration` |
| Serial, USB, infrared | `test-uart-lifecycle`, `test-uart-checkpoint`, `test-uart-flow`, `test-uart-dma-peer`, `test-tx-serial`, `test-hotsync-line`, `test-host-serial`, `test-pty-link`, `test-usb-enumeration`, `test-usb-host-fixture`, `test-usb-bridge`, `test-usb-bridge-soc`, `test-irda-endpoint`, `test-irda-link`, `test-irda-results`, `test-keyboard` |
| Link protocol | `test-link-protocol` |
| Bluetooth | `test-bluetooth`, `test-bluetooth-link`, `test-bluetooth-results`, `test-csr-bcsp`, `test-csr-controller-lifecycle`, `test-csr-peer-services` |
| Network | `test-network`, `test-network-reliability`, `test-host-network`, `test-wifi` |
| Debugger and tools | `test-gdbstub`, `test-pace-debug`, `test-pace-step`, `test-debugger-inspect`, `test-developer-run`, `test-compatibility`, `test-gameplay-metrics`, `test-build-tools`, `test-render-link`, `test-peripheral-qualification` |

Not in `make test`. The first group builds a program and runs it with a private session, the rest are firmware-free and can be run by name:

| Target | Needs |
| --- | --- |
| `test-tx-lifecycle-rom`, `test-tx-power-rom`, `test-tx-usb-rom` | Build only. Run the program with a TX session. |
| `test-frond-68k-peripherals` | IIIx ROM. Piezo, backlight, sleep, serial HotSync (pilot-link) and databases. |
| `test-frond-68k-gdb` | IIIx ROM, `build/hello-debug`, `m68k-palmos-gdb` and gdb-multiarch. |
| `test-frond-68k-busy` | IIIx 3.1 ROM, `m68k-palmos-gcc` and the 3.5 SDK in `../palm-os-sdk`. Database commands while an app is busy for 45 s. |
| `test-frond-68k-install` | IIIx 3.1 ROM, `m68k-palmos-gcc` and the 3.5 SDK in `../palm-os-sdk`. Installs at the launcher and with another app in front, and install and export while hidden. |
| `test-frond-arm-install` | T\|X, E2, Zire 72 and TG50 smoke sessions (`FROND_ARM_SESSIONS`, else `build/smoke/machines`), `m68k-palmos-gcc` and the 3.5 SDK. The same installs on each ARM device, and host calls while the Palm sleeps. |
| `test-link-irda-cores` | T\|X smoke session (`FROND_ARM_SESSIONS`, else `build/smoke/machines`) and the IR probe (`FROND_IR_PROBE`, else pilrc and the SDK). Two T\|X cores beam to each other over two link hubs on loopback, also after one sleeps and wakes. |
| `test-host-call-wake` | T\|X and Zire 72 smoke sessions as above. A GDB breakpoint on EvtWakeup: `guest-ready` wakes nothing, a host call wakes at most once. |
| `test-patched-traps` | T\|X smoke session as above. Hooks in the native trap table like UDMH's, written over the GDB stub: the database list, an install, an export and a launch still work, and the sync probe still gets its notify. |
| `test-session-folder-card` | Nothing, plus the T\|X smoke session as above for its last case. A session folder that doesn't load is refused with one line, not a crash, and names the card file when one is there. |
| `test-core-control` | ARM smoke session (`FROND_ARM_SESSION`) and IIIx ROM. The control socket on both runners, and the save when it closes without a quit. |
| `test-tg50-usb-hotsync` | TG50 session (`make smoke-sessions`) and pilot-link. |
| `test-tg50-audio-lifecycle`, `test-tg50-audio-queue` | Nothing. TG50 audio unit tests. |
| `test-host-microphone`, `test-host-microphone-state` | Nothing. Host microphone unit tests. |
| `test-csr-bcsp-lifetime`, `test-bluetooth-opp-peer` | Nothing. Bluetooth unit tests. |
