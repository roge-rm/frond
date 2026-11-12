#ifndef FROND_STORAGE_LEGACY_H
#define FROND_STORAGE_LEGACY_H

#include "storage/Container.h"
#include "storage/Io.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <set>
#include <utility>
#include <vector>

// The two files Files… in the 0.14 apps leaves beside a card while a change is written, settled as
// desktop/CardFile.h and Android's CardFile.kt settle them:
// <card>.files-intent: "FRONDFI1", card bytes u64, generation u64, then a u32 chunk number per
// chunk that got data (little endian).
// <card>.files-commit: "FRONDFC1", record count u32, file length u64, records (file offset u64,
// length u32, bytes), "FCEND\0\0\0", CRC32 of everything before it (big endian).
// A complete commit is replayed (the change is done). An intent then gets its chunks' CRCs and the
// header rewritten at its generation (only free clusters changed). Repeating either is harmless.
namespace frondStorage {
namespace legacy {

constexpr size_t intentHeader = 24;
constexpr uint64_t commitMaxBytes = uint64_t(64) << 20;

inline std::filesystem::path intentOf(const std::filesystem::path& card) {
    return card.string() + ".files-intent";
}
inline std::filesystem::path commitOf(const std::filesystem::path& card) {
    return card.string() + ".files-commit";
}

inline bool present(const std::filesystem::path& card) {
    std::error_code ec;
    return std::filesystem::exists(intentOf(card), ec) || std::filesystem::exists(commitOf(card), ec);
}

inline uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

inline bool readWhole(const std::filesystem::path& path, uint64_t limit, std::vector<uint8_t>& out) {
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    Fd closer(fd);
    const auto size = sizeOf(fd);
    if (size < 0 || uint64_t(size) > limit) return false;
    out.resize(size_t(size));
    return out.empty() || readAllAt(fd, out.data(), out.size(), 0);
}

struct Record {
    uint64_t at{};
    std::vector<uint8_t> bytes;
};

inline bool decodeCommit(const std::vector<uint8_t>& b, uint64_t& finalLength, std::vector<Record>& records) {
    if (b.size() < 20 + 12 || std::memcmp(b.data(), "FRONDFC1", 8)) return false;
    const size_t end = b.size() - 12;
    if (std::memcmp(b.data() + end, "FCEND\0\0\0", 8)) return false;
    if (mz_crc32(0, b.data(), end + 8) != be32(b.data() + end + 8)) return false;
    const auto count = int32_t(be32(b.data() + 8));
    finalLength = be64(b.data() + 12);
    if (int64_t(finalLength) < 0) return false;
    size_t at = 20;
    records.clear();
    for (int32_t i = 0; i < count; ++i) {
        if (at + 12 > end) return false;
        const auto offset = int64_t(be64(b.data() + at));
        const auto length = int32_t(be32(b.data() + at + 8));
        if (offset < 0 || length < 0 || at + 12 + size_t(length) > end || uint64_t(offset) + uint64_t(length) > finalLength)
            return false;
        records.push_back({uint64_t(offset), std::vector<uint8_t>(b.begin() + at + 12, b.begin() + at + 12 + length)});
        at += 12 + size_t(length);
    }
    return at == end;
}

inline bool chunkCrc(int fd, uint64_t payload, uint64_t i, uint32_t& out) {
    const uint64_t start = i * frondSdFile::chunkBytes;
    const uint64_t n = std::min<uint64_t>(frondSdFile::chunkBytes, payload - start);
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    if (!readAllAt(fd, buf.data(), buf.size(), frondSdFile::headerBytes + start)) return false;
    out = mz_crc32(0, buf.data(), buf.size());
    return true;
}

// Rewrites the CRCs of the chunks named (all of them for a card that isn't v2 yet) and the header
// at `target`, or the card's own generation if that is later.
inline bool refresh(int fd, uint64_t payload, uint64_t target, const std::vector<int32_t>& listed) {
    uint8_t head[frondSdFile::headerBytes];
    if (!readAllAt(fd, head, sizeof(head), 0)) return false;
    const uint64_t count = frondSdFile::chunkCount(payload);
    const bool v2 = !std::memcmp(head, "FRONDSD\0", 8) && frondSdFile::get32(head + 8) == 2 &&
                    frondSdFile::get64(head + 16) == payload &&
                    sizeOf(fd) == int64_t(frondSdFile::headerBytes + payload + 4 * count);
    std::vector<uint32_t> table(size_t(count), 0);
    if (v2) {
        std::vector<uint8_t> raw(static_cast<size_t>(4 * count));
        if (!readAllAt(fd, raw.data(), raw.size(), frondSdFile::headerBytes + payload)) return false;
        for (size_t i = 0; i < table.size(); ++i) table[i] = frondSdFile::get32(raw.data() + 4 * i);
    }
    std::set<uint64_t> which;
    if (v2) {
        for (auto i : listed)
            if (i >= 0 && uint64_t(i) < count) which.insert(uint64_t(i));
    } else {
        for (uint64_t i = 0; i < count; ++i) which.insert(i);
    }
    for (auto i : which)
        if (!chunkCrc(fd, payload, i, table[size_t(i)])) return false;
    if (ftruncate(fd, off_t(frondSdFile::headerBytes + payload + 4 * count)) != 0) return false;
    std::vector<uint8_t> t;
    frondSdFile::tableBytes(table, t);
    if (!writeAllAt(fd, t.data(), t.size(), frondSdFile::headerBytes + payload) || fsync(fd) != 0) return false;
    uint8_t h[frondSdFile::headerBytes];
    frondSdFile::makeHeaderV2(h, payload, std::max(target, v2 ? frondSdFile::get64(head + 28) : uint64_t(0)), table);
    return writeAllAt(fd, h, sizeof(h), 0) && fsync(fd) == 0;
}

// Settles both files on the card open as `fd` (read and write, locked).
inline bool settle(const std::filesystem::path& card, int fd) {
    const auto commit = commitOf(card);
    removePath(commit.string() + ".tmp");
    std::error_code ec;
    if (std::filesystem::exists(commit, ec)) {
        std::vector<uint8_t> raw;
        uint64_t finalLength = 0;
        std::vector<Record> records;
        if (readWhole(commit, commitMaxBytes, raw) && !raw.empty() && decodeCommit(raw, finalLength, records)) {
            if (sizeOf(fd) < int64_t(finalLength) && ftruncate(fd, off_t(finalLength)) != 0) return false;
            for (const auto& r : records)
                if (!writeAllAt(fd, r.bytes.data(), r.bytes.size(), r.at)) return false;
            if (fsync(fd) != 0) return false;
        }
        removePath(commit);
        syncDir(folderOf(card));
    }
    const auto intent = intentOf(card);
    if (std::filesystem::exists(intent, ec)) {
        std::vector<uint8_t> b;
        readWhole(intent, uint64_t(64) << 20, b);
        // A short header means nothing was written yet: it is fsynced before any data.
        if (b.size() >= intentHeader && !std::memcmp(b.data(), "FRONDFI1", 8)) {
            const uint64_t payload = frondSdFile::get64(b.data() + 8);
            const uint64_t target = frondSdFile::get64(b.data() + 16);
            std::vector<int32_t> listed;
            for (size_t i = 0; i < (b.size() - intentHeader) / 4; ++i)
                listed.push_back(int32_t(frondSdFile::get32(b.data() + intentHeader + 4 * i)));
            if (payload >= frondSdFile::sectorBytes && payload <= frondSdFile::maxPayloadBytes &&
                payload % frondSdFile::sectorBytes == 0 &&
                sizeOf(fd) >= int64_t(frondSdFile::headerBytes + payload) && !refresh(fd, payload, target, listed))
                return false;
        }
        removePath(intent);
        syncDir(folderOf(card));
    }
    return true;
}

}  // namespace legacy
}  // namespace frondStorage

#endif
