# TX UART transfers and wired serial investigation

`0039-tx-serial.patch` fixes PXA27x UART FIFO transfers in 32-bit bus mode.
PIO accesses now transfer 1/2/4 little-endian bytes rather than dropping the upper
bytes. Byte bus mode, divisor accesses and older UART paths retain their existing
behavior. No saved fields or chunk versions change.

Wired host serial and USB/HotSync remain unsupported. Step 9 also supplies
reproducible capability probes. No PTY, host serial device, socket listener,
USB passthrough or HotSync service is opened by this change.

## UART timing and export lifecycle (patch 0042)

`0042-uart-lifecycle.patch` adds baud-paced PXA27x transmission using the guest's
DLL/DLH divisor and LCR framing. At the existing 102400 Hz peripheral tick, it
carries fractional character time between queued frames; rounding each character
separately would accumulate drift. Five-bit frames with 1.5 stop bits and fast
short frames are included. Earlier UART byte-transfer behavior remains covered.
This is guest-clock timing, not electrical timing or host wall-clock accuracy.
Callbacks remain byte-oriented; this adds no wire-bit or parity encoder.

Clearing IER[UUE] suppresses UART interrupts and all byte/tick/activity callbacks,
retaining pending guest data. A zero divisor stops the PXA27x baud clock, as
specified by section 10.4.7 of the Intel manual. Pending transmission resumes
when the unit and clock are re-enabled. The callback source owns unread data;
a physical host transport would still need a policy for traffic arriving while
the receiver is disabled. This does not implement host-side flow control.
FIFO reset and mode changes clear queued data while allowing the current PXA27x
shift frame to finish, including its remaining time (table 10-12).

The UART chunk is now version 2, adding only the remaining transmit-frame time.
Version-0 and version-1 chunks remain readable; their first pending frame starts
with a full character time because those formats contain no partial-frame clock.
New version-2 chunks require this core or newer. Existing input sessions are not
rewritten by the probe runner. Keep copies before normal session upgrades.

`ReadOnlyGuest` now acquires `SoC::SerialCheckpoint` before saving guest state.
The guard freezes every PXA UART read/write/tick/activity callback, including polls
triggered by register writes or scheduler queries. It stays active until RAM,
flash, UART state, dirty flags and the framebuffer have been restored. Its
nesting count is host-only and is not serialized. Constructor failure releases
the guard; the outermost release reschedules pending serial work. Nested serial
guards are supported; `ReadOnlyGuest`'s existing injected-call recovery scope
still rejects nesting of that entire export object.

```sh
heavy build make -j1 test-uart-lifecycle test-uart-checkpoint test-tx-serial test-sd
```

The lifecycle test checks exact character boundaries over 64-byte streams,
framing variants, maximum divisor, fractional carry, zero-clock/unit pauses,
FIFO resets, pending RX/IRQs and partial-frame checkpoint restoration. The
production export-guard test uses synthetic TX memory and an in-memory callback
peer. Successful and exception exits, nested serial guards and save-constructor
failure must produce no speculative external I/O. Original pending bytes resume
once. It also checks rejected restore and reset. No firmware or SDK is required.

The fresh normal and updated observer builds completed initial and restored
runs on the same private TX session with its input hash unchanged. The Palm
Serial Manager selected divisor 96 and LCR `07` (9600 8N2); the eight output
bytes completed at UART ticks `118, 235, 352, 470, 587, 704, 822, 939` in both
phases. The strict checker verified the actual framing rather than assuming
8N1. This is an eight-byte emulator driver observation, not host transport,
physical TX timing, sustained throughput or receive-path acceptance.

An ordinary session restore intentionally restores guest FIFO contents. The test
also reproduces its remaining external-peer hazard: if a byte was already sent
after the snapshot, restoring that snapshot sends it again to a retained
callback. Reset clears guest pending data but retains callback bindings.
Internal accessory models need these bindings; a future host attachment must
identify and disconnect external peers on reset/normal restore, then require
explicit reconnection. This change does not add attachment ownership, close
notifications or host descriptors, and therefore does not make a live endpoint
safe to enable.

