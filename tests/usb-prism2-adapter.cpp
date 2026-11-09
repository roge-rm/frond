// Drives UsbPrism2Adapter with the byte layouts the Dana 1.5 ROM's WiFi NetIF uses, through a
// small host model with the ISP1161 executor's packet rules (toggle check, 512-byte passes,
// ZLPs ignored, a short packet ends an IN transfer).
#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include "UsbPrism2Adapter.h"

enum class R { ack, nak, stall, noAnswer };
struct Pk { uint8_t address, endpoint, direction; bool toggle, lowSpeed; unsigned requested; std::vector<uint8_t> bytes; };
struct Rp { R result = R::noAnswer; bool toggle = false; std::vector<uint8_t> bytes; };
using Bytes = std::vector<uint8_t>;

static unsigned le(const Bytes& b, size_t o) { return b[o] | (b[o + 1] << 8); }
static void w16(Bytes& b, size_t o, unsigned v) { b[o] = v; b[o + 1] = v >> 8; }

struct Host {
    UsbPrism2Adapter& d;
    unsigned addr = 0;
    bool outT = false, inT = false;  // the host's own bulk toggles
    explicit Host(UsbPrism2Adapter& dev) : d(dev) {}
    Rp send(unsigned ep, unsigned dir, bool t, unsigned req, Bytes b = {}, unsigned a = ~0u) {
        return d.packet<Rp>(Pk{uint8_t(a == ~0u ? addr : a), uint8_t(ep), uint8_t(dir), t, false, req, std::move(b)});
    }
    // Control transfer: SETUP, data stage in packets of mps, status stage. Fails on any stall.
    bool control(Bytes setup, Bytes* out = nullptr, unsigned mps = 64, unsigned a = ~0u) {
        if(send(0, 0, false, 8, setup, a).result != R::ack) return false;
        const unsigned len = le(setup, 6);
        bool t = true;
        if(setup[0] & 0x80) {
            Bytes got;
            while(got.size() < len) {
                auto r = send(0, 2, t, std::min<unsigned>(mps, len - got.size()), {}, a);
                if(r.result != R::ack) return false;
                t = !t; got.insert(got.end(), r.bytes.begin(), r.bytes.end());
                if(r.bytes.size() < mps) break;
            }
            if(out) *out = got;
            return send(0, 1, true, 0, {}, a).result == R::ack;
        }
        return send(0, 2, true, 0, {}, a).result == R::ack;
    }
    bool req(unsigned t, unsigned r, unsigned v, unsigned i, unsigned len, Bytes* out = nullptr, unsigned mps = 64, unsigned a = ~0u) {
        return control({uint8_t(t), uint8_t(r), uint8_t(v), uint8_t(v >> 8), uint8_t(i), uint8_t(i >> 8), uint8_t(len), uint8_t(len >> 8)}, out, mps, a);
    }
    // The ROM's enumeration, then the NetIF's configuration.
    void enumerate() {
        Bytes b;
        assert(req(0x80, 6, 0x100, 0, 8, &b, 8, 0) && b.size() == 8 && b[7] == 64);
        assert(req(0, 5, 2, 0, 0, nullptr, 64, 0)); addr = 2;
        assert(req(0x80, 6, 0x100, 0, 18, &b) && b.size() == 18);
        assert(le(b, 8) == 0x045e && le(b, 10) == 0x006e && b[4] == 0 && b[17] == 1);
        assert(req(0x80, 6, 0x200, 0, 4, &b) && b.size() == 4 && le(b, 2) == 32);
        assert(req(0x80, 6, 0x200, 0, 32, &b) && b.size() == 32);
        assert(b[4] == 1 && b[5] == 1 && b[13] == 2 && b[14] == 0xff && b[15] == 0xff && b[16] == 0xff);
        assert(b[9 + 9 + 2] == 0x81 && b[9 + 9 + 3] == 2 && le(b, 9 + 9 + 4) == 64);
        assert(b[9 + 9 + 7 + 2] == 0x02 && b[9 + 9 + 7 + 3] == 2 && le(b, 9 + 9 + 7 + 4) == 64);
        assert(req(0, 9, 1, 0, 0));
    }
    // ROM send 0x308146: pad to 64, one packet each, no ZLP.
    bool out(Bytes m, bool pad = true) {
        if(pad) m.resize((m.size() + 63) & ~size_t(63));
        for(size_t o = 0; o < m.size(); o += 64) {
            Bytes c(m.begin() + o, m.begin() + std::min<size_t>(o + 64, m.size()));
            auto r = send(2, 1, outT, c.size(), c);
            if(r.result != R::ack) return false;
            outT = !outT;
        }
        return true;
    }
    // Bulk IN transfer of up to len bytes in 512-byte passes. Ends on a short packet or when
    // full; empty when nothing is queued.
    Bytes in(unsigned len = 0x980) {
        Bytes got;
        for(;;) {
            for(unsigned budget = 512; budget; budget -= 64) {
                if(got.size() >= len) return got;
                const unsigned want = std::min<unsigned>(64, len - got.size());
                auto r = send(1, 2, inT, want);
                if(r.result != R::ack) return got;
                assert(r.toggle == inT && r.bytes.size() <= want);
                inT = !inT;
                got.insert(got.end(), r.bytes.begin(), r.bytes.end());
                if(r.bytes.size() % 64) return got;
            }
        }
    }
};

