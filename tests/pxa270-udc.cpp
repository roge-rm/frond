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
    f.write(0, 1); f.write(0x404, 0); assert(f.read(0x404) == 0x07ffffff);
    f.write(0, 0); f.write(0x404, 0); assert(!f.read(0x404)); f.write(0, 1);
    f.write(0x100, 0x328); assert(f.read(0x100) == 0x328); // AREN/FST/ACM/DME
    f.write(0x100, 0); assert(f.read(0x100) == 0x120); // W1S survives zero; RW clears
    f.write(0, 0); f.write(0, 1); assert(!f.read(0x100));
    // Sixteen bytes auto-arm; loading does not complete a bus transaction.
    for (unsigned i = 0; i < 4; ++i) f.write(0x300, 0x03020100 + i * 0x04040404);
    for (unsigned i = 0; i < 16; ++i) assert(f.udc->tx0[i] == i);
    assert(f.read(0x100) == 2 && !irq && !f.read(0x0c));
    uint32_t extra = 0xffffffff;
    assert(!f.access(0x300, 4, true, &extra) && f.udc->tx0Count == 16);
    f.write(0x100, 0); assert(f.read(0x100) == 2);
    f.write(0x100, 4); assert(!f.read(0x100) && !f.udc->tx0Count && !irq);
    // Final partial words can use byte and halfword stores, in low-lane order.
    f.write(0x300, 0x03020100);
    uint8_t byte = 4; uint16_t half = 0x0605;
    assert(f.access(0x300, 1, true, &byte) && f.access(0x300, 2, true, &half));
    for (unsigned i = 0; i < 7; ++i) assert(f.udc->tx0[i] == i);
    f.write(0x100, 2); assert(f.read(0x100) == 2 && f.udc->tx0Count == 7);
    f.write(0x100, 4); f.write(0x100, 2); // ZLP is ready but never completed locally
    assert(f.read(0x100) == 2 && !f.udc->tx0Count && !irq);
    f.write(0x100, 4);
    for (unsigned length = 0; length <= 16; ++length) {
        unsigned written = 0;
        while (length - written >= 4) { f.write(0x300, 0x55555555); written += 4; }
        while (written < length) { assert(f.access(0x300, 1, true, &byte)); ++written; }
        assert(f.udc->tx0Count == length && bool(f.read(0x100) & 2) == (length == 16));
        if (length == 15) {
            const auto before = f.save(); assert(!f.access(0x300, 2, true, &half));
            assert(f.save() == before); // crossing the boundary is atomic
        }
        f.write(0x100, 2); assert(f.read(0x100) == 2);
        f.write(0x100, 4); assert(!f.udc->tx0Count && !f.read(0x100));
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
    for (unsigned offset = 0x304; offset < 0x360; offset += 4)
        for (bool write : {false, true}) { uint32_t v = 0; assert(!f.access(offset, 4, write, &v)); }
    for (unsigned size : {0u, 1u, 2u, 3u, 8u}) {
        uint64_t guard = 0xabcdef0123456789ull;
        assert(!f.access(0x300, size, false, &guard));
        assert(!f.access(0x100, size, true, &guard));
        assert(guard == 0xabcdef0123456789ull);
    }
    assert(f.save() == unchanged);
    f.write(0, 0); uint32_t value = 0;
    assert(!f.access(0x300, 4, true, &value) && !f.access(0x300, 4, false, &value));
    puts("PASS: configuration lock, alignment/width checks and unsupported FIFOs fail without process abort");
}
