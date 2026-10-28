# TX UART development

The cradle serial port (FFUART) can be connected to a host PTY or a TCP listener
on 127.0.0.1. It is off by default. For USB HotSync see
[peripherals](peripherals.md#usb-hotsync).

## Host endpoint

```sh
out/frond session.frond --serial pty        # prints: serial: pty /dev/pts/N connected
out/frond session.frond --serial tcp:5023   # prints: serial: tcp 127.0.0.1:5023 waiting
```

The desktop has the same choice under **Serial port**, kept in its settings, and
shows the PTY path or port and whether a peer is connected. The desktop control
commands are `serial-state` and `serial-mode off|pty|tcp:PORT`; once serial is in use
the core also sends a `serial` event with the same text whenever it changes, such
as on a connect or hangup. A failed switch leaves serial off and is not saved. `tcp:0` picks a
free port. If the endpoint cannot open, the session still starts with serial off
and `serial-state` reports why.

- The PTY is raw (no echo or line editing). A new PTY counts as connected until
  the first program that opened it closes it; after that it counts as connected
  while some program has it open.
- TCP binds 127.0.0.1 only and takes one client at a time. A second client is
  closed at once.
- A connected peer asserts CTS, DSR and DCD. EOF, a TCP reset or the last PTY
  close drops all three, even while the guest holds RTS low, and bytes the guest
  sends with no peer are dropped. Bytes already queued stay for the guest; bytes
  the host sent that are still unread in the kernel when it hangs up are lost,
  and a PTY's leftovers are flushed so the next program doesn't see them.
- The peer honours RTS: no byte reaches the receiver while RTS is low, including
  automatic RTS from MCR[AFE]. Host bytes then wait in a 4096-byte queue, and the
  worker stops reading the host when that is full, so TCP or the PTY hold the rest.
- Guest bytes wait in another 4096-byte queue. CTS drops while it is at least half
  full and rises again at a quarter. Without MCR[AFE] the guest can overrun it and
  the excess is dropped.
- DTR and RTS are not passed to the host, and there is no break or parity on the
  wire.
- While the guest has the UART off (IER[UUE] clear), host bytes are dropped, as on
  a wire with no receiver. A close and reopen between two emulation slices drops
  the bytes that were already waiting before it.

A worker thread owns the descriptors and the queues' host side. It never touches
the UART; the emulator thread moves bytes between the queues and the UART and sets
the modem inputs between emulation slices. Each received byte still takes one
frame at the programmed baud on emulated time.

Nothing from the host is session state. Reset, session load and a mode change
detach the UART and drop both queues; a refused load leaves it attached; the host connection stays open (except on a
mode change) and the UART is attached again once its TX and RX are idle. A
restored session starts detached and reattaches the same way. Saved MSR bits are
recomputed from the live peer after a load. Input replays do not record serial
traffic.

With a live host the run is not deterministic: when a host byte arrives relative
to emulated time depends on host scheduling and the emulator's speed, and modem
input changes land between emulation slices (a timeslice, about 1/60 s). Byte
order, pacing within a burst and flow control do not depend on it.

## Model behavior

PXA27x FIFO PIO transfers 1/2/4 little-endian bytes when FCR[BUS] is set. TX and RX
follow DLL/DLH and LCR with fractional time carried between frames: a byte from the
peer lands in the receiver at once, then the line takes no other byte for one frame.
A full receive FIFO keeps its 64 bytes and sets LSR[OE], which clears when LSR is read.
A FIFO read restarts the four-character receive timeout. UUE clear or a zero
divisor pauses transmission and callbacks. FIFO reset drops queued bytes while
the current frame finishes. Callbacks exchange bytes; wire-level break/parity
encoding is unsupported.

Modem inputs use asserted logical levels through `pxaUartSetModemInputs`. FFUART
supports CTS/DSR/RI/DCD and DTR/RTS; BTUART supports only CTS/RTS; STUART has no
modem pins. In FIFO mode, MCR[AFE] gates new frames on CTS; a frame
already started finishes after CTS drops. With AFE and RTS set, automatic RTS
follows the receive FIFO threshold. MSR delta bits acknowledge on read, including
RI's trailing edge. PXA27x loopback isolates external callbacks and physical
outputs, loops data/modem controls internally, and bypasses OUT2 for interrupts.

See the [PXA27x Developer's Manual](https://docs.toradex.com/100211-colibri-arm-som-pxa270-technical-reference-manual.pdf),
sections 5.4.6, 5.5.14 and 10.4–10.6. Older UARTs retain byte-mode behavior. UART
state is version 3 (RX frame time, trailing-byte and EOC flags) and reads versions
0-2; DMA state remains version 0.

## Ownership and restore

`pxaUartSetFuncs` is for internal accessories whose bindings survive restore;
it must not hide live host resources. `pxaUartAttachExternal` requires idle TX/RX,
a detach notification and explicit modem readiness, and accepts TX FFUART only.
`HostSerial` (uarm/HostSerial.cpp) is its only user and owns the host endpoint.

Reset and nonempty load attempts detach external bindings once, including a
rejected load. Live bindings and physical inputs are not serialized; saved pins
cannot resurrect a peer. Busy guest TX/RX prevents reattachment. Empty/no-op loads
preserve the binding. Detach notifications cannot rebind during notification.

Temporary database exports preserve peers: `ReadOnlyGuest` freezes all UART
callbacks until rollback completes, including error paths. Binding/pin changes
are rejected while frozen and must be deferred by their owner. Nested serial
guards are supported. These APIs run on the emulator thread; callers must detach
before destroying the UART or callback data.

## DMA limits

Aligned direct-buffer memory copies support documented WIDTH=00 and exact short
tails. Legacy width encodings remain accepted. Tail splitting is confined to
RAM/ROM buffers; MMIO accesses are never split. Unsupported partial peripheral
beats, invalid peripheral widths and wrapping/unmapped accesses stop with BUSERR
and its interrupt rather than aborting or repeating a failed burst. Completed
partial transfers remain completed.

Cyclic memory-copy descriptor chains yield after at most 2048 bursts per channel
in a scheduler callback, then continue on a later dispatch. This keeps other
channels and guest lifecycle operations reachable; it is not a wall-time latency
guarantee. `make test-lifecycle` exercises rings across sleep, reset and accepted
or rejected loads, alongside local socket and UART peer ownership checks.

PXA27x UARTs support DMA with IER[DMAE] (sections 5.4.6 and 10.4.2.3-10.4.2.6). The
TX request is raised while the TX FIFO is under half full, or empty with FCR[TIL]; a
short tail is written in byte lanes. The RX request is raised at the trigger level.
With FCR[TRAIL], the character timeout requests the trailing bytes, the DMA reads only
the valid bytes, and an empty FIFO signals EOR (DCSR[EORINT], EORIRQEN, EORSTOPEN and
EORJMPEN in descriptor mode). A receive chain that ends with bytes left sets IIR[EOC]
and the timeout interrupt so the driver drains by PIO. Any error byte in the FIFO stops
RX requests and raises the error interrupt without RLSE until PIO reads it out. PXA25x
UART DMA remains fatal. Arbitrary unaligned/DALGN transfers are not established.

## Tests and private probes

```sh
heavy build make -j1 test-uart-flow test-uart-lifecycle test-uart-dma-peer test-tx-serial test-host-serial test-sd
heavy build make -j2 out/frond
heavy build python3 tools/build-palm-probe.py --probe serial --sdk /path/to/sdk-5r3
python3 tools/probe-tx-serial.py --session /private/tx-ready.session \
  --output /private/results/serial
```

Use `make` directly if the shared-machine `heavy` wrapper is unavailable. Firmware-
free tests use synthetic memory and unnamed socketpairs to check DMA guards,
FIFO-safe rejection, flow control, interrupts, restore ownership and checkpoint
side effects; `make test` includes them.

`test-host-serial` runs the worker against a model UART: both endpoints both ways,
frame pacing, one TCP client, RTS and CTS backpressure without overrun or drops,
EOF and PTY hangup, a closed port, and detach on reset, load, checkpoint, mode
change and stop.

The host probe sends a 70-byte request from a Palm app through the Serial Manager
with RTS/CTS flow control at 9600, 57600 and 115200 baud. A host peer checks it and
answers with 64 bytes and a 2048-byte burst while the app waits a second, so the
reply only arrives intact if RTS holds the host back:

```sh
heavy build python3 tools/build-palm-probe.py --probe serial-host --sdk /path/to/sdk-5r3
python3 tools/probe-tx-serial-host.py --session /private/tx-ready.session \
  --endpoint pty --output /private/results/serial-pty    # or --endpoint tcp
```

The probe needs a setup-complete, user-supplied TX session. It uses private copies,
fresh token-marked logs and a restored second phase. The discovery probe checks RS232 driver API
calls and metadata and opens no endpoint. The runner uses `heavy emulator` automatically when present.

For byte/baud observation, apply `tools/debug/tx-serial-trace.patch` to generated
`build/source`, build and copy the observer binary, then reverse the patch and
rebuild the normal binary. Run the probe with `FROND_SERIAL_TRACE=1`,
`--core /path/to/observer` and `--require-baud-trace`. Output is bounded and not
session state. Keep firmware, sessions, captured data and detailed results outside
Git. This observes guest-driver output into the model. UART DMA through the Serial
Manager and serial HotSync remain unvalidated.
