# TX UART development

Host serial ports and USB/HotSync are unsupported. The core opens no serial
device, PTY or serial socket listener.

## Model behavior

PXA27x FIFO PIO transfers 1/2/4 little-endian bytes when FCR[BUS] is set. TX follows
DLL/DLH and LCR with fractional time carried between frames. UUE clear or a zero
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
state remains version 2 and reads versions 0/1; DMA state remains version 0.

## Ownership and restore

`pxaUartSetFuncs` is for internal accessories whose bindings survive restore;
it must not hide live host resources. Model-only `pxaUartAttachExternal` requires
idle TX/RX, a detach notification and explicit modem readiness. It accepts TX
FFUART only and creates no operating-system endpoint.

Reset and nonempty load attempts detach external bindings once, including a
rejected load. Live bindings and physical inputs are not serialized; saved pins
cannot resurrect a peer. Busy guest TX/RX prevents reattachment. Empty/no-op loads
preserve the binding. Detach notifications cannot rebind during notification.

Temporary database exports preserve peers: `ReadOnlyGuest` freezes all UART
callbacks until rollback completes, including error paths. Binding/pin changes
are rejected while frozen and must be deferred by their owner. Nested serial
guards are supported. These APIs run on the emulator thread; callers must detach
before destroying the UART or callback data. Desktop stop/worker integration,
bounded queues, EOF handling, disabled-RX buffering, receive pacing and transparent
reconnection remain unsupported. Model tests alone do not justify a host endpoint.

## DMA limits

Aligned direct-buffer memory copies support documented WIDTH=00 and exact short
tails. Legacy width encodings remain accepted. Tail splitting is confined to
RAM/ROM buffers; MMIO accesses are never split. Unsupported partial peripheral
beats, invalid peripheral widths and wrapping/unmapped accesses stop with BUSERR
and its interrupt rather than aborting or repeating a failed burst. Completed
partial transfers remain completed.

UART DMA remains unsupported and enabling IER[DMAE] remains fatal. Independent
TX/RX requests, per-word valid-byte counts, EOR/EOC and error draining are missing.
Memory-copy fixes do not implement those interfaces. Arbitrary unaligned/DALGN,
peripheral trailing-byte and descriptor EOR behavior are not established.

## Tests and private probes

```sh
heavy build make -j1 test-uart-flow test-uart-lifecycle test-uart-dma-peer test-tx-serial test-sd
heavy build make -j2 out/frond
heavy build python3 tools/build-palm-probe.py --probe serial --sdk /path/to/sdk-5r3
python3 tools/probe-tx-serial.py --session /private/tx-ready.session \
  --output /private/results/serial
```

Use `make` directly if the shared-machine `heavy` wrapper is unavailable. Firmware-
free tests use synthetic memory and unnamed socketpairs to check DMA guards,
FIFO-safe rejection, flow control, interrupts, restore ownership and checkpoint
side effects; `make test` includes them.

The probe needs a setup-complete, user-supplied TX session. It uses private copies,
fresh token-marked logs and a restored second phase. It checks RS232 driver API
calls and metadata; completed sends do not establish host delivery. It opens no
USB or host endpoint. The runner uses `heavy emulator` automatically when present.

For byte/baud observation, apply `tools/debug/tx-serial-trace.patch` to generated
`build/source`, build and copy the observer binary, then reverse the patch and
rebuild the normal binary. Run the probe with `FROND_SERIAL_TRACE=1`,
`--core /path/to/observer` and `--require-baud-trace`. Output is bounded and not
session state. Keep firmware, sessions, captured data and detailed results outside
Git. This observes guest-driver output into the model; UART DMA, host receive,
endpoint lifecycle and HotSync remain unvalidated.
