// Firmware-free tests for the RNDIS host (RndisHost: control messages, bring-up, keepalives, packet framing on
// the bulk endpoints) and the desktop end of the link (DesktopLink's DHCP client and connect request forward).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "RndisHost.h"
#include "net/DesktopLink.h"

namespace {
using Bytes = std::vector<uint8_t>;
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void put32(Bytes& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
constexpr uint64_t ms = 1000000;
const uint8_t mac[6] = {0x80, 0x00, 0x60, 0x0f, 0xe8, 0x00};

// A completion for request `id`: type | 0x80000000, length, id, status, then `rest`.
Bytes completion(uint32_t type, uint32_t id, uint32_t status, const Bytes& rest = {}) {
    Bytes m;
    put32(m, type | 0x80000000);
    put32(m, uint32_t(16 + rest.size()));
    put32(m, id);
    put32(m, status);
    m.insert(m.end(), rest.begin(), rest.end());
    return m;
}
Bytes initDone(uint32_t id) {
    Bytes rest;
    for (uint32_t x : {1u, 0u, 1u, 0u, 32u, 1642u, 0u, 0u, 0u}) put32(rest, x);  // 1.0, flags, medium, 32, 1642, ...
    return completion(2, id, 0, rest);
}
Bytes queryDone(uint32_t id, uint32_t status, const Bytes& info) {
    Bytes rest;
    put32(rest, uint32_t(info.size()));
    put32(rest, 16);  // from the RequestId field: right after this header
    rest.insert(rest.end(), info.begin(), info.end());
    return completion(4, id, status, rest);
}
const uint8_t notify[8] = {1, 0, 0, 0, 0, 0, 0, 0};

// Takes the next command, checks its type, sends it and answers it after a RESPONSE_AVAILABLE.
uint32_t command(RndisHost& h, uint32_t type, uint64_t now) {
    Bytes m;
    assert(h.NextCommand(m, now));
    assert(get32(m.data()) == type && get32(m.data() + 4) == m.size());
    h.CommandSent(true, now);
    assert(!h.WantResponse(now));
    return get32(m.data() + 8);
}
void answer(RndisHost& h, const Bytes& m, uint64_t now) {
    h.Notification(notify, 8, now);
    assert(h.WantResponse(now));
    h.Response(true, m.data(), m.size(), now);
}
RndisHost up(uint64_t& now) {
    RndisHost h;
    h.Start(64, now);
    uint32_t id = command(h, RndisHost::initMsg, now);
    answer(h, initDone(id), now += ms);
    id = command(h, RndisHost::queryMsg, now);
    answer(h, queryDone(id, 0, Bytes(mac, mac + 6)), now += ms);
    id = command(h, RndisHost::queryMsg, now);
    answer(h, queryDone(id, 0, {0xdc, 0x05, 0, 0}), now += ms);
    id = command(h, RndisHost::setMsg, now);
    answer(h, completion(5, id, 0), now += ms);
    assert(h.Up());
    return h;
}
}  // namespace

static void messageTests() {
    const Bytes init = RndisHost::InitMessage(7);
    assert(init.size() == 24 && get32(&init[0]) == 2 && get32(&init[4]) == 24 && get32(&init[8]) == 7);
    assert(get32(&init[12]) == 1 && get32(&init[16]) == 0 && get32(&init[20]) == RndisHost::maxTransfer);
    // A query carries a zeroed buffer the size of the answer; WM5 refuses one without.
    const Bytes query = RndisHost::QueryMessage(8, RndisHost::oidPermanentAddress, 6);
    assert(query.size() == 34 && get32(&query[0]) == 4 && get32(&query[4]) == 34 && get32(&query[12]) == 0x01010101);
    assert(get32(&query[16]) == 6 && get32(&query[20]) == 20 && get32(&query[24]) == 0);
    const uint8_t filter[4] = {0x0b, 0, 0, 0};
    const Bytes set = RndisHost::SetMessage(9, RndisHost::oidCurrentPacketFilter, filter, 4);
    assert(set.size() == 32 && get32(&set[0]) == 5 && get32(&set[16]) == 4 && get32(&set[20]) == 20 && get32(&set[28]) == 0x0b);
    const Bytes keep = RndisHost::KeepaliveComplete(0x55);
    assert(keep.size() == 16 && get32(&keep[0]) == 0x80000008 && get32(&keep[8]) == 0x55 && get32(&keep[12]) == 0);
    // PACKET_MSG: 44-byte header, DataOffset 36 from the RequestId field.
    Bytes frame(60);
    for (size_t i = 0; i < frame.size(); ++i) frame[i] = uint8_t(i);
    const Bytes packet = RndisHost::PacketMessage(frame.data(), frame.size());
    assert(packet.size() == 104 && get32(&packet[0]) == 1 && get32(&packet[4]) == 104);
    assert(get32(&packet[8]) == 36 && get32(&packet[12]) == 60);
    for (int i = 16; i < 44; ++i) assert(packet[i] == 0);
    assert(!std::memcmp(&packet[44], frame.data(), frame.size()));
}

static void bringUpTests() {
    uint64_t now = 0;
    RndisHost h = up(now);
    assert(!std::memcmp(h.Mac(), mac, 6) && h.MaxFrame() == 1514);
    auto messages = h.TakeMessages();
    assert(messages.size() == 2 && messages[1].find("up, MAC 80:00:60:0f:e8:00") != std::string::npos);

    // An optional query that fails is skipped; any other failure starts again from INIT.
    RndisHost s;
    s.Start(64, 0);
    uint32_t id = command(s, RndisHost::initMsg, 0);
    answer(s, initDone(id), ms);
    id = command(s, RndisHost::queryMsg, ms);
    answer(s, queryDone(id, 0, Bytes(mac, mac + 6)), 2 * ms);
    id = command(s, RndisHost::queryMsg, 2 * ms);
    answer(s, queryDone(id, 0xc00000bb, {}), 3 * ms);
    id = command(s, RndisHost::setMsg, 3 * ms);
    answer(s, completion(5, id, 0xc0000001), 4 * ms);
    assert(!s.Up() && s.GetState() == RndisHost::State::init);
    command(s, RndisHost::initMsg, 5 * ms);

    // A QUERY completion cut off after its status has no length or offset fields: it fails the bring-up,
    // and a length or offset that points past the message does too.
    for (int variant = 0; variant < 3; ++variant) {
        RndisHost t;
        t.Start(64, 0);
        id = command(t, RndisHost::initMsg, 0);
        answer(t, initDone(id), ms);
        id = command(t, RndisHost::queryMsg, ms);
        Bytes rest;
        if (variant) {
            put32(rest, variant == 1 ? 6 : 0xfffffff0);  // length
            put32(rest, variant == 1 ? 0xfffffff0 : 16);  // offset
        }
        const Bytes cut = completion(4, id, 0, rest);
        assert(cut.size() == (variant ? 24u : 16u));
        answer(t, cut, 2 * ms);
        assert(!t.Up() && t.GetState() == RndisHost::State::init);
    }

    // A short reply to the optional frame size query skips that step.
    {
        RndisHost t;
        t.Start(64, 0);
        id = command(t, RndisHost::initMsg, 0);
        answer(t, initDone(id), ms);
        id = command(t, RndisHost::queryMsg, ms);
        answer(t, queryDone(id, 0, Bytes(mac, mac + 6)), 2 * ms);
        id = command(t, RndisHost::queryMsg, 2 * ms);
        answer(t, completion(4, id, 0, {}), 3 * ms);
        assert(t.GetState() == RndisHost::State::query);
        id = command(t, RndisHost::setMsg, 3 * ms);
        answer(t, completion(5, id, 0), 4 * ms);
        assert(t.Up());
    }

    // No notification: GET_ENCAPSULATED_RESPONSE after 100 ms anyway; an empty answer waits another 100 ms.
    RndisHost w;
    w.Start(64, 0);
    command(w, RndisHost::initMsg, 0);
    assert(!w.WantResponse(99 * ms) && w.WantResponse(100 * ms) && w.NextWake() == 100 * ms);
    const uint8_t zero = 0;
    w.Response(true, &zero, 1, 100 * ms);
    assert(!w.WantResponse(150 * ms) && w.WantResponse(200 * ms));
    // A completion for another request is ignored.
    const Bytes late = initDone(99);
    w.Response(true, late.data(), late.size(), 200 * ms);
    assert(w.GetState() == RndisHost::State::init && w.WantResponse(300 * ms));
    // Nothing for 5 s: start again.
    w.Response(false, nullptr, 0, 5000 * ms);
    assert(w.GetState() == RndisHost::State::init);

    // Answers that aren't the awaited completion back off like empty ones and time out after 5 s.
    RndisHost o;
    o.Start(64, 0);
    command(o, RndisHost::initMsg, 0);
    Bytes status;
    put32(status, 7);
    put32(status, 12);
    put32(status, 0x40010000);
    o.Response(true, status.data(), status.size(), 100 * ms);
    assert(!o.WantResponse(150 * ms) && o.WantResponse(200 * ms));
    o.Response(true, late.data(), late.size(), 5000 * ms);
    assert(o.GetState() == RndisHost::State::init && !o.WantResponse(5100 * ms));
    Bytes again;
    assert(o.NextCommand(again, 5000 * ms) && get32(again.data()) == RndisHost::initMsg);
}

static void keepaliveTests() {
    uint64_t now = 0;
    RndisHost h = up(now);
    Bytes m;
    assert(!h.NextCommand(m, now + 4999 * ms) && h.NextWake() == now + RndisHost::keepaliveNs);
    now += RndisHost::keepaliveNs;
    const uint32_t id = command(h, RndisHost::keepaliveMsg, now);
    answer(h, completion(8, id, 0), now += ms);
    assert(h.Up() && h.GetCounters().keepalives == 1 && h.GetCounters().ups == 1);
    // Failed keepalives start again from INIT; coming up again clears the count, so failures spread over
    // a long session never give up.
    for (int round = 0; round < 5; ++round) {
        now += RndisHost::keepaliveNs;
        const uint32_t k = command(h, RndisHost::keepaliveMsg, now);
        answer(h, completion(8, k, 0xc0000001), now += ms);
        assert(h.GetState() == RndisHost::State::init);
        uint32_t i = command(h, RndisHost::initMsg, now);
        answer(h, initDone(i), now += ms);
        i = command(h, RndisHost::queryMsg, now);
        answer(h, queryDone(i, 0, Bytes(mac, mac + 6)), now += ms);
        i = command(h, RndisHost::queryMsg, now);
        answer(h, queryDone(i, 0, {0xdc, 0x05, 0, 0}), now += ms);
        i = command(h, RndisHost::setMsg, now);
        answer(h, completion(5, i, 0), now += ms);
        assert(h.Up() && h.GetCounters().ups == uint32_t(round + 2));
    }
    // The device's own KEEPALIVE gets a KEEPALIVE_CMPLT that waits for nothing.
    Bytes ka;
    put32(ka, 8);
    put32(ka, 12);
    put32(ka, 0x1234);
    answer(h, ka, now += ms);
    assert(h.NextCommand(m, now) && m == RndisHost::KeepaliveComplete(0x1234));
    h.CommandSent(true, now);
    assert(!h.WantResponse(now + 200 * ms));
}

static void bulkTests() {
    uint64_t now = 0;
    RndisHost h = up(now);
    // OUT: one PACKET_MSG a transfer, split into max-size packets; a transfer that fills its last packet ends
    // with a zero-length packet.
    Bytes frame(84, 0xa5);  // 44 + 84 = 128 bytes, two full packets
    h.FrameToDevice(frame.data(), frame.size());
    Bytes small(100, 0x5a);  // 144 bytes: 64, 64, 16
    h.FrameToDevice(small.data(), small.size());
    std::vector<unsigned> sizes;
    Bytes stream;
    uint8_t packet[64];
    unsigned size = 0;
    while (h.BulkOutPacket(packet, 64, size)) {
        sizes.push_back(size);
        stream.insert(stream.end(), packet, packet + size);
    }
    assert((sizes == std::vector<unsigned>{64, 64, 0, 64, 64, 16}));
    assert(get32(&stream[0]) == 1 && get32(&stream[4]) == 128 && stream[44] == 0xa5);
    assert(get32(&stream[128 + 4]) == 144 && get32(&stream[128 + 12]) == 100 && stream[128 + 44] == 0x5a);
    assert(!h.OutPending());
    Bytes fullFrame(1514, 0x5a);
    h.FrameToDevice(fullFrame.data(), fullFrame.size());
    assert(h.OutPending());
    size_t fullSize = 0;
    while (h.BulkOutPacket(packet, sizeof(packet), size)) fullSize += size;
    assert(fullSize == 44 + fullFrame.size());

    // Frames too short or too long are dropped.
    Bytes tiny(10), huge(1515);
    h.FrameToDevice(tiny.data(), tiny.size());
    h.FrameToDevice(huge.data(), huge.size());
    assert(!h.OutPending() && h.GetCounters().dropped == 2);

    // IN: packets gather until a short one; one transfer can hold two messages.
    std::vector<Bytes> got;
    h.frameFromDevice = [&](const uint8_t* f, size_t n) { got.emplace_back(f, f + n); };
    Bytes a(60, 1), b(30, 2);
    Bytes transfer = RndisHost::PacketMessage(a.data(), a.size());
    const Bytes second = RndisHost::PacketMessage(b.data(), b.size());
    transfer.insert(transfer.end(), second.begin(), second.end());  // 104 + 74 = 178: 64, 64, 50
    for (size_t pos = 0; pos < transfer.size(); pos += 64)
        h.BulkInPacket(transfer.data() + pos, std::min<size_t>(64, transfer.size() - pos));
    assert(got.size() == 2 && got[0] == a && got[1] == b);
    // WM5 sends a 128-byte RNDIS reply in two full packets without a short one.
    Bytes c(84, 3);
    const Bytes full = RndisHost::PacketMessage(c.data(), c.size());
    h.BulkInPacket(full.data(), 64);
    assert(got.size() == 2);
    h.BulkInPacket(full.data() + 64, 64);
    assert(got.size() == 3 && got[2] == c);
    const uint8_t padding = 0;
    h.BulkInPacket(&padding, 1);
    assert(got.size() == 3);
    // A bad message stops the transfer; a transfer longer than INIT allowed is dropped to its short packet.
    Bytes bad = full;
    bad[0] = 9;
    h.BulkInTransfer(bad.data(), bad.size());
    assert(got.size() == 3 && h.GetCounters().badMessages == 1);
    Bytes chunk(64, 0);
    for (unsigned i = 0; i < RndisHost::maxTransfer / 64 + 1; ++i) h.BulkInPacket(chunk.data(), 64);
    h.BulkInPacket(full.data(), 64);
    h.BulkInPacket(full.data() + 64, 64);
    h.BulkInPacket(chunk.data(), 10);
    assert(got.size() == 3 && h.GetCounters().badMessages == 2);
    h.BulkInPacket(full.data(), 64);
    h.BulkInPacket(full.data() + 64, 64);
    h.BulkInPacket(nullptr, 0);
    assert(got.size() == 4);
}

// The handheld's DHCP server's answer to `request`: OFFER (2) or ACK (5) of 169.254.2.2 from 169.254.2.1.
static Bytes dhcpAnswer(const Bytes& request, uint8_t type) {
    Bytes f = request;
    std::memcpy(&f[0], &request[6], 6);
    const uint8_t device[6] = {0x82, 0x00, 0x60, 0x0f, 0xe8, 0x00};
    std::memcpy(&f[6], device, 6);
    uint8_t* udp = &f[14 + 20];
    std::swap(udp[1], udp[3]);  // 67 -> 68
    uint8_t* d = udp + 8;
    d[0] = 2;
    const uint8_t yiaddr[4] = {169, 254, 2, 2};
    std::memcpy(d + 16, yiaddr, 4);
    uint8_t* o = d + 240;
    std::memset(o, 0, f.size() - (o - f.data()));
    const uint8_t options[] = {53, 1, type, 54, 4, 169, 254, 2, 1, 1, 4, 255, 255, 255, 0, 51, 4, 0, 0, 0x0e, 0x10, 255};
    std::memcpy(o, options, sizeof(options));
    return f;
}

static void dhcpTests() {
    const Bytes discover = frondNet::DesktopLink::DhcpFrame(mac, 1, 0x1234, 0, 0);
    assert(discover.size() == 342 && !std::memcmp(&discover[6], mac, 6) && discover[12] == 8 && discover[23] == 17);
    assert(discover[14 + 20 + 8 + 240] == 53 && discover[14 + 20 + 8 + 242] == 1);
    // The IPv4 header checksum adds up.
    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2) sum += uint32_t(discover[14 + i]) << 8 | discover[15 + i];
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    assert(sum == 0xffff);
    auto a = frondNet::DesktopLink::ParseDhcp(dhcpAnswer(discover, 2).data(), 342, mac, 0x1234);
    assert(a.type == 2 && a.yiaddr == 0xa9fe0202 && a.serverId == 0xa9fe0201 && a.netmask == 0xffffff00);
    // Another transaction or another client's MAC isn't ours.
    assert(frondNet::DesktopLink::ParseDhcp(dhcpAnswer(discover, 2).data(), 342, mac, 0x1235).type == 0);
    const uint8_t other[6] = {2, 0, 0, 0, 0, 1};
    assert(frondNet::DesktopLink::ParseDhcp(dhcpAnswer(discover, 2).data(), 342, other, 0x1234).type == 0);

