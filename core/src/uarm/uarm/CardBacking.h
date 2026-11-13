#ifndef FROND_CARD_BACKING_H
#define FROND_CARD_BACKING_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

// Milestone 1 inventory of whole-card paths (do not rewrite these yet):
//
// sdCardData: uarm/sdcard.cpp; web/uarm.cpp GetSdCardData*; native/Commands.cpp
// save/export; native/DesktopControl.h sd-export and save; native/ReadOnlyGuest.h
// copy/restore; tests/tx-lifecycle-rom.cpp; tests/e2-state.cpp.
//
// SessionFile5 GetSd/SetSd: common/session/session_file5.{h,cpp}; native/main.cpp
// readSession; native/Commands.cpp and DesktopControl.h save; tests/sd-session.cpp,
// e2-state.cpp, device-profiles.cpp, tx-lifecycle-rom.cpp; GetSdSize guards in
// tests/tx-power-rom.cpp, tests/device-session.cpp, tools/benchmark-*.cpp.
//
// There is no sdCardInsert. Insertion is SoC::SdInsert / SdEject / SdRemount and
// deviceSetSdCardInserted: SoC.cpp, soc_generic_impl.h, native/main.cpp,
// Commands.cpp, DesktopControl.h, web/uarm.cpp, device boards, tests.
//
// Memory Stick savestate v1 stores protocol state and a present/protect
// reference (mmiodev_MemoryStickController.cpp). v0 still loads an embedded
// compressed image for v6 extraction. Ordinary v7 save does not call
// sdCardData(); sidecars hold card bytes. sdCardData remains a compatibility
// view for older tests until milestone 4.
//
// Checked 64-bit card payload. offset+length is never formed until both fit.

inline bool cardRangeOk(uint64_t size, uint64_t offset, uint64_t length, size_t* hostLength = nullptr) {
    if (offset > size) return false;
    if (length > size - offset) return false;
    if (offset > std::numeric_limits<size_t>::max()) return false;
    if (length > std::numeric_limits<size_t>::max()) return false;
    if (hostLength) *hostLength = static_cast<size_t>(length);
    return true;
}

class CardSnapshot {
   public:
    virtual ~CardSnapshot() = default;
    virtual uint64_t Size() const = 0;
    virtual uint64_t GenerationId() const = 0;

    bool Read(uint64_t offset, uint64_t length, void* dest) const {
        size_t n = 0;
        if (!cardRangeOk(Size(), offset, length, &n)) return false;
        if (n == 0) return true;
        if (!dest) return false;
        return ReadBytes(static_cast<size_t>(offset), n, dest);
    }

   protected:
    virtual bool ReadBytes(size_t offset, size_t length, void* dest) const = 0;
};

class CardBacking {
   public:
    using OverlayFn = std::function<bool(uint64_t offset, uint64_t length, const uint8_t* data)>;

    virtual ~CardBacking() = default;
    virtual uint64_t Size() const = 0;

    bool Read(uint64_t offset, uint64_t length, void* dest) const {
        size_t n = 0;
        if (!cardRangeOk(Size(), offset, length, &n)) return false;
        if (n == 0) return true;
        if (!dest) return false;
        return ReadBytes(static_cast<size_t>(offset), n, dest);
    }

    bool Write(uint64_t offset, uint64_t length, const void* src) {
        size_t n = 0;
        if (!cardRangeOk(Size(), offset, length, &n)) return false;
        if (n == 0) return true;
        if (!src) return false;
        return WriteBytes(static_cast<size_t>(offset), n, src);
    }

    // Pin the current visible state. Does not clone the payload.
    virtual std::shared_ptr<const CardSnapshot> Snapshot() = 0;
    // Publish the working branch as the new root. Does not clone the payload.
    virtual bool Commit() = 0;
    // Discard the working branch. Root bytes stay as they were.
    virtual bool Rollback() = 0;
    // Make a pinned generation the working branch. Refuses a foreign snapshot.
    virtual bool Restore(const CardSnapshot& snapshot) = 0;
    // Fold unpinned ancestors. Failed compact leaves the old root readable.
    virtual bool Compact() = 0;
    virtual bool ForEachOverlay(const OverlayFn& fn) const = 0;
    virtual uint32_t LiveGenerations() const = 0;

