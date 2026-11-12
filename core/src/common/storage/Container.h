#ifndef FROND_STORAGE_CONTAINER_H
#define FROND_STORAGE_CONTAINER_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "zip/miniz.h"

// The card container, .frondsd, .frondmsp and v2 .frondms: 64-byte header then the stored bytes.
// Identity stays in the media descriptor, not this header.
//
// v1: 8-11 version, 12-15 sector size, 16-23 payload bytes, 24-27 CRC32 of the payload.
// v2: as v1 to byte 23, then 24-27 header CRC, 28-35 content generation, 36-43 CRC table offset
// (64 + stored bytes), 44-47 chunk bytes (1 MiB), 48-55 side region bytes. Bytes 56-63 are zero.
// The stored bytes are the payload (logical sectors) and, when the side region isn't empty, zeros
// to the next 1 MiB boundary and the side region. The table after them has one CRC32 per chunk of
// the stored bytes, the last one short. The header CRC covers the header with its own field
// zeroed, then the table. A side region of 0 is every v1 and v2 file there was before it.
// A classic Memory Stick (v2 .frondms) is the same v2 header with magic "FRONDMS\0" and a side
// region (storage/MsLayout.h). Its v1 file is another format (MemoryStickImage.h).
// Internal NAND (nand.frondnand) is a v2 header with magic "FRONDNA\0": the main bytes of every page
// in page order as the payload, and the spare bytes of every page in page order as the side region.
namespace frondSdFile {
constexpr size_t headerBytes = 64;
constexpr uint32_t versionV1 = 1, versionV2 = 2;
constexpr uint32_t sectorBytes = 512;
constexpr uint32_t chunkBytes = 1u << 20;
constexpr uint64_t maxPayloadBytes = 2048ull << 20;  // largest SD card Frond makes
constexpr uint64_t maxSideBytes = 1ull << 32;
constexpr uint64_t maxNandRawBytes = 256ull << 20;  // largest internal NAND, main and spare bytes

inline uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | (uint64_t(get32(p + 4)) << 32); }
inline void put32(uint8_t* p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(n >> (8 * i));
}
inline void put64(uint8_t* p, uint64_t n) {
    put32(p, uint32_t(n));
    put32(p + 4, uint32_t(n >> 32));
}
inline uint32_t crc32(const uint8_t* bytes, size_t size) { return mz_crc32(0, bytes, size); }

inline bool validPayloadSize(uint64_t size) {
    return size >= sectorBytes && size % sectorBytes == 0 && size <= maxPayloadBytes;
}

// Where the side region starts in the stored bytes: the chunk boundary after the payload.
constexpr uint64_t sideOffset(uint64_t payloadBytes) {
    return (payloadBytes + chunkBytes - 1) / chunkBytes * chunkBytes;
}

// Payload, padding and side region; just the payload when there is no side region.
inline uint64_t storedBytes(uint64_t payloadBytes, uint64_t sideBytes) {
    return sideBytes ? sideOffset(payloadBytes) + sideBytes : payloadBytes;
}

inline bool encode(const uint8_t* payload, uint64_t size, std::vector<uint8_t>& file) {
    if (!validPayloadSize(size)) return false;
    if (size > std::numeric_limits<size_t>::max()) return false;
    if (!payload) return false;
    const auto n = static_cast<size_t>(size);
    std::vector<uint8_t> out(headerBytes + n);
    std::memcpy(out.data(), "FRONDSD", 8);
    put32(out.data() + 8, versionV1);
    put32(out.data() + 12, sectorBytes);
    put64(out.data() + 16, size);
    put32(out.data() + 24, crc32(payload, n));
    std::memcpy(out.data() + headerBytes, payload, n);
    file.swap(out);
    return true;
}

inline bool encode(const std::vector<uint8_t>& payload, std::vector<uint8_t>& file) {
    return encode(payload.data(), payload.size(), file);
}

