# Palm IIIx network over serial PPP

The IIIx reaches the network the way the real one did, with PPP over its serial
port to a host running pppd. Frond doesn't redirect NetLib for it.

## Host

Start the machine with its serial port on a PTY:

```sh
out/frond-68k iiix.frond --serial pty     # prints: serial: pty /dev/pts/N connected
```

Then run pppd on that PTY:

```sh
sudo tools/palm-pppd.sh /dev/pts/N
```

pppd needs root, or the `dip` group and `/dev/ppp` (the `ppp_generic` module).
The guest gets 10.68.0.2, the host is 10.68.0.1 and the DNS server is the host's
first one from `/etc/resolv.conf`. For the guest to reach past the host, turn on
IP forwarding and NAT, for example:

```sh
sudo sysctl net.ipv4.ip_forward=1
sudo iptables -t nat -A POSTROUTING -s 10.68.0.2 -j MASQUERADE
```

The second argument is the speed, 57600 by default. It should match the guest's.

## Palm

1. In Prefs, Network, make a new service, leave the user name and password empty
   and set the phone to `00` (no number is dialled on a direct cable).
2. In Details, connection type PPP, Query DNS on, IP address automatic. On 3.5
   and later pick Cradle/Cable or Direct Serial under Connection; on 3.1 set the
   modem to Direct Connect if it's offered.
3. Tap Connect. The panel shows the connection come up, and the pppd log shows the
   addresses.

Programs that use NetLib, like a web clipping app or a telnet client, then work
until you tap Disconnect or the machine sleeps.