   protected:
    virtual bool ReadBytes(size_t offset, size_t length, void* dest) const = 0;
    virtual bool WriteBytes(size_t offset, size_t length, const void* src) = 0;
};

// RAM backing with copy-on-write generations. Overlay is per dirty page; the
// base vector is shared. Tiny fixtures and Memory Stick Attach use this.
class RamCardBacking : public CardBacking {
   public:
    explicit RamCardBacking(size_t size, uint32_t pageSize = 512)
        : RamCardBacking(std::vector<uint8_t>(size), pageSize) {}
    explicit RamCardBacking(std::vector<uint8_t> bytes, uint32_t pageSize = 512) {
        if (pageSize == 0) pageSize = 512;
        auto base = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
        auto gen = std::make_shared<Gen>();
        gen->id = 0;
        gen->base = std::move(base);
        gen->size = gen->base->size();
        gen->pageSize = pageSize;
        root_ = gen;
        working_ = std::move(gen);
        identity_ = std::make_shared<char>();
    }

    uint64_t Size() const override { return working_->size; }
    uint32_t PageSize() const { return working_->pageSize; }

    const std::vector<uint8_t>& Bytes() const {
        if (contiguous()) return *working_->base;
        materialize();
        return materialized_;
    }

    const uint8_t* ContiguousData() const { return contiguous() ? working_->base->data() : nullptr; }
    // A snapshot of the working state is still held.
    bool Pinned() const { return working_.use_count() > (working_ == root_ ? 2 : 1); }

    std::shared_ptr<const CardSnapshot> Snapshot() override {
        return std::make_shared<Pin>(identity_, working_);
    }

    bool Commit() override {
        root_ = working_;
        return true;
    }

    bool Rollback() override {
        working_ = root_;
        materialized_.clear();
        return true;
    }

    bool Restore(const CardSnapshot& snapshot) override {
        const auto* pin = dynamic_cast<const Pin*>(&snapshot);
        if (!pin || pin->identity != identity_ || !pin->gen) return false;
        working_ = std::const_pointer_cast<Gen>(pin->gen);
        materialized_.clear();
        return true;
    }

    bool Compact() override {
        std::vector<uint64_t> pages;
        std::vector<uint8_t> data;
        std::vector<uint8_t> tomb;
        if (!coalesce(pages, data, tomb)) return false;
        auto flat = std::make_shared<Gen>();
        flat->id = nextId_++;
        flat->base = working_->base;
        flat->size = working_->size;
        flat->pageSize = working_->pageSize;
        flat->pageNumbers = std::move(pages);
        flat->pageData = std::move(data);
        flat->tombstone = std::move(tomb);
        working_ = flat;
        root_ = std::move(flat);
        materialized_.clear();
        return true;
    }

    bool ForEachOverlay(const OverlayFn& fn) const override {
        std::vector<uint64_t> pages;
        std::vector<uint8_t> data;
        std::vector<uint8_t> tomb;
        if (!coalesce(pages, data, tomb)) return false;
        const uint32_t ps = working_->pageSize;
        for (size_t i = 0; i < pages.size(); ++i) {
            if (tomb[i]) continue;
            const uint64_t off = pages[i] * uint64_t(ps);
            if (off >= working_->size) continue;
            const uint64_t n = std::min(uint64_t(ps), working_->size - off);
            if (!fn(off, n, data.data() + i * ps)) return false;
        }
        return true;
    }

    uint32_t LiveGenerations() const override {
        uint32_t n = 0;
        for (auto* g = working_.get(); g; g = g->parent.get()) ++n;
        return n;
    }

   protected:
    bool ReadBytes(size_t offset, size_t length, void* dest) const override {
        return readFrom(working_.get(), offset, length, dest);
    }