// What the ROM callback 0x3099ee expects for each type.
static size_t romLength(const Bytes& m) {
    unsigned t = le(m, 0);
    if(t == 0x8000 || t == 0x8003 || t == 0x8005) return le(m, 2) * 2 + 4;
    if(t == 0x8001 || t == 0x8002 || t == 0x8004) return 10;
    if(t == 0 || t == 0x2000) return le(m, 44) + 60;
    return 0;
}
static Bytes next(Host& h) {
    Bytes m = h.in();
    assert(m.size() >= 2);
    const size_t n = romLength(m);
    assert(n && (m.size() == n || (m.size() == n + 1 && n % 64 == 0)));
    assert(m.size() % 64 != 0);  // the host sees the end of every message
    m.resize(n);
    return m;
}
// ROM request builders.
static Bytes cmd(unsigned c, unsigned p0 = 0, unsigned p1 = 0, unsigned p2 = 0) {
    Bytes m(10); w16(m, 0, 1); w16(m, 2, c); w16(m, 4, p0); w16(m, 6, p1); w16(m, 8, p2); return m;
}
static Bytes wrid(unsigned rid, std::vector<unsigned> words, unsigned frmlen = 0) {
    if(!frmlen) frmlen = 1 + words.size();
    Bytes m(frmlen * 2 + 6); w16(m, 0, 2); w16(m, 2, frmlen); w16(m, 4, rid);
    for(size_t k = 0; k < words.size(); ++k) w16(m, 6 + 2 * k, words[k]);
    return m;
}
static Bytes rrid(unsigned rid) { Bytes m(6); w16(m, 0, 3); w16(m, 2, 2); w16(m, 4, rid); return m; }
static unsigned status(const Bytes& m, unsigned type) { assert(m.size() == 10 && le(m, 0) == type); return le(m, 2); }
static bool failed(unsigned s) { return (s >> 8) == 0x7f || (s >> 8) == 0x01; }

static bool doCmd(Host& h, unsigned c, unsigned p0 = 0) { assert(h.out(cmd(c, p0))); return !failed(status(next(h), 0x8001)); }
static bool doWrid(Host& h, unsigned rid, std::vector<unsigned> words, unsigned frmlen = 0) {
    assert(h.out(wrid(rid, words, frmlen))); return !failed(status(next(h), 0x8002));
}
static Bytes doRrid(Host& h, unsigned rid) {
    assert(h.out(rrid(rid))); Bytes m = next(h); assert(le(m, 0) == 0x8003 && le(m, 4) == rid); return m;
}

