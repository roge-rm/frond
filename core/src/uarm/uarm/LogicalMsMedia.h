#ifndef FROND_LOGICAL_MS_MEDIA_H
#define FROND_LOGICAL_MS_MEDIA_H

#include "storage/MsLayout.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

// A classic Memory Stick's physical pages kept in the v2 .frondms layout (storage/MsLayout.h):
// boot blocks, extra bytes and the location map in the side region, block data in logical slots
// and the spill area. Reads give exactly the bytes a flat physical image would hold. Programs AND
// into what is there and erases set a block to 0xff, wherever it lives.
//
// A block takes a slot when one of its bytes first stops being 0xff: logical slot L when its
// page 0 claims L and that slot is free, otherwise a spill slot, otherwise a free logical slot of
// an L nobody claims. When page 0 extra bytes change, or a block is erased, the holder of each L
// involved is chosen again (frondMsLayout::holderBetter) and moved into slot L, swapping with
// whatever was there. Slots never run out: there is one for every block but the boot blocks.
//
// The bytes go through `read` and `write` on the stored bytes (payload, padding, side region).
// Not thread safe; the controller calls it from the emulation thread.
class LogicalMsMedia {
   public:
    using Layout = frondMsLayout::Layout;
    using Claim = frondMsLayout::Claim;
    using ReadFn = std::function<bool(uint64_t offset, uint64_t length, void* dest)>;
    using WriteFn = std::function<bool(uint64_t offset, uint64_t length, const void* src)>;
    static constexpr uint32_t dataBytes = frondMsLayout::dataBytes, extraBytes = frondMsLayout::extraBytes;
    static constexpr uint32_t recordBytes = dataBytes + extraBytes;

    // Reads the map and page 0 extra bytes. False when the map doesn't fit the layout.
    bool Open(const Layout& layout, ReadFn read, WriteFn write) {
        layout_ = layout;
        read_ = std::move(read);
        write_ = std::move(write);
        return Reload();
    }
    void Close() {
        read_ = {};
        write_ = {};
        map_.clear();
        owner_.clear();
        page0_.clear();
    }
    bool IsOpen() const { return bool(read_); }
    const Layout& Shape() const { return layout_; }

    // After the stored bytes changed underneath, as a checkpoint restore does.
    bool Reload() {
        const auto& l = layout_;
        std::vector<uint8_t> raw(size_t(4) * l.blocks);
        std::vector<uint8_t> extras(size_t(l.Pages()) * extraBytes);
        if (!read_(l.MapAt(), raw.size(), raw.data()) || !read_(l.ExtrasAt(), extras.size(), extras.data()))
            return false;
        std::vector<uint32_t> map(l.blocks);
        std::vector<int32_t> owner(l.Slots(), -1);
        std::vector<std::array<uint8_t, 4>> page0(l.blocks);
        for (uint32_t b = 0; b < l.blocks; ++b) {
            map[b] = frondSdFile::get32(raw.data() + 4 * b);
            std::memcpy(page0[b].data(), extras.data() + size_t(b) * l.pagesPerBlock * extraBytes, 4);
            if (b < 2) {
                if (map[b] != frondMsLayout::bootSlot) return false;
                continue;
            }
            if (map[b] == frondMsLayout::erasedSlot) continue;
            const uint32_t slot = SlotIndex(map[b]);
            if (slot >= l.Slots() || owner[slot] >= 0) return false;
            owner[slot] = int32_t(b);
        }
        map_ = std::move(map);
        owner_ = std::move(owner);
        page0_ = std::move(page0);
        return true;
    }

    bool Read(uint32_t block, uint32_t page, uint8_t* record) const {
        if (!read_(layout_.ExtraAt(block, page), extraBytes, record + dataBytes)) return false;
        if (block < 2) return read_(layout_.BootPageAt(block, page), dataBytes, record);
        if (map_[block] == frondMsLayout::erasedSlot) {
            std::memset(record, 0xff, dataBytes);
            return true;
        }
        return read_(layout_.SlotAt(SlotIndex(map_[block])) + uint64_t(page) * dataBytes, dataBytes, record);
    }

