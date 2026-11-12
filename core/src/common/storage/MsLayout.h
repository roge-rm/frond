#ifndef FROND_STORAGE_MS_LAYOUT_H
#define FROND_STORAGE_MS_LAYOUT_H

#include "storage/Container.h"

#include <cstdint>

// Where a classic Memory Stick keeps its bytes in a v2 .frondms (storage/Container.h).
//
// The payload is the logical region: user blocks x block bytes, logical block L at L x block
// bytes, so logical sector = L x pages per block + page. Segment 0 has 494 user blocks (L 0-493),
// each later 512-block segment 496.
//
// The side region, from the chunk boundary after the payload:
// - boot blocks: the data bytes of physical blocks 0 and 1, page by page;
// - extra bytes: 9 per physical page, every page of every block, in page order;
// - location map: a u32 (little endian) per physical block: erasedSlot, bootSlot, a logical slot
//   L, or spillBit | S for spill slot S;
// - spill area: blocks - 2 - user blocks whole blocks.
// A block with any byte that isn't 0xff has a slot; its data lives there, its extra bytes always
// in the table. The block that holds L (holderBetter) is in logical slot L.
namespace frondMsLayout {
constexpr uint32_t dataBytes = 512, extraBytes = 9, segmentBlocks = 512;
constexpr uint32_t erasedSlot = 0xffffffffu, bootSlot = 0xfffffffeu, spillBit = 0x80000000u;

struct Layout {
    uint32_t megabytes{}, blocks{}, pagesPerBlock{};

    constexpr uint32_t Segments() const { return blocks / segmentBlocks; }
    constexpr uint32_t UserBlocks() const { return Segments() * 496 - 2; }
    constexpr uint32_t SpillBlocks() const { return blocks - 2 - UserBlocks(); }
    constexpr uint32_t Slots() const { return UserBlocks() + SpillBlocks(); }
    constexpr uint64_t Pages() const { return uint64_t(blocks) * pagesPerBlock; }
    constexpr uint64_t BlockBytes() const { return uint64_t(pagesPerBlock) * dataBytes; }
    constexpr uint64_t PayloadBytes() const { return UserBlocks() * BlockBytes(); }
    constexpr uint64_t SideAt() const { return frondSdFile::sideOffset(PayloadBytes()); }
    constexpr uint64_t BootAt() const { return SideAt(); }
    constexpr uint64_t ExtrasAt() const { return BootAt() + 2 * BlockBytes(); }
    constexpr uint64_t MapAt() const { return ExtrasAt() + extraBytes * Pages(); }
    constexpr uint64_t SpillAt() const { return MapAt() + 4ull * blocks; }
    constexpr uint64_t SideBytes() const { return SpillAt() + SpillBlocks() * BlockBytes() - SideAt(); }
    constexpr uint64_t StoredBytes() const { return SideAt() + SideBytes(); }
    // The data of slot i: logical slot L for i < user blocks, spill slot i - user blocks after.
    constexpr uint64_t SlotAt(uint32_t slot) const {
        return slot < UserBlocks() ? uint64_t(slot) * BlockBytes() : SpillAt() + (slot - UserBlocks()) * BlockBytes();
    }
    constexpr uint64_t ExtraAt(uint32_t block, uint32_t page) const {
        return ExtrasAt() + (uint64_t(block) * pagesPerBlock + page) * extraBytes;
    }
    constexpr uint64_t BootPageAt(uint32_t block, uint32_t page) const {
        return BootAt() + (uint64_t(block) * pagesPerBlock + page) * dataBytes;
    }
    // Logical blocks [first, end) of a segment.
    constexpr uint32_t SegmentFirst(uint32_t segment) const { return segment ? 494 + (segment - 1) * 496 : 0; }
    constexpr uint32_t SegmentEnd(uint32_t segment) const { return 494 + segment * 496; }
    constexpr uint32_t SegmentOfLogical(uint32_t logical) const { return logical < 494 ? 0 : (logical - 494) / 496 + 1; }
    // The physical block the Sony driver looks at first for L, and where an import puts it.
    constexpr uint32_t Home(uint32_t logical) const {
        return logical < 494 ? logical + 2 : SegmentOfLogical(logical) * segmentBlocks + (logical - 494) % 496;
    }
};

constexpr Layout layouts[] = {{4, 512, 16},   {8, 1024, 16},  {16, 1024, 32},
                              {32, 2048, 32}, {64, 4096, 32}, {128, 8192, 32}};

inline const Layout* byShape(uint64_t blocks, uint64_t pagesPerBlock) {
    for (const auto& l : layouts)
        if (l.blocks == blocks && l.pagesPerBlock == pagesPerBlock) return &l;
    return nullptr;
}
inline const Layout* byMegabytes(uint64_t megabytes) {
    for (const auto& l : layouts)
        if (l.megabytes == megabytes) return &l;
    return nullptr;
}
// The layout of a classic container header's payload and side region sizes.
inline const Layout* byContainer(uint64_t payloadBytes, uint64_t sideBytes) {
    for (const auto& l : layouts)
        if (l.PayloadBytes() == payloadBytes && l.SideBytes() == sideBytes) return &l;
    return nullptr;
}
inline const Layout* byStoredBytes(uint64_t stored) {
    for (const auto& l : layouts)
        if (l.StoredBytes() == stored) return &l;
    return nullptr;
}
inline const Layout* of(const frondSdFile::Info& info) {
    return info.classic ? byContainer(info.payloadBytes, info.sideBytes) : nullptr;
}

// Page 0 extra bytes: overwrite flag, management flag, L big endian.
struct Claim {
    int32_t logical{-1};  // -1: none
    bool updating{};      // UDST set: an older copy may still be the one in use
};

// What the Sony driver takes from a block's page 0 extra bytes at mount: BKST set, PGST 11,
// ATFLG set, and an L inside the block's own segment. Blocks 0 and 1 hold the boot blocks.
inline Claim claimOf(const Layout& l, uint32_t block, const uint8_t* extra) {
    Claim c;
    if (block < 2 || block >= l.blocks) return c;
    const uint8_t overwrite = extra[0], management = extra[1];
    const uint32_t logical = uint32_t(extra[2]) << 8 | extra[3];
    const uint32_t segment = block / segmentBlocks;
    if (!(overwrite & 0x80) || (overwrite & 0x60) != 0x60 || !(management & 0x08) || logical == 0xffff ||
        logical < l.SegmentFirst(segment) || logical >= l.SegmentEnd(segment))
        return c;
    c.logical = int32_t(logical);
    c.updating = overwrite & 0x10;
    return c;
}

// Which of two blocks claiming the same L the driver keeps: UDST clear beats UDST set, and a tie
// goes to the higher physical block.
inline bool holderBetter(uint32_t a, const Claim& ca, uint32_t b, const Claim& cb) {
    if (ca.updating != cb.updating) return !ca.updating;
    return a > b;
}
}  // namespace frondMsLayout

#endif