// ROM connect sequence 0x3089c0.
static void romConnect(Host& h, bool adhoc, const char* ssid, unsigned channel, bool wep) {
    std::vector<unsigned> s(16, 0);
    s[0] = strlen(ssid);
    for(size_t k = 0; k < strlen(ssid); ++k) s[1 + k / 2] |= unsigned(uint8_t(ssid[k])) << (8 * (k % 2));
    assert(doWrid(h, 0xfc00, {adhoc ? 0u : 1u}));
    assert(doWrid(h, 0xfc02, s, 17));
    assert(doWrid(h, 0xfc04, s, 17));
    assert(doWrid(h, 0xfc05, {0}, 2));
    assert(doWrid(h, 0xfc07, {1522}));
    assert(doWrid(h, 0xfc83, {1600}));
    assert(doWrid(h, 0xfc42, {1}));
    assert(doWrid(h, 0xfc03, {channel}));
    assert(doWrid(h, 0xfc84, {15}));
    assert(doWrid(h, 0xfc09, {0}));
    if(wep) {
        assert(doWrid(h, 0xfc24, {0x3412, 0x7856, 0xab90}, 4));
        assert(doWrid(h, 0xfc25, {1, 2, 3}, 4));
        assert(doWrid(h, 0xfc26, {1, 2, 3, 4, 5, 6, 7}, 8));
        assert(doWrid(h, 0xfc27, {1, 2, 3, 4, 5, 6, 7}, 8));
        assert(doWrid(h, 0xfc23, {2}));
        assert(doWrid(h, 0xfc2a, {2}));
        assert(doWrid(h, 0xfc28, {3}));
    }
}
static bool linkStatusIs(const Bytes& m, unsigned want) { return le(m, 0) == 0x8000 && le(m, 2) == 2 && le(m, 4) == 0xf200 && le(m, 6) == want; }

// ROM scan 0x3084ee: returns the SSIDs kept, using the ROM's count and stride arithmetic.
static std::vector<std::string> romScan(Host& h, std::vector<int>* channels = nullptr) {
    std::vector<std::string> found;
    for(unsigned ch = 1; ch <= 13; ++ch) {
        assert(doWrid(h, 0xfce5, {1u << (ch - 1), 3, 0}, 4));
        Bytes m = doRrid(h, 0xfd89);
        assert(m.size() <= 506);
        const unsigned frmlen = le(m, 2);
        const unsigned count = (frmlen - 3) >> 5;
        const unsigned stride = le(m, 6) & ~1u;
        if(count) assert(stride == 64);
        for(unsigned k = 0; k < count && k < 20; ++k) {
            const size_t o = 10 + k * stride;
            assert(o + 64 <= m.size());
            if(m[o + 18] == 0) continue;
            found.emplace_back((const char*)&m[o + 18], le(m, o + 16));
            if(channels) channels->push_back(le(m, o));
        }
    }
    return found;
}

// An Ethernet frame the guest TX path builds (0x307e52): 60 bytes of header, 802.3 + SNAP.
static Bytes romTx(const uint8_t* dst, const uint8_t* src, unsigned ethertype, const Bytes& payload) {
    const size_t n = payload.size() + 8;
    Bytes m(60 + n, 0);
    std::memcpy(&m[46], dst, 6); std::memcpy(&m[52], src, 6);
    m[58] = n >> 8; m[59] = n;
    const uint8_t snap[6] = {0xaa, 0xaa, 3, 0, 0, 0};
    std::memcpy(&m[60], snap, 6);
    m[66] = ethertype >> 8; m[67] = ethertype;
    std::copy(payload.begin(), payload.end(), m.begin() + 68);
    return m;
}

