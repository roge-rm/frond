// The rx1950's ACX100 Wi-Fi model and the emulated access point on their own: the register window, the
// firmware download and start, the command mailbox, scans, the TX and RX rings, and authentication,
// association and the data bridge through the access point. Builds with the S3C2442 sources only.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "acx100.h"
#include "machine_state_archive.h"
#include "wifi_access_point.h"

namespace {
    // Host layout, as the driver sets it up: rings and the block pool after the stand-in's code end.
    constexpr uint32_t queueStart = 0xc000, txRing = queueStart, txCount = 4, rxRing = txRing + txCount * 0x30,
                       rxCount = 4, queueEnd = rxRing + rxCount * 0x34, poolStart = 0xc400, blockSize = 0x100,
                       txPool = poolStart, txBlocks = 16, rxPool = poolStart + txBlocks * blockSize, rxBlocks = 32;

    struct Rig {
        Acx100 chip;
        WifiAccessPoint ap;
        uint64_t now = 0;
        std::vector<std::vector<uint8_t>> ethernet;

        Rig() {
            chip.airSend = [this](const uint8_t* f, size_t n, unsigned ch, uint64_t t) { ap.stationFrame(f, n, ch, t); };
            chip.airTuned = [this](unsigned ch, uint64_t t) { ap.tuned(ch, t); };
            ap.toStation = [this](const uint8_t* f, size_t n, unsigned ch, uint64_t t) { return chip.airReceive(f, n, ch, t); };
            ap.ethernetOut = [this](const uint8_t* f, size_t n) { ethernet.emplace_back(f, f + n); };
        }
        uint32_t reg(uint32_t offset) {
            if (offset < 0x20) return chip.read(offset, 4, now);
            chip.write(0x04, 4, offset, now);
            return chip.read(0x08, 4, now);
        }
        void setReg(uint32_t offset, uint32_t value) {
            if (offset < 0x20) return chip.write(offset, 4, value, now);
            chip.write(0x04, 4, offset, now);
            chip.write(0x08, 4, value, now);
        }
        uint32_t mem32(uint32_t a) {
            setReg(0x14, a);
            return reg(0x18);
        }
        void setMem32(uint32_t a, uint32_t v) {
            setReg(0x14, a);
            setReg(0x18, v);
        }
        // Byte access as the driver's helpers do it: the word at the address, its byte picked out.
        uint8_t mem8(uint32_t a) { return uint8_t(mem32(a) >> (8 * (a & 3))); }
        void setMem8(uint32_t a, uint8_t v) {
            uint32_t w = mem32(a);
            w = (w & ~(0xffu << (8 * (a & 3)))) | uint32_t(v) << (8 * (a & 3));
            setMem32(a, w);
        }
        uint16_t mem16(uint32_t a) { return uint16_t(mem32(a) >> (8 * (a & 2))); }
        void setMem16(uint32_t a, uint16_t v) {
            uint32_t w = mem32(a);
            w = (w & ~(0xffffu << (8 * (a & 2)))) | uint32_t(v) << (8 * (a & 2));
            setMem32(a, w);
        }
        void write(uint32_t a, const std::vector<uint8_t>& bytes) {
            for (size_t i = 0; i < bytes.size(); ++i) setMem8(uint32_t(a + i), bytes[i]);
        }
        std::vector<uint8_t> read(uint32_t a, size_t n) {
            std::vector<uint8_t> out(n);
            for (size_t i = 0; i < n; ++i) out[i] = mem8(uint32_t(a + i));
            return out;
        }
        void run(uint64_t until) {
            for (;;) {
                const uint64_t next = std::min(chip.nextEvent(), ap.nextEvent(chip.listening() != 0));
                if (next > until) break;
                now = std::max(now, next);
                chip.advance(now);
                ap.advance(now, chip.listening() != 0);
            }
            now = until;
        }
        void ack(uint32_t bits) { setReg(0xac, bits); }
        // A command through the mailbox, waiting for completion as the driver does.
        void command(uint16_t cmd, const std::vector<uint8_t>& params) {
            const uint32_t m = reg(0x2a4);
            setMem16(m, cmd);
            setMem16(m + 2, 0);
            write(m + 4, params);
            setReg(0x7c, 1);
            assert(reg(0xa4) & Acx100::irqCommand);
            assert(mem16(m + 2) == 1);
            ack(Acx100::irqCommand);
        }
        void configure(uint16_t type, const std::vector<uint8_t>& data) {
            std::vector<uint8_t> p = {uint8_t(type), uint8_t(type >> 8), uint8_t(data.size()), uint8_t(data.size() >> 8)};
            p.insert(p.end(), data.begin(), data.end());
            command(2, p);
        }
        std::vector<uint8_t> interrogate(uint16_t type, uint16_t length) {
            command(1, {uint8_t(type), uint8_t(type >> 8), uint8_t(length), uint8_t(length >> 8)});
            return read(reg(0x2a4) + 8, length);
        }
    };

