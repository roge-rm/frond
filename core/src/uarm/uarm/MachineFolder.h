#ifndef FROND_MACHINE_FOLDER_H
#define FROND_MACHINE_FOLDER_H

#include "FrondSdImage.h"
#include "MemoryStickGeometry.h"
#include "storage/MsLayout.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// Per-machine folder: the .frond sits beside sd.frondsd and/or ms.frondms or ms.frondmsp, and
// nand.frondnand for a device with internal NAND (session v9).
// List/import/export ignore temps and generation files. Packing copies those
// packable names at the archive or destination root.
namespace frondMachine {
constexpr const char* sdSibling = "sd.frondsd";
constexpr const char* msSibling = "ms.frondms";
// A Memory Stick Pro: flat sectors in the .frondsd format, saved like an SD card.
constexpr const char* msProSibling = "ms.frondmsp";
// Internal NAND: main bytes then spare bytes of every page (storage/Container.h).
constexpr const char* nandSibling = "nand.frondnand";
constexpr const char* androidSession = "session.frond";
// Which card the frontend's slot holds; packed with the machine, never read by the core.
constexpr const char* slotFile = "slot.json";
constexpr uint32_t kindSd = 1, kindMs = 2, kindMsPro = 3, kindNand = 4;
constexpr size_t nameBytes = 32, identityBytes = 32;
// v7 descriptors are 92 bytes; v8 adds the SD content generation as a u64 at 92, which for a classic
// Memory Stick is its change counter.
constexpr size_t descriptorBytesV7 = 92, descriptorBytes = 100;
constexpr const char* sdJournalPrefix = "sd.frondsd.journal-";
constexpr const char* msProJournalPrefix = "ms.frondmsp.journal-";
constexpr const char* msJournalPrefix = "ms.frondms.journal-";
constexpr const char* nandJournalPrefix = "nand.frondnand.journal-";
// Raw NAND pages are 528-byte units, 512 main and 16 spare, so 1/33 of a raw NAND is spare.
constexpr uint64_t nandUnitBytes = 528, nandUnitMainBytes = 512, nandUnitSpareBytes = 16;
constexpr uint64_t nandMaxBytes = frondSdFile::maxNandRawBytes;
inline bool validNandBytes(uint64_t raw) { return raw && raw % nandUnitBytes == 0 && raw <= nandMaxBytes; }
inline uint64_t nandMainBytes(uint64_t raw) { return raw / nandUnitBytes * nandUnitMainBytes; }
inline uint64_t nandSpareBytes(uint64_t raw) { return raw / nandUnitBytes * nandUnitSpareBytes; }
// Cards kept as .frondsd sidecars with journals and generations: SD cards and Memory Stick Pros.
inline bool fileCardKind(uint32_t kind) { return kind == kindSd || kind == kindMsPro; }
// Cards in the card store (sdcard.h), saved through the same journals: those, and a classic
// Memory Stick in a v2 .frondms.
inline bool storeCardKind(uint32_t kind) { return fileCardKind(kind) || kind == kindMs; }
// Every medium kept in a container with journals: the card store's, and internal NAND.
inline bool journalledKind(uint32_t kind) { return storeCardKind(kind) || kind == kindNand; }
inline const char* fileCardSibling(uint32_t kind) {
    return kind == kindNand ? nandSibling : kind == kindMsPro ? msProSibling : kind == kindMs ? msSibling : sdSibling;
}
inline const char* fileCardJournalPrefix(uint32_t kind) {
    return kind == kindNand    ? nandJournalPrefix
           : kind == kindMsPro ? msProJournalPrefix
           : kind == kindMs    ? msJournalPrefix
                               : sdJournalPrefix;
}
// The 128 MB stick, the only size before v7 sessions and the one v6 sessions embed.
constexpr uint64_t msLogicalBytes = frondMsGeometry::largest.LogicalBytes();
constexpr uint64_t msPhysicalBytes = frondMsGeometry::largest.PhysicalBytes();
static_assert(msPhysicalBytes == 136577024, "classic 128 MB physical size");

inline bool ignoredName(std::string_view name) {
    return name.find(".frond.tmp-") != std::string_view::npos ||
           name.find(".frondsd.tmp-") != std::string_view::npos ||
           name.find(".frondms.tmp-") != std::string_view::npos ||
           name.find(".frondsd.journal-") != std::string_view::npos ||
           name.find(".frondmsp.journal-") != std::string_view::npos ||
           name.find(".frondms.journal-") != std::string_view::npos ||
           name.find(".frondnand.tmp-") != std::string_view::npos ||
           name.find(".frondnand.journal-") != std::string_view::npos ||
           name.find(".gen-") != std::string_view::npos;
}

inline bool endsWith(std::string_view name, std::string_view suffix) {
    return name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix;
}

inline bool packableName(std::string_view name) {
    if (ignoredName(name)) return false;
    if (name == sdSibling || name == msSibling || name == msProSibling || name == nandSibling || name == slotFile)
        return true;
    // An exported session's NAND, packed (storage/Packed.h); a load unpacks it.
    if (name == std::string(nandSibling) + ".gz") return true;
    if (endsWith(name, ".frond")) return true;
    if (endsWith(name, ".frond.settings.json")) return true;
    return false;
}

struct Media {
    uint32_t kind{};
    std::string name;
    bool inserted{};
    bool writeProtected{};
    std::array<uint8_t, identityBytes> identity{};
    uint64_t logicalSize{};
    uint64_t physicalSize{};
    // The card file's v2 header generation, 0 for an SD or Pro v1 file. For a classic stick in a v1
    // .frondms, its change counter.
    uint64_t contentGeneration{};
};
// Internal NAND's descriptor: logicalSize is the main bytes, physicalSize the raw NAND with spare
// bytes, as --nand and save-nand have it. Identity is unused.

// The bytes a card keeps in the card store: the payload, or a classic stick's v2 stored bytes.
inline uint64_t storeBytes(const Media& media) {
    if (media.kind == kindNand) return frondSdFile::storedBytes(nandMainBytes(media.physicalSize), nandSpareBytes(media.physicalSize));
    if (media.kind != kindMs) return media.physicalSize;
    const auto* g = frondMsGeometry::byPhysicalBytes(media.physicalSize);
    const auto* l = g ? frondMsLayout::byShape(g->blocks, g->pagesPerBlock) : nullptr;
    return l ? l->StoredBytes() : 0;
}
// What the container header's payload is for the card: the logical region for a classic stick.
inline uint64_t containerPayload(const Media& media) {
    if (media.kind == kindNand) return nandMainBytes(media.physicalSize);
    if (media.kind != kindMs) return media.physicalSize;
    const auto* g = frondMsGeometry::byPhysicalBytes(media.physicalSize);
    const auto* l = g ? frondMsLayout::byShape(g->blocks, g->pagesPerBlock) : nullptr;
    return l ? l->PayloadBytes() : 0;
}

inline bool validMedia(const Media& media) {
    if (media.kind == kindSd)
        return media.name == sdSibling && media.logicalSize == media.physicalSize &&
               frondSdFile::validPayloadSize(media.physicalSize);
    if (media.kind == kindMsPro)
        return media.name == msProSibling && media.logicalSize == media.physicalSize &&
               frondMsPro::mibOfBytes(media.physicalSize);
    if (media.kind == kindMs) {
        const auto* g = frondMsGeometry::byPhysicalBytes(media.physicalSize);
        return g && media.name == msSibling && media.logicalSize == g->LogicalBytes();
    }
    if (media.kind == kindNand)
        return media.name == nandSibling && validNandBytes(media.physicalSize) &&
               media.logicalSize == nandMainBytes(media.physicalSize);
    return false;
}

inline bool encodeDescriptor(const Media& media, std::vector<uint8_t>& out, size_t size = descriptorBytes) {
    if (!validMedia(media) || media.name.size() >= nameBytes) return false;
    if (size != descriptorBytes && size != descriptorBytesV7) return false;
    if (size == descriptorBytesV7 && media.contentGeneration) return false;
    std::vector<uint8_t> bytes(size);
    auto put32 = [](uint8_t* p, uint32_t n) {
        for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(n >> (8 * i));
    };
    auto put64 = [&](uint8_t* p, uint64_t n) {
        put32(p, uint32_t(n));
        put32(p + 4, uint32_t(n >> 32));
    };
    put32(bytes.data(), media.kind);
    std::memcpy(bytes.data() + 4, media.name.c_str(), media.name.size());
    put32(bytes.data() + 36, media.inserted ? 1u : 0u);
    put32(bytes.data() + 40, media.writeProtected ? 1u : 0u);
    std::memcpy(bytes.data() + 44, media.identity.data(), identityBytes);
    put64(bytes.data() + 76, media.logicalSize);
    put64(bytes.data() + 84, media.physicalSize);
    if (size == descriptorBytes) put64(bytes.data() + 92, media.contentGeneration);
    out.swap(bytes);
    return true;
}

inline bool decodeDescriptor(const uint8_t* bytes, size_t size, Media& media) {
    if (!bytes || (size != descriptorBytes && size != descriptorBytesV7)) return false;
    auto get32 = [](const uint8_t* p) {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    };
    auto get64 = [&](const uint8_t* p) { return uint64_t(get32(p)) | (uint64_t(get32(p + 4)) << 32); };
    Media parsed;
    parsed.kind = get32(bytes);
    const auto* name = bytes + 4;
    size_t n = 0;
    while (n < nameBytes && name[n]) ++n;
    if (n == nameBytes) return false;
    for (size_t i = n; i < nameBytes; ++i)
        if (name[i]) return false;
    parsed.name.assign(reinterpret_cast<const char*>(name), n);
    const uint32_t inserted = get32(bytes + 36), protect = get32(bytes + 40);
    if (inserted > 1 || protect > 1) return false;
    parsed.inserted = inserted;
    parsed.writeProtected = protect;
    std::memcpy(parsed.identity.data(), bytes + 44, identityBytes);
    parsed.logicalSize = get64(bytes + 76);
    parsed.physicalSize = get64(bytes + 84);
    if (size == descriptorBytes) parsed.contentGeneration = get64(bytes + 92);
    if (!validMedia(parsed)) return false;
    media = std::move(parsed);
    return true;
}

inline bool sessionFile(const std::filesystem::path& folder, std::filesystem::path& out) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) return false;
    const auto named = folder / (folder.filename().string() + ".frond");
    const auto android = folder / androidSession;
    if (std::filesystem::is_regular_file(named, ec) && !ignoredName(named.filename().string())) {
        out = named;
        return true;
    }
    if (std::filesystem::is_regular_file(android, ec) && !ignoredName(android.filename().string())) {
        out = android;
        return true;
    }
    std::filesystem::path found;
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (ec || !entry.is_regular_file(ec)) continue;
        const auto name = entry.path().filename().string();
        if (ignoredName(name) || !endsWith(name, ".frond")) continue;
        found = entry.path();
        ++count;
    }
    if (count != 1) return false;
    out = found;
    return true;
}

