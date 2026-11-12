#ifndef FROND_MEMORY_STICK_GEOMETRY_H
#define FROND_MEMORY_STICK_GEOMETRY_H

#include <cstddef>
#include <cstdint>

// Classic Memory Stick sizes, the rows of the TG50 Sony driver's boot block table.
// Pages are 512 data bytes plus 9 extra bytes. Each 512-block segment has 496 effective blocks.
namespace frondMsGeometry {
constexpr uint32_t dataBytes = 512, extraBytes = 9, recordBytes = dataBytes + extraBytes;

struct Geometry {
    uint32_t megabytes, blocks, pagesPerBlock;
    constexpr uint32_t BlockKib() const { return pagesPerBlock / 2; }
    constexpr uint32_t Effective() const { return blocks / 512 * 496; }
    constexpr uint32_t Pages() const { return blocks * pagesPerBlock; }
    constexpr uint64_t PhysicalBytes() const { return uint64_t(Pages()) * recordBytes; }
    constexpr uint64_t LogicalBytes() const { return uint64_t(megabytes) << 20; }
};

constexpr Geometry sizes[] = {{4, 512, 16},   {8, 1024, 16},  {16, 1024, 32},
                              {32, 2048, 32}, {64, 4096, 32}, {128, 8192, 32}};
constexpr Geometry largest = sizes[5];

inline const Geometry* byMegabytes(uint64_t megabytes) {
    for (const auto& g : sizes)
        if (g.megabytes == megabytes) return &g;
    return nullptr;
}
inline const Geometry* byShape(uint64_t blocks, uint64_t pagesPerBlock) {
    for (const auto& g : sizes)
        if (g.blocks == blocks && g.pagesPerBlock == pagesPerBlock) return &g;
    return nullptr;
}
inline const Geometry* byPhysicalBytes(uint64_t bytes) {
    for (const auto& g : sizes)
        if (g.PhysicalBytes() == bytes) return &g;
    return nullptr;
}
}  // namespace frondMsGeometry

// Memory Stick Pro sizes in MiB. The media are flat 512-byte sectors; "2 GB" is 1886 MiB, the
// size of a real 2 GB SD card, which keeps Palm's own FAT16 format within its cluster limit.
namespace frondMsPro {
constexpr unsigned cardMibs[] = {256, 512, 1024, 1886};
inline bool validCardMib(unsigned mib) {
    for (unsigned size : cardMibs)
        if (mib == size) return true;
    return false;
}
inline uint32_t sectorsOf(unsigned mib) { return uint32_t((uint64_t(mib) << 20) / 512); }
// The size in MiB of a stick with this many bytes, or 0.
inline unsigned mibOfBytes(uint64_t bytes) {
    for (unsigned mib : cardMibs)
        if (bytes == (uint64_t(mib) << 20)) return mib;
    return 0;
}
}  // namespace frondMsPro

#endif
