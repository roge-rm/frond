#ifndef FROND_STORAGE_PENDING_H
#define FROND_STORAGE_PENDING_H

#include "storage/Container.h"
#include "storage/Io.h"
#include "storage/Journal.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <set>
#include <vector>

// <card>.journal-N.pending, for a standalone change to generation N: "FRONDJP1", stored bytes u64,
// generation u64, then a u32 chunk number per chunk that gets data (little endian). Each number
// is appended and fsynced before data goes into the chunk, and data only goes where the card's
// file system has nothing. With no complete journal-N, settle rewrites those chunks' CRCs and the
// header at N (only free space changed); with one, the journal is replayed and this goes.
namespace frondStorage {

constexpr size_t pendingHeaderBytes = 24;

class PendingWriter {
   public:
    ~PendingWriter() { close(); }

    // Makes the file with its header, fsynced, and the folder fsynced.
    bool open(const std::filesystem::path& card, uint64_t storedBytes, uint64_t generation) {
        close();
        listed_.clear();
        path_ = pendingPath(card, generation);
        fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd_ < 0) return false;
        uint8_t head[pendingHeaderBytes]{};
        std::memcpy(head, "FRONDJP1", 8);
        frondSdFile::put64(head + 8, storedBytes);
        frondSdFile::put64(head + 16, generation);
        end_ = 0;
        return append(head, sizeof(head)) && syncDir(folderOf(card));
    }

    // Adds chunk numbers not listed yet; returns once they are on disk.
    bool add(const std::vector<uint32_t>& chunks) {
        std::vector<uint8_t> bytes;
        for (auto c : chunks) {
            if (!listed_.insert(c).second) continue;
            bytes.resize(bytes.size() + 4);
            frondSdFile::put32(bytes.data() + bytes.size() - 4, c);
        }
        return bytes.empty() || append(bytes.data(), bytes.size());
    }

    const std::filesystem::path& path() const { return path_; }

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

   private:
    bool append(const uint8_t* bytes, size_t n) {
        if (fd_ < 0 || !writeAllAt(fd_, bytes, n, end_) || fsync(fd_) != 0) return false;
        end_ += n;
        return true;
    }

    std::filesystem::path path_;
    int fd_{-1};
    uint64_t end_{};
    std::set<uint32_t> listed_;
};

struct Pending {
    bool started{};  // false: the header never got to disk, so no data was written
    uint64_t storedBytes{}, generation{};
    std::vector<uint32_t> chunks;
};

inline bool readPending(const std::filesystem::path& path, Pending& out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    Fd closer(fd);
    const auto size = sizeOf(fd);
    if (size < 0 || uint64_t(size) > (uint64_t(64) << 20)) return false;
    std::vector<uint8_t> b(static_cast<size_t>(size));
    if (!b.empty() && !readAllAt(fd, b.data(), b.size(), 0)) return false;
    Pending parsed;
    if (b.size() >= pendingHeaderBytes && !std::memcmp(b.data(), "FRONDJP1", 8)) {
        parsed.started = true;
        parsed.storedBytes = frondSdFile::get64(b.data() + 8);
        parsed.generation = frondSdFile::get64(b.data() + 16);
        for (size_t at = pendingHeaderBytes; at + 4 <= b.size(); at += 4)
            parsed.chunks.push_back(frondSdFile::get32(b.data() + at));
    }
    out = std::move(parsed);
    return true;
}

// CRC32 of chunk i of the stored bytes.
inline bool storedChunkCrc(int fd, uint64_t stored, uint64_t i, uint32_t& out) {
    const uint64_t start = i * frondSdFile::chunkBytes;
    const uint64_t n = std::min<uint64_t>(frondSdFile::chunkBytes, stored - start);
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    if (!readAllAt(fd, buf.data(), buf.size(), frondSdFile::headerBytes + start)) return false;
    out = mz_crc32(0, buf.data(), buf.size());
    return true;
}

// Whether a pending list fits the card open as `fd`: its header parses (damaged otherwise) and
// holds the list's stored size (mismatch otherwise, a stale list).
enum class PendingFit { fits, damaged, mismatch };
inline PendingFit pendingFits(int fd, uint64_t storedBytes) {
    uint8_t head[frondSdFile::headerBytes];
    frondSdFile::Info info;
    if (!readAllAt(fd, head, sizeof(head), 0) || !frondSdFile::parseContainer(head, info)) return PendingFit::damaged;
    if (info.stored() != storedBytes || sizeOf(fd) < int64_t(frondSdFile::headerBytes + storedBytes))
        return PendingFit::mismatch;
    return PendingFit::fits;
}

// Rewrites the listed chunks' CRCs (every chunk of a v1 card) and the header at `generation`, or
// the card's own if that is later. The card is open read and write as `fd`. False, with nothing
// written, unless the list fits the card (pendingFits): the header gives the side region and
// magic, so a guess could cut the card or rewrite a stick's or NAND's magic.
inline bool refreshChunks(int fd, uint64_t storedBytes, uint64_t generation, const std::vector<uint32_t>& listed) {
    if (pendingFits(fd, storedBytes) != PendingFit::fits) return false;
    uint8_t head[frondSdFile::headerBytes];
    if (!readAllAt(fd, head, sizeof(head), 0)) return false;
    frondSdFile::Info info;
    const int64_t size = sizeOf(fd);
    if (!frondSdFile::parseContainer(head, info)) return false;
    const bool v2 = info.version == frondSdFile::versionV2 && size == int64_t(info.fileBytes());
    const uint64_t count = frondSdFile::chunkCount(storedBytes);
    std::vector<uint32_t> table(size_t(count), 0);
    std::set<uint64_t> which;
    if (v2) {
        std::vector<uint8_t> raw(static_cast<size_t>(4 * count));
        if (!readAllAt(fd, raw.data(), raw.size(), frondSdFile::headerBytes + storedBytes)) return false;
        for (size_t i = 0; i < table.size(); ++i) table[i] = frondSdFile::get32(raw.data() + 4 * i);
        for (auto i : listed)
            if (i < count) which.insert(i);
    } else {
        for (uint64_t i = 0; i < count; ++i) which.insert(i);
    }
    for (auto i : which)
        if (!storedChunkCrc(fd, storedBytes, i, table[size_t(i)])) return false;
    if (ftruncate(fd, off_t(frondSdFile::headerBytes + storedBytes + 4 * count)) != 0) return false;
    std::vector<uint8_t> t;
    frondSdFile::tableBytes(table, t);
    if (!writeAllAt(fd, t.data(), t.size(), frondSdFile::headerBytes + storedBytes) || fsync(fd) != 0) return false;
    uint8_t h[frondSdFile::headerBytes];
    frondSdFile::makeHeaderV2(h, info.payloadBytes, std::max(generation, v2 ? info.generation : uint64_t(0)), table,
                              info.sideBytes, frondSdFile::magicOf(info));
    return writeAllAt(fd, h, sizeof(h), 0) && fsync(fd) == 0;
}

}  // namespace frondStorage

#endif
