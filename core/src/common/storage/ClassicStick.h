#ifndef FROND_STORAGE_CLASSIC_STICK_H
#define FROND_STORAGE_CLASSIC_STICK_H

#include "storage/Container.h"
#include "storage/Errors.h"
#include "storage/MsLayout.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <vector>

// What Files needs to know about a classic Memory Stick in a v2 .frondms (storage/MsLayout.h), and
// the sectors the TG50's slot driver writes when it formats one (3452Slot 2.10, S0 findings).
//
// Files reads and writes the stick's logical sectors as the Sony driver does: L's data from the
// block that holds it, zeros for an L nobody holds. It only opens a settled stick: every L has at
// most one claiming block, and that block sits in logical slot L. Then L's data is the payload at
// L x block bytes, so a held L is read and written in place. An L nobody holds gets a free block in
// its segment first (claim()), with every page's extra bytes f8 ff Lhi Llo ff ff ff ff ff as the
// driver writes a new block, and zero data.
namespace frondStorage {

constexpr const char* unsettledStickMessage = "Put the stick in the TG50 once to finish its last write.";
constexpr const char* blankStickMessage = "The Memory Stick is blank.";
constexpr const char* stickBootMessage = "The Memory Stick's boot blocks are damaged.";
constexpr const char* stickFullMessage = "The Memory Stick has no free block left.";

namespace classicStick {

using frondMsLayout::Layout;

// A stick's map, page 0 claims and disabled blocks, read once when a CardFile opens it.
struct State {
    Layout layout{};
    std::vector<uint32_t> map;     // per physical block
    std::vector<int32_t> holder;   // per L: the claiming block, or -1
    std::vector<int32_t> owner;    // per slot: the block in it, or -1
    std::vector<uint8_t> unusable; // per physical block: in the disabled-block table
    std::array<uint8_t, 8> made{};  // boot block 0x1ac-0x1b3, the stick's date and time
    bool settled{true}, bootOk{false};