inline uint64_t chunkCount(uint64_t payloadBytes) { return (payloadBytes + chunkBytes - 1) / chunkBytes; }

struct Info {
    uint32_t version{};
    uint64_t payloadBytes{};
    uint64_t sideBytes{};
    uint32_t crc{};  // v1: payload CRC, v2: header CRC
    uint64_t generation{};
    bool classic{};  // "FRONDMS\0": a classic Memory Stick
    bool nand{};     // "FRONDNA\0": internal NAND
    uint64_t stored() const { return storedBytes(payloadBytes, sideBytes); }
    uint64_t chunks() const { return chunkCount(stored()); }
    uint64_t fileBytes() const { return headerBytes + stored() + (version == versionV2 ? 4 * chunks() : 0); }
};

// Any container header, with or without a side region.
inline bool parseContainer(const uint8_t* header, Info& info) {
    if (!header) return false;
    const bool classic = !std::memcmp(header, "FRONDMS\0", 8);
    const bool nand = !std::memcmp(header, "FRONDNA\0", 8);
    if ((!classic && !nand && std::memcmp(header, "FRONDSD\0", 8)) || get32(header + 12) != sectorBytes) return false;
    Info parsed;
    parsed.classic = classic;
    parsed.nand = nand;
    parsed.version = get32(header + 8);
    if ((classic || nand) && parsed.version != versionV2) return false;
    parsed.payloadBytes = get64(header + 16);
    parsed.crc = get32(header + 24);
    if (!validPayloadSize(parsed.payloadBytes)) return false;
    unsigned zeroFrom = 28;
    if (parsed.version == versionV2) {
        parsed.generation = get64(header + 28);
        parsed.sideBytes = get64(header + 48);
        if (parsed.sideBytes > maxSideBytes || get64(header + 36) != headerBytes + parsed.stored() ||
            get32(header + 44) != chunkBytes || ((classic || nand) && !parsed.sideBytes))
            return false;
        zeroFrom = 56;
    } else if (parsed.version != versionV1) {
        return false;
    }
    for (unsigned i = zeroFrom; i < headerBytes; ++i)
        if (header[i]) return false;
    info = parsed;
    return true;
}

// An SD or Pro card header: no side region, which neither has.
inline bool parseHeader(const uint8_t* header, Info& info) {
    Info parsed;
    if (!parseContainer(header, parsed) || parsed.sideBytes || parsed.classic || parsed.nand) return false;
    info = parsed;
    return true;
}

inline bool parseHeader(const uint8_t* header, uint64_t& payloadBytes, uint32_t& crc) {
    Info info;
    if (!parseHeader(header, info)) return false;
    payloadBytes = info.payloadBytes;
    crc = info.crc;
    return true;
}

inline void tableBytes(const std::vector<uint32_t>& table, std::vector<uint8_t>& out) {
    out.resize(table.size() * 4);
    for (size_t i = 0; i < table.size(); ++i) put32(out.data() + 4 * i, table[i]);
}

// The header CRC of a v2 header (any value in 24-27) and its table.
inline uint32_t headerCrc(const uint8_t* header, const std::vector<uint32_t>& table) {
    uint8_t copy[headerBytes];
    std::memcpy(copy, header, headerBytes);
    put32(copy + 24, 0);
    uint32_t crc = mz_crc32(0, copy, headerBytes);
    std::vector<uint8_t> bytes;
    tableBytes(table, bytes);
    return mz_crc32(crc, bytes.data(), bytes.size());
}

// Which magic a v2 header carries. A bool true is magicMs, for callers that only know classic sticks.
enum Magic : unsigned { magicSd = 0, magicMs = 1, magicNand = 2 };
inline unsigned magicOf(const Info& info) { return info.nand ? magicNand : info.classic ? magicMs : magicSd; }

