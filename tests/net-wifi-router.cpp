// The shared Ethernet router in libcommon-net.a: DHCP, ARP, one TCP connect and one DNS-format
// UDP query to loopback listeners, and the Network HotSync wakeup answer. Loopback only.

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#include "net/WifiRouter.h"

using frondNet::WifiRouter;
using Bytes = std::vector<uint8_t>;

static int failures = 0;
#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            ++failures;                                                  \
        }                                                                \
    } while (0)
#define REQUIRE(x)                                                       \
    do {                                                                 \
        if (!(x)) {                                                      \
            fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #x); \
            exit(1);                                                     \
        }                                                                \
    } while (0)

static const uint8_t guestMac[6] = {0x02, 0x46, 0x52, 0x4f, 0x4e, 0x44};
static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t slirpMac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
constexpr uint32_t guestIp = 0x0a00020f, hostIp = 0x0a000202;

static void put16(Bytes& b, size_t at, uint16_t v) {
    b[at] = uint8_t(v >> 8);
    b[at + 1] = uint8_t(v);
}
static void put32(Bytes& b, size_t at, uint32_t v) {
    put16(b, at, uint16_t(v >> 16));
    put16(b, at + 2, uint16_t(v));
}
static uint16_t get16(const Bytes& b, size_t at) { return uint16_t((b[at] << 8) | b[at + 1]); }
static uint32_t get32(const Bytes& b, size_t at) { return (uint32_t(get16(b, at)) << 16) | get16(b, at + 2); }
static uint16_t checksum(const Bytes& b, size_t start, size_t length, uint32_t sum = 0) {
    for (size_t i = 0; i < length; i += 2)
        sum += uint32_t(b[start + i] << 8) | (i + 1 < length ? b[start + i + 1] : 0);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return uint16_t(~sum);
}
// The UDP or TCP checksum of an IPv4 frame, 0 when it's right.
static uint16_t transportChecksum(const Bytes& b) {
    const size_t ip = size_t(b[14] & 15) * 4, length = get16(b, 16) - ip;
    const uint32_t pseudo = get16(b, 26) + get16(b, 28) + get16(b, 30) + get16(b, 32) + b[23] + length;
    return checksum(b, 14 + ip, length, pseudo);
}

// An Ethernet II IPv4 frame from the guest with a UDP or TCP header and payload.
static Bytes ipFrame(const uint8_t* to, uint8_t protocol, uint32_t src, uint32_t dst, const Bytes& transport) {
    Bytes b(34 + transport.size(), 0);
    std::copy(to, to + 6, b.begin());
    std::copy(guestMac, guestMac + 6, b.begin() + 6);
    put16(b, 12, 0x0800);
    b[14] = 0x45;
    put16(b, 16, uint16_t(20 + transport.size()));
    b[22] = 64;
    b[23] = protocol;
    put32(b, 26, src);
    put32(b, 30, dst);
    put16(b, 24, checksum(b, 14, 20));
    std::copy(transport.begin(), transport.end(), b.begin() + 34);
    const uint16_t sum = transportChecksum(b);
    put16(b, protocol == 17 ? 40 : 50, sum);
    return b;
}
static Bytes udp(const uint8_t* to, uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, const Bytes& data) {
    Bytes t(8 + data.size(), 0);
    put16(t, 0, sport);
    put16(t, 2, dport);
    put16(t, 4, uint16_t(t.size()));
    std::copy(data.begin(), data.end(), t.begin() + 8);
    return ipFrame(to, 17, src, dst, t);
}
static Bytes tcp(const uint8_t* to, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags) {
    Bytes t(20, 0);
    put16(t, 0, sport);
    put16(t, 2, dport);
    put32(t, 4, seq);
    put32(t, 8, ack);
    t[12] = 0x50;
    t[13] = flags;
    put16(t, 14, 65535);
    return ipFrame(to, 6, guestIp, hostIp, t);
}
static Bytes dhcp(uint8_t type) {
    Bytes b(240, 0);
    b[0] = 1;
    b[1] = 1;
    b[2] = 6;
    put32(b, 4, 0x46524f4e);
    put16(b, 10, 0x8000);
    std::copy(guestMac, guestMac + 6, b.begin() + 28);
    put32(b, 236, 0x63825363);
    b.insert(b.end(), {53, 1, type});
    if (type == 3) b.insert(b.end(), {50, 4, 10, 0, 2, 15, 54, 4, 10, 0, 2, 2});
    b.push_back(255);
    return udp(broadcast, 0, 0xffffffff, 68, 67, b);
}

