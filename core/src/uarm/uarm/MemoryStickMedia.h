#ifndef FROND_MEMORY_STICK_MEDIA_H
#define FROND_MEMORY_STICK_MEDIA_H

#include "CardBacking.h"
#include "LogicalMsMedia.h"
#include "MemoryStickGeometry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

// Physical classic flash, 4 to 128 MB: pages of 512 data bytes and 9 extra bytes, with no host
// ECC or FAT-sector translation. The card/controller layer supplies boot blocks and FTL metadata.
// The pages are kept in the v2 .frondms layout (LogicalMsMedia), on stored bytes that are either
// the media's own RAM (Attach of a flat physical image) or a card store (AttachStore).
class MemoryStickMedia {
   public:
    using Geometry = frondMsGeometry::Geometry;
    static constexpr uint32_t dataBytes = frondMsGeometry::dataBytes, extraBytes = frondMsGeometry::extraBytes;
    static constexpr uint32_t recordBytes = frondMsGeometry::recordBytes;
    static constexpr size_t maxImageBytes = size_t(frondMsGeometry::largest.PhysicalBytes());
    using Page = std::array<uint8_t, recordBytes>;
    enum class Result { ok, absent, address, writeProtected, invalidImage };

    // Stored bytes somewhere else, such as the card store a session saves. `release` runs when
    // the stick is detached.
    struct Store {
        LogicalMsMedia::ReadFn read;
        LogicalMsMedia::WriteFn write;
        std::function<std::shared_ptr<const CardSnapshot>()> snapshot;
        std::function<bool(const std::shared_ptr<const CardSnapshot>&)> restore;
        std::function<void()> release;
    };

    MemoryStickMedia() = default;
    ~MemoryStickMedia() { Detach(); }
    MemoryStickMedia(const MemoryStickMedia&) = delete;
    MemoryStickMedia& operator=(const MemoryStickMedia&) = delete;
    MemoryStickMedia(MemoryStickMedia&&) = delete;
    MemoryStickMedia& operator=(MemoryStickMedia&&) = delete;

    static const frondMsLayout::Layout* LayoutOf(const Geometry& g) {
        return frondMsLayout::byShape(g.blocks, g.pagesPerBlock);
    }

   public:
    // Pin of the stored bytes. Valid only on the same attachment.
    struct Checkpoint {
        std::shared_ptr<const CardSnapshot> pin;
        bool writeProtected{};
    };

    Result Attach(const std::vector<uint8_t>& image, bool protect) {
        if (!frondMsGeometry::byPhysicalBytes(image.size())) return Result::invalidImage;
        return AttachOwned(image.data(), image.size(), protect);
    }
    Result Attach(std::vector<uint8_t>&& image, bool protect) {
        const auto result = AttachOwned(image.data(), image.size(), protect);
        if (result == Result::ok) std::vector<uint8_t>().swap(image);
        return result;
    }
    // Stored bytes already in the v2 layout for `geometry`.
    Result AttachStore(const Geometry& geometry, Store store, bool protect) {
        const auto* layout = LayoutOf(geometry);
        if (!layout || !store.read || !store.write || !store.snapshot || !store.restore) return Result::invalidImage;
        LogicalMsMedia logical;
        if (!logical.Open(*layout, store.read, store.write)) return Result::invalidImage;
        Detach();
        store_ = std::move(store);
        logical_ = std::move(logical);
        geometry_ = geometry;
        writeProtected_ = protect;
        return Result::ok;
    }

    void Detach() {
        auto release = std::move(store_.release);
        logical_.Close();
        store_ = {};
        ram_.reset();
        geometry_ = frondMsGeometry::largest;
        writeProtected_ = false;
        changed_ = false;
        if (release) release();
    }

    bool Present() const { return logical_.IsOpen(); }
    // On a card store rather than the media's own RAM.
    bool OnStore() const { return Present() && !ram_; }
    // The stick's geometry; the 128 MB one while none is attached.
    const Geometry& Shape() const { return geometry_; }
    uint32_t Blocks() const { return geometry_.blocks; }
    uint32_t PagesPerBlock() const { return geometry_.pagesPerBlock; }
    size_t ImageBytes() const { return size_t(geometry_.PhysicalBytes()); }
    // Written since it was attached.
    bool Dirty() const { return changed_; }
    void SetWriteProtected(bool protect) { writeProtected_ = protect; }
    bool WriteProtected() const { return writeProtected_; }
    const LogicalMsMedia& Logical() const { return logical_; }