inline void makeHeaderV2(uint8_t* header, uint64_t payloadBytes, uint64_t generation,
                         const std::vector<uint32_t>& table, uint64_t sideBytes = 0, unsigned magic = magicSd) {
    std::memset(header, 0, headerBytes);
    std::memcpy(header, magic == magicNand ? "FRONDNA" : magic == magicMs ? "FRONDMS" : "FRONDSD", 8);
    put32(header + 8, versionV2);
    put32(header + 12, sectorBytes);
    put64(header + 16, payloadBytes);
    put64(header + 28, generation);
    put64(header + 36, headerBytes + storedBytes(payloadBytes, sideBytes));
    put32(header + 44, chunkBytes);
    put64(header + 48, sideBytes);
    put32(header + 24, headerCrc(header, table));
}

// Chunk CRCs of a payload fed in order, in pieces of any size.
class ChunkCrcs {
   public:
    void add(const uint8_t* bytes, size_t n) {
        while (n) {
            if (filled_ == 0) table_.push_back(0);
            const size_t take = std::min<size_t>(n, chunkBytes - filled_);
            table_.back() = mz_crc32(table_.back(), bytes, take);
            filled_ = (filled_ + take) % chunkBytes;
            bytes += take;
            n -= take;
        }
    }
    const std::vector<uint32_t>& table() const { return table_; }

   private:
    std::vector<uint32_t> table_;
    size_t filled_{};
};

inline bool encodeV2(const uint8_t* payload, uint64_t size, uint64_t generation, std::vector<uint8_t>& file) {
    if (!validPayloadSize(size) || !payload || size > std::numeric_limits<size_t>::max()) return false;
    const auto n = static_cast<size_t>(size);
    ChunkCrcs crcs;
    crcs.add(payload, n);
    std::vector<uint8_t> out(headerBytes + n);
    makeHeaderV2(out.data(), size, generation, crcs.table());
    std::memcpy(out.data() + headerBytes, payload, n);
    std::vector<uint8_t> table;
    tableBytes(crcs.table(), table);
    out.insert(out.end(), table.begin(), table.end());
    file.swap(out);
    return true;
}

inline bool encodeV2(const std::vector<uint8_t>& payload, uint64_t generation, std::vector<uint8_t>& file) {
    return encodeV2(payload.data(), payload.size(), generation, file);
}

// Reads the table of a v2 file in memory or after its header and checks the header CRC.
inline bool readTable(const uint8_t* header, const uint8_t* tableData, size_t tableSize, const Info& info,
                      std::vector<uint32_t>& table) {
    const uint64_t count = info.chunks();
    if (info.version != versionV2 || tableSize != count * 4) return false;
    std::vector<uint32_t> parsed(static_cast<size_t>(count));
    for (size_t i = 0; i < parsed.size(); ++i) parsed[i] = get32(tableData + 4 * i);
    if (headerCrc(header, parsed) != info.crc) return false;
    table.swap(parsed);
    return true;
}

// Accepts v1 and v2 with no side region and checks every byte.
inline bool decode(const uint8_t* file, size_t size, std::vector<uint8_t>& payload, Info* infoOut = nullptr) {
    if (!file || size < headerBytes) return false;
    Info info;
    if (!parseHeader(file, info)) return false;
    if (info.payloadBytes > std::numeric_limits<size_t>::max()) return false;
    if (size != info.fileBytes()) return false;
    const auto n = static_cast<size_t>(info.payloadBytes);
    if (info.version == versionV1) {
        if (crc32(file + headerBytes, n) != info.crc) return false;
    } else {
        std::vector<uint32_t> table;
        if (!readTable(file, file + headerBytes + n, size - headerBytes - n, info, table)) return false;
        ChunkCrcs crcs;
        crcs.add(file + headerBytes, n);
        if (crcs.table() != table) return false;
    }
    std::vector<uint8_t> candidate(file + headerBytes, file + headerBytes + n);
    payload.swap(candidate);
    if (infoOut) *infoOut = info;
    return true;
}
}  // namespace frondSdFile

#endif
