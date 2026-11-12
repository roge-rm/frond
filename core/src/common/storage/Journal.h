#ifndef FROND_STORAGE_JOURNAL_H
#define FROND_STORAGE_JOURNAL_H

#include "storage/Container.h"
#include "storage/Io.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Card journals, <card>.journal-<to generation>, little endian:
// header 64 bytes: "FRONDJNL", version u32, flags u32, stored bytes u64, from generation u64,
// to generation u64, record count u64, table count u64, 8 zero bytes;
// records: stored-bytes offset u64, length u32, bytes; then the complete new chunk table;
// trailer 16 bytes: "FRONDEND", CRC32 of everything before the trailer u32, zero u32.
// Version 1 has flags 0 and is a session journal. Version 2 has flags bit 0 for standalone, 0
// for a session journal; both kinds are written as version 2.
// A session journal is replayed only when the .frond asks for its generation; a standalone one
// (a change made outside a running machine) is always replayed.
// A journal is written as a temp and renamed, so one under its own name is complete. A standalone
// journal's temp is <folder>/.frondsd.tmp-edit-<card>-XXXXXX, written only by a CardFile holding
// the card's lock, so one found while holding that lock is left from a crash.
namespace frondStorage {

constexpr size_t journalHeaderBytes = 64, journalTrailerBytes = 16, journalRecordBytes = 12;
constexpr uint32_t journalStandalone = 1;

enum class JournalKind { session, standalone };

inline std::string journalPrefix(const std::filesystem::path& card) { return card.filename().string() + ".journal-"; }

inline std::filesystem::path journalPath(const std::filesystem::path& card, uint64_t toGeneration) {
    return folderOf(card) / (journalPrefix(card) + std::to_string(toGeneration));
}

// A full copy at `generation`, renamed onto the card by a session load.
inline std::filesystem::path fullPath(const std::filesystem::path& card, uint64_t generation) {
    return folderOf(card) / (journalPrefix(card) + std::to_string(generation) + ".full");
}

// The prefix of a standalone journal's temp.
inline std::string editTempPrefix(const std::filesystem::path& card) {
    return ".frondsd.tmp-edit-" + card.filename().string() + "-";
}

// Temps a cut-off standalone change left beside the card.
inline std::vector<std::filesystem::path> editTemps(const std::filesystem::path& card) {
    std::vector<std::filesystem::path> out;
    const auto prefix = editTempPrefix(card);
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folderOf(card), ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename().string().rfind(prefix, 0) == 0) out.push_back(it->path());
    return out;
}

// The chunks a standalone change put data into before its journal.
inline std::filesystem::path pendingPath(const std::filesystem::path& card, uint64_t generation) {
    return folderOf(card) / (journalPrefix(card) + std::to_string(generation) + ".pending");
}

