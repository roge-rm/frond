#ifndef FROND_SD_CANDIDATE_H
#define FROND_SD_CANDIDATE_H

#include "CardBacking.h"
#include "FileCardBacking.h"
#include "FrondSdImage.h"
#include "sdcard.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

// SD cards above 128 MiB run from a candidate sidecar in the machine folder,
// `.frondsd.tmp-live-XXXXXX`, a complete .frondsd file on FileCardBacking. sd.frondsd stays as it
// was until a save publishes a new one; then the card moves onto that and the candidate goes.
// The candidate is flock'd while it's live, so startup cleanup only removes abandoned ones.
namespace frondSdCandidate {
constexpr const char* prefix = ".frondsd.tmp-live-";
constexpr uint64_t ramMaxBytes = 128ull << 20;  // larger cards never live in RAM
constexpr size_t chunkBytes = 1u << 20;
constexpr uint64_t spareBytes = 16ull << 20;    // left free on the disk after a new card

// Called after each chunk with bytes done and the total; false cancels.
using Progress = std::function<bool(uint64_t, uint64_t)>;

struct Result {
    bool ok{false};
    std::string code, message;
};
inline Result fail(const char* code, std::string message) { return {false, code, std::move(message)}; }

struct Candidate {
    std::filesystem::path path;
    int fd{-1};  // open for writing and holding the lock
};

inline Candidate& live() {
    static Candidate current;
    return current;
}

inline bool candidateName(std::string_view name) { return name.rfind(prefix, 0) == 0; }

inline void discard(Candidate& candidate) {
    if (candidate.fd >= 0) ::close(candidate.fd);
    if (!candidate.path.empty()) {
        FileCardBacking::DiscardGenerations(candidate.path);
        std::error_code ec;
        std::filesystem::remove(candidate.path, ec);
    }
    candidate = {};
}

// Makes `candidate` the live one. An earlier candidate is no longer used by the card and goes.
inline void setLive(Candidate&& candidate) {
    auto& current = live();
    if (current.path != candidate.path) discard(current);
    current = std::move(candidate);
    candidate = {};
}

// Removes the live candidate once the card no longer runs on it.
inline void releaseUnused() {
    auto& current = live();
    if (current.path.empty()) return;
    std::error_code ec;
    const auto running = sdCardBackingPath();
    if (!running.empty() && std::filesystem::equivalent(running, current.path, ec) && !ec) return;
    discard(current);
}

inline bool liveIn(const std::filesystem::path& folder) {
    const auto& current = live();
    std::error_code ec;
    return !current.path.empty() && std::filesystem::equivalent(current.path.parent_path(), folder, ec) && !ec;
}

// Removes candidates nobody holds, left by a process that stopped before a save adopted them,
// and generation files whose candidate is gone.
inline void cleanup(const std::filesystem::path& folder) {
    std::error_code ec;
    std::vector<std::filesystem::path> doomed, orphans;
    const std::string genPrefix = std::string(".gen-") + prefix;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (candidateName(name)) doomed.push_back(it->path());
        else if (name.rfind(genPrefix, 0) == 0) orphans.push_back(it->path());
    }
    for (const auto& path : doomed) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            FileCardBacking::DiscardGenerations(path);
            std::filesystem::remove(path, ec);
        }
        ::close(fd);
    }
    // .gen-<candidate>-<rest>: the candidate name is the prefix plus mkstemp's six characters.
    const size_t nameLength = std::strlen(prefix) + 6;
    for (const auto& path : orphans) {
        const auto name = path.filename().string();
        if (name.size() <= 5 + nameLength) continue;
        if (!std::filesystem::exists(folder / name.substr(5, nameLength), ec))
            std::filesystem::remove(path, ec);
    }
}