    // The whole exchange: DISCOVER at the first poll, REQUEST for the offer, bound at the ACK.
    frondNet::DesktopLink link(mac, 0);
    Bytes f;
    assert(!link.Receive(f));
    link.Poll(0);
    assert(link.Receive(f) && f[14 + 20 + 8 + 242] == 1);
    const uint32_t xid = get32(&f[14 + 20 + 8 + 4]);
    (void)xid;
    link.Input(dhcpAnswer(f, 2).data(), 342, ms);
    Bytes request;
    assert(link.Receive(request) && request[14 + 20 + 8 + 242] == 3);
    // REQUEST asks for the offered address from that server (options 50 and 54).
    assert(request[14 + 20 + 8 + 243] == 50 && request[14 + 20 + 8 + 245] == 169 && request[14 + 20 + 8 + 248] == 2);
    assert(link.Address() == 0);
    link.Input(dhcpAnswer(request, 5).data(), 342, 2 * ms);
    assert(link.Address() == 0xa9fe0202 && link.Server() == 0xa9fe0201);
    // No answer: DISCOVER again 4 s later with a new transaction.
    frondNet::DesktopLink quiet(mac, 0);
    quiet.Poll(0);
    assert(quiet.Receive(f));
    quiet.Poll(frondNet::DesktopLink::discoverNs - 1);
    assert(!quiet.Receive(f));
    quiet.Poll(frondNet::DesktopLink::discoverNs);
    assert(quiet.Receive(f) && get32(&f[14 + 20 + 8 + 4]) != 0);
}

