# Infrared beaming endpoint

The optional native SIR endpoint connects the original TX or E2 IrDA stack to
another Frond instance. The TX and E2 desktop controls and native CLI support this endpoint. The default
is off. Select **Infrared beaming: TCP** and choose a different local port for
each instance, or use the native commands below.

Detach the infrared keyboard, then start two independently running sessions:

```sh
out/frond first.frond --irda tcp:5030
out/frond second.frond --irda tcp:5031
python3 tools/link-irda.py 5030 5031
```

Use Palm OS's normal Beam command. Both guests must be awake and the receiving
Palm must have Beam Receive enabled. Ports bind to the local machine. The relay
forwards bounded byte queues without changing the guest's IrLAP, IAS, TinyTP or
OBEX frames. It exits when either connection closes; restart it after restoring,
resetting or waking a guest, then start a fresh Beam operation. Each endpoint accepts one peer. `--irda pty` exposes
a local byte stream for other experiments; host IrDA stack compatibility has not
been qualified.

The keyboard and beaming endpoint share STUART and cannot be attached together.
The model uses the guest's UART clock, baud, character format, infrared direction
and board power controls. Queue sizes and service work are bounded. While the endpoint is configured, the native frontend services it in 1 ms
execution slices. It publishes UART-paced bytes at each service boundary, without
waiting for a complete frame. This reduces added transport delay during the original
stack's short discovery windows.

This is a decoded serial transport. Host worker scheduling and the two guests'
independent clocks are not an optical arrival-time model; baud mismatch rejection,
physical pulse timing, range and interference are not qualified. Receive-window
cuts discard bytes sampled outside the active window. Unsampled kernel-buffered
bytes cannot be assigned an exact physical arrival time.

A read-only checkpoint or a refused load preserves the live peer and queued
input. An accepted load, reset or true guest sleep closes the old peer and drops
its host queues. Host connections are not serialized into a `.frond` file. A new
process starts with the endpoint off unless explicitly configured again. A PTY
has stream semantics and cannot identify every rapid change of slave opener.

`make test-irda-endpoint` exercises production TX/E2 UART and host ownership
without firmware. `make test-irda-results test-irda-link` checks the relay and the
independent result checker. Private firmware qualification uses the original
Palm Exchange Manager with an SDK-built probe, not a replacement IrDA stack:

```sh
python3 tools/build-palm-probe.py --probe irda
python3 tools/check-irda-probe.py sender.zip receiver.zip
```

The checker requires original sender and receiver completion logs and an exact
independently generated 4096-byte binary payload. For a retry in the same guests,
pass `--sender-before` and `--receiver-before` exports to require unchanged prior
history plus a fresh successful invocation. Firmware, installed applications,
sessions and captures remain private.

The record-database probe exercises the original Palm `ExgDBWrite` and
`ExgDBRead` APIs, including the OS database importer:

```sh
python3 tools/build-palm-probe.py --probe irda-db
python3 tools/check-irda-probe.py --database sender.zip receiver.zip \
  --sender-before sender-before.zip --receiver-before receiver-before.zip
```

Install and launch **Frond database beam** in both private guest copies, export
the before archives, then press **Beam database** on the sender. Use fresh guest
copies for this probe: it creates `Frond5FRecord` once and refuses to replace an
existing database. Its two records contain an identifier and the binary pattern.
The checker requires successful serialization, import and disconnect on both
sides; exact records, IDs, attributes and other database metadata; and absence
of the test database in the before archives. Importing changes the database's
modification date, which is excluded from the metadata comparison.

Binary transfers pass in all four TX/E2 directions; record databases pass in
both cross-device directions. Qualification also covers cancellation during a
payload and a fresh retry, interrupted-peer recovery, exact checkpoint/refused-
load rollback, accepted-load reconnection, and true sleep/wake including a fresh
process restoring a sleeping session.
Firmware-free checks cover FIFO direction/pacing, power and receive-window cuts,
peer generations and bounded service. A PWM save-state channel-index bug found
during these checks is corrected so independent channels restore independently.
