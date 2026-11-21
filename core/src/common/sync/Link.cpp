#include "Link.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace frondSync {

namespace {

using Clock = std::chrono::steady_clock;

int MsLeft(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? int(std::min<long long>(left, 1 << 30)) : 0;
}

// Buffered reads on a stream, with a deadline per call.
class Reader {
   public:
    explicit Reader(SyncStream& stream) : stream(stream) {}
    // Makes at least n bytes ready. False on timeout or when closed (closed is set).
    bool Need(size_t n, Clock::time_point deadline, const std::atomic<bool>* stop = nullptr) {
        while (buffer.size() - at < n) {
            if (stop && *stop) return false;
            uint8_t chunk[4096];
            const int wait = std::min(MsLeft(deadline), 200);
            const long got = stream.Read(chunk, sizeof(chunk), wait);
            if (got < 0) return closed = true, false;
            if (got > 0) {
                if (at > 65536) buffer.erase(buffer.begin(), buffer.begin() + long(at)), at = 0;
                buffer.insert(buffer.end(), chunk, chunk + got);
            } else if (!MsLeft(deadline)) {
                return false;
            }
        }
        return true;
    }
    const uint8_t* Peek() const { return buffer.data() + at; }
    size_t Ready() const { return buffer.size() - at; }
    void Skip(size_t n) { at += n; }
    bool closed{false};

   private:
    SyncStream& stream;
    Bytes buffer;
    size_t at{0};
};

uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

constexpr uint8_t slpSocketDlp = 3;
constexpr uint8_t slpTypePadp = 2;
constexpr uint8_t padpData = 1, padpAck = 2, padpTickle = 4, padpAbort = 8;
constexpr uint8_t padpFirst = 0x80, padpLast = 0x40, padpLong = 0x10;
constexpr size_t padpFragment = 1024;
constexpr int ackTimeoutMs = 2000, ackRetries = 10;

struct SlpFrame {
    uint8_t dest, src, type, xid;
    Bytes body;
};

// Finds the next good SLP packet, skipping noise and bad checksums.
bool ReadSlp(Reader& in, SlpFrame& frame, Clock::time_point deadline, const std::atomic<bool>* stop = nullptr) {
    for (;;) {
        if (!in.Need(10, deadline, stop)) return false;
        const uint8_t* p = in.Peek();
        if (p[0] != 0xbe || p[1] != 0xef || p[2] != 0xed || SlpHeaderSum(p) != p[9]) {
            in.Skip(1);
            continue;
        }
        const size_t size = Get16(p + 6);
        if (!in.Need(10 + size + 2, deadline, stop)) return false;
        p = in.Peek();
        if (SlpCrc(p, 10 + size) != Get16(p + 10 + size)) {
            in.Skip(1);
            continue;
        }
        frame = {p[3], p[4], p[5], p[8], Bytes(p + 10, p + 10 + size)};
        in.Skip(10 + size + 2);
        return true;
    }
}

// SLP and PADP, desktop side, on socket 3. Each data fragment is acknowledged with its own
// transaction ID; the desktop's requests take fresh IDs and the handheld answers each with the
// request's ID.
class PadpLink final : public HandheldLink {
   public:
    PadpLink(SyncStream& stream, Reader& reader, uint8_t xid) : stream(stream), in(reader), xid(xid) {}
    std::string Name() const override { return "serial"; }

    bool Send(const Bytes& message) override { return SendAs(message, NextXid()); }

    bool SendAs(const Bytes& message, uint8_t id) {
        const bool longForm = message.size() > 0xffff;
        for (size_t offset = 0; offset < message.size() || (offset == 0 && message.empty());) {
            const size_t n = std::min(padpFragment, message.size() - offset);
            uint8_t flags = uint8_t((offset == 0 ? padpFirst : 0) | (offset + n == message.size() ? padpLast : 0) | (longForm ? padpLong : 0));
            const uint32_t size = offset == 0 ? uint32_t(message.size()) : uint32_t(offset);
            Bytes body{padpData, flags};
            if (longForm) body.push_back(uint8_t(size >> 24)), body.push_back(uint8_t(size >> 16));
            body.push_back(uint8_t(size >> 8)), body.push_back(uint8_t(size));
            body.insert(body.end(), message.begin() + long(offset), message.begin() + long(offset + n));
            if (!SendFragment(body, id)) return false;
            offset += n;
            if (message.empty()) break;
        }
        return true;
    }

