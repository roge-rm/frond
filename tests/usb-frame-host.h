#ifndef FROND_USB_FRAME_HOST_H
#define FROND_USB_FRAME_HOST_H

// Frame-scheduled full-speed USB host for the PXA27x UDC model. Test host, no real USB,
// no threads: it owns emulated bus time and runs the guest through `runUntil` between
// bus events. Rules it keeps:
// - SOF every 1 ms (UDCFNR, IRSOF). A transaction never crosses the end of a frame.
// - Every event lands after its own bus time: SETUP and OUT data are delivered, and an
//   IN is ACKed, only once the whole packet has been on the wire.
// - A NAKed token is retried after nakRetryNs in the same frame, or in the next one.
// - A control transfer starts on a frame boundary at least 1 ms after the previous one
//   finished, as a host driver's completion and resubmission take. This keeps a SETUP
//   out of the window between the guest releasing the status stage and its UDCISR0 W1C.
// - Tokens go to `address`; a device answering another address gets Timeout.
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>
#include "pxa270_UDC.h"

struct UsbFrameHost {
    enum class Status { Ok, Stall, Timeout, Error };
    struct Result {
        Status status = Status::Error;
        std::vector<uint8_t> data;
        unsigned packets = 0, naks = 0; // ACKed transactions (control: data and status stages)
        bool zlp = false;
        uint64_t startNs = 0, endNs = 0;
        std::string error;
    };
    static constexpr uint64_t kFrameNs = 1000000;
    // Full-speed bit times: 12 Mbit/s.
    static uint64_t bitsNs(uint64_t bits) { return (bits * 1000 + 11) / 12; }
    // Token (SYNC, PID, ADDR, ENDP, CRC5: 32), DATAx (SYNC, PID, CRC16: 32 + 8 * bytes), handshake
    // (16), a 2-bit EOP after each packet and two 2-bit inter-packet gaps. No bit stuffing.
    static uint64_t transactionBits(unsigned bytes) { return 32 + 32 + 8ull * bytes + 16 + 3 * 2 + 2 * 2; }
    static constexpr uint64_t kNakBits = 32 + 16 + 2 * 2 + 2;     // token, NAK/STALL handshake
    static constexpr uint64_t kTimeoutBits = 32 + 2 + 18;         // token, bus turnaround timeout
    static constexpr uint64_t kSofBits = 32 + 2;
    static constexpr uint64_t kEofGuardNs = 50000;                // keep clear of the frame end

    std::function<Pxa270Udc *()> udc;            // fetched on each use: a restore may move it
    std::function<void(uint64_t nowNs)> runUntil; // run the guest up to this emulated time
    uint64_t now = 0, nextControlNs = 0, nakRetryNs = 10000;
    uint8_t address = 0, maxPacket0 = 16;
    unsigned frames = 0;
    // Optional hook run after every frame boundary (for example to snapshot mid-transfer).
    std::function<void()> onFrame;
    // Optional record of every transaction's bus interval [start, end) in ns.
    std::vector<std::pair<uint64_t, uint64_t>> *log = nullptr;

    UsbFrameHost(std::function<Pxa270Udc *()> udcFn, std::function<void(uint64_t)> run, uint64_t start = 0)
        : udc(std::move(udcFn)), runUntil(std::move(run)), now(start) {}

    // Runs the guest for `ns`, issuing a SOF at each 1 ms boundary.
    void advance(uint64_t ns) {
        const uint64_t end = now + ns;
        while (now < end) {
            const uint64_t boundary = (now / kFrameNs + 1) * kFrameNs;
            if (boundary > end) { now = end; runUntil(now); break; }
            now = boundary; runUntil(now);
            pxa270UdcHostFrame(udc());
            ++frames;
            if (onFrame) onFrame();
            const uint64_t sof = bitsNs(kSofBits);
            if (end < now + sof) { now += sof; runUntil(now); return; }
            now += sof; runUntil(now);
        }
    }
    void toNextFrame() { advance((now / kFrameNs + 1) * kFrameNs + bitsNs(kSofBits) - now); }
    // Bus time for one transaction; waits for the next frame if it would cross EOF.
    void bus(uint64_t bits) {
        const uint64_t ns = bitsNs(bits);
        if (now % kFrameNs + ns + kEofGuardNs > kFrameNs) toNextFrame();
        if (log) log->push_back({now, now + ns});
        advance(ns);
    }
    bool addressed() { return pxa270UdcHostAddress(udc()) == address; }
    void reset() {
        // 10 ms of SE0 then recovery, as a root hub port reset. Address returns to 0.
        advance(10 * kFrameNs);
        pxa270UdcHostReset(udc());
        address = 0;
        advance(10 * kFrameNs);
        nextControlNs = now;
    }