    void put32(std::vector<uint8_t>& b, uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
    }
    void put16(std::vector<uint8_t>& b, uint16_t v) {
        b.push_back(uint8_t(v));
        b.push_back(uint8_t(v >> 8));
    }

    // Powers the chip, downloads a stand-in image and waits for the firmware to start.
    void boot(Rig& r) {
        r.chip.reset();
        assert(r.reg(0x900) == Acx100::chipId);
        assert(r.reg(0x2d8) & 1);
        r.setReg(0x14, 0);
        r.setReg(0x1c, 0x10000);
        for (uint32_t i = 0; i < 64; ++i) r.setReg(0x18, 0x01020304 + i);
        r.setReg(0x1c, 0);
        assert(r.mem32(4) == 0x01020305);
        // Halfword access: the low half then the high half of one word.
        r.setReg(0x14, 0x100);
        r.chip.write(0x18, 2, 0xbeef, r.now);
        r.chip.write(0x1a, 2, 0xdead, r.now);
        assert(r.mem32(0x100) == 0xdeadbeef);
        // An unaligned address reads its whole word.
        assert(r.mem32(0x102) == 0xdeadbeef);
        r.setReg(0x98, 0);
        r.setReg(0x2d8, 0);
        assert(!r.chip.running());
        assert(!r.chip.irq());
        r.run(r.now + 2000000);
        assert(r.chip.running());
        assert(r.reg(0xa4) & Acx100::irqStarted);
        assert(r.chip.irq());
        r.ack(Acx100::irqStarted);
        assert(!r.chip.irq());
        assert(r.reg(0x2a4) == Acx100::commandMailbox);
    }

    // The memory map, queues, block size and pool, and the rings in chip memory.
    void layout(Rig& r) {
        auto map = r.interrogate(8, 0x28);
        assert(map.size() == 0x28);
        const uint32_t codeEnd = uint32_t(map[4]) | map[5] << 8 | map[6] << 16;
        assert(codeEnd == Acx100::codeEnd);
        std::vector<uint8_t> m;
        put32(m, 0);
        put32(m, codeEnd);
        put32(m, codeEnd);  // WEP start
        put32(m, 0);
        put32(m, codeEnd + 0x200);  // templates
        put32(m, 0);
        put32(m, queueStart);
        put32(m, 0);
        put32(m, poolStart);
        put32(m, 0);
        r.configure(8, m);
        map = r.interrogate(8, 0x28);
        const uint32_t poolEnd = uint32_t(map[0x24]) | map[0x25] << 8 | map[0x26] << 16;
        assert(poolEnd == Acx100::memoryBytes);
        std::vector<uint8_t> q;
        put32(q, queueEnd - queueStart);
        put32(q, rxRing);
        q.push_back(1);
        q.push_back(1);
        q.push_back(0);
        q.push_back(0);
        put32(q, queueEnd);
        put32(q, queueEnd + 8);
        put32(q, txRing);
        q.push_back(0);
        q.push_back(0);
        q.push_back(0);
        q.push_back(0);
        r.configure(3, q);
        r.configure(4, {uint8_t(blockSize), uint8_t(blockSize >> 8)});
        std::vector<uint8_t> mc;
        put32(mc, 0);
        put32(mc, 0);
        put32(mc, rxPool);
        put32(mc, txPool);
        put16(mc, rxBlocks);
        put16(mc, txBlocks);
        r.configure(5, mc);
        for (uint32_t i = 0; i < txCount; ++i) {
            const uint32_t d = txRing + i * 0x30;
            r.setMem32(d, txRing + ((i + 1) % txCount) * 0x30);
            r.setMem8(d + 0x24, 0x82);
        }
        for (uint32_t i = 0; i < rxCount; ++i) {
            const uint32_t d = rxRing + i * 0x34;
            r.setMem32(d, rxRing + ((i + 1) % rxCount) * 0x34);
            r.setMem8(d + 0x28, 0);
        }
    }