inline std::vector<std::filesystem::path> packableFiles(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) return files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (ec || !entry.is_regular_file(ec)) continue;
        if (!packableName(entry.path().filename().string())) continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

enum class SiblingStatus { ok, missing, extra, mismatch, malformed };

inline bool readBytes(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto n = in.tellg();
    if (n < 0) return false;
    bytes.resize(static_cast<size_t>(n));
    in.seekg(0);
    return !n || bool(in.read(reinterpret_cast<char*>(bytes.data()), n));
}

// A v1 .frondms header. It must name the geometry of physicalSize. `changes` gets the change
// counter at 32.
inline bool msHeaderLooksRight(const std::filesystem::path& path, uint64_t physicalSize, uint64_t* changes = nullptr) {
    std::ifstream in(path, std::ios::binary);
    uint8_t header[frondSdFile::headerBytes];
    if (!in || !in.read(reinterpret_cast<char*>(header), sizeof(header))) return false;
    if (std::memcmp(header, "FRONDMS\0", 8)) return false;
    auto get32 = [](const uint8_t* p) {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    };
    const auto* g = frondMsGeometry::byShape(get32(header + 12), get32(header + 16));
    if (get32(header + 8) != 1 || !g || g->PhysicalBytes() != physicalSize || get32(header + 20) != 512 ||
        get32(header + 24) != 9)
        return false;
    for (unsigned i = 40; i < 64; ++i)
        if (header[i]) return false;
    if (changes) *changes = get32(header + 32) | (uint64_t(get32(header + 36)) << 32);
    return true;
}

