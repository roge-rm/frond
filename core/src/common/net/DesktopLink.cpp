#include "DesktopLink.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "WifiRouter.h"
#include "../sync/ActiveSync.h"

namespace frondNet {

namespace {
constexpr size_t queueLimit = 64;
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void put16(uint8_t* p, uint32_t x) {
    p[0] = uint8_t(x >> 8);
    p[1] = uint8_t(x);
}
void put32(uint8_t* p, uint32_t x) {
    put16(p, x >> 16);
    put16(p + 2, x);
}
}  // namespace

std::vector<uint8_t> DesktopLink::DhcpFrame(const uint8_t* mac, uint8_t type, uint32_t xid, uint32_t requested, uint32_t serverId) {
    // Ethernet, IPv4 (20), UDP (8) and a 300-byte BOOTP message.
    std::vector<uint8_t> f(14 + 20 + 8 + 300);
    std::fill(f.begin(), f.begin() + 6, 0xff);
    std::copy(mac, mac + 6, f.begin() + 6);
    f[12] = 0x08;
    uint8_t* ip = &f[14];
    ip[0] = 0x45;
    put16(ip + 2, 20 + 8 + 300);
    ip[8] = 64;
    ip[9] = 17;
    put32(ip + 16, 0xffffffff);
    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2) sum += uint32_t(ip[i]) << 8 | ip[i + 1];
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    put16(ip + 10, ~sum & 0xffff);
    uint8_t* udp = ip + 20;
    put16(udp, 68);
    put16(udp + 2, 67);
    put16(udp + 4, 8 + 300);  // no UDP checksum
    uint8_t* d = udp + 8;
    d[0] = 1;  // BOOTREQUEST
    d[1] = 1;
    d[2] = 6;
    put32(d + 4, xid);
    put16(d + 10, 0x8000);  // broadcast answers: no address yet
    std::copy(mac, mac + 6, d + 28);
    put32(d + 236, 0x63825363);
    uint8_t* o = d + 240;
    *o++ = 53, *o++ = 1, *o++ = type;
    if (requested) *o++ = 50, *o++ = 4, put32(o, requested), o += 4;
    if (serverId) *o++ = 54, *o++ = 4, put32(o, serverId), o += 4;
    *o++ = 55, *o++ = 3, *o++ = 1, *o++ = 3, *o++ = 6;  // netmask, router, DNS
    *o = 255;
    return f;
}

DesktopLink::Answer DesktopLink::ParseDhcp(const uint8_t* f, size_t size, const uint8_t* mac, uint32_t xid) {
    Answer a;
    if (size < 14 + 20 + 8 + 240 || f[12] != 0x08 || f[13] != 0x00 || (f[14] >> 4) != 4) return a;
    const uint8_t* ip = f + 14;
    const size_t ihl = (ip[0] & 15) * 4u;
    if (ip[9] != 17 || size < 14 + ihl + 8 + 240) return a;
    const uint8_t* udp = ip + ihl;
    if ((udp[0] << 8 | udp[1]) != 67 || (udp[2] << 8 | udp[3]) != 68) return a;
    const uint8_t* d = udp + 8;
    const size_t length = size - 14 - ihl - 8;
    if (d[0] != 2 || get32(d + 4) != xid || get32(d + 236) != 0x63825363 || !std::equal(mac, mac + 6, d + 28))
        return a;
    uint8_t type = 0;
    for (size_t o = 240; o < length && d[o] != 255;) {
        if (d[o] == 0) {
            ++o;
            continue;
        }
        if (o + 2 > length || o + 2 + d[o + 1] > length) break;
        const uint8_t code = d[o], n = d[o + 1];
        if (code == 53 && n >= 1) type = d[o + 2];
        if (code == 54 && n >= 4) a.serverId = get32(d + o + 2);
        if (code == 1 && n >= 4) a.netmask = get32(d + o + 2);
        o += 2 + n;
    }
    a.type = type;
    a.yiaddr = get32(d + 16);
    return a;
}

DesktopLink::DesktopLink(const uint8_t* address, uint16_t hostPort,
                         std::shared_ptr<frondSync::ActiveSyncSession> activeSync)
    : hostConnectPort(hostPort), activeSync(std::move(activeSync)) {
    std::copy(address, address + 6, mac);
}
DesktopLink::~DesktopLink() = default;

void DesktopLink::Send(std::vector<uint8_t> frame) {
    if (outgoing.size() < queueLimit) outgoing.push_back(std::move(frame));
}

