// No host transport is exposed. Seed hardware-owned state directly to test the
// MMIO/serialization foundation independently of an unimplemented USB bus.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "pxa270_UDC.cpp"

static ArmMemAccessF accessRegister;
static bool irq;
extern "C" void uarmAbort() { std::abort(); }
bool memRegionAdd(ArmMem*, uint32_t base, uint32_t size, ArmMemAccessF f, void* p) {
    assert(base == 0x40600000 && size == 0x1000);
    accessRegister = f; (void)p; return true;
}
void pxaIcInt(PxaIc*, uint_fast8_t number, bool level) { assert(number == PXA_I_USB); irq = level; }
struct Fixture {
    Pxa270Udc* udc = pxa270UdcInit(nullptr, nullptr, nullptr);
    ~Fixture() { free(udc); }
    bool access(unsigned offset, unsigned size, bool write, void* data) {
        return accessRegister(udc, 0x40600000 + offset, size, write, data);
    }
    void write(unsigned offset, uint32_t value) { assert(access(offset, 4, true, &value)); }
    uint32_t read(unsigned offset) { uint32_t value = 0xdeadbeef; assert(access(offset, 4, false, &value)); return value; }
    void receive(unsigned count) {
        assert(count <= 16);
        for (unsigned i = 0; i < count; ++i) udc->rx0[i] = 0x80 + i;
        udc->rx0Pos = 0; udc->ep[0].bcr = count;
        udc->ep[0].csr = 0x81 | (count ? 0x40 : 0); // SETUP + OPC, optionally RNE
    }
    // Host IN through the model; returns the packet size, asserts it was ACKed.
    unsigned in() {
        uint8_t data[16]; unsigned size = 99;
        assert(pxa270UdcHostIn(udc, data, &size) == Pxa270UdcHostResult::Ack);
        return size;
    }
    template<class T> void Save(T& state) { pxa270UdcSave(udc, state); }
    void Load(SavestateLoader<ChunkType>& state) { pxa270UdcLoad(udc, state); }
    std::vector<uint8_t> save() {
        Savestate<ChunkType> state; assert(state.Save(*this));
        auto* bytes = static_cast<uint8_t*>(state.GetBuffer());
        return {bytes, bytes + state.GetSize()};
    }
    bool load(std::vector<uint8_t>& bytes) {
        SavestateLoader<ChunkType> loader; return loader.Load(bytes.data(), bytes.size(), *this);
    }
};
// A v1 chunk (FIFOs, no transfer direction) with UDA set and an empty EP0.
struct LegacyV1 {
    uint32_t udccr = 3;
    template<class T> void Save(T& state) {
        auto* c = state.GetChunk(ChunkType::pxa270udc, 1); assert(c);
        for (unsigned i = 0; i < 24; ++i) { c->Put32(0); c->Put16(0); c->Put16(0); }
        c->Put32(udccr); for (unsigned i = 0; i < 4; ++i) c->Put32(0);
        c->Put32(0); c->Put32(0); c->Put32(0x30000); c->Put16(0); c->Put8(0);
        for (unsigned i = 0; i < 32; ++i) c->Put8(0);
        c->Put8(0); c->Put8(0);
    }
};
struct Legacy {
    template<class T> void Save(T& state) {
        auto* c = state.GetChunk(ChunkType::pxa270udc, 0); assert(c);
        // Write the old wire format independently, including impossible ISR bits
        // produced by the previous stub's write-to-set acknowledgement bug.
        for (unsigned i = 0; i < 24; ++i) {
            c->Put32(i ? 0x0200d041 : 0); c->Put16(i ? 0x1a8 : 0x3ff); c->Put16(0);
        }
        c->Put32(1); c->Put32(0xffffffff); c->Put32(0xf800ffff);
        c->Put32(0xffffffff); c->Put32(0xf800ffff);
        c->Put32(0x010303ff); c->Put32(0); c->Put32(0x30000); c->Put16(0); c->Put8(0);
    }
};
int main() {
    Fixture f;
    assert(!irq && f.read(0) == 0 && f.read(0x20) == 0x30000);
    f.write(0x0c, ~0u); f.write(0x10, ~0u); f.write(0x1c, ~0u);
    f.write(4, ~0u); f.write(8, ~0u); f.write(0x18, ~0u);
    assert(f.read(8) == 0xf800ffff && f.read(0x18) == 0x010303ff);
    assert(!irq && !f.read(0x0c) && !f.read(0x10) && !f.read(0x1c));
    // Test each valid interrupt source, selective W1C, masking and reassertion.
    for (auto pair : {std::pair<unsigned, unsigned>{4, 0x0c}, {8, 0x10}, {0x18, 0x1c}}) {
        const uint32_t valid = f.read(pair.first);
        for (unsigned bit = 0; bit < 32; ++bit) {
            const uint32_t mask = 1u << bit;
            if (!(valid & mask)) continue;
            if (pair.second == 0x0c) f.udc->udcisr[0] = mask;
            else if (pair.second == 0x10) f.udc->udcisr[1] = mask;
            else f.udc->udcotgisr = mask;
            f.write(pair.first, valid); assert(irq);
            f.write(pair.second, ~mask); assert(irq && f.read(pair.second) == mask);
            f.write(pair.first, 0); assert(!irq && f.read(pair.second) == mask);
            f.write(pair.first, valid); assert(irq);
            f.write(pair.second, mask); assert(!irq && !f.read(pair.second));
        }
    }
    puts("PASS: IRQ11 aggregation, all valid sources, masks and selective W1C; no phantom events");

    f.write(0x404, ~0u); assert(f.read(0x404) == 0x07ffffff);
    // Interrupt endpoint with MPS 1023: EMCE, UDE stays clear until EMCE is cleared.
    f.write(0, 1); assert(f.read(0) == 8);
    f.write(0x404, 0x0200d041); f.write(0, 1); assert(f.read(0) == 8);
    f.write(0, 9); assert(f.read(0) == 1);
    f.write(0x404, 0); assert(f.read(0x404) == 0x0200d041);
    f.write(0, 0); f.write(0x404, 0); assert(!f.read(0x404)); f.write(0, 1);
    f.write(0x100, 0x328); assert(f.read(0x100) == 0x328); // AREN/FST/ACM/DME
    f.write(0x100, 0); assert(f.read(0x100) == 0x120); // W1S survives zero; RW clears
    f.write(0, 0); f.write(0, 1); assert(!f.read(0x100));
    // Sixteen bytes auto-arm; loading does not complete a bus transaction.
    for (unsigned i = 0; i < 4; ++i) f.write(0x300, 0x03020100 + i * 0x04040404);
    for (unsigned i = 0; i < 16; ++i) assert(f.udc->tx0[i] == i);
    assert(f.read(0x100) == 2 && !irq && !f.read(0x0c));
    // A write to a committed packet is dropped without a guest fault.
    uint32_t extra = 0xffffffff;
    assert(f.access(0x300, 4, true, &extra) && f.udc->tx0Count == 16 && f.udc->tx0[0] == 0);
    f.write(0x100, 0); assert(f.read(0x100) == 2);
    f.write(0x100, 4); assert(f.read(0x100) == 2 && f.udc->tx0Count == 16 && !irq); // FTF keeps it
    assert(f.in() == 16 && f.read(0x100) == 0 && !f.udc->tx0Count);
    f.write(0x0c, 1);
    // Final partial words can use byte and halfword stores, in low-lane order.
    f.write(0x300, 0x03020100);
    uint8_t byte = 4; uint16_t half = 0x0605;
    assert(f.access(0x300, 1, true, &byte) && f.access(0x300, 2, true, &half));
    for (unsigned i = 0; i < 7; ++i) assert(f.udc->tx0[i] == i);
    f.write(0x100, 2); assert(f.read(0x100) == 2 && f.udc->tx0Count == 7);
    f.write(0x100, 4); assert(f.read(0x100) == 2 && f.udc->tx0Count == 7); // ready: FTF ignored
    assert(f.in() == 7 && !f.read(0x100)); f.write(0x0c, 1);
    f.write(0x100, 2); // ZLP is ready but never completed locally
    assert(f.read(0x100) == 2 && !f.udc->tx0Count && !irq);
    assert(f.in() == 0 && !f.read(0x100)); f.write(0x0c, 1);
    for (unsigned length = 0; length <= 16; ++length) {
        unsigned written = 0;
        while (length - written >= 4) { f.write(0x300, 0x55555555); written += 4; }
        while (written < length) { assert(f.access(0x300, 1, true, &byte)); ++written; }
        assert(f.udc->tx0Count == length && bool(f.read(0x100) & 2) == (length == 16));
        if (length == 15) {
            const auto before = f.save(); assert(!f.access(0x300, 2, true, &half));
            assert(f.save() == before); // crossing the boundary is atomic
        }
        if (length < 16) {
            // FTF on a packet still being loaded drops it, with no IRQ.
            f.write(0x100, 4); assert(!f.udc->tx0Count && !f.read(0x100) && !irq);
            while (written--) assert(f.access(0x300, 1, true, &byte));
            f.write(0x100, 2);
        }
        assert(f.read(0x100) == 2 && f.udc->tx0Count == length);
        f.write(0x100, 4); assert(f.read(0x100) == 2 && f.udc->tx0Count == length);
        assert(f.in() == length && !f.read(0x100) && !f.udc->tx0Count); f.write(0x0c, 1);
    }
    puts("PASS: EP0 full/short/zero packet staging, little-endian lanes, W1S and flush");

    for (unsigned length = 0; length <= 16; ++length) {
        f.receive(length);
        f.write(0x200, ~0u); assert(f.read(0x200) == length); // BCR is read-only
        for (unsigned pos = 0; pos < length; pos += 4) {
            const unsigned count = std::min(4u, length - pos);
            uint32_t expected = 0;
            for (unsigned i = 0; i < count; ++i) expected |= (0x80u + pos + i) << (8 * i);
            assert(f.read(0x300) == expected && f.read(0x200) == length - pos - count);
        }
        assert(f.read(0x300) == 0 && !f.read(0x200) && f.read(0x100) == 0x81);
        f.write(0x100, 0x80); assert(f.read(0x100) == 1); // SA W1C does not clear OPC
        f.write(0x100, 1); assert(!f.read(0x100));
    }
    f.receive(16); assert(f.read(0x300) == 0x83828180);
    f.write(0x100, 1); assert(!f.read(0x200) && !(f.read(0x100) & 0x41) && f.read(0x300) == 0);
    puts("PASS: seeded EP0 RX lengths 0..16, partial reads, byte counts, RNE, SA/OPC and discard");

    {
        // EP0 FTF and the events that drop a ready IN packet (EP0-SPEC tests 1-6).
        Fixture g; g.write(0, 1); g.write(4, 3);
        uint8_t data[16]; unsigned size = 0;
        auto stage = [&](unsigned words) { for (unsigned i = 0; i < words; ++i) g.write(0x300, 0x11111111 * (i + 1)); };
        auto host = [&]() { return pxa270UdcHostIn(g.udc, data, &size); };
        const uint8_t request[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
        // 1: FTF drops bytes still being loaded, without an event.
        stage(1); uint16_t half = 0x6655; assert(g.access(0x300, 2, true, &half));
        g.write(0x100, 4); assert(!g.udc->tx0Count && !g.read(0x100) && !g.read(0x0c) && !irq);
        assert(host() == Pxa270UdcHostResult::Nak && !irq);
        // 2: a full packet is committed by hardware; FTF leaves it for the host.
        stage(4); assert(g.read(0x100) == 2 && !irq);
        g.write(0x100, 4); assert(g.read(0x100) == 2 && g.udc->tx0Count == 16 && !irq);
        assert(host() == Pxa270UdcHostResult::Ack && size == 16 && data[0] == 0x11 && data[15] == 0x44);
        assert(!g.read(0x100) && g.read(0x0c) == 1 && irq);
        g.write(0x0c, 1); assert(!irq && host() == Pxa270UdcHostResult::Nak);
        // 3: short packet and ZLP survive FTF too.
        half = 0x2211; assert(g.access(0x300, 2, true, &half));
        g.write(0x100, 2); g.write(0x100, 4);
        assert(host() == Pxa270UdcHostResult::Ack && size == 2 && data[0] == 0x11 && data[1] == 0x22);
        g.write(0x0c, 1);
        g.write(0x100, 2); g.write(0x100, 4);
        assert(host() == Pxa270UdcHostResult::Ack && size == 0 && g.read(0x0c) == 1); g.write(0x0c, 1);
        // 4: SETUP, status OUT, bus reset and UDE=0 drop a ready packet without an IN event.
        stage(4); assert(pxa270UdcHostSetup(g.udc, request) == Pxa270UdcHostResult::Ack);
        assert(!g.udc->tx0Count && g.read(0x100) == 0xc1 && g.read(0x200) == 8 && g.read(0x0c) == 1);
        assert(host() == Pxa270UdcHostResult::Nak);
        g.read(0x300); g.read(0x300); g.write(0x100, 0x81); g.write(0x0c, 1);
        assert(host() == Pxa270UdcHostResult::Nak);
        g.write(0x300, 1); g.write(0x100, 2);
        assert(pxa270UdcHostOut(g.udc, nullptr, 0) == Pxa270UdcHostResult::Ack);
        assert(!g.udc->tx0Count && g.read(0x100) == 1 && !g.read(0x200) && g.read(0x0c) == 1);
        assert(pxa270UdcHostOut(g.udc, nullptr, 0) == Pxa270UdcHostResult::Nak); // OPC not released
        assert(host() == Pxa270UdcHostResult::Nak);
        g.write(0x100, 1); g.write(0x0c, 1);
        g.write(8, 1u << 27); stage(4); assert(pxa270UdcHostReset(g.udc));
        assert(!g.udc->tx0Count && !g.read(0x100) && !g.read(0x0c) && g.read(0x10) == (1u << 27) && irq);
        assert(host() == Pxa270UdcHostResult::Nak);
        g.write(0x10, 1u << 27); assert(!irq);
        stage(4); g.write(0, 0);
        assert(!g.udc->tx0Count && host() == Pxa270UdcHostResult::Disabled && !irq);
        assert(pxa270UdcHostReset(g.udc) == false && pxa270UdcHostSetup(g.udc, request) == Pxa270UdcHostResult::Disabled);
        g.write(0, 1); assert(!g.read(0x100) && host() == Pxa270UdcHostResult::Nak);
        // 5: FST|FTF on a ready packet: the next IN is a STALL, FST self-clears and the
        // next SETUP drops the packet.
        stage(4); g.write(0x100, 0x24); assert(g.read(0x100) == 0x22);
        assert(host() == Pxa270UdcHostResult::Stall && size == 0 && g.read(0x100) == 2 && !irq);
        assert(pxa270UdcHostSetup(g.udc, request) == Pxa270UdcHostResult::Ack && !g.udc->tx0Count);
        g.read(0x300); g.read(0x300); g.write(0x100, 0x81); g.write(0x0c, 1);
        g.write(0x100, 0x20);
        assert(pxa270UdcHostOut(g.udc, nullptr, 0) == Pxa270UdcHostResult::Stall && !(g.read(0x100) & 0x20));
        // 6: FTF|IPR in one write flushes the partial bytes, then commits a ZLP.
        stage(2); g.write(0x100, 6); assert(g.read(0x100) == 2 && !g.udc->tx0Count);
        assert(host() == Pxa270UdcHostResult::Ack && size == 0); g.write(0x0c, 1);
        // DMA on EP0 OUT is not modelled; host packets are refused, not lost.
        g.write(0x100, 8); assert(pxa270UdcHostSetup(g.udc, request) == Pxa270UdcHostResult::Unsupported);
        assert(g.read(0x100) == 8 && !irq);
    }
    {
        // Bus reset adds IRRS and leaves unacknowledged ISR bits latched for the guest.
        Fixture g; g.write(0, 1); g.write(4, 1); g.write(8, (1u << 27) | (1u << 29));
        const uint8_t request[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
        assert(pxa270UdcHostSetup(g.udc, request) == Pxa270UdcHostResult::Ack && (g.read(0) & 2));
        g.udc->udcisr[1] = 1u << 29; // IRSU latched, as a suspend would leave it
        assert(pxa270UdcHostReset(g.udc) && irq);
        assert(g.read(0x0c) == 1 && g.read(0x10) == ((1u << 27) | (1u << 29)) && !(g.read(0) & 2) && !g.read(0x100));
        g.write(0x10, 1u << 27); assert(irq && g.read(0x10) == (1u << 29));
        g.write(0x10, 1u << 29); g.write(0x0c, 1); assert(!irq);
        // UDA and the transfer direction survive save/restore; v1 states with UDA load as a
        // control read, and a direction that disagrees with UDA is refused.
        const uint8_t write0[8] = {0x40, 1, 0, 0, 0, 0, 0, 0};
        assert(pxa270UdcHostSetup(g.udc, write0) == Pxa270UdcHostResult::Ack);
        g.read(0x300); g.read(0x300); g.write(0x100, 0x81); g.write(0x0c, 1);
        auto saved = g.save();
        assert(pxa270UdcHostOut(g.udc, nullptr, 0) == Pxa270UdcHostResult::Ack && (g.read(0) & 2));
        g.write(0x100, 1); g.write(0x0c, 1);
        assert(g.load(saved) && (g.read(0) & 2) && g.udc->ctl0 == 2);
        g.write(0x100, 2); uint8_t data[16]; unsigned size = 1;
        assert(pxa270UdcHostIn(g.udc, data, &size) == Pxa270UdcHostResult::Ack && !size && !(g.read(0) & 2));
        g.write(0x0c, 1);
        Savestate<ChunkType> v1State; LegacyV1 v1; assert(v1State.Save(v1));
        auto* raw = static_cast<uint8_t*>(v1State.GetBuffer());
        std::vector<uint8_t> v1Bytes(raw, raw + v1State.GetSize());
        assert(g.load(v1Bytes) && (g.read(0) & 2) && g.udc->ctl0 == 1);
        assert(pxa270UdcHostOut(g.udc, nullptr, 0) == Pxa270UdcHostResult::Ack && !(g.read(0) & 2));
        g.write(0x100, 1); g.write(0x0c, 1);
        g.udc->ctl0 = 2; auto bad = g.save(); g.udc->ctl0 = 0;
        const auto before = g.save(); assert(!g.load(bad) && g.save() == before);
    }
    puts("PASS: bus reset keeps latched ISR bits and adds IRRS; UDA follows control stages and is saved (v2), v1 migrates");
    puts("PASS: EP0 FTF only drops unready bytes; SETUP/OUT/reset/UDE=0 drop ready packets; STALL, ZLP and FTF|IPR");

    f.receive(7); f.write(0x300, 0x44332211); assert(f.read(0x300) == 0x83828180);
    f.udc->udcisr[0] = 1; f.write(4, 1); assert(irq);
    auto snapshot = f.save();
    {
        Fixture fresh;
        assert(fresh.load(snapshot) && irq && fresh.read(0x200) == 3);
        assert(fresh.read(0x300) == 0x00868584 && fresh.udc->tx0Count == 4);
    }
    f.write(0, 0); f.write(0x0c, ~0u); assert(!irq);
    assert(f.load(snapshot) && irq && f.read(0x200) == 3);
    assert(f.udc->tx0Count == 4 && f.udc->tx0[0] == 0x11 && f.udc->tx0[3] == 0x44);
    assert(f.read(0x300) == 0x00868584 && !f.read(0x200));
    f.write(0x100, 0x81); assert(irq); // endpoint acknowledgement is not ISR acknowledgement
    f.write(0x0c, 1); assert(!irq);
    f.udc->tx0Count = 17; auto bad = f.save(); f.udc->tx0Count = 4;
    const auto before = f.save(); assert(!f.load(bad) && f.save() == before && !irq);
    f.udc->rx0Pos = 15; f.udc->ep[0].bcr = 2; bad = f.save();
    f.udc->rx0Pos = 0; f.udc->ep[0].bcr = 0;
    const auto beforeRx = f.save(); assert(!f.load(bad) && f.save() == beforeRx);
    bad = snapshot; bad.resize(bad.size() - 4); assert(!f.load(bad));
    Savestate<ChunkType> legacyState; Legacy legacy; assert(legacyState.Save(legacy));
    auto* old = static_cast<uint8_t*>(legacyState.GetBuffer());
    std::vector<uint8_t> legacyBytes(old, old + legacyState.GetSize());
    assert(f.load(legacyBytes) && !irq && !f.udc->tx0Count && !f.read(0x200));
    assert(f.read(0x100) == 0x328 && !f.read(0x0c) && !f.read(0x10));
    assert(f.read(4) == 0xffffffff && f.read(0x404) == 0x0200d041);
    puts("PASS: FIFO/IRQ round trip, atomic invalid/truncated-state rejection and legacy-v0 migration");

    // Every unsupported access fails without aliasing, modifying memory, or aborting the process.
    auto unchanged = f.save();
    for (unsigned offset : {0x28u, 0x301u, 0x302u, 0x303u, 0x360u, 0x400u, 0x460u, 0x1000u})
        for (bool write : {false, true}) { uint32_t v = 0xabcdef01; assert(!f.access(offset, 4, write, &v) && v == 0xabcdef01); }
    // Enabled IN endpoints A-X: reading the transmit FIFO returns zero and changes nothing.
    for (unsigned offset = 0x304; offset < 0x360; offset += 4) { uint32_t v = 1; assert(f.access(offset, 4, false, &v) && !v); }
    for (unsigned size : {0u, 1u, 2u, 3u, 8u}) {
        uint64_t guard = 0xabcdef0123456789ull;
        assert(!f.access(0x300, size, false, &guard));
        assert(!f.access(0x100, size, true, &guard));
        assert(guard == 0xabcdef0123456789ull);
    }
    assert(f.save() == unchanged);
    f.write(0, 0); uint32_t value = 0;
    assert(!f.access(0x300, 4, true, &value) && !f.access(0x300, 4, false, &value));
    // A-X FIFOs with UDE=0 or the endpoint disabled read 0 and ignore writes, without an abort.
    value = 0x12345678; assert(f.access(0x304, 4, false, &value) && !value);
    value = 0x12345678; assert(f.access(0x304, 4, true, &value));
    unchanged = f.save();
    f.write(0x404, 0); f.write(0, 1); unchanged = f.save();
    value = 0x12345678; assert(f.access(0x304, 4, true, &value) && f.save() == unchanged);
    value = 0x12345678; assert(f.access(0x304, 4, false, &value) && !value && f.access(0x308, 4, false, &value));
    f.write(0, 0);
    puts("PASS: configuration lock, alignment/width checks, unsupported FIFOs fail and disabled A-X FIFOs read 0 without process abort");
}