    // Flash rules: the result is what is there AND `incoming`, for the data, the extra bytes or
    // both. `changed` says whether any stored byte changed.
    bool Program(uint32_t block, uint32_t page, const uint8_t* incoming, bool data, bool extra, bool& changed) {
        changed = false;
        if (!data && !extra) return true;
        uint8_t current[recordBytes], result[recordBytes];
        if (!Read(block, page, current)) return false;
        std::memcpy(result, current, recordBytes);
        if (data)
            for (unsigned i = 0; i < dataBytes; ++i) result[i] &= incoming[i];
        if (extra)
            for (unsigned i = dataBytes; i < recordBytes; ++i) result[i] &= incoming[i];
        const bool dataChanged = std::memcmp(result, current, dataBytes) != 0;
        const bool extraChanged = std::memcmp(result + dataBytes, current + dataBytes, extraBytes) != 0;
        if (!dataChanged && !extraChanged) return true;
        changed = true;
        if (extraChanged && !write_(layout_.ExtraAt(block, page), extraBytes, result + dataBytes)) return false;
        if (block < 2) return !dataChanged || write_(layout_.BootPageAt(block, page), dataBytes, result);
        const Claim before = ClaimOf(block);
        if (page == 0) std::memcpy(page0_[block].data(), result + dataBytes, 4);
        const bool taking = map_[block] == frondMsLayout::erasedSlot;
        if (taking && !Take(block)) return false;
        if (dataChanged &&
            !write_(layout_.SlotAt(SlotIndex(map_[block])) + uint64_t(page) * dataBytes, dataBytes, result))
            return false;
        const Claim after = ClaimOf(block);
        if (taking) return Rebalance(after.logical);
        if (before.logical == after.logical && before.updating == after.updating) return true;
        return Rebalance(before.logical) && Rebalance(after.logical);
    }

    bool Erase(uint32_t block, bool& changed) {
        changed = false;
        const auto& l = layout_;
        std::vector<uint8_t> blank(size_t(l.pagesPerBlock) * extraBytes, 0xff);
        if (block < 2) {
            std::vector<uint8_t> current(blank.size()), data(l.BlockBytes());
            if (!read_(l.ExtraAt(block, 0), current.size(), current.data()) ||
                !read_(l.BootPageAt(block, 0), data.size(), data.data()))
                return false;
            if (current == blank && std::all_of(data.begin(), data.end(), [](uint8_t b) { return b == 0xff; }))
                return true;
            changed = true;
            std::fill(data.begin(), data.end(), 0xff);
            return write_(l.ExtraAt(block, 0), blank.size(), blank.data()) &&
                   write_(l.BootPageAt(block, 0), data.size(), data.data());
        }
        if (map_[block] == frondMsLayout::erasedSlot) {
            // A block with no slot has 0xff data; only a damaged file leaves it extra bytes.
            std::vector<uint8_t> current(blank.size());
            if (!read_(l.ExtraAt(block, 0), current.size(), current.data())) return false;
            if (current == blank) return true;
            changed = true;
            page0_[block].fill(0xff);
            return write_(l.ExtraAt(block, 0), blank.size(), blank.data());
        }
        changed = true;
        const Claim before = ClaimOf(block);
        owner_[SlotIndex(map_[block])] = -1;
        if (!SetMap(block, frondMsLayout::erasedSlot) || !write_(l.ExtraAt(block, 0), blank.size(), blank.data()))
            return false;
        page0_[block].fill(0xff);
        return Rebalance(before.logical);
    }

    // The block in logical slot L when it claims L, or -1. That block's data is L's.
    int32_t Holder(uint32_t logical) const {
        if (logical >= layout_.UserBlocks()) return -1;
        const int32_t b = owner_[logical];
        return b >= 0 && ClaimOf(uint32_t(b)).logical == int32_t(logical) ? b : -1;
    }
    uint32_t MapEntry(uint32_t block) const { return map_[block]; }