// A random 128-bit card identity as 32 hex digits. Small cards keep the MD5 of their bytes.
inline std::string randomIdentity() {
    uint8_t bytes[16]{};
    bool filled = false;
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        filled = ::read(fd, bytes, sizeof(bytes)) == ssize_t(sizeof(bytes));
        ::close(fd);
    }
    if (!filled) {
        std::random_device device;
        for (auto& b : bytes) b = uint8_t(device());
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (auto b : bytes) {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}

inline bool hasSpace(const std::filesystem::path& folder, uint64_t bytes) {
    struct statvfs fs{};
    if (statvfs(folder.c_str(), &fs) != 0) return false;
    const uint64_t free = uint64_t(fs.f_bavail) * uint64_t(fs.f_frsize);
    return free >= bytes + spareBytes;
}

inline bool writeAll(int fd, const void* data, size_t size, uint64_t offset) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (size) {
        const auto n = ::pwrite(fd, p, size, off_t(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= size_t(n);
        offset += uint64_t(n);
    }
    return true;
}

inline bool readAll(int fd, void* data, size_t size, size_t& got) {
    auto* p = static_cast<uint8_t*>(data);
    got = 0;
    while (got < size) {
        const auto n = ::read(fd, p + got, size - got);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        if (n == 0) break;
        got += size_t(n);
    }
    return true;
}

// CRC32 after `crc` with `length` zero bytes appended, without touching them: the CRC register
// update for a zero byte is linear, so this applies its matrix power (as zlib's crc32_combine).
inline uint32_t gf2Times(const uint32_t* matrix, uint32_t vector) {
    uint32_t sum = 0;
    for (; vector; vector >>= 1, ++matrix)
        if (vector & 1) sum ^= *matrix;
    return sum;
}
inline void gf2Square(uint32_t* square, const uint32_t* matrix) {
    for (int n = 0; n < 32; n++) square[n] = gf2Times(matrix, matrix[n]);
}
inline uint32_t crcZeros(uint32_t crc, uint64_t length) {
    if (!length) return crc;
    uint32_t even[32], odd[32];
    odd[0] = 0xedb88320u;  // one zero bit
    for (int n = 1; n < 32; n++) odd[n] = 1u << (n - 1);
    gf2Square(even, odd);  // two bits
    gf2Square(odd, even);  // four bits
    uint32_t reg = ~crc;
    for (;;) {
        gf2Square(even, odd);  // first pass: one byte
        if (length & 1) reg = gf2Times(even, reg);
        length >>= 1;
        if (!length) break;
        gf2Square(odd, even);
        if (length & 1) reg = gf2Times(odd, reg);
        length >>= 1;
        if (!length) break;
    }
    return ~reg;
}

inline bool writeHeader(int fd, uint64_t payloadBytes, uint32_t crc) {
    uint8_t header[frondSdFile::headerBytes]{};
    std::memcpy(header, "FRONDSD", 8);
    frondSdFile::put32(header + 8, frondSdFile::versionV1);
    frondSdFile::put32(header + 12, frondSdFile::sectorBytes);
    frondSdFile::put64(header + 16, payloadBytes);
    frondSdFile::put32(header + 24, crc);
    return writeAll(fd, header, sizeof(header), 0);
}

// Test hook, run between creating a candidate and locking it.
inline std::function<void(const std::filesystem::path&)>& beforeLockHook() {
    static std::function<void(const std::filesystem::path&)> hook;
    return hook;
}

// Creates an empty, locked candidate file in `folder`. Another process's cleanup() can lock and
// remove a new file before it is locked here, so once locked the name must still be this file;
// otherwise it tries again under a new name.
inline bool openCandidate(const std::filesystem::path& folder, Candidate& out) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::string name = (folder / prefix).string() + "XXXXXX";
        const int fd = mkstemp(name.data());
        if (fd < 0) return false;
        if (beforeLockHook()) beforeLockHook()(name);
        struct stat mine {}, named {};
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fstat(fd, &mine) < 0) {
            ::close(fd);
            unlink(name.c_str());
            return false;
        }
        const bool locked = flock(fd, LOCK_EX | LOCK_NB) == 0;
        if (locked && stat(name.c_str(), &named) == 0 && named.st_dev == mine.st_dev && named.st_ino == mine.st_ino) {
            out.path = name;
            out.fd = fd;
            return true;
        }
        // Cleanup holds it or has removed it, and removes it if it's still there.
        ::close(fd);
    }
    return false;
}