// 1 or 2 for a .frondms that says it is that version, else 0.
inline uint32_t msFileVersion(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    uint8_t header[12];
    if (!in || !in.read(reinterpret_cast<char*>(header), sizeof(header)) || std::memcmp(header, "FRONDMS\0", 8))
        return 0;
    const uint32_t version = frondSdFile::get32(header + 8);
    return version == 1 || version == 2 ? version : 0;
}

// Reads the header of a card store sidecar (SD, Pro or a v2 classic stick), and the chunk table of a v2 one, checking the header CRC.
inline SiblingStatus readSdHeader(const std::filesystem::path& path, frondSdFile::Info& info,
                                  std::vector<uint32_t>* table = nullptr) {
    std::ifstream in(path, std::ios::binary);
    uint8_t header[frondSdFile::headerBytes];
    if (!in || !in.read(reinterpret_cast<char*>(header), sizeof(header))) return SiblingStatus::malformed;
    if (!frondSdFile::parseContainer(header, info) || (info.sideBytes && !info.nand && !frondMsLayout::of(info)) ||
        (info.nand && info.sideBytes * (nandUnitMainBytes / nandUnitSpareBytes) != info.payloadBytes))
        return SiblingStatus::malformed;
    in.seekg(0, std::ios::end);
    const auto n = in.tellg();
    if (n < 0 || uint64_t(n) != info.fileBytes()) return SiblingStatus::malformed;
    if (info.version == frondSdFile::versionV2) {
        std::vector<uint8_t> bytes(static_cast<size_t>(4 * info.chunks()));
        in.seekg(std::streamoff(frondSdFile::headerBytes + info.stored()));
        if (!in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())))
            return SiblingStatus::malformed;
        std::vector<uint32_t> parsed;
        if (!frondSdFile::readTable(header, bytes.data(), bytes.size(), info, parsed)) return SiblingStatus::malformed;
        if (table) table->swap(parsed);
    }
    return SiblingStatus::ok;
}

