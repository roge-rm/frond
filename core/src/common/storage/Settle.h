#ifndef FROND_STORAGE_SETTLE_H
#define FROND_STORAGE_SETTLE_H

#include "storage/Container.h"
#include "storage/Io.h"
#include "storage/Journal.h"
#include "storage/Legacy.h"
#include "storage/Pending.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// One settle and one verify for every card file, used by the core's card loads, `frond --card`
// and the apps.
//
// settle() finishes what a cut-off write left beside the card, in this order:
// 1. the apps' legacy .files-commit and .files-intent (Legacy.h);
// 2. standalone journals, always replayed onto the card at their from or to generation;
// 3. pending lists with no journal left (Pending.h), and standalone journal temps;
// 4. with a session generation, the session save: a full copy or a session journal to that
//    generation, as a load of that .frond needs. Without one, any session file left refuses.
// Steps 1-3 lock the card as the apps do, so a card open in Files… isn't touched. Each step is
// safe to repeat after a crash in it, and a card with nothing beside it is left alone.
namespace frondStorage {

struct Result {
    bool ok{false};
    std::string code, message;
};
inline Result failed(const char* code, std::string message) { return {false, code, std::move(message)}; }

constexpr const char* unsettledMessage = "The card has an unfinished save. Open its machine once to finish it.";
constexpr const char* inFilesMessage = "The card is open in Files. Close it first.";
constexpr const char* mismatchMessage = "The card has an unfinished change that doesn't match it.";
constexpr const char* damagedMessage = "The card file is damaged.";

// The header and, for v2, the table of a card file, with the header CRC and file size checked.
inline bool readHeader(int fd, frondSdFile::Info& info, std::vector<uint32_t>* table = nullptr) {
    uint8_t header[frondSdFile::headerBytes];
    if (!readAllAt(fd, header, sizeof(header), 0) || !frondSdFile::parseContainer(header, info)) return false;
    if (sizeOf(fd) != int64_t(info.fileBytes())) return false;
    if (info.version != frondSdFile::versionV2) return true;
    std::vector<uint8_t> bytes(static_cast<size_t>(4 * info.chunks()));
    std::vector<uint32_t> parsed;
    if (!readAllAt(fd, bytes.data(), bytes.size(), frondSdFile::headerBytes + info.stored()) ||
        !frondSdFile::readTable(header, bytes.data(), bytes.size(), info, parsed))
        return false;
    if (table) table->swap(parsed);
    return true;
}

inline bool readHeader(const std::filesystem::path& card, frondSdFile::Info& info,
                       std::vector<uint32_t>* table = nullptr) {
    const int fd = ::open(card.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    Fd closer(fd);
    return readHeader(fd, info, table);
}

// The journal files beside a card that steps 2 and 3 settle.
struct Changes {
    std::vector<std::pair<std::filesystem::path, Journal>> standalone;
    std::vector<std::filesystem::path> pending;
};

inline Changes changesOf(const std::filesystem::path& card) {
    Changes out;
    for (const auto& path : journalFiles(card)) {
        uint64_t generation = 0;
        std::string suffix;
        if (!journalName(card, path, generation, suffix)) continue;
        Journal journal;
        if (suffix.empty() && readJournal(path, journal) && journal.standalone && journal.to == generation)
            out.standalone.emplace_back(path, std::move(journal));
        else if (suffix == ".pending")
            out.pending.push_back(path);
    }
    return out;
}

// Whether anything is beside the card for steps 1-3.
inline bool hasChanges(const std::filesystem::path& card) {
    if (legacy::present(card) || !editTemps(card).empty()) return true;
    const auto changes = changesOf(card);
    return !changes.standalone.empty() || !changes.pending.empty();
}

// Whether a standalone journal fits the card open as `fd`: the same stored size, and a generation
// it goes from or to (a v1 card only from 0).
inline bool journalFits(int fd, const Journal& journal) {
    uint8_t header[frondSdFile::headerBytes];
    frondSdFile::Info info;
    if (!readAllAt(fd, header, sizeof(header), 0) || !frondSdFile::parseContainer(header, info) ||
        info.stored() != journal.storedBytes)
        return false;
    const uint64_t v2Bytes = frondSdFile::headerBytes + journal.storedBytes + 4 * journal.table.size();
    const int64_t size = sizeOf(fd);
    if (info.version == frondSdFile::versionV1)
        return journal.from == 0 && (size == int64_t(frondSdFile::headerBytes + info.stored()) || size == int64_t(v2Bytes));
    return size == int64_t(v2Bytes) && (info.generation == journal.from || info.generation == journal.to);
}

// Replays one standalone journal onto the card open as `fd`. A v1 card at generation 0 first
// gets room for its table; the journal holds the whole new table.
inline Result replayStandalone(const std::filesystem::path& card, int fd, const std::filesystem::path& path,
                               const Journal& journal) {
    if (!journalFits(fd, journal)) return failed("mismatch", mismatchMessage);
    const uint64_t v2Bytes = frondSdFile::headerBytes + journal.storedBytes + 4 * journal.table.size();
    uint8_t header[frondSdFile::headerBytes];
    frondSdFile::Info info;
    if (!readAllAt(fd, header, sizeof(header), 0) || !frondSdFile::parseContainer(header, info))
        return failed("mismatch", mismatchMessage);
    if (info.version == frondSdFile::versionV1 && (ftruncate(fd, off_t(v2Bytes)) != 0 || fsync(fd) != 0))
        return failed("write", "Could not write the card file.");
    if (!applyJournal(card, path, nullptr, JournalStep::header)) return failed("write", "Could not write the card file.");
    removePath(pendingPath(card, journal.to));
    removePath(path);
    syncDir(folderOf(card));
    return {true, "", {}};
}

// Steps 1-3 on the card open as `fd` (read and write) under the caller's lock, as CardFile has it.
inline Result settleChangesLocked(const std::filesystem::path& card, int fd) {
    if (!hasChanges(card)) return {true, "", {}};
    if (!legacy::settle(card, fd)) return failed("write", "Could not write the card file.");
    auto changes = changesOf(card);
    for (const auto& [path, journal] : changes.standalone)
        if (auto replayed = replayStandalone(card, fd, path, journal); !replayed.ok) return replayed;
    for (const auto& path : changesOf(card).pending) {
        Pending pending;
        if (!readPending(path, pending)) return failed("read", "Could not read the card's journal.");
        if (pending.started) {
            const auto fit = pendingFits(fd, pending.storedBytes);
            if (fit == PendingFit::damaged) return failed("image", damagedMessage);
            if (fit == PendingFit::mismatch) return failed("mismatch", mismatchMessage);
            if (!refreshChunks(fd, pending.storedBytes, pending.generation, pending.chunks))
                return failed("write", "Could not write the card file.");
        }
        removePath(path);
        syncDir(folderOf(card));
    }
    // Under the card's lock no standalone journal is being written, so these are a crash's.
    for (const auto& temp : editTemps(card)) removePath(temp);
    return {true, "", {}};
}

// Steps 1-3: the apps' leftovers, standalone journals and pending lists. A card open in Files…
// is left alone.
inline Result settleChanges(const std::filesystem::path& card) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(card, ec) || !hasChanges(card)) return {true, "", {}};
    const int fd = ::open(card.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return failed("write", "Could not write the card file.");
    Fd closer(fd);
    if (!tryLock(fd)) return failed("busy", inFilesMessage);
    return settleChangesLocked(card, fd);
}

// For a card refused with "mismatch": drops each standalone journal that doesn't fit it, with its
// pending list, and each pending list of another size, under the card's lock, then settles what
// is left.
inline Result discardMismatched(const std::filesystem::path& card) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(card, ec)) return failed("read", "Could not read the card file.");
    const int fd = ::open(card.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return failed("write", "Could not write the card file.");
    Fd closer(fd);
    if (!tryLock(fd)) return failed("busy", inFilesMessage);
    if (!legacy::settle(card, fd)) return failed("write", "Could not write the card file.");
    bool dropped = false;
    for (const auto& [path, journal] : changesOf(card).standalone) {
        if (journalFits(fd, journal)) continue;
        removePath(pendingPath(card, journal.to));
        removePath(path);
        dropped = true;
    }
    for (const auto& path : changesOf(card).pending) {
        Pending pending;
        if (!readPending(path, pending) || !pending.started || pendingFits(fd, pending.storedBytes) != PendingFit::mismatch)
            continue;
        removePath(path);
        dropped = true;
    }
    if (dropped) syncDir(folderOf(card));
    return settleChangesLocked(card, fd);
}