struct LayoutSector {
    uint64_t index;
    const uint8_t* bytes;  // 512 bytes
};

// Builds a fresh card of `payloadBytes` in a candidate: zeros apart from `layout` (ascending,
// distinct sectors). Allocates with posix_fallocate, or writes zeros where that isn't supported.
inline Result create(const std::filesystem::path& folder, uint64_t payloadBytes,
                     const std::vector<LayoutSector>& layout, Candidate& out, const Progress& progress = {}) {
    if (!frondSdFile::validPayloadSize(payloadBytes)) return fail("size", "That card size isn't supported.");
    if (!hasSpace(folder, frondSdFile::headerBytes + payloadBytes))
        return fail("space", "There isn't enough free space for the card.");
    Candidate candidate;
    if (!openCandidate(folder, candidate)) return fail("write", "Could not create the card file.");
    auto abandon = [&](const char* code, const char* message) {
        discard(candidate);
        return fail(code, message);
    };
    const uint64_t total = frondSdFile::headerBytes + payloadBytes;
    const int allocated = posix_fallocate(candidate.fd, 0, off_t(total));
    if (allocated == ENOSPC) return abandon("space", "There isn't enough free space for the card.");
    uint32_t crc = 0;
    uint64_t at = 0;
    for (const auto& sector : layout) {
        if (sector.index * 512 < at || sector.index * 512 + 512 > payloadBytes)
            return abandon("size", "SD layout is out of range.");
        crc = crcZeros(crc, sector.index * 512 - at);
        crc = uint32_t(mz_crc32(crc, sector.bytes, 512));
        at = sector.index * 512 + 512;
    }
    crc = crcZeros(crc, payloadBytes - at);
    if (allocated == 0) {
        for (const auto& sector : layout)
            if (!writeAll(candidate.fd, sector.bytes, 512, frondSdFile::headerBytes + sector.index * 512))
                return abandon("write", "Could not write the card file.");
        if (progress && !progress(payloadBytes, payloadBytes)) return abandon("cancelled", "Cancelled.");
    } else {
        // No fallocate here (some Android file systems): write every byte.
        std::vector<uint8_t> chunk(chunkBytes);
        size_t next = 0;
        for (uint64_t off = 0; off < payloadBytes; off += chunkBytes) {
            const auto n = size_t(std::min<uint64_t>(chunkBytes, payloadBytes - off));
            std::fill(chunk.begin(), chunk.begin() + n, 0);
            while (next < layout.size() && layout[next].index * 512 < off + n) {
                std::memcpy(chunk.data() + (layout[next].index * 512 - off), layout[next].bytes, 512);
                ++next;
            }
            if (!writeAll(candidate.fd, chunk.data(), n, frondSdFile::headerBytes + off))
                return abandon(errno == ENOSPC ? "space" : "write", errno == ENOSPC ?
                               "There isn't enough free space for the card." : "Could not write the card file.");
            if (progress && !progress(off + n, payloadBytes)) return abandon("cancelled", "Cancelled.");
        }
    }
    if (!writeHeader(candidate.fd, payloadBytes, crc) || fsync(candidate.fd) < 0)
        return abandon("write", "Could not write the card file.");
    out = std::move(candidate);
    return {true, "created", {}};
}