// Every payload byte against the v1 CRC or the v2 chunk table.
inline SiblingStatus checkSdPayload(const std::filesystem::path& path, const frondSdFile::Info& info,
                                    const std::vector<uint32_t>& table) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return SiblingStatus::malformed;
    in.seekg(frondSdFile::headerBytes);
    uint32_t got = 0;
    frondSdFile::ChunkCrcs chunks;
    std::vector<uint8_t> chunk(65536);
    uint64_t left = info.stored();
    while (left) {
        const auto want = std::min(left, uint64_t(chunk.size()));
        if (!in.read(reinterpret_cast<char*>(chunk.data()), std::streamsize(want))) return SiblingStatus::malformed;
        if (info.version == frondSdFile::versionV1) got = mz_crc32(got, chunk.data(), static_cast<size_t>(want));
        else chunks.add(chunk.data(), static_cast<size_t>(want));
        left -= want;
    }
    const bool ok = info.version == frondSdFile::versionV1 ? got == info.crc : chunks.table() == table;
    return ok ? SiblingStatus::ok : SiblingStatus::malformed;
}

// A v1 sidecar has its whole payload checked here. A v2 sidecar has its header and table checked,
// and its payload a chunk at a time as it is read, unless `full`. Its generation must match.
inline SiblingStatus checkSdSibling(const std::filesystem::path& path, const Media& card, bool full = false) {
    frondSdFile::Info info;
    std::vector<uint32_t> table;
    const auto header = readSdHeader(path, info, &table);
    if (header != SiblingStatus::ok) return header;
    if (info.classic != (card.kind == kindMs) || info.nand != (card.kind == kindNand) ||
        info.payloadBytes != containerPayload(card) ||
        info.stored() != storeBytes(card) ||
        (fileCardKind(card.kind) && card.logicalSize != card.physicalSize))
        return SiblingStatus::mismatch;
    if (info.generation != card.contentGeneration) return SiblingStatus::mismatch;
    if (info.version == frondSdFile::versionV2 && !full) return SiblingStatus::ok;
    return checkSdPayload(path, info, table);
}

