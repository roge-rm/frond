#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

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
    puts(
        "PASS: virtual Wi-Fi router DHCP offer/ACK, ARP and UDP through the host; malformed frame "
        "bounds");
}
