#!/bin/sh
# Connects a frond-68k serial port to a host PPP endpoint so the guest's Network panel can dial
# out over it.
# Usage: tools/palm-pppd.sh [--pppd] [--log DIR] /dev/pts/N|tcp:PORT [speed]
# By default it runs slirp (Debian's slirp package): no root, the guest gets 10.0.2.15, slirp's
# DNS is 10.0.2.3 and slirp makes the guest's connections from the host. --log DIR keeps slirp's
# debug log (DIR/slirp_debug) and its stats there. --pppd runs pppd instead, which needs root
# (or the dip group and /dev/ppp) and IP forwarding with NAT on the host. tcp:PORT needs socat,
# slirp needs a C compiler for tools/slirp-select-fix.c.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
mode=slirp
log=
while [ $# -gt 0 ]; do
    case $1 in
        --pppd) mode=pppd; shift ;;
        --log) log=${2:?--log needs a folder}; shift 2 ;;
        *) break ;;
    esac
done
endpoint=${1:?usage: palm-pppd.sh [--pppd] [--log DIR] /dev/pts/N|tcp:PORT [speed]}
speed=${2:-57600}
work=$(mktemp -d "${TMPDIR:-/tmp}/palm-ppp.XXXXXX")
bridge=
cleanup() {
    [ -n "$bridge" ] && kill "$bridge" 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT INT TERM

# A tcp:PORT serial port becomes a local PTY for slirp or pppd.
case $endpoint in
    tcp:*)
        socat "TCP:127.0.0.1:${endpoint#tcp:}" "PTY,link=$work/tty,raw,echo=0" &
        bridge=$!
        n=0
        while [ ! -e "$work/tty" ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
        tty=$work/tty
        ;;
    *) tty=$endpoint ;;
esac

if [ "$mode" = slirp ]; then
    dir=${log:-$work}
    mkdir -p "$dir"
    cd "$dir"
    # slirp reads ~/.slirprc; an empty HOME keeps a user's own settings out.
    mkdir -p "$work/home"
    # Debian's 64-bit slirp exits on its first idle select(); the shim fixes the timeout.
    cc -shared -fPIC -O2 -o "$work/select-fix.so" "$here/slirp-select-fix.c" -ldl
    set -- -P "baudrate $speed" nozeros "log stats"
    [ -n "$log" ] && set -- "$@" "debug 7" -dppp
    HOME=$work/home LD_PRELOAD=$work/select-fix.so slirp "$@" <>"$tty" >&0
else
    dns=$(awk '/^nameserver/ { print $2; exit }' /etc/resolv.conf)
    case $dns in 127.*|"") dns=1.1.1.1 ;; esac
    PATH=$PATH:/usr/sbin
    pppd "$tty" "$speed" nodetach noauth local nocrtscts passive persist \
        10.68.0.1:10.68.0.2 ms-dns "$dns" proxyarp lcp-echo-interval 0 debug
fi
