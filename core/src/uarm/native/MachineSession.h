#ifndef FROND_MACHINE_SESSION_H
#define FROND_MACHINE_SESSION_H

#include "CardBacking.h"
#include "MemoryStickImage.h"
#include "CardSave.h"
#include "NandFile.h"
#include "SdCards.h"
#include "SessionIo.h"
#include "SoC.h"
#include "device_profile5.h"
#include "mmiodev_MemoryStickController.h"
#include "sdcard.h"
#include "soc_PXA.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

// Native save/load of a machine folder. A lone v6 .frond extracts into <stem>/.
namespace frondNative {

// What the slots keep from load to save outside the card store: SD write protection (the card
// doesn't model it, the descriptor carries it), and the classic stick's identity and the generation
// (a v1 file's change counter) it was opened at. `takenOut` are card
// files a take-out left in the folder for the frontend to move; a save doesn't remove them.
struct SlotCards {
    bool sdProtected{};
    std::array<uint8_t, frondMachine::identityBytes> msIdentity{};
    uint64_t msChanges{};
    std::vector<std::string> takenOut;
};
inline SlotCards& slotCards() {
    static SlotCards cards;
    return cards;
}

// A classic stick opened or inserted at generation `changes`.
inline void msOpened(const std::array<uint8_t, frondMachine::identityBytes>& identity, uint64_t changes) {
    auto& cards = slotCards();
    cards.msIdentity = identity;
    cards.msChanges = changes;
}

// A lone v6 file outside a machine folder, which loading extracts into a folder of its own.
inline bool extractsOnLoad(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || frondSession::frondInFolder(file)) return false;
    uint8_t header[SessionFile5::kHeaderBytes]{};
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(header), sizeof header)) return false;
    uint32_t version = 0;
    std::string device;
    return frondSession::peekHeader(header, sizeof header, version, device) && version == SessionFile5::kVersionV6;
}

// Where a save to `path` goes. `loadedFrom` and `loadedFolder` are the session this core loaded and
// the machine folder it runs in (the --work-folder when there is one). A save back to that session, or
// to its own session file, goes to that folder. A .frond saves into its parent only when that is this
// machine's folder, the parent's own session file, or a new one named after a folder with no machine
// in it. Any other file, a lone v6 .frond too, saves to loneExtractFolder(path), the folder loading it
// would make, but never into another machine's folder there. Empty, with `why` set, when there's
// nowhere safe.
inline std::filesystem::path machineFolderForSave(const std::filesystem::path& path, std::string& why,
                                                  const std::filesystem::path& loadedFrom = {},
                                                  const std::filesystem::path& loadedFolder = {}) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const bool hasFolder = !loadedFolder.empty() && fs::is_directory(loadedFolder, ec);
    const auto ours = [&](const fs::path& folder) { return hasFolder && fs::equivalent(folder, loadedFolder, ec); };
    if (hasFolder && !loadedFrom.empty()) {
        fs::path own;
        if (fs::equivalent(path, loadedFrom, ec) ||
            (fs::is_directory(loadedFrom, ec) && frondMachine::sessionFile(loadedFrom, own) && fs::equivalent(path, own, ec)))
            return loadedFolder;
    }
    const bool frond = frondMachine::endsWith(path.filename().string(), ".frond");
    if (fs::is_directory(path, ec) || (!fs::exists(path, ec) && !frond)) {
        fs::create_directories(path, ec);
        return path;
    }
    if (frond && !extractsOnLoad(path)) {
        const auto parent = path.parent_path();
        const auto name = path.filename().string();
        fs::path session;
        if (ours(parent)) return parent;
        if (frondMachine::sessionFile(parent, session)) {
            if (fs::equivalent(path, session, ec)) return parent;
        } else if (!frondSession::isMachineFolder(parent) &&
                   (name == frondMachine::androidSession || name == parent.filename().string() + ".frond")) {
            return parent;
        }
    }
    const auto folder = frondSession::loneExtractFolder(path);
    if (frondSession::isMachineFolder(folder) && !ours(folder)) {
        why = folder.string() + " holds another machine";
        return {};
    }
    fs::create_directories(folder, ec);
    return folder;
}

inline void fillState(SoC& soc, frondSession::Contents& contents) {
    const auto* profile = deviceProfile5(soc.GetDeviceType());
    contents.deviceName = profile ? profile->name : "";
    contents.displayMode = int32_t(soc.GetDisplayMode());
    contents.ramSize = soc.GetRamSize();
    auto copy = [](const Buffer& b) {
        auto* p = static_cast<const uint8_t*>(b.data);
        return b.size && p ? std::vector<uint8_t>(p, p + b.size) : std::vector<uint8_t>{};
    };
    contents.nor = copy(soc.GetRomData());
    // Internal NAND saves to nand.frondnand (frondNand), never into the .frond.
    contents.memory = copy(soc.GetMemoryData());
    contents.savestate = copy(soc.GetSavestate());
}

// A classic stick in its own RAM (a v6 session's, or one made or inserted from an image while
// running) moves into the empty card store first, as card `identity`, so it saves like the others.
inline bool classicOnStore(MemoryStickController* ms) {
    if (msCtrlrClassicOnStore(ms)) return true;
    char id[SD_CARD_ID_MAX_LEN + 1]{};
    std::memcpy(id, slotCards().msIdentity.data(), SD_CARD_ID_MAX_LEN);
    return msCtrlrMoveClassicToStore(ms, id);
}

