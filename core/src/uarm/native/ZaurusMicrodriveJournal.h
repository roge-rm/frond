#ifndef FROND_ZAURUS_MICRODRIVE_JOURNAL_H
#define FROND_ZAURUS_MICRODRIVE_JOURNAL_H

#include "ZaurusMicrodrive.h"
#include "soc_PXA.h"

namespace frondNative {

namespace microdriveJournal {
constexpr uint64_t headerBytes = 56;
constexpr uint64_t maxPageBytes = 4096;
constexpr char magic[] = "FRONDMD1";

inline std::filesystem::path path(const std::filesystem::path& folder, const std::string& token) {
    return folder / (std::string(frondMachine::microdriveJournalPrefix) + token);
}

// Range and CRC pass before the first sector is overwritten. An interrupted apply can be repeated.
template <typename Page>
bool scan(int fd, uint64_t rawBytes, uint64_t total, bool verify, uint32_t crc, Page page) {
    uint32_t actual = 0;
    uint64_t previousEnd = 0;
    std::array<uint8_t, maxPageBytes> bytes{};
    for (uint64_t cursor = headerBytes; cursor < total;) {
        uint8_t descriptor[12];
        if (total - cursor < sizeof descriptor ||
            !frondStorage::readAllAt(fd, descriptor, sizeof descriptor, cursor)) return false;
        cursor += sizeof descriptor;
        const uint64_t offset = frondSdFile::get64(descriptor);
        const uint64_t length = frondSdFile::get32(descriptor + 8);
        if (!length || length > bytes.size() || length > total - cursor ||
            offset < previousEnd || offset % maxPageBytes || length % SD_SECTOR_SIZE ||
            offset > rawBytes || length > rawBytes - offset ||
            !frondStorage::readAllAt(fd, bytes.data(), size_t(length), cursor))
            return false;
        previousEnd = offset + length;
        cursor += length;
        if (verify) {
            actual = mz_crc32(actual, descriptor, sizeof descriptor);
            actual = mz_crc32(actual, bytes.data(), size_t(length));
        } else if (!page(offset, bytes.data(), size_t(length))) {
            return false;
        }
    }
    return !verify || actual == crc;
}

inline bool recordCommit(const std::filesystem::path& folder, const std::string& token) {
    std::filesystem::path temp;
    if (!frondStorage::createTemp(folder, ".frond.tmp-microdrive-commit-", temp)) return false;
    const frondStorage::Fd fd(::open(temp.c_str(), O_WRONLY | O_CLOEXEC));
    if (fd.fd < 0 || !frondStorage::writeAllAt(fd.fd, token.data(), token.size(), 0) || fsync(fd.fd) < 0 ||
        rename(temp.c_str(), (folder / frondMachine::microdriveCommit).c_str()) < 0) {
        frondStorage::removePath(temp);
        return false;
    }
    return frondStorage::syncDir(folder);
}

// A journal is reachable only from the published .frond. Other files are left by failed saves.
inline void removeUnreferenced(const std::filesystem::path& folder, const std::string& keep = {}) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        const std::string_view prefix(frondMachine::microdriveJournalPrefix);
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        const auto token = name.substr(prefix.size());
        if (token != keep && validMicrodriveToken(token)) frondStorage::removePath(it->path());
    }
}
}  // namespace microdriveJournal