    // The stored bytes of a stick in the media's own RAM, to move it into a card store.
    bool CopyStored(std::vector<uint8_t>& out) const {
        if (!ram_) return false;
        std::vector<uint8_t> bytes(size_t(ram_->Size()));
        if (!ram_->Read(0, bytes.size(), bytes.data())) return false;
        out.swap(bytes);
        return true;
    }

    Result Read(uint32_t block, uint32_t page, Page& result) const {
        const auto status = Address(block, page);
        if (status != Result::ok) return status;
        Page candidate;
        if (!logical_.Read(block, page, candidate.data())) return Result::absent;
        result = candidate;
        return Result::ok;
    }

    Result Program(uint32_t block, uint32_t page, const Page& incoming, bool data, bool extra) {
        const auto status = Address(block, page);
        if (status != Result::ok) return status;
        if (writeProtected_) return Result::writeProtected;
        bool changed = false;
        const bool ok = logical_.Program(block, page, incoming.data(), data, extra, changed);
        changed_ |= changed;
        if (!ok) {
            logical_.Reload();
            return Result::absent;
        }
        return Result::ok;
    }

    Result Erase(uint32_t block) {
        const auto status = Address(block, 0);
        if (status != Result::ok) return status;
        if (writeProtected_) return Result::writeProtected;
        bool changed = false;
        const bool ok = logical_.Erase(block, changed);
        changed_ |= changed;
        if (!ok) {
            logical_.Reload();
            return Result::absent;
        }
        return Result::ok;
    }

    // The flat physical image, block by block and page by page.
    Result CopyImage(std::vector<uint8_t>& result) const {
        if (!Present()) return Result::absent;
        std::vector<uint8_t> candidate(ImageBytes());
        for (uint32_t b = 0; b < geometry_.blocks; ++b)
            for (uint32_t p = 0; p < geometry_.pagesPerBlock; ++p)
                if (!logical_.Read(b, p, candidate.data() + (size_t(b) * geometry_.pagesPerBlock + p) * recordBytes))
                    return Result::absent;
        result.swap(candidate);
        return Result::ok;
    }

    Checkpoint SaveCheckpoint() {
        if (!Present()) return {};
        return {store_.snapshot(), writeProtected_};
    }

    Result RestoreCheckpoint(const Checkpoint& checkpoint) {
        if (!Present()) return Result::absent;
        if (!checkpoint.pin || !store_.restore(checkpoint.pin)) return Result::invalidImage;
        if (!logical_.Reload()) return Result::invalidImage;
        writeProtected_ = checkpoint.writeProtected;
        return Result::ok;
    }

   private:
    friend struct MemoryStickMediaStateAccess;

    Result AttachOwned(const uint8_t* image, size_t size, bool protect) {
        const auto* geometry = frondMsGeometry::byPhysicalBytes(size);
        const auto* layout = geometry ? LayoutOf(*geometry) : nullptr;
        if (!layout) return Result::invalidImage;
        std::vector<uint8_t> stored;
        if (!LogicalMsMedia::Build(*layout, image, stored)) return Result::invalidImage;
        auto ram = std::make_shared<RamCardBacking>(std::move(stored), 4096);
        auto* raw = ram.get();
        Store store;
        store.read = [raw](uint64_t off, uint64_t n, void* dest) { return raw->Read(off, n, dest); };
        store.write = [raw](uint64_t off, uint64_t n, const void* src) { return raw->Write(off, n, src); };
        store.snapshot = [raw] { return raw->Snapshot(); };
        store.restore = [raw](const std::shared_ptr<const CardSnapshot>& pin) { return raw->Restore(*pin); };
        const auto result = AttachStore(*geometry, std::move(store), protect);
        if (result == Result::ok) ram_ = std::move(ram);
        return result;
    }

    Result Address(uint32_t block, uint32_t page) const {
        if (!Present()) return Result::absent;
        if (block >= geometry_.blocks || page >= geometry_.pagesPerBlock) return Result::address;
        return Result::ok;
    }

    LogicalMsMedia logical_;
    Store store_;
    std::shared_ptr<RamCardBacking> ram_;
    Geometry geometry_{frondMsGeometry::largest};
    bool writeProtected_{};
    bool changed_{};
};

#endif