// Attaches the folder's ms.frondms through the card store: a v2 file read and written in place,
// a v1 file read into RAM, so its first save writes v2.
inline bool attachClassicFile(MemoryStickController* ms, const std::filesystem::path& path,
                              const StoreIdentity& identity, bool inserted, bool protect) {
    if (!ms) return false;
    char id[SD_CARD_ID_MAX_LEN + 1]{};
    std::memcpy(id, identity.data(), SD_CARD_ID_MAX_LEN);
    if (frondMachine::msFileVersion(path) == 2) {
        frondSdFile::Info info;
        if (frondMachine::readSdHeader(path, info) != frondMachine::SiblingStatus::ok || !info.classic ||
            !sdCardInitializeFromFile(path.c_str(), info.stored(), frondSdFile::headerBytes, id))
            return false;
    } else {
        std::vector<uint8_t> stored;
        {
            std::vector<uint8_t> physical;
            if (!frondMs::decodeFile(path, physical) || !frondMs::storedOf(physical, stored)) return false;
        }
        sdCardInitializeWithBytes(std::move(stored), id);
    }
    if (!msCtrlrAttachClassicStore(ms, inserted, protect)) {
        sdCardReset();
        return false;
    }
    return true;
}

// Step 1 of a classic stick's save: a journal against its v2 ms.frondms, or a full copy to one.
inline bool beginMsSave(const std::filesystem::path& folder, MemoryStickController* ms, frondMachine::Media& media,
                        SdSave& save) {
    if (!classicOnStore(ms)) return false;
    auto identity = slotCards().msIdentity;
    if (!beginSdSave(folder, media, msCtrlrInserted(ms), save, frondMachine::kindMs, msCtrlrReadOnly(ms), &identity))
        return false;
    slotCards().msIdentity = media.identity;
    return true;
}

inline bool saveMachine(const std::filesystem::path& path, SoC& soc, const std::filesystem::path& loadedFrom = {},
                        const std::filesystem::path& loadedFolder = {}) {
    if (!soc.Save()) {
        std::cerr << "save: the device state snapshot failed" << std::endl;
        return false;
    }
    std::string why;
    const auto folder = machineFolderForSave(path, why, loadedFrom, loadedFolder);
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) {
        std::cerr << "save: " << (why.empty() ? folder.string() + " is not a folder and can't be made one" : why)
                  << std::endl;
        return false;
    }
    frondSession::Contents contents;
    fillState(soc, contents);
    std::vector<std::filesystem::path> temps;
    SdSave sd, nand;
    auto fail = [&](const char* why) {
        std::cerr << "save: " << why << " in " << folder << std::endl;
        for (const auto& t : temps) frondSession::removePath(t);
        abandonSdSave(sd);
        abandonSdSave(nand);
        return false;
    };
    // Internal NAND first, in the same three steps as a card.
    if (auto* chip = soc.GetNand(); chip && nandRawBytes(chip)) {
        frondMachine::Media media;
        if (!frondNand::beginSave(folder, chip, media, nand)) return fail("the NAND could not be written");
        temps.emplace_back();  // published by frondNand::completeSave
        contents.media.push_back(std::move(media));
    }
    // SD order: (1) journal or full copy, fsynced; (2) the .frond at the new generation, folder
    // fsynced; (3) the journal applied in place or the copy renamed, journals deleted.
    // A Memory Stick Pro lives in the SD card store and saves as ms.frondmsp the same way.
    auto* pxa = dynamic_cast<SocPXA*>(&soc);
    auto* ms = pxa ? pxa->GetMemoryStick() : nullptr;
    const bool pro = ms && msCtrlrPro(ms);
    if (sdCardInitialized() && (!ms || pro)) {
        frondMachine::Media media;
        const bool ok = pro ? beginSdSave(folder, media, msCtrlrInserted(ms), sd, frondMachine::kindMsPro, msCtrlrReadOnly(ms))
                            : beginSdSave(folder, media, soc.SdInserted(), sd, frondMachine::kindSd,
                                          slotCards().sdProtected);
        if (!ok) return fail("the card save could not start");
        temps.emplace_back();  // published by completeSdSave, not renamed with the .frond
        contents.media.push_back(std::move(media));
    }
    if (ms && msCtrlrPresent(ms) && !pro) {
        frondMachine::Media media;
        if (!beginMsSave(folder, ms, media, sd)) return fail("the Memory Stick could not be written");
        temps.emplace_back();  // published by completeSdSave
        contents.media.push_back(std::move(media));
    }
    // Once the .frond is published the card file is finished, or left for the next load.
    // As publishWithSd, telling a .frond that wasn't written from a card step after it.
    bool published = false;
    const bool saved = frondSession::saveFolderFromTemps(folder, contents, temps, &published, &slotCards().takenOut);
    if (!published) {
        abandonSdSave(sd);
        abandonSdSave(nand);
        std::cerr << "save: the session file could not be written in " << folder << std::endl;
        return false;
    }
    if (soc.GetNand() && nandRawBytes(soc.GetNand())) frondNand::completeSave(folder, soc.GetNand(), nand);
    const bool finished = completeSdSave(folder, sd);
    if (!saved)
        std::cerr << "save: the session file is written, but a card file could not be renamed or an unused one removed in "
                  << folder << std::endl;
    return finished && saved;
}

inline bool attachFolderMedia(const frondSession::Contents& contents, const std::filesystem::path& folder) {
    for (const auto& media : contents.media) {
        if (frondMachine::fileCardKind(media.kind)) {
            char id[SD_CARD_ID_MAX_LEN + 1]{};
            std::memcpy(id, media.identity.data(), SD_CARD_ID_MAX_LEN);
            if (!sdCardInitializeFromFile((folder / media.name).c_str(), media.physicalSize,
                                          frondSdFile::headerBytes, id))
                return false;
        }
    }
    return true;
}

}  // namespace frondNative

#endif