DMA is still unsupported and enabling IER[DMAE] remains fatal. UART has no DMA
controller/request wiring. The DMA engine also rejects a three-byte remainder
with a four-byte beat; hardware UART DMA needs a valid-byte/EOR contract for
trailing bytes, independent TX/RX requests, thresholds, error handling and state
restore. `test-sd` now reproduces that partial-beat rejection in an isolated
child process. Acknowledging DMAE without this machinery would falsely claim
successful support. No such acknowledgement or partial UART DMA implementation
is introduced.

## Hardware and code evidence

The staged TX core creates FFUART at `0x40100000`, BTUART at `0x40200000` and
STUART at `0x40700000`. In `src/uarm/uarm/soc_PXA.cpp`, only STUART is connected
to a TX accessory: the infrared keyboard. TX board setup returns before the E2
Bluetooth/debug-UART wiring in `devicePalmTungstenE2.cpp`. There is no TX wired
serial attachment, modem-control peer or host device selection. The default UART
read callback returns no character; its write callback discards output.

Linux's [TX board configuration](https://github.com/torvalds/linux/blob/v4.0/arch/arm/mach-pxa/palmtx.c)
assigns FFUART RX/TX to GPIO34/39 and treats USB separately, with cable detection
on GPIO13 and pullup control on GPIO93. This makes FFUART a candidate for further
investigation. It does not establish which Palm Serial Manager port alias uses
it, which accessory detection Palm requires, or that HotSync can run over it.
The locally supplied Palm OS 5 SDK's `SerialMgr.h` distinguishes autodetected
cradle, cradle RS232, cradle USB, infrared and console aliases. An alias's driver
metadata or successful `SrmSend` only establishes an OS API observation.

The TX uses `pxa270_UDC.cpp`, not the PXA255 controller. It preserves control and
endpoint configuration registers in session chunks. Endpoint FIFO accesses call
`ERR` and terminate emulation. There is no USB host transaction/packet interface,
EP0 setup/enumeration, bulk endpoint transport or cable lifecycle. A Unix socket
carrying raw UART bytes cannot substitute for those USB operations.

## Reproducible model regressions

```sh
heavy build make -j1 test-tx-serial
```

When the shared-machine `heavy` wrapper is unavailable, run `make` directly.
This target uses synthetic data, the actual UART/UDC implementations and the
small common library; it needs no Palm firmware, SDK or emulator process.
It also runs negative tests for the optional guest probe's result checker.
`make test` includes it.

`tests/tx-serial-model.cpp` establishes:

- FFUART callback TX/RX handles NUL, high bytes and ordering through the guest
  register interface, including receive occupancy and interrupts.
- UART RX/TX and UDC endpoint configuration survive a chunk roundtrip. Sending
  a pending byte, restoring the chunk and continuing sends that byte again to
  the external callback. A host peer cannot be rolled back with guest memory.
- PXA27x TX is now baud-paced by patch 0042. The previous five-characters-in-five-
  ticks observation has been replaced by exact cadence and fractional-carry tests.
- Packed PXA27x FIFO TX/RX transfers move exactly 1/2/4 requested bytes,
  including partial trailing transfers, occupancy and interrupt clearing. Error
  flags do not leak into adjacent data lanes. Over-reading an empty FIFO chooses
  zero for unspecified lanes and does not create phantom bytes. This follows
  Intel's [PXA27x manual](https://docs.toradex.com/100211-colibri-arm-som-pxa270-technical-reference-manual.pdf),
  sections 10.4.5 and 10.5.6. BUS and FIFO enable must both be set. Divisor
  accesses, byte mode, earlier FFUART and the separate PXA25x HWUART remain
  unchanged. Version-0 and version-1 chunks preserve partial RX and queued TX.
- With UUE cleared, patch 0042 no longer consumes callback RX or polls the client.
  FIFO capacity, overrun and host-side buffering/flow control still need a complete
  transport contract.
- UART DMA enable, UDC EP0 FIFO read/write and EP1 FIFO write reach fatal
  unimplemented paths. Each is isolated in a child process; the test replaces
  `uarmAbort` with a distinctive exit status and produces no crash dumps.