// Step 4: brings the card to the generation a .frond describes. Same generation: stale journals
// go. One behind with a complete journal to this generation: the journal is replayed. A complete
// journal to this generation is also replayed over a card already there, which is harmless. A
// sound full copy at this generation, when the card isn't there yet, is renamed into place.
// Anything else refuses. `payloadBytes` is what the .frond says the card holds.
inline bool settleSession(const std::filesystem::path& card, uint64_t want, uint64_t payloadBytes) {
    frondSdFile::Info info;
    bool readable = false;
    {
        const int fd = ::open(card.c_str(), O_RDONLY | O_CLOEXEC);
        Fd closer(fd);
        uint8_t header[frondSdFile::headerBytes];
        readable = fd >= 0 && readAllAt(fd, header, sizeof(header), 0) && frondSdFile::parseContainer(header, info);
    }
    std::error_code ec;
    const auto full = fullPath(card, want);
    if (want > 0 && (!readable || info.generation != want) && std::filesystem::exists(full, ec)) {
        frondSdFile::Info copy;
        if (!readHeader(full, copy) || copy.version != frondSdFile::versionV2 || copy.generation != want ||
            copy.payloadBytes != payloadBytes || rename(full.c_str(), card.c_str()) < 0)
            return false;
        syncDir(folderOf(card));
        removeJournalFiles(card);
        return true;
    }
    if (!readable) return journalFiles(card).empty();  // the load's own check refuses it
    const auto journalFile = journalPath(card, want);
    Journal parsed;
    const bool replay = want > 0 && info.version == frondSdFile::versionV2 && std::filesystem::exists(journalFile, ec) &&
                        readJournal(journalFile, parsed) && parsed.to == want && parsed.storedBytes == info.stored() &&
                        (info.generation == want || info.generation == parsed.from);
    if (replay) {
        if (!applyJournal(card, journalFile)) return false;
        removeJournalFiles(card);
        return true;
    }
    if (info.generation != want) return false;
    removeJournalFiles(card);
    return true;
}