    bool WriteBytes(size_t offset, size_t length, const void* src) override {
        if (!ensureWritable()) return false;
        auto* raw = static_cast<const uint8_t*>(src);
        const uint32_t ps = working_->pageSize;
        while (length) {
            const uint64_t page = uint64_t(offset) / ps;
            const size_t skip = offset % ps;
            const size_t n = std::min(length, size_t(ps) - skip);
            if (working_->parent || !working_->pageNumbers.empty() || working_.use_count() > 1) {
                std::vector<uint8_t> buf(ps);
                if (!readPage(working_.get(), page, buf.data())) return false;
                std::memcpy(buf.data() + skip, raw, n);
                if (!storePage(page, buf.data())) return false;
            } else {
                std::memcpy(working_->base->data() + offset, raw, n);
            }
            offset += n;
            raw += n;
            length -= n;
        }
        materialized_.clear();
        return true;
    }

   private:
    struct Gen {
        uint64_t id{};
        std::shared_ptr<Gen> parent;
        std::shared_ptr<std::vector<uint8_t>> base;
        uint64_t size{};
        uint32_t pageSize{512};
        std::vector<uint64_t> pageNumbers;
        std::vector<uint8_t> pageData;
        std::vector<uint8_t> tombstone;
    };

    class Pin final : public CardSnapshot {
       public:
        Pin(std::shared_ptr<const void> id, std::shared_ptr<const Gen> gen)
            : identity(std::move(id)), gen(std::move(gen)) {}
        uint64_t Size() const override { return gen->size; }
        uint64_t GenerationId() const override { return gen->id; }
        std::shared_ptr<const void> identity;
        std::shared_ptr<const Gen> gen;

       protected:
        bool ReadBytes(size_t offset, size_t length, void* dest) const override {
            return RamCardBacking::readFrom(gen.get(), offset, length, dest);
        }
    };

    static bool readPage(const Gen* gen, uint64_t page, uint8_t* dest) {
        const uint32_t ps = gen->pageSize;
        for (const Gen* g = gen; g; g = g->parent.get()) {
            const auto it = std::lower_bound(g->pageNumbers.begin(), g->pageNumbers.end(), page);
            if (it != g->pageNumbers.end() && *it == page) {
                const size_t i = static_cast<size_t>(it - g->pageNumbers.begin());
                if (g->tombstone[i]) break;
                std::memcpy(dest, g->pageData.data() + i * ps, ps);
                return true;
            }
        }
        const uint64_t off = page * uint64_t(ps);
        if (off >= gen->size) return false;
        const size_t n = static_cast<size_t>(std::min(uint64_t(ps), gen->size - off));
        std::memcpy(dest, gen->base->data() + static_cast<size_t>(off), n);
        if (n < ps) std::memset(dest + n, 0, ps - n);
        return true;
    }

    static bool readFrom(const Gen* gen, size_t offset, size_t length, void* dest) {
        auto* out = static_cast<uint8_t*>(dest);
        if (!gen->parent && gen->pageNumbers.empty() && gen->base &&
            offset + length <= gen->base->size()) {
            std::memcpy(out, gen->base->data() + offset, length);
            return true;
        }
        const uint32_t ps = gen->pageSize;
        while (length) {
            const uint64_t page = uint64_t(offset) / ps;
            const size_t skip = offset % ps;
            const size_t n = std::min(length, size_t(ps) - skip);
            if (skip == 0 && n == ps) {
                if (!readPage(gen, page, out)) return false;
            } else {
                std::vector<uint8_t> buf(ps);
                if (!readPage(gen, page, buf.data())) return false;
                std::memcpy(out, buf.data() + skip, n);
            }
            offset += n;
            out += n;
            length -= n;
        }
        return true;
    }

    bool contiguous() const {
        return !working_->parent && working_->pageNumbers.empty();
    }

    bool ensureWritable() {
        const long pinned = working_.use_count() - (working_ == root_ ? 2 : 1);
        if (pinned <= 0 && !working_->parent && working_->pageNumbers.empty() &&
            working_->base.use_count() == 1)
            return true;
        if (pinned <= 0 && working_ != root_) return true;
        auto child = std::make_shared<Gen>();
        child->id = nextId_++;
        child->parent = working_;
        child->base = working_->base;
        child->size = working_->size;
        child->pageSize = working_->pageSize;
        working_ = std::move(child);
        return true;
    }

