#pragma once
#include "MemoryStickBackend.h"
#include "../../uarm/uarm/MemoryStickCard.h"
#include "../../uarm/uarm/MemoryStickImage.h"
#include "storage/MsLayout.h"

#include <cstring>

// The T415's Memory Stick on the TG50's stick model: pages and extra bytes in a v2 .frondms, the
// guest's own driver owns the block map, so a stick moves between the two machines unchanged.
class FrondMsBackend : public MemoryStickBackend {
    MemoryStickCard card;
    CardImage* image{};

    static const frondMsGeometry::Geometry* GeometryOf(uint64_t storedBytes) {
        const auto* layout = frondMsLayout::byStoredBytes(storedBytes);
        return layout ? frondMsGeometry::byShape(layout->blocks, layout->pagesPerBlock) : nullptr;
    }
    static MemoryStickMedia::Store StoreOn(CardImage* img) {
        MemoryStickMedia::Store store;
        store.read = [img](uint64_t off, uint64_t n, void* dest) {
            return img->ReadByteRange(static_cast<uint8_t*>(dest), size_t(off), size_t(n));
        };
        store.write = [img](uint64_t off, uint64_t n, const void* src) {
            return img->WriteByteRange(static_cast<const uint8_t*>(src), size_t(off), size_t(n));
        };
        // A linked card never rewinds with the machine, so there is nothing to pin.
        store.snapshot = [] { return std::shared_ptr<const CardSnapshot>(); };
        store.restore = [](const std::shared_ptr<const CardSnapshot>&) { return false; };
        return store;
    }

   public:
    // True when the image is a classic stick whose pages open and whose boot blocks the Sony
    // driver accepts.
    static bool Valid(CardImage& img) {
        const auto* geometry = GeometryOf(uint64_t(img.BlocksTotal()) * 512);
        if (!geometry) return false;
        MemoryStickMedia probe;
        if (probe.AttachStore(*geometry, StoreOn(&img), true) != MemoryStickMedia::Result::ok) return false;
        std::vector<uint8_t> boot(size_t(2) * geometry->pagesPerBlock * MemoryStickMedia::recordBytes);
        for (uint32_t b = 0; b < 2; ++b)
            for (uint32_t p = 0; p < geometry->pagesPerBlock; ++p) {
                MemoryStickMedia::Page page;
                if (probe.Read(b, p, page) != MemoryStickMedia::Result::ok) return false;
                std::memcpy(boot.data() + (size_t(b) * geometry->pagesPerBlock + p) * MemoryStickMedia::recordBytes,
                            page.data(), page.size());
            }
        return frondMs::validBootBlocks(boot.data(), *geometry);
    }

    bool SupportsImage(size_t blocksTotal) const override { return GeometryOf(uint64_t(blocksTotal) * 512); }
    bool Mount(CardImage& img) override {
        if (!Attach(img)) return false;
        ResetCard();
        return true;
    }
    bool Remount(CardImage& img) override {
        if (!Attach(img)) return false;
        if (!MemoryStickCard::Valid(card.state, card.media.Shape())) ResetCard();
        return true;
    }
    void Unmount() override {
        card.media.Detach();
        image = nullptr;
        ResetCard();
    }
    bool Present() const override { return card.media.Present(); }
    void Reset() override { ResetCard(); }
    bool TpcRead(uint8_t tpc, uint8_t* out, size_t size) override { return card.TpcRead(tpc, out, size); }
    bool TpcWrite(uint8_t tpc, const uint8_t* in, size_t size) override { return card.TpcWrite(tpc, in, size); }
    void FinishRead(uint8_t tpc) override { card.FinishRead(tpc); }
    uint8_t Interrupt() const override { return card.Interrupt(); }
    void SetInterrupt(uint8_t value) override { card.Interrupt(value); }

    void SaveState(uint8_t* out) const override {
        const auto& s = card.state;
        std::memcpy(out, s.reg.data(), 32);
        std::memcpy(out + 32, s.page.data(), 512);
        const uint32_t words[6] = {s.readStart, s.readSize, s.writeStart, s.writeSize, s.operation, s.bufferFlags};
        for (unsigned i = 0; i < 6; ++i) frondMs::put32(out + 544 + 4 * i, words[i]);
    }
    bool LoadState(const uint8_t* in) override {
        MemoryStickCard::State s;
        std::memcpy(s.reg.data(), in, 32);
        std::memcpy(s.page.data(), in + 32, 512);
        uint32_t* fields[6] = {&s.readStart, &s.readSize, &s.writeStart, &s.writeSize, &s.operation, &s.bufferFlags};
        for (unsigned i = 0; i < 6; ++i) *fields[i] = frondMs::get32(in + 544 + 4 * i);
        const auto& shape = card.media.Present() ? card.media.Shape() : frondMsGeometry::largest;
        if (!MemoryStickCard::Valid(s, shape)) return false;
        card.state = s;
        return true;
    }

   private:
    // The Sony driver reads all 31 register bytes without setting a read window first.
    void ResetCard() {
        card.Reset();
        card.state.readSize = 31;
    }
    bool Attach(CardImage& img) {
        const auto* geometry = GeometryOf(uint64_t(img.BlocksTotal()) * 512);
        if (!geometry) return false;
        image = &img;
        return card.media.AttachStore(*geometry, StoreOn(&img), img.IsReadOnly()) == MemoryStickMedia::Result::ok;
    }
};
