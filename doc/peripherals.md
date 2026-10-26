# Palm TX peripherals

## Virtual SD cards

The desktop's **SD Card** menu creates 32, 64 or 128 MiB FAT16 cards, imports raw
card images, ejects/reinserts the current card and exports an image. One card is
kept per session. **Launch Application** in the File menu can start an installed
Palm application by database name.

A card is a private working copy in emulator memory. Importing an image does not
overwrite it. Normal session save/close embeds the entire card with the CPU,
controller, RAM and flash state in one atomic `.frond` file. Save a Copy includes
the card too. No separate image file is required when resuming that session.
Inserted and ejected states are preserved. An ejected card remains available for
reinsertion, export and session saving. Creating/importing a replacement discards
that ejected working card after the replacement has loaded successfully; export
its image first if you want to retain it.

Close files on the Palm before ejecting. Export Image is enabled after ejection;
this exports the currently stored sectors and does not force an application to
flush files it has left open. Normal session snapshots also preserve guest RAM
and open-file state, which raw image exports do not. No host filesystem mount or
root privileges are required. Direct host-directory sharing is not implemented.

The native CLI also accepts `--sd card.img`, `mount`, `unmount` and `save-sd`.
TX images are limited to 32–128 MiB and must be aligned to 512-byte sectors.
Inputs remain unchanged unless explicitly selected as an export destination.
Sessions with embedded cards reject a conflicting `--sd` argument.

Card-containing session files use format version 5; earlier sessions remain
readable. New peripheral state may not be readable by older Frond builds. Copy
important older sessions before opening them in a newer version.

### Host↔Palm file exchange

Install `mtools` (`sudo apt install mtools` on Debian trixie), then use
**SD Card → Exchange Files…**. Frond itself runs as your normal desktop user,
including on KDE Plasma and Wayland. There is no loop mount, FUSE mount or
root filesystem access.

1. Save your work and close every file using the card inside the Palm app.
   Confirm the prompt. Frond ejects the card if needed. Ejection cannot flush
   files an app has left open, and Frond cannot detect those open files.
2. Frond saves an ejected recovery session, exports the card to private storage
   and opens the exchange dialog. If the save or export fails, editing stops.
3. Enter a card folder such as `/` or `/PALM/Programs` and choose **List Folder**.
   **New Card Folder…** creates one folder at a time; its parent must exist.
   **Copy Host File to Card…** asks for the full destination filename.
   **Copy Card File to Host…** asks for the full source filename and a new host
   filename. Copies preserve binary bytes, including music and application data.
4. Choose **Apply and Reinsert** to load the edited image into the running Palm.
   Open the file again in the Palm app after reinsertion. **Cancel** discards
   staged card edits and leaves the original card ejected; use **Reinsert** to
   return to it. Files already copied to the host remain there.

Names already present on either side are refused, including card names that
match only in case or by a short DOS alias. Choose a new name. There is no
replace, delete or recursive folder copy operation. Each card edit uses a
separate candidate image; failed or full-card copies leave the last successful
revision unchanged. Copied bytes are checked before accepting a card edit.
The running guest never shares an image with the host editor. Applying uses
an explicit image insertion only after the dialog closes; failed insertion
leaves the original card ejected. Normal session saving then includes the
new card and guest state together. Imported images and sessions stay unchanged.

The dialog shows the recovery path, under
`$XDG_DATA_HOME/frond/sd-exchange/recovery-*/Before exchange.frond` (normally
`~/.local/share/frond/sd-exchange`). To undo an exchange, **Save and Stop**, then
open that recovery session. It contains the whole Palm as it was just after
ejection. Recovery sessions are kept even after Cancel; remove them yourself
when no longer needed. They contain firmware and personal data, and can be
large. Private temporary images use Frond's cache directory and are removed on
normal completion. A crash can leave cache files, but does not change the
recovery session or imported files. A crash after applying and before saving
can lose the applied changes.