    // Every claimed L in its slot, held by the block the driver would keep, and the map and
    // owners agreeing. For tests.
    bool Consistent() const {
        const auto& l = layout_;
        for (uint32_t b = 2; b < l.blocks; ++b) {
            if (map_[b] == frondMsLayout::erasedSlot) continue;
            if (owner_[SlotIndex(map_[b])] != int32_t(b)) return false;
        }
        for (uint32_t L = 0; L < l.UserBlocks(); ++L) {
            const int32_t best = Best(L);
            if (best >= 0 && owner_[L] != best) return false;
        }
        return true;
    }

    // Lays out a flat physical image (blocks x pages x 512+9) in `stored`, which must be
    // StoredBytes() long. Used for v1 files and imports.
    static bool Build(const Layout& l, const uint8_t* physical, std::vector<uint8_t>& stored) {
        stored.assign(size_t(l.StoredBytes()), 0);
        auto record = [&](uint32_t b, uint32_t p) { return physical + (size_t(b) * l.pagesPerBlock + p) * recordBytes; };
        for (uint32_t b = 0; b < l.blocks; ++b)
            for (uint32_t p = 0; p < l.pagesPerBlock; ++p)
                std::memcpy(stored.data() + l.ExtraAt(b, p), record(b, p) + dataBytes, extraBytes);
        for (uint32_t b = 0; b < 2; ++b)
            for (uint32_t p = 0; p < l.pagesPerBlock; ++p)
                std::memcpy(stored.data() + l.BootPageAt(b, p), record(b, p), dataBytes);
        std::vector<uint32_t> map(l.blocks, frondMsLayout::erasedSlot);
        map[0] = map[1] = frondMsLayout::bootSlot;
        std::vector<int32_t> owner(l.Slots(), -1);
        std::vector<uint8_t> written(l.blocks);
        for (uint32_t b = 2; b < l.blocks; ++b) {
            const uint8_t* first = record(b, 0);
            const size_t n = size_t(l.pagesPerBlock) * recordBytes;
            written[b] = !std::all_of(first, first + n, [](uint8_t v) { return v == 0xff; });
        }
        auto claimOf = [&](uint32_t b) { return frondMsLayout::claimOf(l, b, record(b, 0) + dataBytes); };
        // Holders into their logical slots first, then everything else into spill and free slots.
        std::vector<int32_t> best(l.UserBlocks(), -1);
        for (uint32_t b = 2; b < l.blocks; ++b) {
            if (!written[b]) continue;
            const auto c = claimOf(b);
            if (c.logical < 0) continue;
            int32_t& h = best[size_t(c.logical)];
            if (h < 0 || frondMsLayout::holderBetter(b, c, uint32_t(h), claimOf(uint32_t(h)))) h = int32_t(b);
        }
        for (uint32_t L = 0; L < l.UserBlocks(); ++L)
            if (best[L] >= 0) {
                owner[L] = best[L];
                map[size_t(best[L])] = L;
            }
        uint32_t spill = l.UserBlocks(), unused = 0;
        for (uint32_t b = 2; b < l.blocks; ++b) {
            if (!written[b] || map[b] != frondMsLayout::erasedSlot) continue;
            uint32_t slot;
            if (spill < l.Slots()) {
                slot = spill++;
            } else {
                while (unused < l.UserBlocks() && owner[unused] >= 0) ++unused;
                if (unused == l.UserBlocks()) return false;
                slot = unused;
            }
            owner[slot] = int32_t(b);
            map[b] = SlotEntry(l, slot);
        }
        for (uint32_t b = 2; b < l.blocks; ++b) {
            frondSdFile::put32(stored.data() + l.MapAt() + 4 * b, map[b]);
            if (map[b] == frondMsLayout::erasedSlot) continue;
            const uint64_t at = l.SlotAt(SlotIndex(l, map[b]));
            for (uint32_t p = 0; p < l.pagesPerBlock; ++p)
                std::memcpy(stored.data() + at + uint64_t(p) * dataBytes, record(b, p), dataBytes);
        }
        frondSdFile::put32(stored.data() + l.MapAt(), map[0]);
        frondSdFile::put32(stored.data() + l.MapAt() + 4, map[1]);
        return true;
    }

