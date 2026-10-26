# TX serial support

Frond models PXA27x UART FIFO transfers and transmit timing. Palm OS can use the
TX's RS-232 driver, but Frond does not yet provide a host serial connection.
UART DMA, host flow control and external connection handling across reset and
session restore need more work. USB and HotSync are not supported.

UART sessions preserve pending guest bytes and frame timing; older session formats
remain readable. A normal session restore can replay previously transmitted bytes
to an existing callback. Temporary database-export checkpoints suppress UART
callbacks while speculative guest work is rolled back.

## Tests

These firmware-free checks exercise FIFO widths, byte-mode compatibility, timing,
session restore and export checkpoints:

```sh
make test-tx-serial test-uart-lifecycle test-uart-checkpoint
```

For a Palm OS driver check, build the included probe with your own Palm OS 5 SDK
and use a private, setup-complete TX session:

```sh
python3 tools/build-palm-probe.py --probe serial --sdk /path/to/sdk-5r3
python3 tools/probe-tx-serial.py --session /private/tx-ready.session \
  --output /private/results/serial-01
```

The probe checks Serial Manager calls before and after restoring a session. Its
results do not establish delivery to a host peer or a working HotSync connection.
The output contains a session and Palm data; keep it private.

An optional UART observer can verify bytes reaching the emulator's FFUART callback.
See `tools/debug/tx-serial-trace.patch` and `tools/probe-tx-serial.py --help`.