int main() {
    UsbPrism2Adapter dev;
    const UsbPrism2Adapter::Mac mac = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    dev.setMac(mac);
    std::vector<Bytes> sent;
    dev.setTxSink([&](const Bytes& f) { sent.push_back(f); });
    Host h(dev);

    // Enumeration as the ROM does it, wrong address ignored, repeated SET_CONFIGURATION.
    assert(h.send(0, 0, false, 8, {0x80, 6, 0, 1, 0, 0, 8, 0}, 9).result == R::noAnswer);
    h.enumerate();
    assert(dev.busAddress() == 2);
    Bytes b;
    assert(h.req(0x80, 8, 0, 0, 1, &b) && b == Bytes{1});
    assert(h.req(0x82, 0, 0, 0x81, 2, &b) && b == (Bytes{0, 0}));
    assert(h.req(0x81, 10, 0, 0, 1, &b) && b == Bytes{0});
    // Unsupported requests stall after the SETUP is accepted, and the next SETUP recovers.
    assert(!h.req(0x80, 6, 0x300, 0, 4));
    assert(!h.req(0x80, 6, 0x101, 0, 18));
    assert(!h.req(0x80, 6, 0x600, 0, 10));
    assert(!h.req(0x40, 0x42, 0, 0, 0));
    assert(!h.req(0x82, 0, 0, 0x83, 2));
    assert(h.req(0x80, 8, 0, 0, 1, &b));

    // Init as the NetIF does: Initialize, then the connect writes, Enable, wait for LinkStatus.
    assert(doCmd(h, 0x00));
    assert(!dev.isEnabled());
    romConnect(h, false, "Frond", 6, true);
    // Toggles carry on through a repeated SET_CONFIGURATION (same value) on both bulk pipes.
    assert(h.req(0, 9, 1, 0, 0));
    assert(doRrid(h, 0xfc01).size() == 4 + 2 + 6 && !std::memcmp(&doRrid(h, 0xfc01)[6], mac.data(), 6));
    assert(doCmd(h, 0x01, 0));
    Bytes ls = next(h);
    assert(linkStatusIs(ls, 1) && dev.linkUp() && dev.isEnabled());
    assert(dev.queuedMessages() == 0);

    // Reads after connect.
    {
        Bytes m = doRrid(h, 0xfd41);
        assert(le(m, 2) == 18 && le(m, 6) == 5 && !std::memcmp(&m[8], "Frond", 5));
        m = doRrid(h, 0xfd42);
        assert(le(m, 2) == 4 && m[6] == 0x02 && m[7] == 0x46);
        m = doRrid(h, 0xfd43);
        assert(le(m, 6) == 92 && le(m, 8) == 154 && le(m, 10) == 27);
        const unsigned q = le(m, 6) * 108 / 100, s = (le(m, 8) - 27) * 100 / 127;
        assert(q == 99 && s == 100);
        m = doRrid(h, 0xfd44); assert(le(m, 6) == 8);
        m = doRrid(h, 0xfdc1); assert(le(m, 6) == 6);
        m = doRrid(h, 0xfc84); assert(le(m, 6) == 15);
        m = doRrid(h, 0xfc24); assert(le(m, 2) == 8 && le(m, 6) == 0x3412 && le(m, 10) == 0xab90);
        m = doRrid(h, 0xfc02); assert(le(m, 2) == 18 && le(m, 6) == 5 && !std::memcmp(&m[8], "Frond", 5));
        // Unknown RID: an empty record, no error status in the format.
        m = doRrid(h, 0xfd00); assert(m.size() == 6 && le(m, 2) == 1);
    }
    // CommTallies by Inquire: CMDRESP then an info frame ROM copies 0x1fa bytes of.
    assert(doCmd(h, 0x11, 0xf100));
    {
        Bytes m = next(h);
        assert(le(m, 0) == 0x8000 && le(m, 2) == 43 && le(m, 4) == 0xf100 && m.size() == 90);
    }
    assert(doCmd(h, 0x11, 0xf200));
    assert(linkStatusIs(next(h), 1));
    assert(!doCmd(h, 0x11, 0xf777));

    // Scan: only the channel 6 mask finds "Frond"; ROM arithmetic for count and stride.
    {
        std::vector<int> ch;
        auto found = romScan(h, &ch);
        assert(found.size() == 1 && found[0] == "Frond" && ch == std::vector<int>{6});
        assert(doWrid(h, 0xfce5, {1u << 5, 3, 0}, 4));
        Bytes m = doRrid(h, 0xfd89);
        assert(le(m, 2) == 35 && m.size() == 4 + 70 && m.size() + 0 <= 506);
        assert(le(m, 6) == 64 && le(m, 10) == 6);
        assert(!std::memcmp(&m[10 + 6], "\x02\x46\x52\x4f\x4e\x44", 6));
        assert(le(m, 10 + 14) == 1);  // ESS only, no privacy bit
        assert(le(m, 10 + 60) == 110);
    }

    // TX: 802.3 + SNAP in, Ethernet II out.
    {
        const uint8_t dst[6] = {0x52, 0x55, 0x0a, 0, 2, 2}, src[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
        Bytes payload(46); for(size_t k = 0; k < payload.size(); ++k) payload[k] = k;
        Bytes frame = romTx(dst, src, 0x0806, payload);
        assert(frame.size() == 60 + 8 + 46);
        assert(h.out(frame));
        assert(sent.size() == 1 && dev.txFrames() == 1);
        Bytes want(dst, dst + 6); want.insert(want.end(), src, src + 6);
        want.push_back(0x08); want.push_back(0x06); want.insert(want.end(), payload.begin(), payload.end());
        assert(sent[0] == want);
        // The largest frame the ROM sends: 1500 byte payload, 1568 bytes.
        Bytes big(1500, 0x5a);
        frame = romTx(dst, src, 0x0800, big);
        assert(frame.size() == 1568 && h.out(frame));
        assert(sent.size() == 2 && sent[1].size() == 1514 && sent[1][14] == 0x5a);
        // A frame whose total is a multiple of 64 (no ZLP, framed by the header).
        Bytes p2(64 * 2 - 68, 7);
        frame = romTx(dst, src, 0x0800, p2);
        assert(frame.size() == 128 && h.out(frame) && sent.size() == 3 && sent[2].size() == 14 + p2.size());
        // Two messages back to back in separate transfers stay in step.
        assert(h.out(cmd(0x11, 0xf200)) && h.out(rrid(0xfc01)));
        assert(status(next(h), 0x8001) == 0x11);
        assert(linkStatusIs(next(h), 1));
        assert(le(next(h), 0) == 0x8003);
    }

    // RX: Ethernet II in, ROM parse out.
    {
        auto rx = [&](size_t payload) {
            Bytes e(14 + payload);
            const uint8_t d[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}, s[6] = {0x52, 0x55, 0x0a, 0, 2, 2};
            std::memcpy(&e[0], d, 6); std::memcpy(&e[6], s, 6); e[12] = 0x08; e[13] = 0x00;
            for(size_t k = 0; k < payload; ++k) e[14 + k] = uint8_t(k * 3);
            return e;
        };
        for(size_t payload : {size_t(46), size_t(124), size_t(1500), size_t(60)}) {
            Bytes e = rx(payload);
            assert(dev.receive(e.data(), e.size()));
            Bytes m = h.in();
            const size_t total = 60 + 8 + payload;
            assert(m.size() == total + (total % 64 == 0 ? 1 : 0));
            assert(payload != 124 || m.size() == 193);  // 192 bytes of message plus the pad byte
            assert(le(m, 0) == 0x2000 && m[14] == 0x08 && m[15] == 0x02);
            assert(romLength(m) == total);
            // ROM parse: data frame bit, SNAP stripped, MACs at +46, data from +66, len data_len-6.
            assert(m[14] & 8 && (le(m, 0) & 0x6000));
            const size_t dl = le(m, 44);
            assert(dl == 8 + payload && m[58] == ((8 + payload) >> 8) && m[59] == uint8_t(8 + payload));
            Bytes out(m.begin() + 46, m.begin() + 58);
            out.insert(out.end(), m.begin() + 66, m.begin() + 66 + dl - 6);
            assert(out == e);
        }
        // Queue limit: 32 frames, then dropped; nothing more once the guest reads.
        Bytes e = rx(46);
        unsigned accepted = 0;
        for(unsigned k = 0; k < 40; ++k) accepted += dev.receive(e.data(), e.size());
        assert(accepted == 32 && dev.rxDropped() == 8);
        for(unsigned k = 0; k < 32; ++k) assert(h.in().size() == 60 + 8 + 46);
        assert(h.in().empty());
        assert(!dev.receive(e.data(), 13));
        assert(!dev.receive(e.data(), 1515));
    }

    // Bad framing is stalled and the pipe halts until CLEAR_FEATURE; the bus state survives.
    {
        auto halted = [&](Bytes m) { m.resize(64); return h.send(2, 1, h.outT, 64, m).result == R::stall; };
        assert(halted([] { Bytes m(10); w16(m, 0, 7); return m; }()));          // unknown type
        assert(h.send(2, 1, h.outT, 64, Bytes(64)).result == R::stall);        // still halted
        assert(h.req(0x82, 0, 0, 0x02, 2, &b) && b[0] == 1);
        assert(h.req(2, 1, 0, 2, 0)); h.outT = false;                           // clears halt and toggle
        assert(h.req(0x82, 0, 0, 0x02, 2, &b) && b[0] == 0);
        assert(halted([] { Bytes m(64); w16(m, 0, 2); w16(m, 2, 0); return m; }()));
        assert(h.req(2, 1, 0, 2, 0)); h.outT = false;
        assert(halted([] { Bytes m(64); w16(m, 0, 2); w16(m, 2, 30); return m; }()));
        assert(h.req(2, 1, 0, 2, 0)); h.outT = false;
        assert(halted([] { Bytes m(64); w16(m, 0, 3); w16(m, 2, 5); return m; }()));
        assert(h.req(2, 1, 0, 2, 0)); h.outT = false;
        assert(halted([] { Bytes m(64); w16(m, 0, 0); m[58] = 0; m[59] = 3; return m; }()));
        assert(h.req(2, 1, 0, 2, 0)); h.outT = false;
        assert(halted([] { Bytes m(64); w16(m, 0, 0); m[58] = 0x07; m[59] = 0xd0; return m; }()));
        assert(h.req(2, 1, 0, 2, 0)); h.outT = false;
        // Wrong packet sizes.
        assert(h.send(2, 1, h.outT, 64, Bytes(63)).result == R::stall);
        assert(h.req(2, 1, 0, 2, 0)); h.outT = false;
        // IN with an impossible request size.
        assert(h.send(1, 2, h.inT, 65).result == R::stall);
        assert(h.req(2, 1, 0, 0x81, 0));
        // Unknown endpoint.
        assert(h.send(3, 2, false, 64).result == R::stall);
        // A SNAP that is not RFC 1042, or a payload over 1500: dropped, pipe stays open.
        const uint8_t a[6] = {1, 2, 3, 4, 5, 6};
        Bytes f = romTx(a, a, 0x0800, Bytes(10, 1)); f[60] = 0;
        unsigned dropped = dev.txDropped(), n = sent.size();
        assert(h.out(f) && dev.txDropped() == dropped + 1 && sent.size() == n);
        f = romTx(a, a, 0x0800, Bytes(1501, 1));
        assert(h.out(f) && dev.txDropped() == dropped + 2 && sent.size() == n);
        assert(doCmd(h, 0x11, 0xf200) == true); assert(linkStatusIs(next(h), 1));
    }
    // Command and RID errors answer with an error status and keep the link.
    assert(!doCmd(h, 0x30));
    assert(!doCmd(h, 0x01, 1));
    assert(!doCmd(h, 0x02, 1));
    assert(dev.linkUp());
    assert(!doWrid(h, 0xfd41, {1}));             // read-only RID
    assert(!doWrid(h, 0xfc05, {1, 2, 3}));       // longer than the RID
    assert(doWrid(h, 0xfc05, {9}));
    assert(le(doRrid(h, 0xfc05), 6) == 9);

    // Repeated SET_CONFIGURATION keeps the bulk OUT toggle; a change resets it.
    {
        assert(h.out(cmd(0x11, 0xf200)));
        next(h); next(h);
        if(!h.outT) { assert(h.out(rrid(0xfc01))); next(h); }
        assert(h.outT);  // the host's toggle is DATA1 across the repeat
        assert(h.req(0, 9, 1, 0, 0));
        assert(h.out(cmd(0x11, 0xf200)));
        next(h); next(h);
        assert(h.req(0, 9, 0, 0, 0));
        assert(h.send(1, 2, h.inT, 64).result == R::stall);  // unconfigured
        assert(h.req(0, 9, 1, 0, 0)); h.outT = false;
        assert(h.out(cmd(0x11, 0xf200)));
        assert(status(next(h), 0x8001) == 0x11);
        assert(linkStatusIs(next(h), 1));
        // Duplicate OUT packet (same toggle repeated) is ACKed without a second message.
        Bytes c = cmd(0x11, 0xf200); c.resize(64);
        bool t = h.outT;
        assert(h.send(2, 1, t, 64, c).result == R::ack);
        assert(h.send(2, 1, t, 64, c).result == R::ack);
        h.outT = !t;
        assert(dev.queuedMessages() == 2);
        next(h); next(h);
        // NAK on an empty IN queue.
        assert(h.send(1, 2, h.inT, 64).result == R::nak);
    }

    // Disable and a mismatched network.
    assert(doCmd(h, 0x02));
    assert(!dev.linkUp() && !dev.isEnabled() && dev.queuedMessages() == 0);
    assert(doRrid(h, 0xfd42)[6] == 0);
    assert(doWrid(h, 0xfc02, {4, 'o' | ('t' << 8), 'h' | ('e' << 8)}, 17));
    assert(doCmd(h, 0x01));
    assert(dev.queuedMessages() == 0 && !dev.linkUp());  // no LinkStatus for another SSID
    assert(h.in().empty());
    {
        const Bytes e(14 + 20);
        assert(!dev.receive(e.data(), e.size()));  // not connected
    }
    // Empty desired SSID connects.
    assert(doCmd(h, 0x02));
    assert(doWrid(h, 0xfc02, {0}, 17));
    assert(doCmd(h, 0x01));
    assert(linkStatusIs(next(h), 1));
    // Ad hoc: connected after Enable, no peers in scans, own SSID reported.
    assert(doCmd(h, 0x02));
    romConnect(h, true, "peer", 3, false);
    assert(doCmd(h, 0x01));
    assert(linkStatusIs(next(h), 1));
    assert(romScan(h).empty());
    {
        Bytes m = doRrid(h, 0xfd41); assert(le(m, 6) == 4 && !std::memcmp(&m[8], "peer", 4));
        m = doRrid(h, 0xfdc1); assert(le(m, 6) == 3);
    }
    // A WEP key never stops the link.
    assert(doCmd(h, 0x02));
    romConnect(h, false, "Frond", 6, true);
    assert(doCmd(h, 0x01));
    assert(linkStatusIs(next(h), 1));
    assert(romScan(h).size() == 1);

    // Save and load round trip, including rejection of damaged data.
    {
        Bytes saved = dev.save();
        UsbPrism2Adapter other;
        other.setMac(mac);
        assert(other.load(saved.data(), saved.size()));
        assert(other.linkUp() && other.isEnabled() && other.save() == saved);
        Host h2(other);
        h2.enumerate();
        assert(le(doRrid(h2, 0xfc24), 6) == 0x3412);
        assert(le(doRrid(h2, 0xfc07), 6) == 1522);
        assert(linkStatusIs(([&] { assert(doCmd(h2, 0x11, 0xf200)); return next(h2); })(), 1));
        // Invalid loads change nothing.
        UsbPrism2Adapter third;
        const Bytes pristine = third.save();
        for(size_t cut : {size_t(0), size_t(7), saved.size() - 1}) assert(!third.load(saved.data(), cut));
        auto bad = [&](size_t at, uint8_t v) {
            Bytes c = saved; c[at] = v; assert(!third.load(c.data(), c.size())); assert(third.save() == pristine);
        };
        bad(0, 'X'); bad(4, 2); bad(5, 4); bad(5, 2); bad(6, 17); bad(7, 1); bad(8, 0x01); bad(10, 99);
        Bytes extra = saved; extra.push_back(0);
        assert(!third.load(extra.data(), extra.size()) && third.save() == pristine);
        Bytes dup = saved;
        std::memcpy(&dup[8 + 4 + 2], &dup[8], 2);
        assert(!third.load(dup.data(), dup.size()) && third.save() == pristine);
        assert(!third.linkUp() && !third.isEnabled());
    }
    // Initialize returns to defaults; detach does too and drops the address.
    assert(doCmd(h, 0x00));
    assert(!dev.isEnabled() && le(doRrid(h, 0xfc07), 6) == 2304 && le(doRrid(h, 0xfc2a), 6) == 1);
    dev.busReset();
    assert(dev.busAddress() == 0 && h.send(0, 0, false, 8, {0x80, 6, 0, 1, 0, 0, 8, 0}, 2).result == R::noAnswer);
    dev.detach();
    assert(dev.busAddress() == 0);

    std::cout << "USB Prism2 adapter PASS\n";
}