inline bool settleMicrodrive(const std::filesystem::path& folder, SocPXA& soc, const std::string& token,
                             bool resetOverlay) {
    using namespace microdriveJournal;
    const auto stamp = folder / frondMachine::microdriveCommit;
    if (token.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(stamp, ec) || ec) return false;
        removeUnreferenced(folder);
        return true;
    }
    if (!validMicrodriveToken(token)) return false;
    std::string committed;
    if (readMicrodriveId(stamp, committed) && committed == token) {
        if (!frondStorage::syncDir(folder)) return false;
        removeUnreferenced(folder);
        return !resetOverlay || soc.ResetMicrodriveOverlay();
    }
    const auto journal = path(folder, token);
    const frondStorage::Fd fd(::open(journal.c_str(), O_RDONLY | O_CLOEXEC));
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(folder / frondMachine::microdriveSibling, ec);
    const auto size = fd.fd < 0 ? -1 : frondStorage::sizeOf(fd.fd);
    uint8_t header[headerBytes]{};
    if (ec || size < int64_t(headerBytes) ||
        !frondStorage::readAllAt(fd.fd, header, sizeof header, 0) ||
        std::memcmp(header, magic, 8) != 0 || frondSdFile::get64(header + 8) != bytes ||
        frondSdFile::get32(header + 52) != frondSdFile::crc32(header, 52) ||
        !std::equal(token.begin(), token.end(), header + 16))
        return false;
    const auto crc = frondSdFile::get32(header + 48);
    const auto apply = [&](uint64_t offset, const uint8_t* data, size_t length) {
        return soc.WriteMicrodriveBase(offset, data, length);
    };
    if (!scan(fd.fd, bytes, uint64_t(size), true, crc, apply) ||
        !scan(fd.fd, bytes, uint64_t(size), false, crc, apply) || !soc.FlushMicrodrive() ||
        !recordCommit(folder, token))
        return false;
    removeUnreferenced(folder);
    return !resetOverlay || soc.ResetMicrodriveOverlay();
}

// Writes this save's redo journal without changing the backing. An empty result means that
// the new state can refer to the last committed version of the raw image.
inline bool prepareMicrodriveJournal(const std::filesystem::path& folder, SocPXA& soc, std::string& token) {
    using namespace microdriveJournal;
    std::error_code ec;
    const auto rawBytes = std::filesystem::file_size(folder / frondMachine::microdriveSibling, ec);
    if (ec) return false;
    std::filesystem::path temp;
    if (!frondStorage::createTemp(folder, ".frond.tmp-microdrive-", temp)) return false;
    const frondStorage::Fd fd(::open(temp.c_str(), O_WRONLY | O_CLOEXEC));
    uint64_t cursor = headerBytes;
    uint32_t crc = 0;
    const bool written = fd.fd >= 0 && soc.ForEachMicrodriveChange([&](uint64_t offset, uint64_t length, const uint8_t* data) {
        if (!length || length > maxPageBytes || offset % maxPageBytes || length % SD_SECTOR_SIZE ||
            offset > rawBytes || length > rawBytes - offset)
            return false;
        uint8_t descriptor[12];
        frondSdFile::put64(descriptor, offset);
        frondSdFile::put32(descriptor + 8, uint32_t(length));
        if (!frondStorage::writeAllAt(fd.fd, descriptor, sizeof descriptor, cursor) ||
            !frondStorage::writeAllAt(fd.fd, data, size_t(length), cursor + sizeof descriptor)) return false;
        cursor += sizeof descriptor + length;
        crc = mz_crc32(crc, descriptor, sizeof descriptor);
        crc = mz_crc32(crc, data, size_t(length));
        return true;
    });
    if (!written || cursor == headerBytes) {
        frondStorage::removePath(temp);
        return written;
    }
    token = frondSdCandidate::randomIdentity();
    uint8_t header[headerBytes]{};
    std::memcpy(header, magic, 8);
    frondSdFile::put64(header + 8, rawBytes);
    std::memcpy(header + 16, token.data(), token.size());
    frondSdFile::put32(header + 48, crc);
    frondSdFile::put32(header + 52, frondSdFile::crc32(header, 52));
    if (!frondStorage::writeAllAt(fd.fd, header, sizeof header, 0) || fsync(fd.fd) < 0 ||
        rename(temp.c_str(), path(folder, token).c_str()) < 0 || !frondStorage::syncDir(folder)) {
        frondStorage::removePath(temp);
        frondStorage::removePath(path(folder, token));
        return false;
    }
    return true;
}

}  // namespace frondNative

#endif