Exchange supports 32–128 MiB FAT16 images with a primary FAT16 partition or a
FAT16 superfloppy. Other image layouts may still import into the emulator but
cannot be edited here. Use literal absolute card paths; wildcards, traversal
components and ambiguous trailing spaces/dots are refused. Long names and
spaces are supported; characters outside the FAT code page depend on mtools.
A single mtools operation has a 15-second timeout. The dialog waits for that
operation to finish. Allow disk space for a recovery session and several full
card copies. This workflow requires cooperative file closing by the Palm app;
it does not make hot editing an inserted card safe.

### Validation

A locally built Palm OS 68k probe uses the real VFS API to enumerate the card,
read a host-prepared file, create/write/close/read a Palm file and append results
to `FrondSDLog`. The native acceptance test uses the same mtools exchange backend to copy the
host file in and retrieve the Palm file, then checks the exported FAT image and
Palm log, restores a mounted snapshot, restores an ejected snapshot, reinserts,
resets and reads again, then repeats creation/save/restore at 128 MiB. Imported
firmware/session/card inputs remain unchanged. These are emulator tests, not
physical SD timing or electrical validation.

```sh
make test-desktop  # includes firmware-free exchange and rollback checks; needs mtools
python3 tools/build-palm-probe.py --sdk ../palm-os-sdk/sdk-5r3
python3 tests/desktop-sd.py --session /path/to/tx.session \
  --output /path/to/new/private/results
```

The default run also checks TX reset. `--skip-reset` runs the exchange,
save/restore, reinsert and 128 MiB stages without that separate reset check;
its final report explicitly marks the omission. Both modes require new,
complete VFS probe results and check that input sessions and images stay unchanged.

