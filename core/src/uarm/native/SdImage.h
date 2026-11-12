#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace frondSd {
constexpr size_t minBytes = size_t(32) << 20, maxBytes = size_t(2048) << 20;
inline bool validSize(size_t size) { return size >= minBytes && size <= maxBytes && size % 512 == 0; }
inline void put16(uint8_t* p, uint16_t value) { p[0] = value; p[1] = value >> 8; }
inline void put32(uint8_t* p, uint32_t value) { put16(p, value); put16(p + 2, value >> 16); }
// Classic SD, DOS partition table and FAT16; no host filesystem mount is needed.
// 32 to 128 MiB keep the original layout. Larger cards use bigger clusters, and the data area
// ends at the last sector of the card, which lines it up with the cluster size.
// "2 GB" is 1886 MiB, the size of a real 2 GB card (CSD C_SIZE 3771, C_SIZE_MULT 7, READ_BL_LEN 10,
// 3,862,528 sectors). Palm OS formats inside the existing partition with 1 reserved sector and 32 KB
// clusters, which on a 2 GiB card gives 65,526 clusters, past FAT16's 65,524, so its format fails there.
constexpr unsigned cardMibs[] = {32, 64, 128, 256, 512, 1024, 1886};
inline bool validCardMib(unsigned mib) {
    for (unsigned size : cardMibs)
        if (mib == size) return true;
    return false;
}
// One sector of a fresh card that isn't all zeros.
struct LayoutSector { uint32_t index; std::array<uint8_t, 512> bytes{}; };
// The non-zero sectors of a fresh card, in ascending order: MBR, boot sector, the first sector
// of each FAT and of the root directory. Everything else is zero.
inline std::vector<LayoutSector> layout(unsigned mib) {
    if (!validCardMib(mib)) throw std::runtime_error("Choose a 32, 64, 128, 256, 512, 1024 or 1886 MiB card.");
    const uint32_t start = 63, total = uint32_t((uint64_t(mib) << 20) / 512), sectors = total - start;
    const uint8_t cluster = mib <= 64 ? 4 : mib <= 256 ? 8 : mib == 512 ? 16 : mib == 1024 ? 32 : 64;
    const bool aligned = mib > 128;
    uint16_t fatSectors = 1, reserved = 1;
    uint32_t clusters = 0;
    for (;;) {
        clusters = (sectors - reserved - 32 - 2 * fatSectors) / cluster;
        if (clusters > 65524) clusters = 65524;
        const auto required = uint16_t(((clusters + 2) * 2 + 511) / 512);
        if (fatSectors >= required) break;
        fatSectors = required;
    }
    if (aligned) reserved = uint16_t(total - clusters * cluster - start - 32 - 2 * fatSectors);
    if (clusters < 4085 || clusters > 65524 || fatSectors * 256u < clusters + 2 || reserved < 1)
        throw std::runtime_error("SD layout is out of range.");
    std::vector<LayoutSector> out(5);
    out[0].index = 0;
    auto* partition = out[0].bytes.data() + 446;
    partition[1] = 1; partition[2] = 1;
    partition[4] = 6; partition[5] = 254; partition[6] = 255; partition[7] = 255;
    put32(partition + 8, start); put32(partition + 12, sectors);
    out[0].bytes[510] = 0x55; out[0].bytes[511] = 0xaa;
    out[1].index = start;
    auto* boot = out[1].bytes.data();
    boot[0] = 0xeb; boot[1] = 0x3c; boot[2] = 0x90;
    const char oem[] = "FROND001", label[] = "FROND      ", type[] = "FAT16   ";
    for (int i = 0; i < 8; i++) boot[3 + i] = oem[i];
    put16(boot + 11, 512); boot[13] = cluster; put16(boot + 14, reserved);
    boot[16] = 2; put16(boot + 17, 512);
    if (sectors < 65536) put16(boot + 19, sectors); else put32(boot + 32, sectors);
    boot[21] = 0xf8; put16(boot + 22, fatSectors); put16(boot + 24, 63); put16(boot + 26, 255);
    put32(boot + 28, start); boot[36] = 0x80; boot[38] = 0x29; put32(boot + 39, 0x46524f4e);
    for (int i = 0; i < 11; i++) boot[43 + i] = label[i];
    for (int i = 0; i < 8; i++) boot[54 + i] = type[i];
    boot[510] = 0x55; boot[511] = 0xaa;
    for (unsigned fat = 0; fat < 2; fat++) {
        out[2 + fat].index = start + reserved + fat * fatSectors;
        put16(out[2 + fat].bytes.data(), 0xfff8); put16(out[2 + fat].bytes.data() + 2, 0xffff);
    }
    out[4].index = start + reserved + 2 * fatSectors;
    for (int i = 0; i < 11; i++) out[4].bytes[i] = uint8_t(label[i]);
    out[4].bytes[11] = 8;
    return out;
}
inline std::vector<uint8_t> create(unsigned mib) {
    const auto sectors = layout(mib);
    std::vector<uint8_t> image(size_t(mib) << 20);
    for (const auto& s : sectors) std::memcpy(image.data() + size_t(s.index) * 512, s.bytes.data(), 512);
    return image;
}
}