The target exits successfully when these observations match the current model.
Its `BLOCKED` lines are missing capabilities, not passing transport tests. If a
core implementation fixes one, update the probe and its documented conclusion.
The test uses direct peripheral ticks and register writes, not Palm serial
drivers, USB enumeration or a HotSync protocol implementation.

## Optional Palm firmware probe

Use a private, setup-complete TX session. The tools and SDK are supplied
separately; the PRC is built from source in this repository.

```sh
heavy build make -j2 out/frond
heavy build python3 tools/build-palm-probe.py --probe serial --sdk /path/to/sdk-5r3
python3 tools/probe-tx-serial.py --session /private/tx-ready.session \
  --output /private/results/serial-01
```

The runner uses `heavy emulator` automatically when available. Do not wrap it
in a second emulator slot. It uses SDL dummy drivers, disables sound and runs
only its own child emulator. Its per-phase timeout includes shared-slot waiting.
The result directory must be new and is created with mode 0700. It contains
firmware, RAM, applications and user data; keep it outside Git.

The Palm probe reports the Serial Manager feature version, device count and
metadata for up to sixteen devices by index. After a successful RS232 open it
also queries metadata using the returned handle. The
[Palm Serial Manager reference](https://users.fuw.edu.pl/~michalj/palmos/NewSerialManager.html)
specifies indices, open handles or creator IDs for `SrmGetDeviceInfo`;
`SrmOpen` aliases are not valid metadata keys. It attempts only
`SrmOpen(serPortCradleRS232Port, 9600)`. If that succeeds, it queues eight fixed
binary bytes, polls `SrmSendCheck` for at most one guest second, flushes and
closes. It never calls `SrmSendWait`, opens USB, changes cable detection or
presses HotSync. Individual OS calls still depend on guest driver behavior;
the outer wall timeout bounds a hung/fatal guest. Errors are recorded as
observations rather than converted into success.

Each invocation deletes its prior log and writes a random host-supplied token
in begin/completion records. The checker requires that token, complete ordered
results, valid database identity and record boundaries. The second phase restores
the first phase's saved working session and launches again with a new token.
It does not reinstall the application. The source session hash must remain
unchanged. The probe uses a bounded idle event wait to cooperate with export;
it does not resolve the general blocked-task injection limitation.

`serial.json` uses `status: observed` for a completed investigation. This is
never a claim of delivery: `serial_delivery_validated` and `hotsync_validated`
remain false, including when Palm accepts all eight bytes and reports no pending
data. That can occur while the UART's default write callback discards output.
A stale/incomplete log, fatal driver path or runner failure is an error.

### Firmware diagnosis before patch 0039 (2026-10-26)

The corrected version-2 probe completed initial and restored-session runs on the
normal core and on the optional observer build, using a private setup-complete
TX firmware session. Both runs produced fresh tokens; the source session hash
was unchanged. Palm reported Serial Manager version 2 and seven devices.
The opened RS232 alias resolved to `ser0` (capability flags `0x13`, advertised
maximum baud 921600). Open, eight-byte send, send-check, flush and close all
returned zero error, with no pending bytes.

The observer identified FFUART at `0x40100000`. Palm wrote the test packet as
32-bit THR values `72537246` and `ff807f00` with `FCR=e1` and `IER=55`.
The default output callback received only `46 00`, in both phases. Thus the guest
route exists, but six of eight bytes are already lost before a host backend
could receive them. The standalone packed-word regression reproduces that loss
without firmware. Neither a completed API call nor a drained guest queue proves
byte delivery. Bidirectional serial and HotSync remain unvalidated.

### Validation after patch 0039

The normal build and a fresh observer build each completed the same initial and
restored-session probe phases. The observer recorded the same two packed THR
writes, followed by all eight bytes `46 72 53 72 00 7f 80 ff` on FFUART in each
phase. Palm's metadata and send/close results were unchanged, and the original
input session hash remained unchanged. This validates outgoing delivery through
the real Palm driver into the UART callback. It does not validate a host peer,
guest receive traffic or HotSync.

Patch 0039 changed only PIO bus transfers. Patch 0042 later adds TX timing,
disabled-unit gating and temporary-export callback freezing, as described above.
DMA, modem control and external connection ownership remain unimplemented. No guest bytes already
lost in an older running or saved session can be recovered by this correction.

## Optional UART output observer

To identify the hardware route without attaching a host peer, build the optional
`tools/debug/tx-serial-trace.patch`. It logs at most 128 THR register writes and
128 characters from the otherwise-discarded default UART output callback per
process. UART addresses
identify the port; `char` includes byte/error bits. `SERIAL_THR` records access
width, the full write value and the current FCR/IER before lane decoding. It does
not alter callback ownership, queues or savestates. Treat these logs as private serial data.

Starting from an unmodified prepared tree:

```sh
git apply --directory=build/source tools/debug/tx-serial-trace.patch
heavy build make -j2 out/frond
cp out/frond out/frond-serial-trace
git apply -R --directory=build/source tools/debug/tx-serial-trace.patch
heavy build make -j2 out/frond
FROND_SERIAL_TRACE=1 python3 tools/probe-tx-serial.py \
  --core out/frond-serial-trace --require-baud-trace \
  --session /private/tx-ready.session \
  --output /private/results/serial-trace-01
```

`--require-ffuart-trace` requires exactly that packet on FFUART in both phases
and records it under `uart_tx_trace`; missing, extra or wrong-UART output fails.
`--require-baud-trace` additionally checks every completion against the divisor,
framing and UART tick at which the shift frame started, using `UART_TX_START`
and `UART_TX_CLOCK` lines. It includes the exact byte check. Observer clocks
count active, unfrozen UART ticks; they are diagnostics, not saved device state.
The host delivery and HotSync flags still remain false.

Compare `SERIAL_THR` and `SERIAL_TX` lines in each phase's private `core.log`
with the exact test packet `46 72 53 72 00 7f 80 ff`. Before patch 0039,
only `46 00` emerged; successful API queueing hid six missing bytes. The fixed
model must emit all eight bytes in order in both phases. An observer callback
receiving output establishes an outgoing guest-driver route into the emulator. It does not
establish receive delivery, a working host transport or HotSync. This optional
patch is separate from production core patch 0039.

## Host endpoint decision and remaining work

An opt-in Unix-domain stream socket in a newly created mode-0700 directory is a
feasible eventual endpoint for developer byte-stream tools. Use one explicitly
selected local peer, bounded queues, nonblocking partial I/O and close-on-exec
file descriptors. A PTY could serve software that requires a terminal device,
but needs raw-mode, slave reopen/hangup and termios/modem-control semantics;
changing host termios is not evidence the guest UART has changed baud rate.
Neither endpoint requires host-device passthrough or a TCP listener.

The desktop already owns a private `QTemporaryDir`/`QLocalServer` control channel
in `DesktopClient.cpp` and owns only its child process. That channel carries
bounded request/reply commands; binary serial data needs its own transport and
lifecycle. The outgoing guest-to-FFUART route is now demonstrated, but host peer I/O,
receive delivery and external connection lifecycle are still unvalidated.

Before adding a backend, establish bidirectional byte delivery with fresh binary
data through a real guest driver and host peer. Validate guest enable/baud/FIFO behavior, CTS/RTS and required DMA, queue
limits and scheduling. Reset, normal session restore, failed restore, stop and
peer reconnect must close or deliberately detach live connections. Temporary
export checkpoints must freeze both input consumption and output delivery and
preserve the live connection; guest rollback must not replay externally sent
bytes. Host descriptors and socket paths must never be serialized or silently
reopened from a session. Older sessions must load without enabling attachment.

USB additionally needs a real controller packet/IRQ/DMA and enumeration model.
HotSync additionally needs an actual desktop peer and protocol transfer tests
(including failure/retry and a saved session). The current probes validate none
of those end-to-end requirements. Keep PRC/PDB installation, named database
export and virtual SD exchange as the working file-transfer tools meanwhile.
