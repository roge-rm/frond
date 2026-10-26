#include <arpa/inet.h>
#include <sys/socket.h>
#include <cerrno>
#include <functional>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "WifiRouter.h"
using Bytes = std::vector<uint8_t>;
static const uint8_t mac[] = {2, 0x46, 0x72, 0x6f, 0x6e, 0x64};
static void put16(Bytes& b, size_t off, uint16_t value) {
    b[off] = value >> 8;
    b[off + 1] = value;
}
static void put32(Bytes& b, size_t off, uint32_t value) {
    put16(b, off, value >> 16);
    put16(b, off + 2, value);
}
static uint16_t get16(const Bytes& b, size_t off) { return (uint16_t(b[off]) << 8) | b[off + 1]; }
static Bytes udpPacket(const Bytes& payload, uint32_t src, uint32_t dst, uint16_t sport,
                       uint16_t dport) {
    Bytes packet(42 + payload.size(), 0);
    std::fill(packet.begin(), packet.begin() + 6, 0xff);
    std::copy(mac, mac + 6, packet.begin() + 6);
    put16(packet, 12, 0x800);
    packet[14] = 0x45;
    put16(packet, 16, packet.size() - 14);
    packet[22] = 64;
    packet[23] = 17;
    put32(packet, 26, src);
    put32(packet, 30, dst);
    uint32_t sum = 0;
    for (unsigned i = 14; i < 34; i += 2) sum += get16(packet, i);
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    put16(packet, 24, ~sum);
    put16(packet, 34, sport);
    put16(packet, 36, dport);
    put16(packet, 38, payload.size() + 8);
    std::copy(payload.begin(), payload.end(), packet.begin() + 42);
    return packet;
}
static Bytes dhcp(unsigned type) {
    Bytes b(240, 0);
    b[0] = 1;
    b[1] = 1;
    b[2] = 6;
    put32(b, 4, 0x12345678);
    put16(b, 10, 0x8000);
    std::copy(mac, mac + 6, b.begin() + 28);
    put32(b, 236, 0x63825363);
    b.insert(b.end(), {53, 1, uint8_t(type)});
    if (type == 3) b.insert(b.end(), {50, 4, 10, 0, 2, 15, 54, 4, 10, 0, 2, 2});
    b.push_back(255);
    return udpPacket(b, 0, 0xffffffff, 68, 67);
}
static Bytes wait(WifiRouter& router, uint64_t& now) {
    Bytes b;
    for (unsigned i = 0; i < 500; ++i) {
        now += 10000000;
        router.Poll(now);
        if (router.Receive(b)) return b;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!"router did not reply");
    return {};
}
static unsigned dhcpType(const Bytes& packet) {
    assert(packet.size() > 282 && get16(packet, 12) == 0x800 && get16(packet, 34) == 67 &&
           get16(packet, 36) == 68);
    assert(packet[58] == 10 && packet[59] == 0 && packet[60] == 2 && packet[61] == 15);
    for (size_t i = 282; i < packet.size();) {
        const auto tag = packet[i++];
        if (!tag) continue;
        if (tag == 255) break;
        assert(i < packet.size());
        auto length = packet[i++];
        assert(length <= packet.size() - i);
        if (tag == 53) {
            assert(length == 1);
            return packet[i];
        }
        i += length;
    }
    return 0;
}
// A small raw IPv4/TCP peer drives libslirp itself, rather than bypassing it
// through HostNetwork. Sequence numbers, bytes and checksums are checked in both
// directions; all host sockets bind loopback with ephemeral ports.
static uint32_t get32(const Bytes& b, size_t off) {
    return (uint32_t(get16(b, off)) << 16) | get16(b, off + 2);
}
static uint16_t checksum(const Bytes& b, size_t start, size_t length, uint32_t sum = 0) {
    for (size_t i = 0; i < length; i += 2)
        sum += (uint16_t(b[start + i]) << 8) | (i + 1 < length ? b[start + i + 1] : 0);
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return uint16_t(~sum);
}
static uint16_t tcpChecksum(const Bytes& b) {
    const size_t ip = (b[14] & 15) * 4, start = 14 + ip;
    const size_t length = get16(b, 16) - ip;
    const auto sum = get16(b, 26) + get16(b, 28) + get16(b, 30) + get16(b, 32) + 6 + length;
    return checksum(b, start, length, sum);
}
static Bytes tcpPacket(const Bytes& gateway, uint16_t sport, uint16_t dport,
                       uint32_t seq, uint32_t ack, uint8_t flags, const Bytes& data = {}) {
    Bytes b(54 + data.size(), 0);
    std::copy(gateway.begin(), gateway.end(), b.begin());
    std::copy(mac, mac + 6, b.begin() + 6);
    put16(b, 12, 0x800);
    b[14] = 0x45;
    put16(b, 16, b.size() - 14);
    b[22] = 64;
    b[23] = 6;
    put32(b, 26, 0x0a00020f);
    put32(b, 30, 0x0a000202);
    put16(b, 24, checksum(b, 14, 20));
    put16(b, 34, sport);
    put16(b, 36, dport);
    put32(b, 38, seq);
    put32(b, 42, ack);
    b[46] = 0x50;
    b[47] = flags;
    put16(b, 48, 65535);
    std::copy(data.begin(), data.end(), b.begin() + 54);
    put16(b, 50, tcpChecksum(b));
    return b;
}
static int localSocket(int type, uint16_t& port) {
    int fd = socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    socklen_t length = sizeof(a);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&a), &length) == 0);
    port = ntohs(a.sin_port);
    return fd;
}
static void until(WifiRouter& router, uint64_t& now, const std::function<bool()>& ready) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
        assert(std::chrono::steady_clock::now() < end);
        now += 1000000;
        router.Poll(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
static void tcp(WifiRouter& router, uint64_t& now, const Bytes& gateway, unsigned round,
                bool refused = false) {
    uint16_t port;
    int listener = localSocket(SOCK_STREAM, port);
    if (!refused) assert(listen(listener, 1) == 0);
    const uint16_t sport = 50000 + round;
    uint32_t seq = 0x10000000 + round * 0x100000, ack = 0;
    auto send = [&](uint8_t flags, const Bytes& bytes = {}) {
        auto b = tcpPacket(gateway, sport, port, seq, ack, flags, bytes);
        router.Input(b.data(), b.size(), now);
    };
    auto next = [&] {
        for (;;) {
            auto b = wait(router, now);
            if (get16(b, 12) != 0x800 || b[23] != 6) continue;
            const auto ip = (b[14] & 15) * 4;
            assert(b.size() >= size_t(14 + ip + 20));
            if (get16(b, 14 + ip + 2) != sport) continue;
            assert(ip == 20 && get16(b, 34) == port);
            assert(checksum(b, 14, ip) == 0 && tcpChecksum(b) == 0);
            return b;
        }
    };
    send(2);
    auto b = next();
    assert(get32(b, 42) == seq + 1);
    if (refused) {
        assert((b[47] & 0x14) == 0x14);
        close(listener);
        return;
    }
    assert((b[47] & 0x12) == 0x12);
    ack = get32(b, 38) + 1;
    ++seq;
    send(0x10);
    int fd = -1;
    until(router, now, [&] {
        fd = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        assert(fd >= 0 || errno == EAGAIN || errno == EWOULDBLOCK);
        return fd >= 0;
    });
    for (unsigned i = 0; i < 128; ++i) {
        Bytes expected(1023 + (i % 2));  // Includes odd TCP checksum lengths.
        for (size_t j = 0; j < expected.size(); ++j) expected[j] = (i + round) ^ (j * 37);
        send(0x18, expected);
        seq += expected.size();
        Bytes received(expected.size());
        size_t used = 0;
        until(router, now, [&] {
            auto n = recv(fd, received.data() + used, received.size() - used, 0);
            assert(n > 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)));
            if (n > 0) used += n;
            return used == received.size();
        });
        assert(received == expected);
        used = 0;
        until(router, now, [&] {
            auto n = ::send(fd, received.data() + used, received.size() - used, MSG_NOSIGNAL);
            assert(n > 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)));
            if (n > 0) used += n;
            return used == received.size();
        });
        received.clear();
        while (received.size() < expected.size()) {
            b = next();
            assert(!(b[47] & 7));
            const size_t header = 34 + (b[46] >> 4) * 4;
            const size_t end = 14 + get16(b, 16);
            assert(header <= end && end <= b.size());
            assert(get32(b, 42) == seq);
            if (header == end) continue;
            assert(get32(b, 38) == ack);
            received.insert(received.end(), b.begin() + header, b.begin() + end);
            ack += end - header;
            send(0x10);
        }
        assert(received == expected);
    }
    assert(shutdown(fd, SHUT_WR) == 0);
    do { b = next(); } while (!(b[47] & 1));
    assert(get32(b, 38) == ack && get32(b, 42) == seq);
    ++ack;
    send(0x11);
    ++seq;
    do { b = next(); } while (get32(b, 42) != seq);
    uint8_t byte;
    until(router, now, [&] {
        auto n = recv(fd, &byte, 1, 0);
        assert(n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)));
        return n == 0;
    });
    close(fd);
    close(listener);
}
// Repeated binary UDP and real DNS wire-format replies share the packet path.
// The resolver is a controlled loopback endpoint on an ephemeral port. This
// tests DNS over the router, not libslirp's host-configured 10.0.2.3 DNS proxy.
static void udpAndDns(WifiRouter& router, uint64_t& now, const Bytes& gateway) {
    uint16_t port;
    int fd = localSocket(SOCK_DGRAM, port);
    for (unsigned i = 0; i < 96; ++i) {
        const bool dns = i % 3 == 0;
        Bytes expected;
        if (dns) {
            expected = {uint8_t(i), 0x42, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
                        5, 'f', 'r', 'o', 'n', 'd', 4, 't', 'e', 's', 't', 0, 0, 1, 0, 1};
        } else {
            expected.resize(i % 2 ? 1400 : 1);
            for (size_t j = 0; j < expected.size(); ++j) expected[j] = i ^ (j * 13);
        }
        auto b = udpPacket(expected, 0x0a00020f, 0x0a000202, 49153, port);
        std::copy(gateway.begin(), gateway.end(), b.begin());
        router.Input(b.data(), b.size(), now);
        Bytes actual(1500);
        sockaddr_in from{};
        socklen_t length = sizeof(from);
        until(router, now, [&] {
            auto n = recvfrom(fd, actual.data(), actual.size(), 0,
                              reinterpret_cast<sockaddr*>(&from), &length);
            assert(n >= 0 || errno == EAGAIN || errno == EWOULDBLOCK);
            if (n < 0) return false;
            actual.resize(n);
            return true;
        });
        assert(actual == expected);
        if (dns) {
            expected[2] = 0x81;
            expected[3] = i % 2 ? 0x83 : 0x80;  // NXDOMAIN and a single A answer.
            if (!(i % 2)) {
                expected[7] = 1;
                expected.insert(expected.end(), {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 127, 0, 0, 1});
            }
        }
        assert(sendto(fd, expected.data(), expected.size(), 0,
                      reinterpret_cast<sockaddr*>(&from), length) == ssize_t(expected.size()));
        b = wait(router, now);
        assert(b.size() >= expected.size() + 42 && b[23] == 17);
        assert(get16(b, 16) == expected.size() + 28);
        assert(get16(b, 34) == port && get16(b, 36) == 49153);
        assert(get32(b, 26) == 0x0a000202 && get32(b, 30) == 0x0a00020f);
        assert(checksum(b, 14, 20) == 0);
        assert(std::equal(expected.begin(), expected.end(), b.begin() + 42));
    }
    close(fd);
}
int main() {
    WifiRouter router;
    assert(router.Valid());
    uint64_t now = 0;
    auto b = dhcp(1);
    router.Input(b.data(), b.size(), now);
    assert(dhcpType(wait(router, now)) == 2);
    b = dhcp(3);
    router.Input(b.data(), b.size(), now);
    assert(dhcpType(wait(router, now)) == 5);
    // Learn guest/host hardware addresses through ARP before sending unicast.
    Bytes arp(42, 0);
    std::fill(arp.begin(), arp.begin() + 6, 0xff);
    std::copy(mac, mac + 6, arp.begin() + 6);
    put16(arp, 12, 0x806);
    put16(arp, 14, 1);
    put16(arp, 16, 0x800);
    arp[18] = 6;
    arp[19] = 4;
    put16(arp, 20, 1);
    std::copy(mac, mac + 6, arp.begin() + 22);
    put32(arp, 28, 0x0a00020f);
    put32(arp, 38, 0x0a000202);
    router.Input(arp.data(), arp.size(), now);
    b = wait(router, now);
    assert(get16(b, 12) == 0x806 && get16(b, 20) == 2);
    const Bytes gatewayMac(b.begin() + 22, b.begin() + 28);
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    timeval timeout{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    std::thread echo([&] {
        char data[4];
        sockaddr_in from{};
        socklen_t len = sizeof(from);
        assert(recvfrom(fd, data, sizeof(data), 0, reinterpret_cast<sockaddr*>(&from), &len) == 4 &&
               !memcmp(data, "ping", 4));
        assert(sendto(fd, "pong", 4, 0, reinterpret_cast<sockaddr*>(&from), len) == 4);
    });
    b = udpPacket({'p', 'i', 'n', 'g'}, 0x0a00020f, 0x0a000202, 49152, ntohs(address.sin_port));
    // Unicast Ethernet destination is the router from the ARP reply.
    std::copy(gatewayMac.begin(), gatewayMac.end(), b.begin());
    router.Input(b.data(), b.size(), now);
    b = wait(router, now);
    assert(get16(b, 12) == 0x800 && get16(b, 36) == 49152 && b.size() >= 46 &&
           !memcmp(b.data() + 42, "pong", 4));
    echo.join();
    close(fd);
    // Bad guest frame lengths are rejected without reaching libslirp.
    router.Input(nullptr, 0, now);
    Bytes huge(2000);
    router.Input(huge.data(), huge.size(), now);
    udpAndDns(router, now, gatewayMac);
    tcp(router, now, gatewayMac, 0);
    tcp(router, now, gatewayMac, 1, true);
    tcp(router, now, gatewayMac, 2);
    // A fresh router models the host resources discarded on normal resume or
    // disassociation. It must relearn ARP and accept new connections.
    WifiRouter fresh;
    assert(fresh.Valid());
    fresh.Input(arp.data(), arp.size(), now);
    b = wait(fresh, now);
    assert(get16(b, 12) == 0x806);
    tcp(fresh, now, Bytes(b.begin() + 22, b.begin() + 28), 3);
    puts("PASS: libslirp DHCP/ARP, 96 UDP/DNS wire exchanges, 3 x 128 binary TCP roundtrips, checksums, FIN/EOF, refused-connect and fresh-router recovery; localhost only");
}