    bool storePage(uint64_t page, const uint8_t* data) {
        const uint32_t ps = working_->pageSize;
        std::vector<uint8_t> basePage(ps);
        const uint64_t off = page * uint64_t(ps);
        const size_t n = static_cast<size_t>(std::min(uint64_t(ps), working_->size - off));
        std::memcpy(basePage.data(), working_->base->data() + static_cast<size_t>(off), n);
        const bool matchesBase = std::memcmp(data, basePage.data(), ps) == 0;
        bool parentHas = false;
        for (auto* g = working_->parent.get(); g; g = g->parent.get()) {
            const auto it = std::lower_bound(g->pageNumbers.begin(), g->pageNumbers.end(), page);
            if (it != g->pageNumbers.end() && *it == page) {
                parentHas = true;
                break;
            }
        }
        // Edited in place, so a write costs one page and not the whole overlay. Room is made first,
        // so an insert can't throw halfway through.
        auto& pages = working_->pageNumbers;
        auto& payload = working_->pageData;
        auto& tomb = working_->tombstone;
        const auto it = std::lower_bound(pages.begin(), pages.end(), page);
        const size_t i = static_cast<size_t>(it - pages.begin());
        if (it != pages.end() && *it == page) {
            if (matchesBase && !parentHas) {
                pages.erase(pages.begin() + static_cast<std::ptrdiff_t>(i));
                tomb.erase(tomb.begin() + static_cast<std::ptrdiff_t>(i));
                payload.erase(payload.begin() + static_cast<std::ptrdiff_t>(i * ps),
                              payload.begin() + static_cast<std::ptrdiff_t>((i + 1) * ps));
            } else {
                tomb[i] = matchesBase ? 1 : 0;
                std::memcpy(payload.data() + i * ps, data, ps);
            }
        } else if (!(matchesBase && !parentHas)) {
            grow(pages, pages.size() + 1);
            grow(tomb, tomb.size() + 1);
            grow(payload, payload.size() + ps);
            pages.insert(pages.begin() + static_cast<std::ptrdiff_t>(i), page);
            tomb.insert(tomb.begin() + static_cast<std::ptrdiff_t>(i), matchesBase ? 1 : 0);
            payload.insert(payload.begin() + static_cast<std::ptrdiff_t>(i * ps), data, data + ps);
        }
        return true;
    }

    // Doubles the capacity when it runs out, so inserts stay amortised.
    template <typename T>
    static void grow(std::vector<T>& v, size_t need) {
        if (v.capacity() < need) v.reserve(std::max(need, v.capacity() * 2));
    }

    bool coalesce(std::vector<uint64_t>& pages, std::vector<uint8_t>& data, std::vector<uint8_t>& tomb) const {
        std::vector<const Gen*> chain;
        for (auto* g = working_.get(); g; g = g->parent.get()) chain.push_back(g);
        std::reverse(chain.begin(), chain.end());
        const uint32_t ps = working_->pageSize;
        for (const Gen* g : chain) {
            for (size_t i = 0; i < g->pageNumbers.size(); ++i) {
                const uint64_t page = g->pageNumbers[i];
                auto it = std::lower_bound(pages.begin(), pages.end(), page);
                const size_t at = static_cast<size_t>(it - pages.begin());
                if (it != pages.end() && *it == page) {
                    tomb[at] = g->tombstone[i];
                    std::memcpy(data.data() + at * ps, g->pageData.data() + i * ps, ps);
                } else {
                    pages.insert(it, page);
                    tomb.insert(tomb.begin() + static_cast<std::ptrdiff_t>(at), g->tombstone[i]);
                    data.insert(data.begin() + static_cast<std::ptrdiff_t>(at * ps),
                                g->pageData.data() + i * ps, g->pageData.data() + (i + 1) * ps);
                }
            }
        }
        return true;
    }

    void materialize() const {
        materialized_.assign(working_->base->begin(), working_->base->end());
        if (!materialized_.empty())
            readFrom(working_.get(), 0, materialized_.size(), materialized_.data());
    }

    std::shared_ptr<void> identity_;
    std::shared_ptr<Gen> root_;
    std::shared_ptr<Gen> working_;
    uint64_t nextId_{1};
    mutable std::vector<uint8_t> materialized_;
};

#endif