inline SiblingStatus checkSiblings(const std::filesystem::path& folder, const std::vector<Media>& cards,
                                   bool full = false) {
    bool expectSd = false, expectMs = false, expectMsPro = false, expectNand = false;
    for (const auto& card : cards) {
        if (!validMedia(card)) return SiblingStatus::mismatch;
        if (card.kind == kindNand) {
            if (expectNand) return SiblingStatus::mismatch;
            expectNand = true;
            const auto path = folder / nandSibling;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec)) return SiblingStatus::missing;
            const auto status = checkSdSibling(path, card, full);
            if (status != SiblingStatus::ok) return status;
        } else if (fileCardKind(card.kind)) {
            bool& expect = card.kind == kindSd ? expectSd : expectMsPro;
            if (expect) return SiblingStatus::mismatch;
            expect = true;
            const auto path = folder / fileCardSibling(card.kind);
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec)) return SiblingStatus::missing;
            const auto status = checkSdSibling(path, card, full);
            if (status != SiblingStatus::ok) return status;
        } else {
            if (expectMs) return SiblingStatus::mismatch;
            expectMs = true;
            const auto path = folder / msSibling;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec)) return SiblingStatus::missing;
            if (msFileVersion(path) == 2) {
                const auto status = checkSdSibling(path, card, full);
                if (status != SiblingStatus::ok) return status;
                continue;
            }
            const auto size = std::filesystem::file_size(path, ec);
            if (ec) return SiblingStatus::malformed;
            if (size != frondSdFile::headerBytes + card.physicalSize) return SiblingStatus::mismatch;
            uint64_t changes = 0;
            if (!msHeaderLooksRight(path, card.physicalSize, &changes)) return SiblingStatus::malformed;
            if (changes != card.contentGeneration) return SiblingStatus::mismatch;
        }
    }
    std::error_code ec;
    if (!expectSd && std::filesystem::is_regular_file(folder / sdSibling, ec)) return SiblingStatus::extra;
    if (!expectMs && std::filesystem::is_regular_file(folder / msSibling, ec)) return SiblingStatus::extra;
    if (!expectMsPro && std::filesystem::is_regular_file(folder / msProSibling, ec)) return SiblingStatus::extra;
    if (!expectNand && std::filesystem::is_regular_file(folder / nandSibling, ec)) return SiblingStatus::extra;
    // One Memory Stick slot: a classic stick or a Pro.
    if (expectMs && expectMsPro) return SiblingStatus::mismatch;
    return SiblingStatus::ok;
}
}  // namespace frondMachine

#endif