    bool Receive(Bytes& message, int timeoutMs) override {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        message.clear();
        size_t total = 0;
        bool started = false;
        for (;;) {
            SlpFrame f;
            if (!ReadSlp(in, f, deadline)) return Fail(in.closed ? "The handheld went away." : "The handheld stopped answering.");
            if (f.type != slpTypePadp || f.body.size() < 4) continue;  // loopback pings and the like
            const uint8_t type = f.body[0], flags = f.body[1];
            const size_t header = flags & padpLong ? 6 : 4;
            if (f.body.size() < header) continue;
            const uint32_t size = flags & padpLong ? Get32(&f.body[2]) : Get16(&f.body[2]);
            if (type == padpAbort) return Fail("The handheld ended the sync.");
            if (type != padpData) continue;  // stray acks and tickles
            Ack(f, flags, size);
            const uint8_t* data = f.body.data() + header;
            const size_t n = f.body.size() - header;
            if (flags & padpFirst) {
                started = true;
                total = size;
                message.assign(data, data + n);
            } else if (started && size == message.size()) {
                message.insert(message.end(), data, data + n);
            } else {
                continue;  // a repeat of a fragment we have, already acknowledged again
            }
            if ((flags & padpLast) || message.size() >= total) {
                lastReceived = f.xid;
                return true;
            }
        }
    }

    void Finish() override {}

    // The CMP wakeup's answer goes back on the wakeup's transaction ID.
    uint8_t lastReceived{0};

   private:
    uint8_t NextXid() {
        // Like a desktop, two apart, never 0 or 0xff.
        xid = uint8_t(xid + 2);
        if (xid == 0 || xid == 0xff) xid = 1;
        return xid;
    }

    void Ack(const SlpFrame& f, uint8_t flags, uint32_t size) {
        Bytes body{padpAck, flags};
        if (flags & padpLong) body.push_back(uint8_t(size >> 24)), body.push_back(uint8_t(size >> 16));
        body.push_back(uint8_t(size >> 8)), body.push_back(uint8_t(size));
        const auto packet = SlpPacket(f.src, f.dest, slpTypePadp, f.xid, body);
        stream.Write(packet.data(), packet.size());
    }

    bool SendFragment(const Bytes& body, uint8_t id) {
        const auto packet = SlpPacket(slpSocketDlp, slpSocketDlp, slpTypePadp, id, body);
        for (int attempt = 0; attempt < ackRetries; ++attempt) {
            if (!stream.Write(packet.data(), packet.size())) return Fail("The handheld went away.");
            const auto deadline = Clock::now() + std::chrono::milliseconds(ackTimeoutMs);
            for (;;) {
                SlpFrame f;
                if (!ReadSlp(in, f, deadline)) {
                    if (in.closed) return Fail("The handheld went away.");
                    break;  // resend
                }
                if (f.type != slpTypePadp || f.body.size() < 4) continue;
                const uint8_t flags = f.body[1];
                const size_t headerSize = flags & padpLong ? 6 : 4;
                if (f.body.size() < headerSize) continue;
                if (f.body[0] == padpAbort) return Fail("The handheld ended the sync.");
                if (f.body[0] == padpAck && f.xid == id && flags == body[1] &&
                    std::equal(f.body.begin() + 2, f.body.begin() + headerSize, body.begin() + 2))
                    return true;
                if (f.body[0] == padpData) {
                    // Our ack to its last message was lost and it sent that again.
                    Ack(f, flags, flags & padpLong ? Get32(&f.body[2]) : Get16(&f.body[2]));
                }
            }
        }
        return Fail("The handheld didn't acknowledge.");
    }

    bool Fail(const std::string& why) {
        error = why;
        return false;
    }