// Streams a raw card image into a candidate in 1 MiB chunks. The caller has checked its size.
inline Result importImage(const std::filesystem::path& folder, const std::filesystem::path& source,
                     uint64_t payloadBytes, Candidate& out, const Progress& progress = {}) {
    if (!frondSdFile::validPayloadSize(payloadBytes)) return fail("size", "That card size isn't supported.");
    const int in = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return fail("read", "Could not read the card image.");
    struct stat st{};
    if (fstat(in, &st) < 0 || !S_ISREG(st.st_mode) || uint64_t(st.st_size) != payloadBytes) {
        ::close(in);
        return fail("read", "Could not read the card image.");
    }
    if (!hasSpace(folder, frondSdFile::headerBytes + payloadBytes)) {
        ::close(in);
        return fail("space", "There isn't enough free space for the card.");
    }
    Candidate candidate;
    if (!openCandidate(folder, candidate)) {
        ::close(in);
        return fail("write", "Could not create the card file.");
    }
    auto abandon = [&](const char* code, const char* message) {
        ::close(in);
        discard(candidate);
        return fail(code, message);
    };
    std::vector<uint8_t> chunk(chunkBytes);
    uint32_t crc = 0;
    for (uint64_t off = 0; off < payloadBytes; off += chunkBytes) {
        const auto want = size_t(std::min<uint64_t>(chunkBytes, payloadBytes - off));
        size_t got = 0;
        if (!readAll(in, chunk.data(), want, got) || got != want)
            return abandon("read", "Could not read the card image.");
        crc = uint32_t(mz_crc32(crc, chunk.data(), want));
        if (!writeAll(candidate.fd, chunk.data(), want, frondSdFile::headerBytes + off))
            return abandon(errno == ENOSPC ? "space" : "write", errno == ENOSPC ?
                           "There isn't enough free space for the card." : "Could not write the card file.");
        if (progress && !progress(off + want, payloadBytes)) return abandon("cancelled", "Cancelled.");
    }
    uint8_t extra = 0;
    size_t got = 0;
    if (!readAll(in, &extra, 1, got) || got) return abandon("read", "The card image changed while reading it.");
    if (!writeHeader(candidate.fd, payloadBytes, crc) || fsync(candidate.fd) < 0)
        return abandon("write", "Could not write the card file.");
    ::close(in);
    out = std::move(candidate);
    return {true, "imported", {}};
}

// Writes the raw sectors of a pinned snapshot to `dest` through a temp beside it, then renames.
inline Result exportRaw(const CardSnapshot& pin, const std::filesystem::path& dest, const Progress& progress = {}) {
    std::string temp = dest.string() + ".tmp-XXXXXX";
    const int fd = mkstemp(temp.data());
    if (fd < 0) return fail("write", "Could not create the output file.");
    auto abandon = [&](const char* code, const char* message) {
        ::close(fd);
        unlink(temp.c_str());
        return fail(code, message);
    };
    const uint64_t size = pin.Size();
    std::vector<uint8_t> chunk(chunkBytes);
    for (uint64_t off = 0; off < size; off += chunkBytes) {
        const auto n = size_t(std::min<uint64_t>(chunkBytes, size - off));
        if (!pin.Read(off, n, chunk.data())) return abandon("read", "Could not read the card.");
        if (!writeAll(fd, chunk.data(), n, off)) return abandon("write", "Could not write the output file.");
        if (progress && !progress(off + n, size)) return abandon("cancelled", "Cancelled.");
    }
    if (fsync(fd) < 0) return abandon("write", "Could not write the output file.");
    if (::close(fd) < 0) {
        unlink(temp.c_str());
        return fail("write", "Could not write the output file.");
    }
    if (rename(temp.c_str(), dest.c_str()) < 0) {
        unlink(temp.c_str());
        return fail("write", "Could not replace the output file.");
    }
    auto dir = dest.parent_path();
    if (dir.empty()) dir = ".";
    const int folder = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (folder >= 0) {
        fsync(folder);
        ::close(folder);
    }
    return {true, "exported", {}};
}

// Puts a finished candidate on as the live card. On failure the candidate goes and the old
// card stays.
inline bool attach(Candidate& candidate, uint64_t payloadBytes, const std::string& identity) {
    if (!sdCardInitializeFromFile(candidate.path.c_str(), payloadBytes, frondSdFile::headerBytes,
                                  identity.c_str())) {
        discard(candidate);
        return false;
    }
    setLive(std::move(candidate));
    return true;
}
}  // namespace frondSdCandidate

#endif
