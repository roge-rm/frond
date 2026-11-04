#!/bin/sh
# Runs pppd on a frond-68k serial PTY so the guest's Network panel can dial out over it.
# Usage: tools/palm-pppd.sh /dev/pts/N [speed]
# pppd needs root (or the dip group and /dev/ppp), and IP forwarding with NAT on the host
# for the guest to reach past it.
set -eu
pty=${1:?usage: palm-pppd.sh /dev/pts/N [speed]}
speed=${2:-57600}
dns=$(awk '/^nameserver/ { print $2; exit }' /etc/resolv.conf)
case $dns in 127.*|"") dns=1.1.1.1 ;; esac
PATH=$PATH:/usr/sbin
exec pppd "$pty" "$speed" nodetach noauth local nocrtscts passive persist \
    10.68.0.1:10.68.0.2 ms-dns "$dns" proxyarp lcp-echo-interval 0 debug