    SyncStream& stream;
    Reader& in;
    uint8_t xid;
};

// NetSync framing: {1, transaction ID, length32} then the message. No acknowledgements.
class NetSyncLink final : public HandheldLink {
   public:
    NetSyncLink(SyncStream& stream, Reader& reader, Transport transport) : stream(stream), in(reader), transport(transport) {}
    std::string Name() const override { return transport == Transport::usb ? "usb" : "netsync"; }

    bool Send(const Bytes& message) override { return SendAs(message, xid); }

    bool SendAs(const Bytes& message, uint8_t id) {
        Bytes frame{1, id, uint8_t(message.size() >> 24), uint8_t(message.size() >> 16), uint8_t(message.size() >> 8),
                    uint8_t(message.size())};
        frame.insert(frame.end(), message.begin(), message.end());
        if (!stream.Write(frame.data(), frame.size())) return Fail("The handheld went away.");
        return true;
    }

    bool Receive(Bytes& message, int timeoutMs) override {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        if (!in.Need(6, deadline)) return Fail(in.closed ? "The handheld went away." : "The handheld stopped answering.");
        const uint8_t* p = in.Peek();
        if (p[0] != 1) return Fail("The handheld's NetSync framing broke.");
        const uint8_t id = p[1];
        const size_t size = Get32(p + 2);
        if (size > 0x1000000) return Fail("The handheld's NetSync framing broke.");
        if (!in.Need(6 + size, deadline)) return Fail(in.closed ? "The handheld went away." : "The handheld stopped answering.");
        p = in.Peek();
        message.assign(p + 6, p + 6 + size);
        in.Skip(6 + size);
        // The next request takes the next ID; answers carry the request's.
        if (id != 0xff) xid = uint8_t(id + 1);
        return true;
    }

    uint8_t xid{0};