void DesktopLink::Input(const uint8_t* frame, size_t size, uint64_t now) {
    if (state == State::bound) {
        router->Input(frame, size, now);
        return;
    }
    const auto a = ParseDhcp(frame, size, mac, xid);
    if (state == State::discover && a.type == 2 && a.yiaddr) {
        offered = a.yiaddr;
        offerServer = a.serverId;
        state = State::request;
        retryAt = now + requestNs;
        Send(DhcpFrame(mac, 3, xid, offered, offerServer));
    } else if (state == State::request && a.type == 5 && a.yiaddr) {
        std::copy_n(frame + 6, 6, guestMac);
        Bind(a.yiaddr, a.serverId ? a.serverId : offerServer, a.netmask ? a.netmask : 0xffffff00);
    } else if (state == State::request && a.type == 6) {
        state = State::discover;
        retryAt = now;
    }
}

void DesktopLink::Bind(uint32_t desktop, uint32_t handheld, uint32_t mask) {
    address = desktop;
    server = handheld;
    netmask = mask;
    RouterLayout layout;
    layout.network = address & netmask;
    layout.netmask = netmask;
    layout.host = address;
    layout.dhcpStart = server;
    // slirp's DNS proxy on the first free address of the subnet from .3 up.
    uint32_t dns = layout.network | 3;
    while (dns == address || dns == server) ++dns;
    layout.dns = dns;
    layout.dhcp = false;
    router = std::make_unique<WifiRouter>(layout, activeSync);
    state = State::bound;
    // Another Frond on this host may hold the port; the link works without it, the desktop just can't ask.
    if (!hostConnectPort || activeSync) return;
    const int error = router->AddUdpForward(hostConnectPort, server, connectPort);
    forwarding = error == 0;
    if (error == EADDRINUSE)
        fprintf(stderr, "USB network: UDP port %u on 127.0.0.1 is taken, the desktop can't ask for a connection\n",
                unsigned(hostConnectPort));
    else if (error)
        fprintf(stderr, "USB network: can't forward UDP port %u on 127.0.0.1 (%s), the desktop can't ask for a connection\n",
                unsigned(hostConnectPort), strerror(error));
}

int DesktopLink::AddTcpForward(uint16_t hostPort, uint16_t guestPort) {
    if (!hostPort || !guestPort) return EINVAL;
    return router ? router->AddTcpForward(hostPort, server, guestPort) : ENOTCONN;
}

int DesktopLink::AddUnixTcpForward(const std::string& path, uint16_t guestPort) {
    return router ? router->AddUnixTcpForward(path, server, guestPort) : ENOTCONN;
}

void DesktopLink::ActiveSyncWakeup() {
    // The WM5 RAPI service waits for this byte before opening its control socket.
    std::vector<uint8_t> frame(14 + 20 + 8 + 1);
    std::copy_n(guestMac, 6, frame.begin());
    std::copy_n(mac, 6, frame.begin() + 6);
    put16(frame.data() + 12, 0x0800);
    auto* ip = frame.data() + 14;
    ip[0] = 0x45;
    put16(ip + 2, 29);
    ip[8] = 64;
    ip[9] = 17;
    put32(ip + 12, address);
    put32(ip + 16, server);
    uint32_t sum = 0;
    for (size_t offset = 0; offset < 20; offset += 2) sum += uint32_t(ip[offset]) << 8 | ip[offset + 1];
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    put16(ip + 10, ~sum & 0xffff);
    auto* udp = ip + 20;
    put16(udp, connectPort);
    put16(udp + 2, connectPort);
    put16(udp + 4, 9);
    udp[8] = 0x7f;
    Send(std::move(frame));
}

void DesktopLink::Poll(uint64_t now) {
    if (state == State::bound) {
        if (activeSync && activeSync->Running() && !router->ActiveSyncControlConnected() && now >= knockAt) {
            ActiveSyncWakeup();
            knockAt = now + 1000000000;
        }
        router->Poll(now);
        return;
    }
    if (now < retryAt) return;
    ++xid;
    state = State::discover;
    retryAt = now + discoverNs;
    Send(DhcpFrame(mac, 1, xid, 0, 0));
}

bool DesktopLink::Receive(std::vector<uint8_t>& frame) {
    if (!outgoing.empty()) {
        frame = std::move(outgoing.front());
        outgoing.pop_front();
        return true;
    }
    if (!router || !router->Receive(frame)) return false;
    // slirp's frames go out from the desktop's MAC, and its ARP answers name it.
    std::copy(mac, mac + 6, frame.begin() + 6);
    if (frame.size() >= 42 && frame[12] == 0x08 && frame[13] == 0x06) std::copy(mac, mac + 6, frame.begin() + 22);
    return true;
}

}  // namespace frondNet
