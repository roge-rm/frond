#ifndef FROND_CARD_SAVE_H
#define FROND_CARD_SAVE_H

#include "FileCardBacking.h"
#include "SdCandidate.h"
#include "SessionIo.h"
#include "sdcard.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <functional>

// The SD card's part of a machine-folder save, apart from the SoC. On the TG50 the same card
// store holds a Memory Stick Pro, saved the same way as ms.frondmsp (kind 3), or a classic stick
// as a v2 ms.frondms (kind 2); `kind` picks which.
namespace frondNative {

using StoreIdentity = std::array<uint8_t, frondMachine::identityBytes>;

// `identity` replaces the card store's id, for a classic stick whose identity is raw bytes.
inline void fillSdMedia(frondMachine::Media& media, bool inserted, uint32_t kind = frondMachine::kindSd,
                        bool writeProtected = false, const StoreIdentity* identity = nullptr) {
    media.kind = kind;
    media.name = frondMachine::fileCardSibling(kind);
    media.inserted = inserted;
    media.writeProtected = writeProtected;
    const uint64_t bytes = uint64_t(sdCardSectorCount()) * SD_SECTOR_SIZE;
    media.logicalSize = bytes;
    media.physicalSize = bytes;
    if (kind == frondMachine::kindMs) {
        const auto* l = frondMsLayout::byStoredBytes(bytes);
        const auto* g = l ? frondMsGeometry::byShape(l->blocks, l->pagesPerBlock) : nullptr;
        media.logicalSize = g ? g->LogicalBytes() : 0;
        media.physicalSize = g ? g->PhysicalBytes() : 0;
    }
    media.identity.fill(0);
    if (identity) {
        media.identity = *identity;
        return;
    }
    const char* id = sdCardGetId();
    if (id && *id) std::memcpy(media.identity.data(), id, std::min(size_t(32), std::strlen(id)));
}

// The generation after any this folder's sidecar or journals have used.
inline uint64_t nextSdGeneration(const std::filesystem::path& folder, uint32_t kind = frondMachine::kindSd) {
    uint64_t newest = 0;
    frondSdFile::Info info;
    std::ifstream in(folder / frondMachine::fileCardSibling(kind), std::ios::binary);
    uint8_t header[frondSdFile::headerBytes];
    if (in && in.read(reinterpret_cast<char*>(header), sizeof(header))) {
        if (frondSdFile::parseContainer(header, info)) newest = info.generation;
        // A v1 .frondms: its change counter, which a converted stick's generations carry on from.
        else if (kind == frondMachine::kindMs && !std::memcmp(header, "FRONDMS\0", 8) && frondSdFile::get32(header + 8) == 1)
            newest = frondSdFile::get64(header + 32);
    }
    const std::string prefix = frondMachine::fileCardJournalPrefix(kind);
    for (const auto& journal : frondSession::sdJournals(folder, kind)) {
        const auto tail = journal.filename().string().substr(prefix.size());
        char* end = nullptr;
        const auto n = std::strtoull(tail.c_str(), &end, 10);
        if (end && (!*end || !std::strcmp(end, ".full"))) newest = std::max<uint64_t>(newest, n);
    }
    return newest + 1;
}

// One SD card's part of a save: a full copy in `full`, or the changed pages in `journal`, to
// `generation`. `chunks` are the chunks the journal touches.
struct SdSave {
    uint32_t kind{frondMachine::kindSd};
    std::filesystem::path full;
    std::filesystem::path journal;
    uint64_t generation{};
    uint64_t payloadBytes{};
    std::vector<uint64_t> chunks;
    // A journal step 1 left for writeJournal (deferJournals): the changed pages as of the snapshot,
    // copied, and the sidecar's chunk table they apply to.
    struct Page {
        uint64_t offset;
        std::vector<uint8_t> data;
    };
    std::vector<Page> pages;
    std::vector<uint32_t> table;
    uint64_t fromGeneration{}, storedBytes{};
    bool journalPending{};
};

// Set while a background save takes its snapshot: journalStore copies the changed pages and leaves
// the journal file to writeJournal on the save's worker. Emulation thread only.
inline bool& deferJournals() {
    static bool defer = false;
    return defer;
}

// Writes a whole medium of `stored` bytes, read through `read`, to its full copy at the next
// generation in `shape`, then fsyncs the folder. `media` is filled in already.
template <typename ReadFn>
inline bool publishStore(const std::filesystem::path& folder, frondMachine::Media& media, SdSave& save,
                         uint64_t stored, const frondSession::StoredShape& shape, ReadFn read) {
    media.contentGeneration = nextSdGeneration(folder, save.kind);
    if (!stored || shape.stored() != stored) return false;
    const auto full = frondSession::sdFullPath(folder, media.contentGeneration, save.kind);
    if (!frondSession::writeSdFromReader(full, stored, read, media.identity, media.contentGeneration, shape))
        return false;
    save.full = full;
    if (!frondSession::syncDir(folder)) return false;
    save.generation = media.contentGeneration;
    save.payloadBytes = stored;
    return true;
}

// Writes the whole live card from a pinned snapshot to sdFullPath at the next generation, then
// fsyncs the folder. The card's own root does not move: a save that fails, or one to another
// folder, leaves the live session as it was.
inline bool publishSd(const std::filesystem::path& folder, frondMachine::Media& media, bool inserted,
                      SdSave& save, bool writeProtected = false, const StoreIdentity* identity = nullptr) {
    fillSdMedia(media, inserted, save.kind, writeProtected, identity);
    auto pin = sdCardSnapshot();
    if (!pin) return false;
    const uint64_t stored = frondMachine::storeBytes(media);
    const auto shape = save.kind == frondMachine::kindMs ? frondSession::msShape(stored) : frondSession::sdShape(stored);
    if (pin->Size() != stored) return false;
    return publishStore(folder, media, save, stored, shape,
                        [&](uint64_t off, uint64_t n, void* dest) { return pin->Read(off, n, dest); });
}

using StoreChangeFn = std::function<bool(uint64_t offset, uint64_t length, const uint8_t* data)>;

// Writes `save.pages` (those that differ from the sidecar), and the new chunk table, to a journal at
// save.generation and fsyncs it. Reads only the sidecar and `save`, so it may run on another
// thread, as long as nothing writes the sidecar meanwhile.
inline bool writeJournal(const std::filesystem::path& folder, SdSave& save) {
    const auto sidecar = folder / frondMachine::fileCardSibling(save.kind);
    auto table = std::move(save.table);
    // A journal left to the same generation is replaced by the rename, never removed first.
    frondSession::SdJournalWriter writer;
    if (!writer.open(folder, save.storedBytes, save.fromGeneration, save.generation, save.kind)) return false;
    const int fd = ::open(sidecar.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    // CRC32 is affine, so for equal lengths crc(new) = crc(old) ^ crc(zeros) ^ crc(diff), with the
    // diff zero outside the changed page. Only the old page is read, never the whole chunk.
    std::vector<uint8_t> old;
    bool pages = true;
    for (const auto& page : save.pages) {
        const uint64_t off = page.offset, n = page.data.size();
        const uint8_t* data = page.data.data();
        const uint64_t c = off / frondSdFile::chunkBytes, start = c * frondSdFile::chunkBytes;
        const uint64_t len = std::min<uint64_t>(frondSdFile::chunkBytes, save.storedBytes - start);
        if (off + n > start + len || c >= table.size()) { pages = false; break; }
        old.resize(size_t(n));
        if (!frondSession::readAllAt(fd, old.data(), old.size(), frondSdFile::headerBytes + off)) { pages = false; break; }
        for (size_t i = 0; i < old.size(); ++i) old[i] ^= data[i];
        const uint64_t at = off - start;
        const uint32_t diff = frondSdCandidate::crcZeros(
            uint32_t(mz_crc32(frondSdCandidate::crcZeros(0, at), old.data(), old.size())), len - at - n);
        table[size_t(c)] ^= frondSdCandidate::crcZeros(0, len) ^ diff;
        if (save.chunks.empty() || save.chunks.back() != c) save.chunks.push_back(c);
        if (!writer.add(off, uint32_t(n), data)) { pages = false; break; }
    }
    ::close(fd);
    save.pages.clear();
    save.pages.shrink_to_fit();
    save.journalPending = false;
    if (!pages) {
        writer.abandon();
        return false;
    }
    return writer.finish(table, save.journal);
}

// Step 1 as a journal: the pages `changes` gives (those that differ from the sidecar) copied, the
// new generation chosen, then writeJournal, here or, under deferJournals, on the save's worker.
// Only for a medium running on this folder's v2 sidecar with no other snapshot held.
inline bool journalStore(const std::filesystem::path& folder, frondMachine::Media& media, SdSave& save,
                         const std::function<bool(const StoreChangeFn&)>& changes) {
    const auto sidecar = folder / frondMachine::fileCardSibling(save.kind);
    frondSdFile::Info info;
    std::vector<uint32_t> table;
    if (frondMachine::readSdHeader(sidecar, info, &table) != frondMachine::SiblingStatus::ok ||
        info.version != frondSdFile::versionV2 || info.classic != (save.kind == frondMachine::kindMs) ||
        info.nand != (save.kind == frondMachine::kindNand) || info.stored() != frondMachine::storeBytes(media))
        return false;
    const uint64_t to = info.generation + 1;
    // A full copy left at this generation by a save that failed before publishing.
    frondSession::removePath(frondSession::sdFullPath(folder, to, save.kind));
    save.pages.clear();
    const bool copied = changes([&](uint64_t off, uint64_t n, const uint8_t* data) {
        save.pages.push_back({off, std::vector<uint8_t>(data, data + n)});
        return true;
    });
    if (!copied) {
        save.pages.clear();
        return false;
    }
    save.table = std::move(table);
    save.fromGeneration = info.generation;
    save.storedBytes = info.stored();
    save.generation = to;
    save.payloadBytes = info.stored();
    media.contentGeneration = to;
    save.journalPending = true;
    if (deferJournals()) return true;
    return writeJournal(folder, save);
}

// The card store's journal: its changes from a snapshot taken now.
inline bool journalSd(const std::filesystem::path& folder, frondMachine::Media& media, SdSave& save) {
    auto pin = sdCardSnapshot();
    if (!pin) return false;
    return journalStore(folder, media, save, [&](const StoreChangeFn& fn) { return sdCardForEachChange(*pin, fn); });
}

// True when a journal or full copy newer than sd.frondsd is there and the folder's .frond is past
// sd.frondsd, or can't be read: a published save whose card file isn't finished. Read from the
// folder itself, so a save left by any earlier save, in this process or another, is found.
inline bool unsettledSd(const std::filesystem::path& folder, uint32_t kind = frondMachine::kindSd) {
    uint64_t current = 0;
    frondSdFile::Info info;
    if (frondMachine::readSdHeader(folder / frondMachine::fileCardSibling(kind), info) == frondMachine::SiblingStatus::ok)
        current = info.generation;
    if (nextSdGeneration(folder, kind) <= current + 1) return false;
    std::filesystem::path session;
    std::vector<uint8_t> bytes;
    frondSession::Contents parsed;
    if (!frondMachine::sessionFile(folder, session) || !frondMachine::readBytes(session, bytes) ||
        !frondSession::parseFolderSession(bytes.data(), bytes.size(), parsed))
        return true;
    return std::any_of(parsed.media.begin(), parsed.media.end(), [&](const frondMachine::Media& media) {
        return media.kind == kind && media.contentGeneration > current;
    });
}

// Step 1 of a save. A card running on this folder's v2 sidecar is journalled; anything else (a
// RAM card, a candidate, a v1 sidecar, another folder, or a card with a snapshot held) is copied.
// A folder with an unsettled save is copied from the live card too, at a generation past every
// journal, and the old journal or copy goes once that copy is in place. Saves never settle one:
// replaying it under a live card would change bytes the card's overlay was compared against.
inline bool beginSdSave(const std::filesystem::path& folder, frondMachine::Media& media, bool inserted,
                        SdSave& save, uint32_t kind = frondMachine::kindSd, bool writeProtected = false,
                        const StoreIdentity* identity = nullptr) {
    save = {};
    save.kind = kind;
    const auto sidecar = folder / frondMachine::fileCardSibling(kind);
    if (!unsettledSd(folder, kind) && sdCardBackedBy(sidecar.c_str()) && !sdCardPinned()) {
        fillSdMedia(media, inserted, kind, writeProtected, identity);
        frondSdFile::Info info;
        if (frondMachine::readSdHeader(sidecar, info) == frondMachine::SiblingStatus::ok &&
            info.version == frondSdFile::versionV2) {
            return journalSd(folder, media, save);
        }
    }
    return publishSd(folder, media, inserted, save, writeProtected, identity);
}

// A save that failed before its .frond was published. A full copy goes; a journal stays, and the
// next load discards it.
inline void abandonSdSave(SdSave& save) {
    frondSession::removePath(save.full);
    const auto kind = save.kind;
    save = {};
    save.kind = kind;
}

// Step 3, after the .frond is published: the folder is fsynced, then the journal is applied in
// place or the full copy renamed onto sd.frondsd, and other journals go. If this fails the journal
// or copy stays for the next load or save to finish.
inline bool finishSdSave(const std::filesystem::path& folder, SdSave& save) {
    if (!frondSession::syncDir(folder)) return false;
    if (!save.full.empty()) {
        if (rename(save.full.c_str(), (folder / frondMachine::fileCardSibling(save.kind)).c_str()) < 0) return false;
        frondSession::syncDir(folder);
        save.full.clear();
    } else if (!save.journal.empty()) {
        std::vector<uint32_t> table;
        if (!frondSession::applySdJournal(folder, save.journal, &table, frondSession::JournalStep::all, save.kind))
            return false;
        sdCardSetChunkCrcs(std::move(table));
    }
    frondSession::removeSdJournals(folder, {}, save.kind);
    return true;
}

// Tests set this to make the card's move onto a saved sidecar fail.
inline bool& sdAdoptFailForTest() {
    static bool fail = false;
    return fail;
}

// After a published save. A live card file-backed on this folder's sidecar, or on a candidate in
// this folder, moves onto the new file; otherwise generations left beside the sidecar belong to a
// card it replaced. An adopted candidate goes.
inline void afterSdPublish(const std::filesystem::path& folder, uint32_t kind = frondMachine::kindSd) {
    const auto sidecar = folder / frondMachine::fileCardSibling(kind);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(sidecar, ec)) return;
    const uint64_t bytes = uint64_t(sdCardSectorCount()) * SD_SECTOR_SIZE;
    // Opened from sd.frondsd: a journal applied in place, or a full copy renamed over it. If the
    // card can't move onto the file now there, the next save copies it whole.
    if (sdCardRunsOn(sidecar.c_str())) {
        if (!sdAdoptFailForTest() && sdCardAdoptSaved(sidecar.c_str(), bytes, frondSdFile::headerBytes)) return;
        sdCardMarkStale();
        std::fprintf(stderr, "frond: the card couldn't move onto the saved file; the next save copies it\n");
        return;
    }
    FileCardBacking::DiscardGenerations(sidecar);
    // A classic stick read from a v1 file, or made in this session, runs in RAM until its first
    // save, then on the v2 file that save wrote, so later saves are journals.
    if (kind == frondMachine::kindMs && !sdCardFileBacked()) {
        if (!sdCardAdoptSavedFromRam(sidecar.c_str(), bytes, frondSdFile::headerBytes))
            std::fprintf(stderr, "frond: the Memory Stick couldn't move onto the saved file; the next save copies it\n");
        return;
    }
    if (frondSdCandidate::liveIn(folder) &&
        sdCardBackedBy(frondSdCandidate::live().path.c_str()) &&
        sdCardAdoptSaved(sidecar.c_str(), bytes, frondSdFile::headerBytes))
        frondSdCandidate::releaseUnused();
}

// Everything after saveFolderFromTemps published the .frond. When the card file can't be
// finished, the live card keeps its overlay and the save stays unsettled for the next load to
// finish or the next save to copy past: the .frond is published and loads, so this still returns
// true. Chunks a failed
// apply touched are no longer checked, since they are part old, part new on disk and the overlay
// holds the new pages.
inline bool completeSdSave(const std::filesystem::path& folder, SdSave& save) {
    if (finishSdSave(folder, save)) {
        afterSdPublish(folder, save.kind);
        return true;
    }
    if (!save.journal.empty()) sdCardTrustChunks(save.chunks);
    std::fprintf(stderr, "frond: saved, but the card file is still to be finished\n");
    return true;
}

// Steps 2 and 3 of a save with `sd` from beginSdSave. Once the .frond is renamed the save is
// published: the SD side always finishes through completeSdSave, and a later failure (a Memory
// Stick rename, a sibling that won't go) is only reported.
inline bool publishWithSd(const std::filesystem::path& folder, const frondSession::Contents& contents,
                          const std::vector<std::filesystem::path>& temps, SdSave& sd) {
    bool published = false;
    const bool saved = frondSession::saveFolderFromTemps(folder, contents, temps, &published);
    if (!published) {
        abandonSdSave(sd);
        return false;
    }
    return completeSdSave(folder, sd) && saved;
}

}  // namespace frondNative

#endif