    // The next filled RX descriptor's 802.11 frame, handed back to the chip as the driver does; empty with none.
    uint32_t rxNext = rxRing;
    std::vector<uint8_t> receive(Rig& r) {
        const uint32_t d = rxNext;
        const uint8_t ctl = r.mem8(d + 0x28);
        if ((ctl & 0xc0) != 0xc0) return {};
        const uint16_t length = r.mem16(d + 0x10);
        uint32_t block = r.mem32(d + 8);
        std::vector<uint8_t> bytes;
        while (block && bytes.size() < 12u + length) {
            auto part = r.read(block + 4, blockSize - 4);
            bytes.insert(bytes.end(), part.begin(), part.end());
            const uint32_t link = r.mem32(block);
            block = (link & 0x2000000) ? 0 : (link & 0x7ffff) << 5;
        }
        assert(bytes.size() >= 12u + length);
        assert(uint16_t(bytes[0] | (bytes[1] & 0x0f) << 8) == length);
        r.setMem8(d + 0x28, uint8_t((ctl & 0x57) | 0x28));
        r.setReg(0x7c, 8);
        assert(r.mem8(d + 0x28) == 0xc8);
        r.setMem8(d + 0x28, 0);
        rxNext = r.mem32(d);
        return std::vector<uint8_t>(bytes.begin() + 12, bytes.begin() + 12 + length);
    }

    uint32_t txNext = txRing, txBlock = 0;
    void transmit(Rig& r, const std::vector<uint8_t>& frame) {
        const uint32_t d = txNext;
        assert(r.mem8(d + 0x24) & 0x80);
        // One block per descriptor, taken round the TX pool.
        const uint32_t block = txPool + (txBlock++ % txBlocks) * blockSize;
        assert(frame.size() <= blockSize - 4);
        r.setMem32(block, 0x2000000);
        r.write(block + 4, frame);
        r.setMem32(d + 8, block);
        r.setMem16(d + 0x10, uint16_t(frame.size()));
        r.setMem8(d + 0x24, 0x02);
        r.setReg(0x7c, 4);
        assert(r.reg(0xa4) & Acx100::irqTxComplete);
        r.ack(Acx100::irqTxComplete);
        assert(r.mem8(d + 0x24) & 0x40);
        r.setMem8(d + 0x24, 0x82);
        txNext = r.mem32(d);
    }

    std::vector<uint8_t> header(uint8_t fc0, uint8_t fc1, const uint8_t* a1, const uint8_t* a2, const uint8_t* a3) {
        std::vector<uint8_t> f = {fc0, fc1, 0, 0};
        f.insert(f.end(), a1, a1 + 6);
        f.insert(f.end(), a2, a2 + 6);
        f.insert(f.end(), a3, a3 + 6);
        f.push_back(0);
        f.push_back(0);
        return f;
    }

    void registerTests() {
        Rig r;
        boot(r);
        // A soft reset stops the firmware.
        r.setReg(0x00, 1);
        assert(!r.chip.running());
        // A start with nothing downloaded never reports in.
        r.chip.reset();
        r.setReg(0x2d8, 0);
        r.run(r.now + 5000000);
        assert(!r.chip.running());
        // Radio registers hold what's written.
        boot(r);
        r.setReg(0x268, 0x11);
        r.setReg(0x26c, 0x5a);
        r.setReg(0x270, 1);
        assert(r.reg(0x270) == 0);
        r.setReg(0x26c, 0);
        r.setReg(0x270, 2);
        assert(r.reg(0x26c) == 0x5a);
    }