   private:
    bool Fail(const std::string& why) {
        error = why;
        return false;
    }
    SyncStream& stream;
    Reader& in;
    Transport transport;
};

// The desktop's two handshake messages after the handheld's first (type 0x90), taken from a
// standard desktop's exchange with a Palm OS 5 handheld. Bytes 30-33 of the first are the
// desktop's IPv4 address; Frond gives the virtual router's.
const uint8_t netSyncReply1[50] = {0x12, 0x01, 0, 0, 0, 0, 0, 0, 0, 0x20, 0, 0, 0, 0x24, 0xff, 0xff, 0xff, 0xff, 0x3c, 0x00, 0x3c, 0x00,
                                   0, 0, 0, 0, 0, 0, 0, 0, 0x0a, 0x00, 0x02, 0x02, 0x04, 0x27, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
const uint8_t netSyncReply2[46] = {0x13, 0x01, 0, 0, 0, 0, 0, 0, 0, 0x20, 0, 0, 0, 0x20, 0xff, 0xff, 0xff, 0xff,
                                   0x00, 0x3c, 0x00, 0x3c, 0, 0, 0, 0, 0, 0, 0, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// Session state shared by both line types: the reader outlives the link object.
class Accepted final : public HandheldLink {
   public:
    Accepted(SyncStream& stream) : reader(stream) {}
    std::string Name() const override { return inner->Name(); }
    bool Send(const Bytes& m) override { return Pass(inner->Send(m)); }
    bool Receive(Bytes& m, int t) override { return Pass(inner->Receive(m, t)); }
    void Finish() override { inner->Finish(); }
    bool Pass(bool ok) {
        if (!ok) error = inner->error;
        return ok;
    }
    Reader reader;
    std::unique_ptr<HandheldLink> inner;
};

}  // namespace

uint16_t SlpCrc(const uint8_t* data, size_t size, uint16_t crc) {
    for (size_t i = 0; i < size; ++i) {
        crc ^= uint16_t(data[i]) << 8;
        for (int b = 0; b < 8; ++b) crc = uint16_t(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

uint8_t SlpHeaderSum(const uint8_t* h) {
    uint8_t sum = 0;
    for (int i = 0; i < 9; ++i) sum = uint8_t(sum + h[i]);
    return sum;
}

Bytes SlpPacket(uint8_t dest, uint8_t src, uint8_t type, uint8_t xid, const Bytes& body) {
    Bytes p{0xbe, 0xef, 0xed, dest, src, type, uint8_t(body.size() >> 8), uint8_t(body.size()), xid, 0};
    p[9] = SlpHeaderSum(p.data());
    p.insert(p.end(), body.begin(), body.end());
    const uint16_t crc = SlpCrc(p.data(), p.size());
    p.push_back(uint8_t(crc >> 8)), p.push_back(uint8_t(crc));
    return p;
}

std::unique_ptr<HandheldLink> AcceptHandheld(SyncStream& stream, Transport transport, const std::atomic<bool>& stop,
                                             std::string& error) {
    error.clear();
    auto accepted = std::make_unique<Accepted>(stream);
    Reader& in = accepted->reader;
    const auto forever = Clock::now() + std::chrono::hours(24 * 365);
    for (;;) {
        if (!in.Need(1, forever, &stop)) {
            if (in.closed) error = "closed";
            return nullptr;
        }
        const uint8_t first = in.Peek()[0];
        if (first == 0xbe) {
            // SLP: wait for the CMP wakeup on socket 3 (loopback pings come first).
            SlpFrame f;
            if (!ReadSlp(in, f, Clock::now() + std::chrono::seconds(5), &stop)) {
                if (in.closed) return error = "closed", nullptr;
                continue;
            }
            if (f.type != slpTypePadp || f.dest != slpSocketDlp || f.body.size() < 4 + 10 || f.body[0] != padpData) continue;
            const uint8_t* cmp = f.body.data() + 4;
            if (cmp[0] != 1) continue;  // not a wakeup
            auto padp = std::make_unique<PadpLink>(stream, in, uint8_t(0xff));
            // Acknowledge the wakeup, then answer it on its own ID. A serial line moves to the
            // handheld's top rate, as a desktop does; USB and the modem keep theirs.
            Bytes ack{padpAck, f.body[1], f.body[2], f.body[3]};
            const auto packet = SlpPacket(f.src, f.dest, slpTypePadp, f.xid, ack);
            stream.Write(packet.data(), packet.size());
            const uint32_t baud = Get32(cmp + 6);
            const bool change = transport == Transport::serial && baud > 9600;
            Bytes init{2, uint8_t(change ? 0x80 : 0), 1, 1, 0, 0, uint8_t(baud >> 24), uint8_t(baud >> 16), uint8_t(baud >> 8), uint8_t(baud)};
            if (!padp->SendAs(init, f.xid)) {
                error = "The handheld didn't take the HotSync answer: " + padp->error;
                return nullptr;
            }
            accepted->inner = std::move(padp);
            return accepted;
        }
        if (first == 0x01) {
            // NetSync: the handheld's first message is type 0x90 on ID 0xff.
            if (!in.Need(6, Clock::now() + std::chrono::seconds(5), &stop)) {
                if (in.closed) return error = "closed", nullptr;
                in.Skip(in.Ready());
                continue;
            }
            const uint8_t* p = in.Peek();
            const size_t size = Get32(p + 2);
            if (p[1] != 0xff || size < 1 || size > 256) {
                in.Skip(1);
                continue;
            }
            if (!in.Need(6 + size, Clock::now() + std::chrono::seconds(5), &stop)) {
                if (in.closed) return error = "closed", nullptr;
                in.Skip(1);
                continue;
            }
            const uint8_t kind = in.Peek()[6];
            in.Skip(6 + size);
            if (kind != 0x90) continue;
            auto net = std::make_unique<NetSyncLink>(stream, in, transport);
            Bytes message;
            if (!net->SendAs(Bytes(netSyncReply1, netSyncReply1 + sizeof(netSyncReply1)), 0xff) || !net->Receive(message, 10000) ||
                message.empty() || message[0] != 0x92 ||
                !net->SendAs(Bytes(netSyncReply2, netSyncReply2 + sizeof(netSyncReply2)), 0xff) || !net->Receive(message, 10000) ||
                message.empty() || message[0] != 0x93) {
                error = "The NetSync handshake failed" + (net->error.empty() ? std::string(".") : ": " + net->error);
                return nullptr;
            }
            net->xid = 0;
            accepted->inner = std::move(net);
            return accepted;
        }
        in.Skip(1);  // line noise before the handheld starts
    }
}

}  // namespace frondSync