// A free port on 127.0.0.1 (bound and let go).
static uint16_t freeLoopbackPort(int type) {
    const int fd = socket(AF_INET, type, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t n = sizeof(a);
    assert(fd >= 0);
    const int bound = bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    assert(bound == 0);
    const int named = getsockname(fd, reinterpret_cast<sockaddr*>(&a), &n);
    assert(named == 0);
    (void)bound, (void)named;
    close(fd);
    return ntohs(a.sin_port);
}

// The desktop's connect request: one UDP byte to the host port reaches the handheld's 5679 from 169.254.2.2.
static void connectRequestTests() {
    // Another program can take the free port before slirp binds it, so try a few.
    std::unique_ptr<frondNet::DesktopLink> bound;
    uint16_t port = 0;
    Bytes f, request;
    for (int attempt = 0; attempt < 5 && !bound; ++attempt) {
        port = freeLoopbackPort(SOCK_DGRAM);
        auto link = std::make_unique<frondNet::DesktopLink>(mac, port);
        link->Poll(0);
        assert(link->Receive(f));
        link->Input(dhcpAnswer(f, 2).data(), 342, ms);
        assert(link->Receive(request));
        link->Input(dhcpAnswer(request, 5).data(), 342, 2 * ms);
        assert(link->Address() == 0xa9fe0202);
        if (link->ConnectForward()) bound = std::move(link);
    }
    if (!bound) {
        std::fprintf(stderr, "rndis-host: no free UDP port could be forwarded in 5 tries\n");
        std::abort();
    }
    frondNet::DesktopLink& link = *bound;
    // The port is taken now.
    const int busy = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    const int taken = bind(busy, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    assert(taken != 0);
    (void)taken;
    close(busy);
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    const uint8_t knock = 0x7f;
    const ssize_t sent = sendto(fd, &knock, 1, 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    assert(sent == 1);
    (void)sent;
    close(fd);
    const uint8_t device[6] = {0x82, 0x00, 0x60, 0x0f, 0xe8, 0x00};
    bool got = false;
    for (int i = 0; i < 200 && !got; ++i) {
        usleep(1000);
        link.Poll((3 + i) * ms);
        while (link.Receive(f)) {
            if (f.size() >= 42 && f[12] == 0x08 && f[13] == 0x06 && f[21] == 1) {
                // slirp asks for the handheld's MAC: answer for 169.254.2.1.
                Bytes reply = f;
                std::memcpy(&reply[0], &f[6], 6);
                std::memcpy(&reply[6], device, 6);
                reply[21] = 2;
                std::memcpy(&reply[22], device, 6);
                std::memcpy(&reply[28], &f[38], 4);
                std::memcpy(&reply[32], &f[22], 10);
                link.Input(reply.data(), reply.size(), (3 + i) * ms);
                continue;
            }
            if (f.size() >= 14 + 20 + 8 + 1 && f[12] == 0x08 && f[13] == 0x00 && f[23] == 17) {
                const uint8_t* ip = &f[14];
                const uint8_t* udp = ip + (ip[0] & 15) * 4;
                assert(!std::memcmp(&f[0], device, 6) && !std::memcmp(&f[6], mac, 6));
                assert(ip[12] == 169 && ip[13] == 254 && ip[14] == 2 && ip[15] == 2);
                assert(ip[16] == 169 && ip[17] == 254 && ip[18] == 2 && ip[19] == 1);
                assert((udp[2] << 8 | udp[3]) == 5679 && (udp[4] << 8 | udp[5]) == 9 && udp[8] == 0x7f);
                got = true;
            }
        }
    }
    assert(got);
}

static void tcpForwardTests() {
    auto link = std::make_unique<frondNet::DesktopLink>(mac, 0);
    assert(link->AddTcpForward(0, 139) == EINVAL);
    assert(link->AddTcpForward(21139, 139) == ENOTCONN);
    Bytes frame, request;
    link->Poll(0);
    assert(link->Receive(frame));
    link->Input(dhcpAnswer(frame, 2).data(), 342, ms);
    assert(link->Receive(request));
    link->Input(dhcpAnswer(request, 5).data(), 342, 2 * ms);
    assert(link->Address() == 0xa9fe0202 && link->Server() == 0xa9fe0201);

    uint16_t port = 0;
    for (int i = 0; i < 5; ++i) {
        const auto candidate = freeLoopbackPort(SOCK_STREAM);
        const int error = link->AddTcpForward(candidate, 139);
        if (!error) { port = candidate; break; }
        assert(error == EADDRINUSE);
    }
    assert(port);
    assert(link->AddTcpForward(port, 139) == EADDRINUSE);

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    link.reset();
    assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    close(fd);
}

int main() {
    messageTests();
    bringUpTests();
    keepaliveTests();
    bulkTests();
    dhcpTests();
    connectRequestTests();
    tcpForwardTests();
    std::puts("rndis-host: ok");
    return 0;
}