    void scanTests() {
        Rig r;
        boot(r);
        layout(r);
        rxNext = rxRing;
        // A station address the driver reads.
        // The station address, last byte first.
        const auto id = r.interrogate(0x1001, 6);
        for (int i = 0; i < 6; ++i) assert(id[i] == r.chip.mac()[5 - i]);
        // A probe request template, then an active scan at 30 TU a channel.
        static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        auto probe = header(0x40, 0, bcast, r.chip.mac(), bcast);
        probe.push_back(0);
        probe.push_back(0);
        std::vector<uint8_t> t;
        put16(t, uint16_t(probe.size()));
        t.insert(t.end(), probe.begin(), probe.end());
        r.command(0x16, t);
        std::vector<uint8_t> s;
        put16(s, 1);
        put16(s, 1);
        put16(s, 0x8000);
        s.push_back(20);
        s.push_back(0);
        put16(s, 30);
        put16(s, 0);
        r.command(8, s);
        unsigned beacons = 0, probeResponses = 0;
        bool done = false;
        for (int step = 0; step < 400 && !done; ++step) {
            r.run(r.now + 1000000);
            for (auto f = receive(r); !f.empty(); f = receive(r)) {
                assert(!std::memcmp(f.data() + 10, WifiAccessPoint::bssid, 6));
                if (f[0] == 0x80) ++beacons;
                if (f[0] == 0x50) ++probeResponses;
                // SSID element after the fixed fields.
                assert(f[36] == 0 && f[37] == 5 && !std::memcmp(f.data() + 38, "Frond", 5));
            }
            if (r.reg(0xa4) & Acx100::irqScan) done = true;
        }
        assert(done);
        assert(beacons >= 1);
        assert(probeResponses == 1);
        // A passive scan hears only the beacon.
        r.ack(Acx100::irqScan | Acx100::irqRxComplete);
        s[7] = 1;
        r.command(8, s);
        probeResponses = beacons = 0;
        for (int step = 0; step < 400 && !(r.reg(0xa4) & Acx100::irqScan); ++step) {
            r.run(r.now + 1000000);
            for (auto f = receive(r); !f.empty(); f = receive(r)) {
                if (f[0] == 0x80) ++beacons;
                if (f[0] == 0x50) ++probeResponses;
            }
        }
        assert(beacons >= 1 && probeResponses == 0);
        // Off the scan and with RX off, nothing arrives.
        r.run(r.now + 500000000);
        assert(receive(r).empty());
    }