// Settles a card file for a load of a .frond that describes it at `want` with `payloadBytes`.
inline Result settle(const std::filesystem::path& card, uint64_t want, uint64_t payloadBytes) {
    if (auto changes = settleChanges(card); !changes.ok) return changes;
    if (!settleSession(card, want, payloadBytes)) return failed("unsettled", unsettledMessage);
    return {true, "", {}};
}

// Settles a card file outside any machine: what the apps or a standalone change left is finished,
// and a session save left beside it refuses.
inline Result settle(const std::filesystem::path& card) {
    if (auto changes = settleChanges(card); !changes.ok) return changes;
    if (!journalFiles(card).empty()) return failed("unsettled", unsettledMessage);
    return {true, "", {}};
}

using Progress = std::function<bool(uint64_t done, uint64_t total)>;

// Checks a card file against its header and CRCs: a v1 file's whole CRC, or a v2 file's header
// CRC and every chunk. Only reads. `progress` gets the stored bytes read; false from it cancels.
inline Result verify(int fd, frondSdFile::Info& info, const Progress& progress = {}) {
    uint8_t header[frondSdFile::headerBytes];
    if (!readAllAt(fd, header, sizeof(header), 0) || !frondSdFile::parseContainer(header, info))
        return failed("image", "That is not a Frond card file.");
    if (sizeOf(fd) != int64_t(info.fileBytes())) return failed("image", "The card file is damaged.");
    const uint64_t stored = info.stored();
    std::vector<uint8_t> chunk(frondSdFile::chunkBytes);
    frondSdFile::ChunkCrcs crcs;
    uint32_t whole = 0;
    for (uint64_t off = 0; off < stored; off += chunk.size()) {
        const auto n = size_t(std::min<uint64_t>(chunk.size(), stored - off));
        if (!readAllAt(fd, chunk.data(), n, frondSdFile::headerBytes + off))
            return failed("read", "Could not read the card file.");
        crcs.add(chunk.data(), n);
        if (info.version == frondSdFile::versionV1) whole = uint32_t(mz_crc32(whole, chunk.data(), n));
        if (progress && !progress(off + n, stored)) return failed("cancelled", "Cancelled.");
    }
    if (info.version == frondSdFile::versionV1)
        return whole == info.crc ? Result{true, "", {}} : failed("image", "The card file is damaged.");
    std::vector<uint8_t> raw(4 * crcs.table().size());
    if (!readAllAt(fd, raw.data(), raw.size(), frondSdFile::headerBytes + stored))
        return failed("read", "Could not read the card file.");
    std::vector<uint32_t> table(crcs.table().size());
    for (size_t i = 0; i < table.size(); ++i) table[i] = frondSdFile::get32(raw.data() + 4 * i);
    if (table != crcs.table() || frondSdFile::headerCrc(header, table) != info.crc)
        return failed("image", "The card file is damaged.");
    return {true, "", {}};
}

inline Result verify(const std::filesystem::path& card, frondSdFile::Info& info, const Progress& progress = {}) {
    const int fd = ::open(card.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return failed("read", "Could not read the card file.");
    Fd closer(fd);
    return verify(fd, info, progress);
}

}  // namespace frondStorage

#endif