    static uint32_t SlotIndex(const Layout& l, uint32_t entry) {
        return entry & frondMsLayout::spillBit ? l.UserBlocks() + (entry & ~frondMsLayout::spillBit) : entry;
    }
    static uint32_t SlotEntry(const Layout& l, uint32_t slot) {
        return slot < l.UserBlocks() ? slot : frondMsLayout::spillBit | (slot - l.UserBlocks());
    }

   private:
    uint32_t SlotIndex(uint32_t entry) const { return SlotIndex(layout_, entry); }

    Claim ClaimOf(uint32_t block) const {
        if (block < 2 || map_[block] == frondMsLayout::erasedSlot) return {};
        return frondMsLayout::claimOf(layout_, block, page0_[block].data());
    }

    // The block the driver would keep for L, or -1 when nothing claims it.
    int32_t Best(uint32_t logical) const {
        const uint32_t segment = layout_.SegmentOfLogical(logical);
        const uint32_t first = std::max<uint32_t>(2, segment * frondMsLayout::segmentBlocks);
        const uint32_t end = std::min(layout_.blocks, (segment + 1) * frondMsLayout::segmentBlocks);
        int32_t best = -1;
        Claim bestClaim;
        for (uint32_t b = first; b < end; ++b) {
            const Claim c = ClaimOf(b);
            if (c.logical != int32_t(logical)) continue;
            if (best < 0 || frondMsLayout::holderBetter(b, c, uint32_t(best), bestClaim)) {
                best = int32_t(b);
                bestClaim = c;
            }
        }
        return best;
    }

    bool SetMap(uint32_t block, uint32_t entry) {
        uint8_t raw[4];
        frondSdFile::put32(raw, entry);
        if (!write_(layout_.MapAt() + 4ull * block, 4, raw)) return false;
        map_[block] = entry;
        return true;
    }

    bool Place(uint32_t block, uint32_t slot) {
        owner_[slot] = int32_t(block);
        return SetMap(block, SlotEntry(layout_, slot));
    }

    // A free spill slot, then a free logical slot (one no claimed L uses), or Slots().
    uint32_t FreeSlot() const {
        const auto& l = layout_;
        for (uint32_t s = l.UserBlocks(); s < l.Slots(); ++s)
            if (owner_[s] < 0) return s;
        for (uint32_t s = 0; s < l.UserBlocks(); ++s)
            if (owner_[s] < 0) return s;
        return l.Slots();
    }

    // Gives an erased block a slot, all 0xff, before its first write.
    bool Take(uint32_t block) {
        const Claim c = frondMsLayout::claimOf(layout_, block, page0_[block].data());
        uint32_t slot = c.logical >= 0 && owner_[size_t(c.logical)] < 0 ? uint32_t(c.logical) : FreeSlot();
        if (slot >= layout_.Slots()) return false;
        std::vector<uint8_t> blank(layout_.BlockBytes(), 0xff);
        return write_(layout_.SlotAt(slot), blank.size(), blank.data()) && Place(block, slot);
    }

    // Puts L's holder into slot L. What was there takes the holder's old slot.
    bool Rebalance(int32_t logical) {
        if (logical < 0) return true;
        const int32_t best = Best(uint32_t(logical));
        const uint32_t target = uint32_t(logical);
        if (best < 0 || owner_[target] == best) return true;
        const uint32_t from = SlotIndex(map_[size_t(best)]);
        const auto& l = layout_;
        std::vector<uint8_t> moving(l.BlockBytes());
        if (!read_(l.SlotAt(from), moving.size(), moving.data())) return false;
        const int32_t other = owner_[target];
        if (other >= 0) {
            std::vector<uint8_t> displaced(l.BlockBytes());
            if (!read_(l.SlotAt(target), displaced.size(), displaced.data()) ||
                !write_(l.SlotAt(from), displaced.size(), displaced.data()) || !Place(uint32_t(other), from))
                return false;
        } else {
            owner_[from] = -1;
        }
        return write_(l.SlotAt(target), moving.size(), moving.data()) && Place(uint32_t(best), target);
    }

    Layout layout_{};
    ReadFn read_;
    WriteFn write_;
    std::vector<uint32_t> map_;
    std::vector<int32_t> owner_;
    std::vector<std::array<uint8_t, 4>> page0_;
};

#endif
