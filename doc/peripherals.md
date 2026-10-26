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

### Validation

A locally built Palm OS 68k probe uses the real VFS API to enumerate the card,
read a host-prepared file, create/write/close/read a Palm file and append results
to `FrondSDLog`. The native acceptance test checks the exported FAT image and
Palm log, restores a mounted snapshot, restores an ejected snapshot, reinserts,
resets and reads again, then repeats creation/save/restore at 128 MiB. Imported
firmware/session/card inputs remain unchanged. These are emulator tests, not
physical SD timing or electrical validation.

```sh
python3 tools/build-palm-probe.py --sdk ../palm-os-sdk/sdk-5r3
python3 tests/desktop-sd.py --session /path/to/tx.session \
  --output /path/to/new/private/results
```

This optional test needs prc-tools-remix, PilRC and the Palm OS 5 SDK, supplied
separately. Its results contain firmware, applications and Palm user data; do not
publish the sessions or RAM export ZIPs. It can take several minutes to run. Boot/API readiness is retried after reset
instead of assuming a fixed boot duration. Model and format regressions run through `make test-sd` and
`make test` without firmware or SDK dependencies.

The controller changes follow Intel's
[PXA27x Developer's Manual](https://support.eurotech-inc.com/developers/datasheets/PXA270_DeveloperManual.pdf),
sections 15.9.10 and 15.9.17–18: the partial-buffer register is recognized and
PXA270 FIFO accesses transfer 1/2/4 bytes. PXA25x retains its existing byte-mode
behaviour. The card supplies SD status and acknowledges DAT3 pull-up control;
function switching is declined safely. DMA startup retains an already pending
peripheral request. The model does not implement a full SD electrical bus,
SDHC/SDXC, arbitrary high-speed modes or an SDIO Wi-Fi card.

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
Older sessions start disabled. RAM database exports restore the network state
along with the rest of the guest. Saved simulated connections have no external
resources to reconnect.

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

Host socket forwarding, Wi-Fi and Bluetooth hardware emulation, USB/HotSync and
host serial forwarding remain unsupported. Host networking needs guest-friendly
asynchronous waiting, bounded I/O, cancellation and explicit reconnect behaviour
after resume. Serial/HotSync also needs an end-to-end Palm transfer test; register
models alone do not establish a working external connection.
