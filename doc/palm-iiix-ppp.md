# Palm IIIx network over serial PPP

The IIIx reaches the network the way the real one did, with PPP over its serial
port to a PPP server on the host. Frond doesn't redirect NetLib for it.

## Host with slirp (no root)

Install Debian's `slirp` package (and `socat` for a TCP serial port). Start the
machine with its serial port on a PTY or a TCP port:

```sh
out/frond-68k iiix.frond --serial pty     # prints: serial: pty /dev/pts/N connected
out/frond-68k iiix.frond --serial tcp:5000
```

Then run slirp on it:

```sh
tools/palm-pppd.sh /dev/pts/N
tools/palm-pppd.sh tcp:5000
```

slirp gives the guest 10.0.2.15 and DNS through 10.0.2.3, and makes the guest's
connections from the host as an ordinary user, so nothing else needs setting up.
Add `--log DIR` to keep slirp's debug log and its packet and connection counts in
`DIR/slirp_debug`. The script needs a C compiler: Debian's 64-bit slirp exits on
its first idle wait, and `tools/slirp-select-fix.c` is preloaded to fix that.

The second argument is the speed, 57600 by default.

## Host with pppd (root)

```sh
sudo tools/palm-pppd.sh --pppd /dev/pts/N
```

pppd needs root, or the `dip` group and `/dev/ppp` (the `ppp_generic` module).
The guest gets 10.68.0.2, the host is 10.68.0.1 and the DNS server is the host's
first one from `/etc/resolv.conf`. For the guest to reach past the host, turn on
IP forwarding and NAT, for example:

```sh
sudo sysctl net.ipv4.ip_forward=1
sudo iptables -t nat -A POSTROUTING -s 10.68.0.2 -j MASQUERADE
```

## Palm

On 3.5 and later:

1. In Prefs, Network, pick a service (or make a new one) and set Connection to
   Direct Serial. Leave the user name empty.
2. In Details, connection type PPP, Query DNS on, IP address automatic, and an
   empty login script.
3. Tap Connect. The panel shows the connection come up and the button turns into
   Disconnect.

On 3.1 set the modem to Direct Connect if it's offered and the phone to `00`.

Programs that use NetLib, like a web clipping app or a telnet client, then work
until you tap Disconnect or the machine sleeps. They also dial by themselves when
the service isn't connected yet.