    // Token helpers. Each returns Ok on ACK, retrying NAKs until `deadline`.
    Status setup(const uint8_t request[8]) {
        bus(transactionBits(8));
        if (!addressed()) return Status::Timeout;
        auto r = pxa270UdcHostSetup(udc(), request);
        return r == Pxa270UdcHostResult::Ack ? Status::Ok : Status::Error;
    }
    Status out(uint8_t endpoint, const uint8_t *data, unsigned size, uint64_t deadline, Result &result) {
        for (;;) {
            if (!addressed()) { bus(kTimeoutBits); return Status::Timeout; }
            bus(transactionBits(size));
            auto r = endpoint ? pxa270UdcHostEpOut(udc(), endpoint, data, size) : pxa270UdcHostOut(udc(), data, size);
            if (r == Pxa270UdcHostResult::Ack) { ++result.packets; return Status::Ok; }
            if (r == Pxa270UdcHostResult::Stall) return Status::Stall;
            if (r == Pxa270UdcHostResult::Timeout) return Status::Timeout;
            if (r != Pxa270UdcHostResult::Nak) return Status::Error;
            ++result.naks;
            if (now >= deadline) return Status::Timeout;
            advance(nakRetryNs);
        }
    }
    Status in(uint8_t endpoint, std::vector<uint8_t> &packet, uint64_t deadline, Result &result) {
        for (;;) {
            if (!addressed()) { bus(kTimeoutBits); return Status::Timeout; }
            unsigned size = 0;
            auto r = pxa270UdcHostPeekIn(udc(), endpoint, &size);
            if (r == Pxa270UdcHostResult::Ack) {
                // The packet goes out now; the device sees the ACK at the end of it.
                bus(transactionBits(size));
                uint8_t data[64]; unsigned got = 0;
                r = endpoint ? pxa270UdcHostEpIn(udc(), endpoint, data, &got) : pxa270UdcHostIn(udc(), data, &got);
                if (r == Pxa270UdcHostResult::Ack) {
                    packet.assign(data, data + got); ++result.packets; return Status::Ok;
                }
            } else {
                bus(r == Pxa270UdcHostResult::Timeout ? kTimeoutBits : kNakBits);
                if (r == Pxa270UdcHostResult::Stall) {
                    // Let the model see the handshake (clears FST, sets SST, ends EP0 transfer).
                    uint8_t data[64]; unsigned got = 0;
                    if (endpoint) pxa270UdcHostEpIn(udc(), endpoint, data, &got); else pxa270UdcHostIn(udc(), data, &got);
                    return Status::Stall;
                }
                if (r == Pxa270UdcHostResult::Timeout) return Status::Timeout;
                if (r != Pxa270UdcHostResult::Nak) return Status::Error;
            }
            ++result.naks;
            if (now >= deadline) return Status::Timeout;
            advance(nakRetryNs);
        }
    }

