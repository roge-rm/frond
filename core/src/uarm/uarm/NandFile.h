#ifndef FROND_NAND_FILE_H
#define FROND_NAND_FILE_H

#include "CardSave.h"
#include "FileCardBacking.h"
#include "MachineFolder.h"
#include "SessionIo.h"
#include "nand.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

// Internal NAND in a machine folder's nand.frondnand (session v9). The NAND runs on the file
// through FileCardBacking's copy on write; a save journals the changed pages, or writes the whole
// file when the NAND is in RAM (a v8 session, a new machine, --nand) or the file can't be journalled.
namespace frondNand {

constexpr const char* identity = "nand";

// Where the spare bytes start in the stored bytes of a raw NAND of `rawBytes`.
inline uint64_t sideOffset(uint64_t rawBytes) { return frondSdFile::sideOffset(frondMachine::nandMainBytes(rawBytes)); }

inline uint64_t storedBytes(uint64_t rawBytes) {
    return frondSdFile::storedBytes(frondMachine::nandMainBytes(rawBytes), frondMachine::nandSpareBytes(rawBytes));
}

// Set when the NAND couldn't move onto a saved file; the next save writes the whole file.
inline bool& stale() {
    static bool value = false;
    return value;
}

inline void fillMedia(frondMachine::Media& media, uint64_t rawBytes) {
    media = {};
    media.kind = frondMachine::kindNand;
    media.name = frondMachine::nandSibling;
    media.inserted = true;
    std::memcpy(media.identity.data(), identity, std::strlen(identity));
    media.logicalSize = frondMachine::nandMainBytes(rawBytes);
    media.physicalSize = rawBytes;
}

// Opens `path` (a checked nand.frondnand) as a backing with its chunk CRCs.
// `keepHeld`: opened inside HoldGenerations, see FileCardOptions.
inline std::unique_ptr<FileCardBacking> openFile(const std::filesystem::path& path, uint64_t rawBytes,
                                                 bool keepHeld = false) {
    frondSdFile::Info info;
    std::vector<uint32_t> table;
    if (frondMachine::readSdHeader(path, info, &table) != frondMachine::SiblingStatus::ok || !info.nand ||
        info.version != frondSdFile::versionV2 || info.stored() != storedBytes(rawBytes) ||
        info.payloadBytes != frondMachine::nandMainBytes(rawBytes))
        return nullptr;
    FileCardOptions options;
    // Saves adopt the file without Commit, like the uARM SD card, so leftover generations are resumed.
    options.recoverRootlessBase = true;
    options.keepHeld = keepHeld;
    options.identity = identity;
    options.chunkCrcs = std::move(table);
    return FileCardBacking::Open(path, info.stored(), frondSdFile::headerBytes, options);
}

// Runs the NAND on the folder's nand.frondnand, for a v9 load.
inline bool attachFile(NAND* nand, const std::filesystem::path& folder, const frondMachine::Media& media) {
    if (!nand || media.physicalSize != nandRawBytes(nand)) return false;
    auto file = openFile(folder / frondMachine::nandSibling, media.physicalSize);
    return file && nandAdoptBacking(nand, std::move(file), sideOffset(media.physicalSize));
}

inline FileCardBacking* fileBacking(NAND* nand) { return dynamic_cast<FileCardBacking*>(nandBacking(nand)); }

// Step 1 of a save: a journal against this folder's nand.frondnand, or a full copy of the NAND.
inline bool beginSave(const std::filesystem::path& folder, NAND* nand, frondMachine::Media& media, frondNative::SdSave& save) {
    save = {};
    save.kind = frondMachine::kindNand;
    const uint64_t raw = nandRawBytes(nand);
    fillMedia(media, raw);
    const auto sidecar = folder / frondMachine::nandSibling;
    auto* file = fileBacking(nand);
    if (file && !stale() && !file->Pinned() && file->DurableIs(sidecar) &&
        !frondNative::unsettledSd(folder, frondMachine::kindNand)) {
        auto pin = file->Snapshot();
        if (frondNative::journalStore(folder, media, save,
                                      [&](const frondNative::StoreChangeFn& fn) { return file->ForEachOverlayIn(*pin, fn); }))
            return true;
        save = {};
        save.kind = frondMachine::kindNand;
    }
    const uint64_t side = sideOffset(raw);
    std::shared_ptr<const CardSnapshot> pin = file ? file->Snapshot() : nullptr;
    return frondNative::publishStore(folder, media, save, storedBytes(raw), frondSession::nandShape(raw),
                                     [&](uint64_t off, uint64_t n, void* dest) {
                                         return pin ? pin->Read(off, n, dest)
                                                    : nandReadStored(nand, off, n, static_cast<uint8_t*>(dest), side);
                                     });
}

// Step 3, after the .frond is published: the journal applied in place or the full copy renamed onto
// nand.frondnand, then the NAND runs on that file, unless it runs on another folder's. If the file
// can't be finished, the journal or copy stays for the next load to finish. Pages the guest wrote
// after a background save's snapshot (nandTrackWrites) go from the running NAND onto the file's
// backing, so none are lost.
inline bool completeSave(const std::filesystem::path& folder, NAND* nand, frondNative::SdSave& save) {
    const auto sidecar = folder / frondMachine::nandSibling;
    auto* file = fileBacking(nand);
    const bool onThisFolder = file && file->DurableIs(sidecar);
    bool finished = frondSession::syncDir(folder);
    if (finished && !save.full.empty()) {
        finished = rename(save.full.c_str(), sidecar.c_str()) == 0;
        if (finished) {
            frondSession::syncDir(folder);
            save.full.clear();
        }
    } else if (finished && !save.journal.empty()) {
        finished = frondSession::applySdJournal(folder, save.journal, nullptr, frondSession::JournalStep::all,
                                                frondMachine::kindNand);
    }
    if (!finished) {
        if (file && !save.journal.empty())
            for (auto c : save.chunks) file->TrustChunk(c);
        std::fprintf(stderr, "frond: saved, but the NAND file is still to be finished\n");
        return true;
    }
    frondSession::removeSdJournals(folder, {}, frondMachine::kindNand);
    if (file && !onThisFolder) {
        FileCardBacking::DiscardGenerations(sidecar);
        return true;
    }
    // The file now holds the live bytes, so the overlay and generations beside it go.
    if (file && file->Pinned()) {
        stale() = true;
        return true;
    }
    NandPages tracked;
    if (!nandTakeTracked(nand, tracked)) {
        stale() = true;
        std::fprintf(stderr, "frond: the NAND couldn't move onto the saved file; the next save copies it\n");
        return true;
    }
    // Generations go once the NAND is on its new backing, and come back if it can't move.
    auto held = FileCardBacking::HoldGenerations(sidecar);
    auto fresh = openFile(sidecar, nandRawBytes(nand), true);
    if (!fresh || !nandAdoptBacking(nand, std::move(fresh), sideOffset(nandRawBytes(nand)), &tracked)) {
        fresh.reset();
        held.Return();
        stale() = true;
        std::fprintf(stderr, "frond: the NAND couldn't move onto the saved file; the next save copies it\n");
        return true;
    }
    held.Drop();
    stale() = false;
    return true;
}

}  // namespace frondNand

#endif