// Polls until a frame for the guest matches, skipping others. Guest time runs 1 ms a step.
static Bytes next(WifiRouter& router, uint64_t& now, const std::function<bool(const Bytes&)>& match) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    Bytes b;
    while (std::chrono::steady_clock::now() < end) {
        now += 1000000;
        router.Poll(now);
        while (router.Receive(b))
            if (match(b)) return b;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    fprintf(stderr, "router did not reply\n");
    exit(1);
}
static void until(WifiRouter& router, uint64_t& now, const std::function<bool()>& ready) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
        REQUIRE(std::chrono::steady_clock::now() < end);
        now += 1000000;
        router.Poll(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
static int loopback(int type, uint16_t& port) {
    const int fd = socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    REQUIRE(fd >= 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    socklen_t length = sizeof(a);
    REQUIRE(getsockname(fd, reinterpret_cast<sockaddr*>(&a), &length) == 0);
    port = ntohs(a.sin_port);
    return fd;
}
static bool isIp(const Bytes& b, uint8_t protocol) {
    return b.size() >= 42 && get16(b, 12) == 0x0800 && b[23] == protocol;
}

// DHCP option 53 of a reply from slirp's server, after checking yiaddr is 10.0.2.15.
static int dhcpType(const Bytes& b) {
    if (!isIp(b, 17) || get16(b, 34) != 67 || get16(b, 36) != 68 || b.size() < 282) return -1;
    CHECK(get32(b, 42 + 16) == guestIp);
    for (size_t i = 282; i + 1 < b.size();) {
        const uint8_t tag = b[i++];
        if (tag == 0) continue;
        if (tag == 255) break;
        const uint8_t length = b[i++];
        if (tag == 53 && length == 1) return b[i];
        i += length;
    }
    return 0;
}

static void testDhcpAndArp(WifiRouter& router, uint64_t& now) {
    auto b = dhcp(1);
    router.Input(b.data(), b.size(), now);
    CHECK(dhcpType(next(router, now, [](const Bytes& f) { return dhcpType(f) >= 0; })) == 2);
    b = dhcp(3);
    router.Input(b.data(), b.size(), now);
    CHECK(dhcpType(next(router, now, [](const Bytes& f) { return dhcpType(f) >= 0; })) == 5);

    Bytes arp(42, 0);
    std::copy(broadcast, broadcast + 6, arp.begin());
    std::copy(guestMac, guestMac + 6, arp.begin() + 6);
    put16(arp, 12, 0x0806);
    put16(arp, 14, 1);
    put16(arp, 16, 0x0800);
    arp[18] = 6;
    arp[19] = 4;
    put16(arp, 20, 1);
    std::copy(guestMac, guestMac + 6, arp.begin() + 22);
    put32(arp, 28, guestIp);
    put32(arp, 38, hostIp);
    router.Input(arp.data(), arp.size(), now);
    b = next(router, now, [](const Bytes& f) { return f.size() >= 42 && get16(f, 12) == 0x0806; });
    CHECK(get16(b, 20) == 2);
    CHECK(std::equal(slirpMac, slirpMac + 6, b.begin() + 22) && get32(b, 28) == hostIp);
    CHECK(std::equal(guestMac, guestMac + 6, b.begin() + 32) && get32(b, 38) == guestIp);
}

static void testTcpConnect(WifiRouter& router, uint64_t& now) {
    uint16_t port;
    const int listener = loopback(SOCK_STREAM, port);
    REQUIRE(listen(listener, 1) == 0);
    const uint16_t sport = 49500;
    uint32_t seq = 0x1000;
    auto b = tcp(slirpMac, sport, port, seq, 0, 0x02);
    router.Input(b.data(), b.size(), now);
    auto mine = [&](const Bytes& f) { return isIp(f, 6) && get16(f, 34) == port && get16(f, 36) == sport; };
    b = next(router, now, mine);
    CHECK((b[47] & 0x12) == 0x12);
    CHECK(get32(b, 42) == seq + 1);
    CHECK(checksum(b, 14, 20) == 0 && transportChecksum(b) == 0);
    CHECK(std::equal(guestMac, guestMac + 6, b.begin()) && std::equal(slirpMac, slirpMac + 6, b.begin() + 6));
    const uint32_t ack = get32(b, 38) + 1;
    ++seq;
    b = tcp(slirpMac, sport, port, seq, ack, 0x10);
    router.Input(b.data(), b.size(), now);
    int fd = -1;
    until(router, now, [&] {
        fd = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        return fd >= 0;
    });
    close(fd);
    close(listener);
}

static void testDnsQuery(WifiRouter& router, uint64_t& now) {
    uint16_t port;
    const int fd = loopback(SOCK_DGRAM, port);
    Bytes query = {0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
                   5, 'f', 'r', 'o', 'n', 'd', 4, 't', 'e', 's', 't', 0, 0, 1, 0, 1};
    auto b = udp(slirpMac, guestIp, hostIp, 49501, port, query);
    router.Input(b.data(), b.size(), now);
    Bytes got(1500);
    sockaddr_in from{};
    socklen_t length = sizeof(from);
    until(router, now, [&] {
        const auto n = recvfrom(fd, got.data(), got.size(), 0, reinterpret_cast<sockaddr*>(&from), &length);
        if (n < 0) return false;
        got.resize(size_t(n));
        return true;
    });
    CHECK(got == query);
    Bytes answer = query;
    answer[2] = 0x81;
    answer[3] = 0x80;
    answer[7] = 1;
    answer.insert(answer.end(), {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 127, 0, 0, 1});
    REQUIRE(sendto(fd, answer.data(), answer.size(), 0, reinterpret_cast<sockaddr*>(&from), length) ==
            ssize_t(answer.size()));
    b = next(router, now, [&](const Bytes& f) { return isIp(f, 17) && get16(f, 34) == port; });
    CHECK(get16(b, 36) == 49501 && get32(b, 26) == hostIp && get32(b, 30) == guestIp);
    CHECK(checksum(b, 14, 20) == 0 && transportChecksum(b) == 0);
    CHECK(get16(b, 38) == 8 + answer.size() && std::equal(answer.begin(), answer.end(), b.begin() + 42));
    close(fd);
}

static void testWakeup(WifiRouter& router, uint64_t& now) {
    // 0xfade, type 1, then the PC's address, mask and name, as the guest sends it.
    const Bytes wakeup = {0xfa, 0xde, 0x01, 0x00, 10, 0, 2, 2, 255, 255, 255, 0, 'p', 'c', 0};
    auto sent = udp(slirpMac, guestIp, hostIp, 1031, 14237, wakeup);
    router.Input(sent.data(), sent.size(), now);
    // Answered at once, before any poll.
    Bytes b;
    REQUIRE(router.Receive(b));
    CHECK(b.size() == 42 + wakeup.size());
    CHECK(std::equal(guestMac, guestMac + 6, b.begin()) && std::equal(slirpMac, slirpMac + 6, b.begin() + 6));
    CHECK(get16(b, 12) == 0x0800 && b[14] == 0x45 && b[23] == 17 && get16(b, 16) == 28 + wakeup.size());
    CHECK(get32(b, 26) == hostIp && get32(b, 30) == guestIp);
    CHECK(get16(b, 34) == 14237 && get16(b, 36) == 1031 && get16(b, 38) == 8 + wakeup.size());
    CHECK(checksum(b, 14, 20) == 0 && transportChecksum(b) == 0);
    Bytes expected = wakeup;
    expected[2] = 2;
    CHECK(std::equal(expected.begin(), expected.end(), b.begin() + 42));
    CHECK(!router.Receive(b));

    // A short wakeup with Ethernet padding to 60 bytes, sent to the broadcast address.
    const Bytes small = {0xfa, 0xde, 0x01, 0x00};
    sent = udp(broadcast, guestIp, hostIp, 1032, 14237, small);
    sent.resize(60, 0);
    router.Input(sent.data(), sent.size(), now);
    REQUIRE(router.Receive(b));
    CHECK(b.size() == 42 + small.size() && get16(b, 16) == 32 && get16(b, 36) == 1032);
    CHECK(std::equal(slirpMac, slirpMac + 6, b.begin() + 6));
    CHECK(transportChecksum(b) == 0 && b[44] == 2);

    // Not wakeups: type 2, too short, another port, another address. None is answered here.
    for (const auto& frame : {udp(slirpMac, guestIp, hostIp, 1031, 14237, {0xfa, 0xde, 0x02, 0x00}),
                              udp(slirpMac, guestIp, hostIp, 1031, 14237, {0xfa, 0xde}),
                              udp(slirpMac, guestIp, hostIp, 1031, 14238, wakeup),
                              udp(slirpMac, guestIp, 0x0a000203, 1031, 14237, wakeup)}) {
        router.Input(frame.data(), frame.size(), now);
        while (router.Receive(b)) CHECK(!(isIp(b, 17) && get16(b, 34) == 14237 && b.size() >= 45 && b[44] == 2));
    }
}

int main() {
    WifiRouter router;
    REQUIRE(router.Valid());
    uint64_t now = 0;
    testDhcpAndArp(router, now);
    testTcpConnect(router, now);
    testDnsQuery(router, now);
    testWakeup(router, now);
    if (failures) {
        fprintf(stderr, "FAIL: %d checks\n", failures);
        return 1;
    }
    puts("PASS: shared Wi-Fi router DHCP offer and ack at 10.0.2.15, ARP for 10.0.2.2, TCP connect, DNS-format UDP, Network HotSync wakeup");
}