    void associationTests() {
        Rig r;
        boot(r);
        layout(r);
        rxNext = rxRing;
        txNext = txRing;
        const uint8_t* me = r.chip.mac();
        const uint8_t* ap = WifiAccessPoint::bssid;
        // Join, then RX and TX on channel 6.
        std::vector<uint8_t> join(ap, ap + 6);
        std::reverse(join.begin(), join.end());
        join.resize(0x30, 0);
        join[14] = 6;
        r.command(0x0b, join);
        r.command(3, {6});
        r.command(4, {6});
        // Beacons every 100 TU while listening.
        r.run(r.now + 250000000);
        unsigned beacons = 0;
        for (auto f = receive(r); !f.empty(); f = receive(r)) beacons += f[0] == 0x80;
        assert(beacons >= 2);
        // Association before authentication is refused.
        auto assoc = header(0x00, 0, ap, me, ap);
        put16(assoc, 0x0021);
        put16(assoc, 1);
        assoc.push_back(0);
        assoc.push_back(5);
        assoc.insert(assoc.end(), {'F', 'r', 'o', 'n', 'd'});
        transmit(r, assoc);
        r.run(r.now + 1000000);
        auto f = receive(r);
        assert(f.size() >= 30 && f[0] == 0x10 && f[26] == 9);
        assert(!r.ap.associated());
        // Open authentication.
        auto auth = header(0xb0, 0, ap, me, ap);
        put16(auth, 0);
        put16(auth, 1);
        put16(auth, 0);
        transmit(r, auth);
        r.run(r.now + 1000000);
        f = receive(r);
        while (!f.empty() && f[0] == 0x80) f = receive(r);
        assert(f.size() == 30 && f[0] == 0xb0 && f[26] == 2 && f[28] == 0 && !std::memcmp(f.data() + 4, me, 6));
        // Shared key is refused.
        auto shared = auth;
        shared[24] = 1;
        transmit(r, shared);
        r.run(r.now + 1000000);
        f = receive(r);
        while (!f.empty() && f[0] == 0x80) f = receive(r);
        assert(f.size() == 30 && f[28] == 13);
        // Association.
        transmit(r, assoc);
        r.run(r.now + 1000000);
        f = receive(r);
        while (!f.empty() && f[0] == 0x80) f = receive(r);
        assert(f.size() >= 30 && f[0] == 0x10 && f[26] == 0 && f[28] == 1 && f[29] == 0xc0);
        assert(r.ap.associated());
        // Data to the DS goes out as Ethernet II.
        static const uint8_t router[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
        auto data = header(0x08, 0x01, ap, me, router);
        data.insert(data.end(), {0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0x06});
        data.insert(data.end(), 28, 0x5a);
        transmit(r, data);
        assert(r.ethernet.size() == 1);
        const auto& e = r.ethernet[0];
        assert(e.size() == 14 + 28 && !std::memcmp(e.data(), router, 6) && !std::memcmp(e.data() + 6, me, 6));
        assert(e[12] == 0x08 && e[13] == 0x06 && e[14] == 0x5a);
        // A protected frame is dropped.
        data[1] |= 0x40;
        transmit(r, data);
        assert(r.ethernet.size() == 1);
        // Ethernet from the network comes in from the DS.
        std::vector<uint8_t> in(me, me + 6);
        in.insert(in.end(), router, router + 6);
        in.insert(in.end(), {0x08, 0x00});
        in.insert(in.end(), 40, 0x33);
        r.ap.ethernetIn(in.data(), in.size(), r.now);
        // Not for this station: dropped.
        auto other = in;
        other[5] ^= 1;
        r.ap.ethernetIn(other.data(), other.size(), r.now);
        r.run(r.now + 1000);
        f = receive(r);
        while (!f.empty() && f[0] == 0x80) f = receive(r);
        assert(f.size() == 24 + 8 + 40 && f[0] == 0x08 && f[1] == 0x02);
        assert(!std::memcmp(f.data() + 4, me, 6) && !std::memcmp(f.data() + 10, ap, 6) && !std::memcmp(f.data() + 16, router, 6));
        assert(f[30] == 0x08 && f[31] == 0x00 && f[32] == 0x33);
        f = receive(r);
        while (!f.empty() && f[0] == 0x80) f = receive(r);
        assert(f.empty());
        // A full RX ring drops frames and counts them.
        const uint64_t dropped = r.chip.rxDropped;
        for (int i = 0; i < 6; ++i) r.ap.ethernetIn(in.data(), in.size(), r.now);
        r.run(r.now + 1000);
        assert(r.chip.rxDropped > dropped);
        // Deauthentication ends the association.
        auto deauth = header(0xc0, 0, ap, me, ap);
        put16(deauth, 3);
        while (!receive(r).empty()) {}
        transmit(r, deauth);
        assert(!r.ap.associated());
    }

    void stateTests() {
        Rig a;
        boot(a);
        layout(a);
        std::vector<uint8_t> join(WifiAccessPoint::bssid, WifiAccessPoint::bssid + 6);
        join.resize(0x30, 0);
        join[14] = 6;
        a.command(0x0b, join);
        a.command(3, {6});
        a.run(a.now + 50000000);
        std::vector<uint8_t> saved;
        StateWriter w(saved);
        a.chip.state(w);
        a.ap.state(w);
        Rig b;
        StateReader rd(saved.data(), saved.size());
        b.chip.state(rd);
        b.ap.state(rd);
        assert(rd.complete());
        b.now = a.now;
        std::vector<uint8_t> again;
        StateWriter w2(again);
        b.chip.state(w2);
        b.ap.state(w2);
        assert(again == saved);
        // Both carry on the same way.
        a.run(a.now + 300000000);
        b.run(b.now + 300000000);
        assert(a.chip.rxFrames == b.chip.rxFrames && a.chip.rxFrames > 0);
        // A short or damaged state is refused.
        Rig c;
        StateReader shortReader(saved.data(), saved.size() / 2);
        c.chip.state(shortReader);
        c.ap.state(shortReader);
        assert(!shortReader.ok());
    }
}

int main() {
    registerTests();
    scanTests();
    associationTests();
    stateTests();
    std::puts("acx100: ok");
    return 0;
}