    // One control transfer on EP0: SETUP, data stage of maxPacket0 packets, status stage.
    Result control(const uint8_t request[8], const std::vector<uint8_t> &outData = {},
                   uint64_t timeoutNs = 5000 * kFrameNs) {
        Result result;
        if (nextControlNs > now) advance(nextControlNs - now);
        toNextFrame();
        result.startNs = now;
        const uint64_t deadline = now + timeoutNs;
        const unsigned length = request[6] | request[7] << 8;
        const bool read = request[0] & 0x80;
        auto finish = [&](Status status, const char *error = "") {
            result.status = status; result.error = error; result.endNs = now;
            nextControlNs = now + kFrameNs;
            return result;
        };
        Status s = setup(request);
        if (s != Status::Ok) return finish(s, "SETUP");
        if (read && length) {
            for (;;) {
                std::vector<uint8_t> packet;
                s = in(0, packet, deadline, result);
                if (s != Status::Ok) return finish(s, "data IN");
                result.data.insert(result.data.end(), packet.begin(), packet.end());
                if (packet.empty()) result.zlp = true;
                if (result.data.size() > length) return finish(Status::Error, "more than wLength");
                if (packet.size() < maxPacket0 || result.data.size() == length) break;
            }
            s = out(0, nullptr, 0, deadline, result);
            if (s != Status::Ok) return finish(s, "status OUT");
        } else {
            if (!read && outData.size() != length) return finish(Status::Error, "OUT data size");
            for (unsigned sent = 0; !read && sent < length;) {
                const unsigned n = std::min<unsigned>(maxPacket0, length - sent);
                s = out(0, outData.data() + sent, n, deadline, result);
                if (s != Status::Ok) return finish(s, "data OUT");
                sent += n;
            }
            std::vector<uint8_t> packet;
            s = in(0, packet, deadline, result);
            if (s != Status::Ok) return finish(s, "status IN");
            if (!packet.empty()) return finish(Status::Error, "status IN carried data");
        }
        return finish(Status::Ok);
    }
    Result request(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t length,
                   const std::vector<uint8_t> &outData = {}) {
        const uint8_t r[8] = {type, req, uint8_t(value), uint8_t(value >> 8), uint8_t(index),
                              uint8_t(index >> 8), uint8_t(length), uint8_t(length >> 8)};
        return control(r, outData);
    }
    Result getDescriptor(uint8_t type, uint8_t index, uint16_t length, uint16_t language = 0) {
        return request(0x80, 6, uint16_t(type << 8 | index), language, length);
    }
    // SET_ADDRESS, then switch this host's tokens to the new address after the status stage.
    Result setAddress(uint8_t newAddress) {
        auto r = request(0x00, 5, newAddress, 0, 0);
        if (r.status == Status::Ok) {
            advance(2 * kFrameNs); // USB 2.0 9.2.6.3: 2 ms SET_ADDRESS recovery
            address = newAddress;
        }
        return r;
    }

    // Bulk OUT transfer: maxPacket packets, then a ZLP if asked and the length is a multiple.
    Result bulkOut(uint8_t endpoint, const std::vector<uint8_t> &data, unsigned maxPacket, bool zlp,
                   uint64_t timeoutNs = 1000 * kFrameNs) {
        Result result; result.startNs = now;
        const uint64_t deadline = now + timeoutNs;
        size_t sent = 0;
        do {
            const unsigned n = unsigned(std::min<size_t>(maxPacket, data.size() - sent));
            const Status s = out(endpoint, data.data() + sent, n, deadline, result);
            if (s != Status::Ok) { result.status = s; result.endNs = now; return result; }
            sent += n;
            if (!n) result.zlp = true;
            if (sent == data.size() && (n < maxPacket || !zlp || !n)) break;
        } while (true);
        result.status = Status::Ok; result.endNs = now;
        return result;
    }
    // Bulk IN transfer: reads until a short packet or `length` bytes.
    Result bulkIn(uint8_t endpoint, unsigned length, unsigned maxPacket, uint64_t timeoutNs = 1000 * kFrameNs) {
        Result result; result.startNs = now;
        const uint64_t deadline = now + timeoutNs;
        while (result.data.size() < length) {
            std::vector<uint8_t> packet;
            const Status s = in(endpoint, packet, deadline, result);
            if (s != Status::Ok) { result.status = s; result.endNs = now; return result; }
            result.data.insert(result.data.end(), packet.begin(), packet.end());
            if (packet.empty()) result.zlp = true;
            if (packet.size() < maxPacket) break;
        }
        result.status = Status::Ok; result.endNs = now;
        return result;
    }
};
#endif
