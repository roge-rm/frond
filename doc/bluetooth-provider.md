# Optional Bluetooth controller provider

Bluetooth is an experimental, opt-in native endpoint on the TX, E2, Zire 72 and TG50. The original Palm OS stack remains unchanged. For the TX, E2 and Zire 72, Frond models the UART radio upload handshake and the TX driver's UART sleep/wake messaging, then forwards ordinary H4 to a local virtual controller. The TG50 transport is described below. Original OPP send/receive, SPP/RFCOMM with credit advancement, L2CAP data, PIN/security APIs and lifecycle recovery are qualified on the TX and E2; the Zire 72's coverage is below. The default configuration is off.

The qualified provider source is BlueZ commit `82667d713949447d7c94725432c61b33689ae68a` plus [the address-programming extension](../tools/bluez-bdaddr.patch). Stock `btvirt` does not implement the Broadcom Write_BD_ADDR command required by the TX driver. The extension actually changes the virtual controller's address before Read_BD_ADDR and discovery; it does not fabricate a readback. It also corrects upstream inquiry filtering to use the inquiry scan bit, so a connectable-only radio is not advertised as discoverable. Controller reset or removal reports link loss to remaining peers, and simultaneous peer closure leaves the provider running. This six-byte command is also implemented by [Linux's Broadcom driver](https://github.com/torvalds/linux/blob/1c8c5a9d38f607c0b6fd12c91cbe1a4418762a21/drivers/bluetooth/btbcm.c). E2 uses its provider-assigned address. The TX transport handles its sleep-mode configuration/readback and UART wake/sleep acknowledgments locally, holding incoming controller data until wake acknowledgment. The extra activation notification is part of the virtual-controller contract; its physical radio byte values were not captured. Other unimplemented vendor commands report Unknown Command.

Run these commands from a Frond source checkout. Build explicitly when needed, with Git, a C toolchain, Make, Autoconf, Automake, Libtool, pkg-config and the development headers required by BlueZ's configure checks (including GLib, D-Bus and readline):

```sh
heavy build python3 tools/build-bluetooth-provider.py --fetch
install -d -m 700 build/bluetooth-sockets
build/bluetooth-provider/emulator/btvirt -s"$PWD/build/bluetooth-sockets"
```

Run the optional wire contract check with `heavy build python3 tests/bluetooth-provider.py build/bluetooth-provider/emulator/btvirt`. This starts and stops its own private provider and verifies address programming, scan modes, peer-loss notifications, reset/reconnection, simultaneous controller closure, split headers, maximum ACL framing and byte-exact backpressure.

The provider retains unsent stream bytes in a bounded 1 MiB output queue per controller. Overflow closes that controller and reports link loss to its peers; it never silently discards a packet suffix. The wire check covers a 512 KiB transfer while the receiving host pauses reads.

The helper only fetches with `--fetch`, verifies the pinned revision, applies the extension and builds `emulator/btvirt`. It never installs software, changes a service or accesses a host Bluetooth adapter. To rebuild an existing checkout, omit `--fetch`; use `--source PATH` for an existing checkout of the exact revision. No provider build or download runs as part of the normal Frond build.

In another terminal, select its BR/EDR socket:

```sh
out/frond owned-session.frond --bluetooth "$PWD/build/bluetooth-sockets/bt-server-bredr"
```

Turn Bluetooth off and on in Palm OS after attaching to an already-running radio. On the receiving device, select Discoverable Yes in Bluetooth settings; after restoring cached preferences, toggle No then Yes to apply visibility to the new controller. Each connected Frond receives a virtual controller; only controllers in this provider instance can communicate. The server socket should remain private to the owning user. Stop `btvirt` with Ctrl-C when finished.

TX programs the factory address supplied by its original image. Two clones of the same TX image therefore share an identity and cannot be treated as distinct devices in one virtual network. Use independently owned images with different original addresses for two TX devices. The provider assigns a distinct locally administered address to each new controller connection, using a random process base and a monotonic counter so disconnected slots do not reuse identities. An HCI reset preserves the current controller's address. E2 uses this address; reconnecting it creates a new identity and old trusted-device entries do not identify the new radio. Display names and cached devices in Palm's chooser do not establish a live connection.

The Zire 72 uses the TX's driver and transport. Its image has no factory address, so the first time Bluetooth goes on it makes up one (01:02:03 and three random bytes), programs it like the TX and keeps it in the session. Copies of a session saved after that share the address and can't pair with each other; set up Bluetooth in each session separately. Between two Zires discovery, OPP send and receive, PIN entry, authentication, encryption and Trusted Devices pass.

Reset, accepted session replacement and true guest sleep close the external peer and discard its queued radio bytes. Turn Bluetooth off and on again in Palm OS to start a fresh controller. A read-only checkpoint or refused load preserves the existing peer without sending speculative commands. Saving a session does not save the host socket or provide portable continuation of a live Bluetooth link.

Qualification uses independent owned peers and exact 4096-byte payload checks,
original send/receive completion records, an actual mid-payload Cancel followed
by a fresh retry, PIN entry and trusted-device records reporting the persistent flag. The virtual
controller reports authentication/encryption state; this is not a physical
cryptography or radio-timing qualification. Other profiles, physical adapters
and host Bluetooth-stack interoperability are not qualified.

Both boards pass active read-only checkpoint/refused-load rollback with exact
model, RAM and NAND state, followed by a complete transfer. Idle accepted load,
reset/reinitialization, true sleep/wake and fresh sleeping-session restore pass
with new transfers. Loading during a transfer closes its old peer immediately
and never counts that partial operation as completed. The original Palm progress
dialog can remain on Canceling after losing its controller; use Reset, then
turn Bluetooth off and on before a fresh operation. Live external connections
cannot continue portably across saved sessions.

Build the optional original Exchange Manager probe with
`python3 tools/build-palm-probe.py --probe bluetooth`. Validate exported logs and
the independent payload with `tools/check-bluetooth-probe.py`, passing `--before`
to require a fresh attempt and unchanged prior history. The
[qualification runner](peripheral-qualification.md) automates readiness, replay,
exports, evidence checks and process cleanup for private device plans.

## Sony CLIÉ TG50

TG50 uses its original CSR BCSP driver with the same pinned virtual provider.
The controller keeps its real manufacturer, version, address and ACL buffer
contract. Original device discovery, Memo OPP sending and standard PIN entry
and Trusted Devices work against an independent virtual peer. Original Exchange
Manager receives an exact 4096-byte object into a registered Palm application
on native desktop. SPP, other profiles and portable continuation of a live link
remain unqualified. Android payload transfers remain unqualified.
No physical radio or cryptography claim follows from these virtual tests.

Bluetooth starts off in each TG50 desktop or Android session. Select the
endpoint in Device controls, then turn Bluetooth off and on in Sony Preferences.
Stop and a fresh session start turn the host endpoint off. Bad or disconnected
endpoints report an error. After reset, sleep or accepted session replacement,
restart the original radio before another transfer.

The provider advertises 192-byte ACL payloads and one shared transmit credit.
The bridge preserves real completions and splits received ACL data into at most
412-byte payloads, plus the four-byte ACL header. Original host receive credits
are mapped back to whole provider packets. HCI events are never split. SCO,
unsupported packet flags, oversized guest transmissions and unknown CSR vendor
operations report errors. No firmware or PS reply is invented.

For background receiving, enable Allow Wake Up and Allow Device to be
Discovered in Sony Preferences. An interrupted 64-byte object was not delivered
to the Palm application; a full retry after rearming these settings passed.
Automatic receive recovery remains unqualified.

Internal radio power changes keep the same controller connection and address.
A real HCI Reset clears the old links before guest transport starts again.
Reset errors and controller EOF require selecting the endpoint again. Endpoint
changes, core reset and accepted session replacement close the old controller;
a new connection gets a new address. The original library can retain an earlier
address across these replacements; use a current discovery result.

A guest HCI Reset cancels unsubmitted receive data after the controller command
is queued. Submitted serial fragments keep their late acknowledgement tokens.
The genuine Reset completion reaches the guest with a full receive pool. After
that exact reliable frame is acknowledged, a successful flow or buffer command
starts a new receive epoch and drops the old tokens without returning credits.

The original TG50 driver can enable flow again without repeating Host Buffer
Size. The bridge keeps its last successfully declared capacity for this same
guest; a successful new declaration replaces it. External controller or session
replacement clears it. This compatibility follows the observed TG50 driver;
HCI cannot identify an old buffer acknowledgement newly queued after the host
has declared its new epoch on a reused handle. Original idle sleep/wake and
automatic incoming service recovery remain unqualified.

The optional virtual OPP test peer uses the provider's pinned BlueZ test host.
Build and check it with `heavy build make test-bluetooth-opp-peer`. Its separate
L2CAP configuration patch starts RFCOMM only after both configuration exchanges
finish. The controller provider stays unchanged. Each peer process owns one
connection; same-process reconnection and general L2CAP renegotiation are not
qualified.

## Bluetooth over the network

Fronds on different machines share one btvirt: the host runs it, the others carry their controller connections to it over TCP. Each connection is still its own controller, so every Frond gets its own radio. Bytes pass unchanged and nothing is encrypted, so use it on a network you trust.

On Android, Bluetooth is on the corner menu's Device section. Host other Fronds runs btvirt in the app and takes peers on a port (5032 by default; the dialog shows this device's addresses). Join another Frond takes HOST:PORT of a Frond that hosts. Two Android Fronds work with one hosting and the other joining.

On the desktop, `tools/link-bluetooth.py` does both:

```sh
# Host: run btvirt as above and take network peers on port 5032.
python3 tools/link-bluetooth.py serve "$PWD/build/bluetooth-sockets/bt-server-bredr" listen:5032

# Join an Android or desktop host, then start Frond on the socket it makes.
python3 tools/link-bluetooth.py join 192.168.1.20:5032 "$PWD/build/bt-join"
out/frond owned-session.frond --bluetooth "$PWD/build/bt-join"
```

An Android Frond joins a desktop host the same way, with the desktop's address and serve's port. Turn Bluetooth off and on in Palm OS after changing any of this, as with a local btvirt. Use two different identities (an E2 and a TX, or TX images with different addresses), as described above.