// Every file beside the card with its journal prefix: journals, full copies, pending lists.
inline std::vector<std::filesystem::path> journalFiles(const std::filesystem::path& card) {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    const std::string prefix = journalPrefix(card);
    for (std::filesystem::directory_iterator it(folderOf(card), ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.compare(0, prefix.size(), prefix) == 0) found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    return found;
}

// The generation and suffix of a journal file name ("", ".full", ".pending" or anything else).
// A generation past 64 bits isn't a journal name.
inline bool journalName(const std::filesystem::path& card, const std::filesystem::path& file, uint64_t& generation,
                        std::string& suffix) {
    const auto name = file.filename().string();
    const auto prefix = journalPrefix(card);
    if (name.compare(0, prefix.size(), prefix) != 0) return false;
    size_t at = prefix.size(), end = at;
    uint64_t value = 0;
    while (end < name.size() && name[end] >= '0' && name[end] <= '9') {
        const unsigned digit = unsigned(name[end] - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
        ++end;
    }
    if (end == at) return false;
    generation = value;
    suffix = name.substr(end);
    return true;
}

inline void removeJournalFiles(const std::filesystem::path& card, const std::filesystem::path& keep = {}) {
    bool removed = false;
    for (const auto& path : journalFiles(card)) {
        if (!keep.empty() && path == keep) continue;
        removePath(path);
        removed = true;
    }
    if (removed) syncDir(folderOf(card));
}

inline bool storedSizeOk(uint64_t stored) {
    return stored >= frondSdFile::sectorBytes &&
           stored <= frondSdFile::sideOffset(frondSdFile::maxPayloadBytes) + frondSdFile::maxSideBytes;
}

inline bool rangeOk(uint64_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= size - offset;
}

// Builds a journal in a temp beside the card: records in any order, then finish() with the new table.
class JournalWriter {
   public:
    ~JournalWriter() { abandon(); }

    bool open(const std::filesystem::path& card, uint64_t storedBytes, uint64_t from, uint64_t to,
              JournalKind kind = JournalKind::session) {
        abandon();
        const auto prefix = kind == JournalKind::standalone ? editTempPrefix(card) : std::string(".frondsd.tmp-");
        if (!createTemp(folderOf(card), prefix.c_str(), temp_)) return false;
        fd_ = ::open(temp_.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0) return false;
        card_ = card;
        kind_ = kind;
        stored_ = storedBytes;
        from_ = from;
        to_ = to;
        end_ = journalHeaderBytes;
        records_ = 0;
        return true;
    }

    bool add(uint64_t offset, uint32_t length, const uint8_t* data) {
        if (fd_ < 0 || !rangeOk(stored_, offset, length)) return false;
        uint8_t rec[journalRecordBytes];
        frondSdFile::put64(rec, offset);
        frondSdFile::put32(rec + 8, length);
        if (!writeAllAt(fd_, rec, sizeof(rec), end_) || !writeAllAt(fd_, data, length, end_ + sizeof(rec)))
            return false;
        end_ += sizeof(rec) + length;
        ++records_;
        return true;
    }

    // Writes table, header and trailer, fsyncs and renames to its journal name.
    bool finish(const std::vector<uint32_t>& table, std::filesystem::path& journal) {
        if (fd_ < 0 || table.size() != frondSdFile::chunkCount(stored_)) return false;
        std::vector<uint8_t> tableBytes;
        frondSdFile::tableBytes(table, tableBytes);
        if (!writeAllAt(fd_, tableBytes.data(), tableBytes.size(), end_)) return false;
        end_ += tableBytes.size();
        uint8_t header[journalHeaderBytes]{};
        std::memcpy(header, "FRONDJNL", 8);
        const bool standalone = kind_ == JournalKind::standalone;
        frondSdFile::put32(header + 8, 2);
        frondSdFile::put32(header + 12, standalone ? journalStandalone : 0);
        frondSdFile::put64(header + 16, stored_);
        frondSdFile::put64(header + 24, from_);
        frondSdFile::put64(header + 32, to_);
        frondSdFile::put64(header + 40, records_);
        frondSdFile::put64(header + 48, table.size());
        if (!writeAllAt(fd_, header, sizeof(header), 0)) return false;
        uint32_t crc = 0;
        std::vector<uint8_t> buf(1u << 20);
        for (uint64_t off = 0; off < end_;) {
            const size_t n = size_t(std::min<uint64_t>(buf.size(), end_ - off));
            if (!readAllAt(fd_, buf.data(), n, off)) return false;
            crc = mz_crc32(crc, buf.data(), n);
            off += n;
        }
        uint8_t trailer[journalTrailerBytes]{};
        std::memcpy(trailer, "FRONDEND", 8);
        frondSdFile::put32(trailer + 8, crc);
        if (!writeAllAt(fd_, trailer, sizeof(trailer), end_) || fsync(fd_) != 0) return false;
        const int fd = fd_;
        fd_ = -1;
        if (::close(fd) != 0) return false;
        const auto dest = journalPath(card_, to_);
        if (rename(temp_.c_str(), dest.c_str()) < 0) return false;
        temp_.clear();
        if (!syncDir(folderOf(card_))) {
            removePath(dest);
            return false;
        }
        journal = dest;
        return true;
    }

    void abandon() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        removePath(temp_);
        temp_.clear();
    }

   private:
    std::filesystem::path card_, temp_;
    int fd_{-1};
    JournalKind kind_{JournalKind::session};
    uint64_t stored_{}, from_{}, to_{}, end_{}, records_{};
};

struct Journal {
    uint32_t version{};
    bool standalone{};
    uint64_t storedBytes{}, from{}, to{}, records{};
    std::vector<uint32_t> table;
};

// Checks a whole journal: trailer CRC, record bounds, exact length. Only reads it.
inline bool readJournal(int fd, uint64_t size, Journal& out) {
    if (size < journalHeaderBytes + journalTrailerBytes) return false;
    uint8_t header[journalHeaderBytes];
    uint8_t trailer[journalTrailerBytes];
    if (!readAllAt(fd, header, sizeof(header), 0) || !readAllAt(fd, trailer, sizeof(trailer), size - sizeof(trailer)))
        return false;
    if (std::memcmp(header, "FRONDJNL", 8)) return false;
    Journal parsed;
    parsed.version = frondSdFile::get32(header + 8);
    const uint32_t flags = frondSdFile::get32(header + 12);
    if (parsed.version == 1 ? flags != 0 : parsed.version != 2 || (flags & ~journalStandalone)) return false;
    parsed.standalone = flags & journalStandalone;
    for (size_t i = 56; i < sizeof(header); ++i)
        if (header[i]) return false;
    if (std::memcmp(trailer, "FRONDEND", 8) || frondSdFile::get32(trailer + 12)) return false;
    parsed.storedBytes = frondSdFile::get64(header + 16);
    parsed.from = frondSdFile::get64(header + 24);
    parsed.to = frondSdFile::get64(header + 32);
    parsed.records = frondSdFile::get64(header + 40);
    const uint64_t tableCount = frondSdFile::get64(header + 48);
    const bool sizeOk = parsed.version == 1 ? frondSdFile::validPayloadSize(parsed.storedBytes)
                                            : storedSizeOk(parsed.storedBytes);
    if (!sizeOk || tableCount != frondSdFile::chunkCount(parsed.storedBytes) || parsed.to != parsed.from + 1)
        return false;
    const uint64_t body = size - journalTrailerBytes;
    uint32_t crc = 0;
    std::vector<uint8_t> buf(1u << 20);
    for (uint64_t off = 0; off < body;) {
        const size_t n = size_t(std::min<uint64_t>(buf.size(), body - off));
        if (!readAllAt(fd, buf.data(), n, off)) return false;
        crc = mz_crc32(crc, buf.data(), n);
        off += n;
    }
    if (crc != frondSdFile::get32(trailer + 8)) return false;
    uint64_t at = journalHeaderBytes;
    for (uint64_t i = 0; i < parsed.records; ++i) {
        uint8_t rec[journalRecordBytes];
        if (at + sizeof(rec) > body || !readAllAt(fd, rec, sizeof(rec), at)) return false;
        const uint64_t offset = frondSdFile::get64(rec);
        const uint32_t length = frondSdFile::get32(rec + 8);
        if (!rangeOk(parsed.storedBytes, offset, length)) return false;
        at += sizeof(rec) + length;
    }
    if (at + tableCount * 4 != body) return false;
    std::vector<uint8_t> tableBytes(static_cast<size_t>(tableCount * 4));
    if (!readAllAt(fd, tableBytes.data(), tableBytes.size(), at)) return false;
    parsed.table.resize(size_t(tableCount));
    for (size_t i = 0; i < parsed.table.size(); ++i) parsed.table[i] = frondSdFile::get32(tableBytes.data() + 4 * i);
    out = std::move(parsed);
    return true;
}

inline bool readJournal(const std::filesystem::path& path, Journal& out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    Fd closer(fd);
    const auto size = sizeOf(fd);
    return size >= 0 && readJournal(fd, uint64_t(size), out);
}

// Steps of applying a journal, for crash tests: pages and table, then header, then journal gone.
enum class JournalStep { pages = 1, header = 2, all = 3 };

// Test hook: when not negative, applying a journal fails after this many records, as ENOSPC or
// EIO would.
inline int64_t& journalFailAfter() {
    static int64_t records = -1;
    return records;
}

// Applies a complete journal to the card in place: pages and table, fsync, header at the new
// generation, fsync, then deletes the journal. Applying it twice gives the same bytes.
inline bool applyJournal(const std::filesystem::path& card, const std::filesystem::path& journal,
                         std::vector<uint32_t>* tableOut = nullptr, JournalStep stop = JournalStep::all) {
    const int jfd = ::open(journal.c_str(), O_RDONLY | O_CLOEXEC);
    if (jfd < 0) return false;
    Fd jcloser(jfd);
    Journal parsed;
    const int sfd = ::open(card.c_str(), O_RDWR | O_CLOEXEC);
    Fd scloser(sfd);
    const auto jsize = sizeOf(jfd);
    bool ok = sfd >= 0 && jsize >= 0 && readJournal(jfd, uint64_t(jsize), parsed) &&
              uint64_t(sizeOf(sfd)) == frondSdFile::headerBytes + parsed.storedBytes + 4 * parsed.table.size();
    // The payload, side region and magic come from the card's header. A header that doesn't parse
    // or holds a different size refuses: guessing could rewrite a stick's or NAND's magic.
    uint64_t payloadBytes = 0, sideBytes = 0;
    unsigned magic = frondSdFile::magicSd;
    if (ok) {
        uint8_t header[frondSdFile::headerBytes];
        frondSdFile::Info info;
        ok = readAllAt(sfd, header, sizeof(header), 0) && frondSdFile::parseContainer(header, info) &&
             info.stored() == parsed.storedBytes;
        payloadBytes = info.payloadBytes;
        sideBytes = info.sideBytes;
        magic = frondSdFile::magicOf(info);
    }
    uint64_t at = journalHeaderBytes;
    std::vector<uint8_t> data;
    for (uint64_t i = 0; ok && i < parsed.records; ++i) {
        if (journalFailAfter() >= 0 && i == uint64_t(journalFailAfter())) {
            ok = false;
            break;
        }
        uint8_t rec[journalRecordBytes];
        ok = readAllAt(jfd, rec, sizeof(rec), at);
        if (!ok) break;
        const uint64_t offset = frondSdFile::get64(rec);
        const uint32_t length = frondSdFile::get32(rec + 8);
        data.resize(length);
        ok = readAllAt(jfd, data.data(), length, at + sizeof(rec)) &&
             writeAllAt(sfd, data.data(), length, frondSdFile::headerBytes + offset);
        at += sizeof(rec) + length;
    }
    if (ok) {
        std::vector<uint8_t> tableBytes;
        frondSdFile::tableBytes(parsed.table, tableBytes);
        ok = writeAllAt(sfd, tableBytes.data(), tableBytes.size(), frondSdFile::headerBytes + parsed.storedBytes);
    }
    if (ok && stop != JournalStep::pages) {
        uint8_t header[frondSdFile::headerBytes];
        frondSdFile::makeHeaderV2(header, payloadBytes, parsed.to, parsed.table, sideBytes, magic);
        ok = fsync(sfd) == 0 && writeAllAt(sfd, header, sizeof(header), 0) && fsync(sfd) == 0;
    }
    if (!ok) return false;
    if (stop == JournalStep::all) {
        removePath(journal);
        syncDir(folderOf(card));
    }
    if (tableOut) *tableOut = std::move(parsed.table);
    return true;
}

// Writes the card as applyJournal would leave it to `out`, changing neither file: the header at
// the journal's generation, the stored bytes with each record laid over them in journal order, then
// the new table. For a settled copy (an export or a duplicate) of a card a save didn't finish.
// Everything is checked first, so `out` gets nothing for a damaged journal. `out` returns false to
// stop. False on any failure.
inline bool writeApplied(const std::filesystem::path& card, const std::filesystem::path& journal,
                         const std::function<bool(const uint8_t*, size_t)>& out) {
    const int jfd = ::open(journal.c_str(), O_RDONLY | O_CLOEXEC);
    if (jfd < 0) return false;
    Fd jcloser(jfd);
    const int sfd = ::open(card.c_str(), O_RDONLY | O_CLOEXEC);
    if (sfd < 0) return false;
    Fd scloser(sfd);
    Journal parsed;
    const auto jsize = sizeOf(jfd);
    if (jsize < 0 || !readJournal(jfd, uint64_t(jsize), parsed)) return false;
    uint8_t header[frondSdFile::headerBytes];
    frondSdFile::Info info;
    if (!readAllAt(sfd, header, sizeof(header), 0) || !frondSdFile::parseContainer(header, info) ||
        info.version != frondSdFile::versionV2 || info.stored() != parsed.storedBytes || info.generation != parsed.from ||
        uint64_t(sizeOf(sfd)) != info.fileBytes())
        return false;
    struct Record {
        uint64_t offset;
        uint32_t length;
        uint64_t data;
    };
    std::vector<Record> records;
    records.reserve(size_t(parsed.records));
    uint64_t at = journalHeaderBytes;
    for (uint64_t i = 0; i < parsed.records; ++i) {
        uint8_t rec[journalRecordBytes];
        if (!readAllAt(jfd, rec, sizeof(rec), at)) return false;
        records.push_back({frondSdFile::get64(rec), frondSdFile::get32(rec + 8), at + sizeof(rec)});
        at += sizeof(rec) + records.back().length;
    }
    frondSdFile::makeHeaderV2(header, info.payloadBytes, parsed.to, parsed.table, info.sideBytes, frondSdFile::magicOf(info));
    if (!out(header, sizeof(header))) return false;
    std::vector<uint8_t> block(frondSdFile::chunkBytes);
    for (uint64_t pos = 0; pos < parsed.storedBytes; pos += block.size()) {
        const uint64_t n = std::min<uint64_t>(block.size(), parsed.storedBytes - pos);
        if (!readAllAt(sfd, block.data(), size_t(n), frondSdFile::headerBytes + pos)) return false;
        for (const auto& r : records) {
            const uint64_t start = std::max(r.offset, pos), end = std::min(r.offset + r.length, pos + n);
            if (start >= end) continue;
            if (!readAllAt(jfd, block.data() + (start - pos), size_t(end - start), r.data + (start - r.offset))) return false;
        }
        if (!out(block.data(), size_t(n))) return false;
    }
    std::vector<uint8_t> tableBytes;
    frondSdFile::tableBytes(parsed.table, tableBytes);
    return out(tableBytes.data(), tableBytes.size());
}

}  // namespace frondStorage

#endif