This optional test needs prc-tools-remix, PilRC and the Palm OS 5 SDK, supplied
separately. Its results contain firmware, applications and Palm user data; do not
publish the sessions or RAM export ZIPs. It can take several minutes to run.
Rebuild the probe when its source changes: acceptance requires its invocation
begin/completion markers, mounted capacity and explicit I/O results. Each stage
must append a fresh complete invocation; a successful launch or an unchanged
older log cannot establish a pass. The 128 MiB replacement must also report
that the old card's host and guest files are absent before creating its own file.
The probe uses a bounded idle event wait to provide periodic host API boundaries.
This fixture workaround does not establish support for indefinitely idle
third-party apps; see the [open bridge issue](tx-native.md#open-developer-workflow-issue-indefinitely-idle-applications).

Guest readiness is bounded and lets startup/mount work progress before a full
RAM export. Only explicit `not-ready` responses are retried; export errors and
completed probe errors still fail immediately. Export preflight advances the
live guest before checkpointing, and that progress remains even if preparation
or export fails. Injected export work rolls back to the prepared checkpoint.
On failure, the test attempts to save `<stage>-failed.frond` and a screen capture
beside the log, preserving the original failure. Model, format and result-checker
regressions run through `make test-sd` and `make test` without firmware or SDK
dependencies.

The controller changes follow Intel's
[PXA27x Developer's Manual](https://support.eurotech-inc.com/developers/datasheets/PXA270_DeveloperManual.pdf),
sections 15.9.10 and 15.9.17–18: the partial-buffer register is recognized and
PXA270 FIFO accesses transfer 1/2/4 bytes. PXA25x retains its existing byte-mode
behaviour. The card supplies SD status and acknowledges DAT3 pull-up control;
function switching is declined safely. DMA startup retains an already pending
peripheral request. The model does not implement a full SD electrical bus,
SDHC/SDXC, arbitrary high-speed modes or an SDIO Wi-Fi card.

## Computer keyboard

Check **Attach Palm infrared keyboard** in the desktop launcher to connect the
host keyboard as a Palm Universal Wireless Keyboard (3169WW). Install its Palm
OS driver separately, reset the Palm if installation requests it, then open
**Keyboard** on the Palm and turn its status **ON**. The 1.13 driver is available
from [PalmDB](https://palmdb.net/app/palm-wireless-keyboard-driver-palm-os-ppc).
The driver is not included in Frond or its Debian package.

Type while the Palm display window has focus. Letters, numbers, punctuation,
Shift, Ctrl, arrows, Return, Tab and Backspace are sent as accessory scan codes.
The mapping uses physical US keyboard positions; choose the corresponding layout
in the Palm driver. Left Alt acts as the accessory's Fn key, and Right Alt as
AltGr. F1–F4 and Escape retain Frond's application-button and power mappings.
With the attachment off, arrows and Return retain five-way navigation.

This is a keyboard peripheral, not clipboard insertion: the Palm driver handles
characters, modifiers and repeat. The driver decides which Palm events to emit;
Palm apps may handle external keyboards differently. Palm SunVox accepts `Z` and
`X` as C and D note input for the selected module; the octave follows the app's
settings. The standard Palm driver delivers character key-down events and handles
accessory releases internally, so this does not add a separate musical note-off
API to apps. Input is ignored until the
driver enables the infrared port. Attachment state and in-flight packets persist
with the session. Host focus loss, detachment and session startup release held
keys so modifiers do not remain stuck after switching windows or resuming.

The model sends press/release packets to the PXA270 STUART at 9600 baud, 8N2,
with guest-time byte spacing. It implements the keyboard's UART byte protocol,
not optical pulses, IrLAP beaming or Bluetooth. The packet format and scan codes
are documented in the author's [p1keyboard protocol notes](https://github.com/arpruss/p1keyboard/blob/master/p1notes.txt).
UART receive occupancy, interrupt masking, FIFO reset and character-time receive
timeout follow chapter 10 of the
[Intel PXA27x manual](https://support.eurotech-inc.com/developers/datasheets/PXA270_DeveloperManual.pdf).

`make test-keyboard` checks packet framing, timing, overflow/release handling,
partial-packet restore, UART reception and SDL focus handling. An optional SDK
probe checks the real Palm driver:

```sh
python3 tools/build-palm-probe.py --probe keyboard --sdk /path/to/sdk-5r3
python3 tests/desktop-keyboard.py --session /path/to/calibrated-tx.session \
  --driver /path/to/Keyboard_v113.prc --output /path/to/new/private/results
```

The acceptance test expects the driver's standard 320-pixel-wide settings form.
It installs the driver, resets, enables it, types lower/upper-case letters and
checks the exported Palm log. It also saves with Shift held and checks that
resume releases it. Firmware, the driver PRC and output sessions stay outside Git.

## Virtual Wi-Fi (experimental)

1. Select **Network: Virtual Wi-Fi (Frond)** in the desktop launcher.
2. Turn Wi-Fi on inside Palm OS. A network app can also prompt to turn it on.
3. Open the Wi-Fi network picker, scan and connect to **Frond**.

Frond presents one open access point, on channel 6, to the TX's existing Marvell
CF8381 driver. The driver loads its firmware from the user's Palm image and
uses its normal commands, interrupts and packet transfers. The Palm keeps its
own NetLib, TCP/IP stack and Wi-Fi configuration. This supports native ARM and
68k applications without substituting a NetLib reference. Blazer can fetch HTTP
pages through the simulated access point. Modern HTTPS sites can still fail
because of the Palm application's TLS version, certificates or browser features.

The access point exists inside the emulator. No radio traffic is transmitted,
and no host Wi-Fi adapter, TAP interface, bridge configuration or root access is
needed. The host may use wired or wireless Internet. A user-mode libslirp router
provides IPv4 NAT, DHCP and DNS on `10.0.2.0/24`. The router/host alias is
`10.0.2.2`, DNS is `10.0.2.3`, and the first DHCP lease is `10.0.2.15`.
Use `10.0.2.2` to reach a service on the computer; `127.0.0.1` refers to the Palm.
There are no incoming host-port forwards or host-directory shares.

The mode, virtual radio state, in-flight device buffers and Palm's saved network
profile persist with the session. Host Internet connections are closed during
normal restore, reset or mode changes. An app may need to reconnect or reload;
a saved browser page can still be available from the Palm's cache. Database
export freezes external I/O and preserves the live router during its temporary
guest checkpoint. Wi-Fi can keep system databases such as `Shim Logs` open.
If a full RAM export fails because one is open, **File → Export Database…** can
export a named application database, such as `AciduliteLog`, into its own ZIP.
It still reports errors for that database; a full export never silently skips
failed databases. Close Palm modal dialogs before installing or exporting if
the operation cannot reach a suitable guest API boundary.

The model implements the firmware transport and command subset used for this
open access point. It does not emulate the radio's processor instruction by
instruction, RF propagation, arbitrary access points, WEP/WPA authentication,
Bluetooth networking or IPv6. Packet queues are bounded. Host socket polling is
nonblocking and runs at the existing 30 Hz event pump. Wi-Fi is disabled by
default; use the other profiles when running deterministic network fixtures.

`make test-wifi` checks the firmware download handshake, IRQ acknowledgement
independent of response data, hardware identity, scan results, in-flight restore,
reset/disable, partial Ethernet TX/RX FIFO restore, packet boundaries and repeated
disassociation/reassociation without Palm firmware. Router tests check DHCP/ARP,
96 binary UDP/DNS wire exchanges, three TCP connections with 128 exact binary
roundtrips each, IPv4/TCP checksums, FIN/EOF, refused connections and a fresh
router after restore. The controlled DNS server uses the host alias on an
ephemeral loopback port; it does not test the host-configured `10.0.2.3` DNS
proxy. The register protocol follows the Linux Libertas
[CompactFlash driver](https://github.com/torvalds/linux/blob/v2.6.35/drivers/net/wireless/libertas/if_cs.c)
and [command structures](https://github.com/torvalds/linux/blob/v2.6.35/drivers/net/wireless/libertas/host.h).
The board wiring is documented by Linux's
[Palm TX PCMCIA driver](https://github.com/torvalds/linux/blob/v2.6.35/drivers/pcmcia/pxa2xx_palmtx.c).
Firmware is supplied separately and is not packaged with Frond.

The optional SDK test uses a session with a saved **Frond** Wi-Fi profile:

```sh
python3 tools/build-palm-probe.py --probe host-network --sdk /path/to/sdk-5r3
python3 tests/desktop-host-network.py --wireless --session /path/to/wifi.frond \
  --output /path/to/new/private/results --audio
```

For a sustained tone, add `--audio --tone-seconds 180 --require-gapless`.
`--wait-control` replaces the blocked HTTP receive with an immediate response
and an ordinary Palm task delay, keeping the same continuous ARM tone. The probe temporarily extends the Palm
inactivity countdown without changing its default preference. Run the
controls separately, with no other benchmark or build from the same session.
The recording ends at the configured hold boundary, before the reply is
released and before later network checks and exports. The strict PCM check
rejects gaps of 10 ms or more between first and last signal. Its metrics are
written even if a later networking check fails. A long idle connection can
currently lose incoming traffic; sustained audio and network reconnection are
separate checks.
With the optional [pipeline diagnostics](tx-native.md#optional-pipeline-diagnostics)
build, `--trace-audio` also saves separate SDL PCM for each emulator process and
records codec/HAL statistics. This helps distinguish guest silence from host
playback starvation. Diagnostic binaries are separate from normal builds.

It exercises the real Palm NetLib through the virtual radio, not the synthetic
NetLib reference. Results and PCM describe the emulator, not physical TX timing.
The test uses single-database export for `FrondHostLog` so open system logs do
not prevent retrieval. Its local server withholds a second response until after
export, checking that the live TCP connection survives the temporary checkpoint.

## Internet via host (experimental)

Choose **Network: Internet via host** before launching a Palm network app. Frond
forwards supported 68k NetLib calls to Linux IPv4 sockets and the host DNS
resolver. The computer supplies its normal routing, firewall and Internet
connection. No TAP device, bridge setup, root access or Palm Wi-Fi configuration
is needed. `127.0.0.1` refers to the host computer. Networking is disabled by
default and switching mode closes existing connections.

This supports outgoing TCP, UDP (including bind and peer addresses), DNS,
select, peek, shutdown, local/remote address queries and common socket options:
nonblocking, send/receive timeouts, reuse-address, keepalive, broadcast,
send/receive buffer sizes and TCP_NODELAY. There are sixteen sockets. TCP sends
can return a partial count; applications must handle it. Blocking operations
poll nonblocking host sockets and yield the calling Palm task for one tick
between attempts. DNS runs on a bounded worker queue, away from the emulation
thread. Other guest tasks, including ARM sound callbacks, can run during a wait.

The selected mode is saved with the session. Live host connections and pending
DNS requests are not saved: resume retains closed Palm socket references so
applications receive an error and can close/reopen them. Reset closes connections
and keeps the mode. Database export preserves live host descriptors while
freezing external I/O during its temporary guest checkpoint; it does not consume
network data on behalf of the export. Finish an app's network operation before
exporting its log, so its output reflects normal execution.

This is a NetLib compatibility layer, not a Wi-Fi radio or a complete Palm
network stack. Native ARM NetLib dispatch, apps that obtain library entry
pointers, listen/accept, PB scatter/gather calls, raw sockets and IPv6 are not
implemented. The synthetic reference is not a real Palm library-table entry.
Unsupported calls return errors; full conventions for every unsupported API are
not implemented. Apps that cached the real NetLib reference must be restarted.
In this NetLib forwarding mode, Blazer's Wi-Fi connection workflow is not
supported; use **Virtual Wi-Fi** for its normal radio connection. A successful
HTTP probe does not establish compatibility with every Palm app. Frond does not upgrade an application's old TLS implementation or
provide HTTPS-to-HTTP conversion.

Run all firmware-free networking checks independently of the emulator binary:

```sh
heavy build make -j1 test-wifi test-host-network test-network test-network-reliability
```

`heavy` is the shared-machine slot wrapper when available; otherwise run `make`
directly. These targets build the small common library and test executables,
without Palm firmware, an SDK or external services. `make test` includes them.

`make test-host-network` covers basic backend calls. `make test-network-reliability`
checks three 512 KiB TCP uploads and byte-exact replies with send backpressure,
partial I/O, half-close/EOF and refused-connect recovery on reused socket slots.
It also checks 96 UDP datagrams (including empty and 8 KiB datagrams), truncated
peek without consumption, source addresses, select/receive timeouts and recovery.
A separate executable wraps libc's resolver at link time: the real asynchronous
worker performs 64 fresh lookups and controlled negative/transient responses,
polling, timeout/reset cancellation and bounded-queue recovery. It never calls
an external resolver. TCP receive retains the connected peer address when Linux
has forgotten it after both FINs but still has unread bytes.

`make test-network` checks the packed Palm ABI, sixteen synthetic queue
saturation/partial-read snapshot cycles, same-mode preservation, library open
ownership and repeated failure-profile recovery. Nested export checkpoints
preserve a live UDP socket and a datagram arriving during the freeze; normal
restore retains a closed Palm handle. The virtual radio also accepts a restored
partial transmit FIFO before its first poll, recreating the router on demand.

These are controlled localhost/model regressions. They establish neither general
Internet/app compatibility, the system DNS proxy's behavior, long idle connection
reliability, nor sustained firmware-backed traffic or audio performance.

The optional Palm probe checks DNS, HTTP, UDP, select, peek, nonblocking reads and
timeouts, with a native ARM tone running during a delayed HTTP response. It also
checks that a live connection survives database export, closes on session resume
and can be reopened:

```sh
python3 tools/build-palm-probe.py --probe host-network --sdk /path/to/sdk-5r3 \
  --output build/host-network-probe
python3 tests/desktop-host-network.py --session /path/to/tx.session \
  --output /path/to/new/private/results --audio
```

The optional audio check requires PulseAudio tools and captures only the test
process's stream. It records PCM gaps; `--require-gapless` additionally makes
gaps of 10 ms or longer fail the test. `--wait-control` compares the delayed
network read with an immediate reply followed by a normal Palm task delay.
`--public` additionally fetches `http://example.com/`; all other
network checks use local servers. Measurements describe emulator behavior,
not physical TX timing. The SDK, ARM/m68k tools and firmware are supplied
separately. Keep exported RAM, sessions and test results private.

## Simulated network (experimental)

The desktop **Network** selector enables a deterministic 68k NetLib fixture.
It is disabled by default. **Simulated echo server** resolves `frond.test` (or
`10.0.0.2`) to `10.0.0.2` and accepts stream connections to TCP port 7. Bytes sent
are queued for the same socket to receive. It uses no host sockets or Internet
connection, and does not emulate a Wi-Fi radio.

The initial API subset is `SysLibFind("Net.lib")`, NetLib Open/Close/OpenCount,
Sleep/Wake, GetHostByName, SocketOpen/Connect/Close, Send/Receive and Select.
It supports IPv4 streams, protocol 0 or TCP, flags 0, and the single-buffer forms
without peer-address outputs. There are sixteen sockets, each with a 4096-byte
queue. Select polls readiness. An empty receive returns WouldBlock for timeout 0
or Timeout otherwise; it does not suspend the Palm task for the requested time.
**Simulated connection timeout** injects a Timeout response at connect;
**Simulated disconnection** makes Open return NoInterfaces. Unknown DNS names
return DNSNonexistantName. These profiles test application error handling, not
physical network latency.

Choose a mode before launching the Palm app. Apps already using a cached real
NetLib reference must be restarted to use the fixture.

Mode, open count, sockets and pending bytes are saved with the running session.
Reset closes the connections while keeping the selected mode. Changing mode
closes existing simulated connections; selecting the current mode preserves them.
Older sessions start disabled. Injected RAM database export work restores network
state to the prepared checkpoint along with the rest of the guest. Saved simulated
connections have no external resources to reconnect.

This is a development fixture, not general Palm network compatibility. Socket
options, UDP, HTTP service, listen/accept, PB scatter/gather APIs, native ARM
NetLib dispatch and apps that obtain library entry pointers are not supported.
The synthetic reference is not a real Palm library-table entry. Unsupported calls
using it are intercepted rather than sent into that table; their full return
conventions are not implemented. Use the supported subset when testing. Disabling
the fixture restores normal guest network behaviour; it does not add a working
radio connection.

Build and run the optional SDK probe:

```sh
python3 tools/build-palm-probe.py --probe network \
  --sdk ../palm-os-sdk/sdk-5r3
python3 tests/desktop-network.py --session /path/to/tx.session \
  --output /path/to/new/private/results
```

The probe writes `FrondNetLog`, leaves an echo reply pending, then receives it
when its button is tapped. The acceptance test saves and resumes before the tap,
checks the exported log, and exercises both error profiles. All measurements and
results are emulator observations. `make test-network` runs the firmware-free
ABI/model regressions.

## Remaining peripherals

The TX also uses Bluetooth accessories and phone/modem links, infrared beaming,
USB/HotSync through its Multi-connector, SD/MMC/SDIO cards, and stereo headphones.
See the [Palm TX user guide](https://palmdr.com/eBay%20Photos/tx_ug.pdf). Frond
currently supplies virtual SD storage, audio and the infrared keyboard attachment.
General infrared beaming, Bluetooth hardware emulation, USB/HotSync, host serial
forwarding and arbitrary SDIO accessories remain unsupported. Serial and
HotSync need end-to-end Palm transfer tests; register models alone do not
establish a working external connection.
