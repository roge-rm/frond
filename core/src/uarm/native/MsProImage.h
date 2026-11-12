#pragma once
#include "MemoryStickGeometry.h"
#include "SdImage.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The Memory Stick Pro attribute area the TG50 driver reads with READ_ATRB. New sticks get the
// SD FAT16 layouts of the same size (SdImage.h).
namespace frondMsPro {
inline void be16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void be32(uint8_t* p, uint32_t v) { be16(p, uint16_t(v >> 16)); be16(p + 2, uint16_t(v)); }

// The serial number in system information, from the card's identity.
inline uint32_t serialOf(const std::string& identity) {
    uint32_t h = 2166136261u;
    for (unsigned char c : identity) h = (h ^ c) * 16777619u;
    return h;
}

// Attribute area: signature A5C3, version 1, then a directory of 12-byte entries at 0x10 (address,
// size, id). The Sony driver needs system information (0x10, 0x60 bytes, class 2), the MBR
// partition entry (0x20, 0x10 bytes) and a FAT16 boot sector start (0x21, 0x40 bytes), each at
// 0x1a0 or later and ending by 0x8000. Its capacity is the MBR entry's start plus length. These are
// the card's factory values; a guest format doesn't change them.
constexpr uint32_t blockSectors = 512;  // 256 KB blocks
inline std::vector<uint8_t> attribute(unsigned mib, const std::string& identity) {
    const auto layout = frondSd::layout(mib);
    const uint32_t userBlocks = sectorsOf(mib) / blockSectors;
    std::vector<uint8_t> out(1024);
    auto* p = out.data();
    p[0] = 0xa5; p[1] = 0xc3; p[2] = 1;
    struct Entry { uint32_t address, size; uint8_t id; };
    const Entry entries[] = {{0x1a0, 0x60, 0x10}, {0x200, 0x10, 0x20}, {0x210, 0x40, 0x21}, {0x260, 0x30, 0x15}};
    p[4] = uint8_t(std::size(entries));
    for (size_t i = 0; i < std::size(entries); ++i) {
        auto* e = p + 0x10 + 12 * i;
        be32(e, entries[i].address); be32(e + 4, entries[i].size); e[8] = entries[i].id;
    }
    auto* info = p + 0x1a0;
    info[0] = 2;                              // class: Memory Stick Pro
    be16(info + 2, blockSectors);             // block size in units
    be16(info + 4, uint16_t((userBlocks + 495) / 496 * 512));
    be16(info + 6, uint16_t(userBlocks));
    be16(info + 8, 0x200);                    // page size
    const uint8_t date[8] = {0x00, 0x07, 0xd4, 1, 1, 0, 0, 0};  // 2004-01-01, GMT
    std::copy(date, date + 8, info + 12);
    be32(info + 0x14, serialOf(identity));
    info[0x24] = 0x33;                        // 3.3 V
    be16(info + 0x2c, 0x200);                 // unit size
    info[0x2e] = 0;                           // sub class: read/write
    info[0x36] = 1;                           // format type: FAT
    const char id[] = "FROND MS PRO";
    std::copy(id, id + sizeof(id) - 1, info + 0x40);
    std::copy_n(layout[0].bytes.data() + 446, 0x10, p + 0x200);
    std::copy_n(layout[1].bytes.data(), 0x40, p + 0x210);
    const std::string model = "MEMORYSTICK-PRO " + std::to_string(mib >= 1024 ? (mib + 512) / 1024 : mib) +
                              (mib >= 1024 ? "GB" : "MB");
    std::fill_n(p + 0x260, 0x30, uint8_t(' '));
    std::copy(model.begin(), model.end(), p + 0x260);
    return out;
}

// The card's FORMAT command: a quick format back to the factory layout. Sectors from the MBR to
// the end of the root directory are zeroed, then the layout's sectors written. The Sony update's
// FATFS 5.01 relies on this and only reads the MBR afterwards; the stock driver writes its own.
inline bool format(unsigned mib, const std::function<bool(uint32_t, const uint8_t*)>& write) {
    const auto layout = frondSd::layout(mib);
    const uint32_t end = layout.back().index + 32;  // root directory: 512 entries
    const std::array<uint8_t, 512> zero{};
    size_t next = 0;
    for (uint32_t sector = 0; sector < end; ++sector) {
        const bool listed = next < layout.size() && layout[next].index == sector;
        if (!write(sector, listed ? layout[next].bytes.data() : zero.data())) return false;
        if (listed) ++next;
    }
    return next == layout.size();
}
}  // namespace frondMsPro