    uint32_t slotOf(uint32_t entry) const { return entry & frondMsLayout::spillBit ? layout.UserBlocks() + (entry & ~frondMsLayout::spillBit) : entry; }
    uint32_t entryOf(uint32_t slot) const { return slot < layout.UserBlocks() ? slot : frondMsLayout::spillBit | (slot - layout.UserBlocks()); }
};

using ReadFn = std::function<bool(uint64_t offset, uint64_t length, void* dest)>;

// Whether boot block 0 has the disabled-block entry the driver needs to mount: 0x170 zero, 0x174
// a big-endian size 1 to 0x200, 0x178 == 1.
inline bool bootEntryValid(const uint8_t* page0) {
    const uint32_t size = uint32_t(page0[0x174]) << 24 | uint32_t(page0[0x175]) << 16 | uint32_t(page0[0x176]) << 8 | page0[0x177];
    return page0[0x178] == 1 && !page0[0x170] && !page0[0x171] && !page0[0x172] && !page0[0x173] && size >= 1 && size <= 0x200;
}

// Reads the state from the stored bytes (payload, padding, side region).
inline bool load(const Layout& l, const ReadFn& read, State& s) {
    s = State{};
    s.layout = l;
    std::vector<uint8_t> raw(size_t(4) * l.blocks), extras(size_t(l.Pages()) * frondMsLayout::extraBytes);
    std::vector<uint8_t> boot(2 * frondMsLayout::dataBytes);
    if (!read(l.MapAt(), raw.size(), raw.data()) || !read(l.ExtrasAt(), extras.size(), extras.data()) ||
        !read(l.BootPageAt(0, 0), boot.size(), boot.data()))
        return false;
    s.map.resize(l.blocks);
    s.holder.assign(l.UserBlocks(), -1);
    s.owner.assign(l.Slots(), -1);
    s.unusable.assign(l.blocks, 0);
    std::memcpy(s.made.data(), boot.data() + 0x1ac, 8);
    s.bootOk = bootEntryValid(boot.data());
    if (s.bootOk) {
        const uint8_t* e = boot.data() + 0x174;
        const uint32_t size = uint32_t(e[0]) << 24 | uint32_t(e[1]) << 16 | uint32_t(e[2]) << 8 | e[3];
        for (uint32_t i = 0; i + 1 < size && i + 1 < frondMsLayout::dataBytes; i += 2) {
            const uint32_t block = uint32_t(boot[512 + i]) << 8 | boot[512 + i + 1];
            if (block < l.blocks) s.unusable[block] = 1;
        }
    }
    for (uint32_t b = 0; b < l.blocks; ++b) {
        s.map[b] = frondSdFile::get32(raw.data() + 4 * b);
        if (b < 2) {
            if (s.map[b] != frondMsLayout::bootSlot) return false;
            continue;
        }
        if (s.map[b] == frondMsLayout::erasedSlot) continue;
        const uint32_t slot = s.slotOf(s.map[b]);
        if (slot >= l.Slots() || s.owner[slot] >= 0) return false;
        s.owner[slot] = int32_t(b);
        const auto c = frondMsLayout::claimOf(l, b, extras.data() + size_t(b) * l.pagesPerBlock * frondMsLayout::extraBytes);
        if (c.logical < 0) continue;
        if (s.holder[size_t(c.logical)] >= 0 || slot != uint32_t(c.logical)) s.settled = false;
        else s.holder[size_t(c.logical)] = int32_t(b);
    }
    return true;
}

// How claim() changes the stored bytes: a borrower moved out of slot L, then L's new block.
struct Claim {
    uint32_t logical{}, block{};
    int32_t borrower{-1};
    uint32_t borrowerSlot{};
};

// Picks the block for an L nobody holds, as the driver's free search does: from L's home block
// forward, wrapping inside its segment, the first erased block that isn't disabled. The segment
// keeps a free block (two in the last one, which the driver reserves). Throws when there is none.
inline Claim plan(State& s, uint32_t logical) {
    const auto& l = s.layout;
    const uint32_t segment = l.SegmentOfLogical(logical);
    const uint32_t first = segment * frondMsLayout::segmentBlocks;
    const uint32_t count = std::min(l.blocks, first + frondMsLayout::segmentBlocks) - first;
    auto usable = [&](uint32_t b) { return b >= 2 && s.map[b] == frondMsLayout::erasedSlot && !s.unusable[b]; };
    uint32_t free = 0;
    for (uint32_t i = 0; i < count; ++i) free += usable(first + i);
    const uint32_t keep = segment + 1 == l.Segments() ? 2 : 1;
    if (free <= keep) throw Error(stickFullMessage);
    const uint32_t home = l.Home(logical);
    Claim c;
    c.logical = logical;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t b = first + (home - first + i) % count;
        if (!usable(b)) continue;
        c.block = b;
        break;
    }
    c.borrower = s.owner[logical];
    if (c.borrower >= 0) {
        // A block that claims nothing borrowed slot L when the spill area was full: it moves to a
        // free spill slot, or a free slot of another L nobody holds.
        uint32_t slot = l.Slots();
        for (uint32_t i = l.UserBlocks(); i < l.Slots() && slot == l.Slots(); ++i)
            if (s.owner[i] < 0) slot = i;
        for (uint32_t i = 0; i < l.UserBlocks() && slot == l.Slots(); ++i)
            if (s.owner[i] < 0 && i != logical) slot = i;
        if (slot == l.Slots()) throw Error(stickFullMessage);
        c.borrowerSlot = slot;
    }
    return c;
}

// Notes a claim done on the stored bytes.
inline void apply(State& s, const Claim& c) {
    if (c.borrower >= 0) {
        s.owner[c.borrowerSlot] = c.borrower;
        s.map[size_t(c.borrower)] = s.entryOf(c.borrowerSlot);
    }
    s.owner[c.logical] = int32_t(c.block);
    s.map[c.block] = c.logical;
    s.holder[c.logical] = int32_t(c.block);
}

// Every page's extra bytes for a new block holding L.
inline std::vector<uint8_t> newBlockExtras(const Layout& l, uint32_t logical) {
    std::vector<uint8_t> out(size_t(l.pagesPerBlock) * frondMsLayout::extraBytes, 0xff);
    for (uint32_t p = 0; p < l.pagesPerBlock; ++p) {
        uint8_t* e = out.data() + size_t(p) * frondMsLayout::extraBytes;
        e[0] = 0xf8;
        e[1] = 0xff;
        e[2] = uint8_t(logical >> 8);
        e[3] = uint8_t(logical);
    }
    return out;
}

// The driver's FORMAT for each size (3452Slot 2.10 0x20a5bd90): partition entry bytes 0-7, start,
// sectors, sectors per cluster, sectors per FAT, heads.
struct FormatRow {
    uint32_t megabytes;
    uint8_t entry[8];
    uint32_t hidden, total, cluster, fatSectors, heads;
};
constexpr FormatRow formatRows[] = {
    {4, {0x80, 0x01, 0x0c, 0x00, 0x01, 0x01, 0x10, 0xf5}, 27, 7845, 16, 2, 2},
    {8, {0x80, 0x01, 0x0a, 0x00, 0x01, 0x01, 0x50, 0xed}, 25, 15783, 16, 3, 2},
    {16, {0x80, 0x01, 0x0a, 0x00, 0x01, 0x03, 0x50, 0xed}, 25, 31591, 32, 3, 4},
    {32, {0x80, 0x01, 0x04, 0x00, 0x01, 0x03, 0xd0, 0xdd}, 19, 63341, 32, 6, 4},
    {64, {0x80, 0x02, 0x08, 0x00, 0x01, 0x07, 0xd0, 0xdd}, 39, 126681, 32, 12, 8},
    {128, {0x80, 0x02, 0x02, 0x00, 0x06, 0x0f, 0xd0, 0xdd}, 33, 253407, 32, 31, 16},
};
inline const FormatRow* formatRow(uint32_t megabytes) {
    for (const auto& r : formatRows)
        if (r.megabytes == megabytes) return &r;
    return nullptr;
}

// The FAT time and date of MEMSTICK.IND from boot bytes 0x1ac-0x1b3: time zone, year (big endian,
// or two decimal halves as Frond's own sticks have it), month, day, hour, minute, second. A field
// of 0xff, or one out of range, gives 0.
inline std::pair<uint16_t, uint16_t> madeStamp(const std::array<uint8_t, 8>& m) {
    auto field = [](uint8_t v, unsigned lo, unsigned hi) { return v != 0xff && v >= lo && v <= hi ? unsigned(v) : 0u; };
    unsigned year = unsigned(m[1]) << 8 | m[2];
    if (m[1] == 0xff || m[2] == 0xff) year = 0;
    else if (year < 1980 || year > 2107) year = m[1] < 100 && m[2] < 100 ? m[1] * 100u + m[2] : 0;
    if (year < 1980 || year > 2107) year = 0;
    const unsigned month = field(m[3], 1, 12), day = field(m[4], 1, 31);
    const unsigned hour = field(m[5], 0, 23), minute = field(m[6], 0, 59), second = field(m[7], 0, 59);
    const uint16_t time = uint16_t(hour << 11 | minute << 5 | second / 2);
    const uint16_t date = year && month && day ? uint16_t((year - 1980) << 9 | month << 5 | day) : 0;
    return {time, date};
}

// The sectors the driver's FORMAT writes, by payload offset: the MBR with one entry, the boot
// sector, both FATs, and the root folder with a hidden, read-only MEMSTICK.IND. The rest of the
// root is zeroed, which the driver leaves as it was.
inline std::map<int64_t, std::vector<uint8_t>> formatSectors(const Layout& l, const std::array<uint8_t, 8>& made) {
    const FormatRow* r = formatRow(l.megabytes);
    if (!r) throw Error("Frond can't format a Memory Stick of that size.");
    std::map<int64_t, std::vector<uint8_t>> out;
    auto sector = [&](int64_t index) -> std::vector<uint8_t>& {
        auto& s = out[index * 512];
        s.assign(512, 0);
        return s;
    };
    auto le16 = [](uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); };
    auto le32 = [](uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); };
    const bool fat16 = r->entry[4] == 0x06;
    auto& mbr = sector(0);
    std::memcpy(mbr.data() + 0x1be, r->entry, 8);
    le32(mbr.data() + 0x1c6, r->hidden);
    le32(mbr.data() + 0x1ca, r->total);
    mbr[0x1fe] = 0x55;
    mbr[0x1ff] = 0xaa;
    auto& boot = sector(r->hidden);
    boot[0] = 0xe9;
    std::memset(boot.data() + 3, ' ', 8);
    le16(boot.data() + 0x0b, 512);
    boot[0x0d] = uint8_t(r->cluster);
    le16(boot.data() + 0x0e, 1);
    boot[0x10] = 2;
    le16(boot.data() + 0x11, 512);
    if (r->total < 0x10000) le16(boot.data() + 0x13, r->total);
    else le32(boot.data() + 0x20, r->total);
    boot[0x15] = 0xf8;
    le16(boot.data() + 0x16, r->fatSectors);
    le16(boot.data() + 0x18, 16);
    le16(boot.data() + 0x1a, r->heads);
    le32(boot.data() + 0x1c, r->hidden);
    boot[0x26] = 0x29;
    std::memcpy(boot.data() + 0x36, fat16 ? "FAT16   " : "FAT12   ", 8);
    boot[0x1fe] = 0x55;
    boot[0x1ff] = 0xaa;
    const int64_t fatAt = int64_t(r->hidden) + 1;
    for (int64_t copy = 0; copy < 2; ++copy)
        for (int64_t i = 0; i < r->fatSectors; ++i) {
            auto& s = sector(fatAt + copy * r->fatSectors + i);
            if (i) continue;
            s[0] = 0xf8;
            s[1] = s[2] = 0xff;
            if (fat16) s[3] = 0xff;
        }
    const int64_t rootAt = fatAt + 2 * int64_t(r->fatSectors);
    for (int64_t i = 0; i < 32; ++i) sector(rootAt + i);
    auto& root = out[rootAt * 512];
    std::memcpy(root.data(), "MEMSTICKIND", 11);
    root[11] = 0x03;
    const auto [time, date] = madeStamp(made);
    le16(root.data() + 0x16, time);
    le16(root.data() + 0x18, date);
    return out;
}

}  // namespace classicStick
}  // namespace frondStorage

#endif
