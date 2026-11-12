#ifndef FROND_CARD_SWAP_H
#define FROND_CARD_SWAP_H

#include "CardSave.h"
#include "MachineFolder.h"
#include "MachineSession.h"
#include "MemoryStickImage.h"
#include "MsProCards.h"
#include "SdCards.h"
#include "SessionIo.h"
#include "device_profile5.h"
#include "mmiodev_MemoryStickController.h"
#include "sdcard.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

// Taking an ejected card out of a running machine into its folder's card file, and putting the
// card file there back in, so a frontend can move the file between machines and the drawer.
// A card taken out is settled: one file, no journals, candidate or generation files.
namespace frondNative {

using CardIdentity = std::array<uint8_t, frondMachine::identityBytes>;

// The identity as 64 hex digits, the 32 descriptor bytes in order.
inline std::string identityHex(const CardIdentity& identity) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto byte : identity) {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}

inline bool parseIdentityHex(std::string_view text, CardIdentity& identity) {
    if (text.size() != 2 * identity.size()) return false;
    auto nibble = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    CardIdentity parsed{};
    for (size_t i = 0; i < parsed.size(); ++i) {
        const int hi = nibble(text[2 * i]), lo = nibble(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        parsed[i] = uint8_t(hi << 4 | lo);
    }
    identity = parsed;
    return true;
}

// The put-in argument: the identity, then "protected" or "writable" (the default).
inline bool parsePutIn(const std::string& argument, CardIdentity& identity, bool& writeProtected) {
    const auto space = argument.find(' ');
    const std::string protection = space == std::string::npos ? "writable" : argument.substr(space + 1);
    if (protection != "protected" && protection != "writable") return false;
    writeProtected = protection == "protected";
    return parseIdentityHex(std::string_view(argument).substr(0, space), identity);
}

// The take-out reply: the identity and protection for the frontend's card.json.
inline std::string takenOutReply(const CardIdentity& identity, bool writeProtected) {
    return identityHex(identity) + (writeProtected ? " protected" : " writable");
}

inline void noteTakenOut(const char* name, bool out) {
    auto& names = slotCards().takenOut;
    names.erase(std::remove(names.begin(), names.end(), name), names.end());
    if (out) names.push_back(name);
}

constexpr const char* takeOutNeedsFolder = "Save the machine before taking its card out.";

// Writes the card store's card (SD, or a Memory Stick with `kind`) into the folder's card file
// and closes it. A card on this folder's file is journalled and the journal applied; any other is
// copied whole. If this fails the card stays in the store as it was.
inline SdResult takeOutFileCard(const std::filesystem::path& folder, uint32_t kind, bool writeProtected,
                                CardIdentity& identity, const CardIdentity* cardIdentity = nullptr) {
    if (folder.empty()) return frondSdCandidate::fail("folder", takeOutNeedsFolder);
    if (!sdCardInitialized()) return frondSdCandidate::fail("empty", "This session has no card.");
    frondMachine::Media media;
    SdSave save;
    if (!beginSdSave(folder, media, false, save, kind, writeProtected, cardIdentity)) {
        abandonSdSave(save);
        return frondSdCandidate::fail("write", "Could not write the card file.");
    }
    if (!finishSdSave(folder, save)) {
        // The journal stays; the folder's .frond is older, so the next load drops it.
        if (!save.journal.empty()) sdCardTrustChunks(save.chunks);
        abandonSdSave(save);
        return frondSdCandidate::fail("write", "Could not write the card file.");
    }
    identity = media.identity;
    sdCardReset();
    frondSdCandidate::releaseUnused();
    const auto sidecar = folder / frondMachine::fileCardSibling(kind);
    FileCardBacking::DiscardGenerations(sidecar);
    frondSession::syncDir(folder);
    noteTakenOut(frondMachine::fileCardSibling(kind), true);
    return {true, "taken-out", {}};
}

// Opens the folder's card file (SD or Pro) in the card store as a new card with `identity`.
inline SdResult openFileCard(const std::filesystem::path& folder, uint32_t kind, int32_t deviceType,
                             const CardIdentity& identity) {
    if (folder.empty()) return frondSdCandidate::fail("folder", "This machine has no folder to take a card from.");
    if (sdCardInitialized()) return frondSdCandidate::fail("busy", "Take the current card out first.");
    const char* name = frondMachine::fileCardSibling(kind);
    frondSession::CardSwap swap;
    swap.identityGiven = true;
    swap.identity = identity;
    frondMachine::Media media;
    if (!frondSession::swappedInCard(folder, name, swap, media))
        return frondSdCandidate::fail("card", "The card file is missing or damaged.");
    const bool fits = kind == frondMachine::kindMsPro
                          ? msProCreateAllowed(deviceType, frondMsPro::mibOfBytes(media.physicalSize))
                          : sdImageFits(deviceType, media.physicalSize);
    if (!fits) return frondSdCandidate::fail("size", "This machine doesn't take a card of that size.");
    char id[SD_CARD_ID_MAX_LEN + 1]{};
    std::memcpy(id, identity.data(), SD_CARD_ID_MAX_LEN);
    if (!sdCardInitializeFromFile((folder / name).c_str(), media.physicalSize, frondSdFile::headerBytes, id))
        return frondSdCandidate::fail("card", "Could not open the card file.");
    noteTakenOut(name, false);
    return {true, "opened", {}};
}

// An SD card into the empty slot; the caller inserts it.
inline SdResult putInSd(const std::filesystem::path& folder, int32_t deviceType, const CardIdentity& identity,
                        bool writeProtected) {
    auto result = openFileCard(folder, frondMachine::kindSd, deviceType, identity);
    if (result.ok) slotCards().sdProtected = writeProtected;
    return result;
}

// The ejected stick, classic or Pro, into the folder's card file, and the slot emptied.
inline SdResult takeOutMs(const std::filesystem::path& folder, MemoryStickController* ms, CardIdentity& identity,
                          bool& writeProtected) {
    if (!msCtrlrPresent(ms)) return frondSdCandidate::fail("empty", "This session has no Memory Stick.");
    if (msCtrlrInserted(ms)) return frondSdCandidate::fail("busy", "Eject the Memory Stick before taking it out.");
    if (folder.empty()) return frondSdCandidate::fail("folder", takeOutNeedsFolder);
    writeProtected = msCtrlrReadOnly(ms);
    if (msCtrlrPro(ms)) {
        auto result = takeOutFileCard(folder, frondMachine::kindMsPro, writeProtected, identity);
        if (result.ok) msCtrlrDetach(ms);
        return result;
    }
    if (!classicOnStore(ms)) return frondSdCandidate::fail("write", "Could not write the Memory Stick file.");
    {
        auto stick = slotCards().msIdentity;
        auto result = takeOutFileCard(folder, frondMachine::kindMs, writeProtected, identity, &stick);
        if (!result.ok) return result;
    }
    msCtrlrDetach(ms);
    msOpened({}, 0);
    noteTakenOut(frondMachine::msSibling, true);
    return {true, "taken-out", {}};
}

// The folder's stick file, classic or Pro, into the empty slot, inserted.
inline SdResult putInMs(const std::filesystem::path& folder, MemoryStickController* ms, int32_t deviceType,
                        const CardIdentity& identity, bool writeProtected) {
    if (!ms) return frondSdCandidate::fail("unsupported", "This device has no Memory Stick slot.");
    if (msCtrlrPresent(ms)) return frondSdCandidate::fail("busy", "Take the current Memory Stick out first.");
    if (folder.empty()) return frondSdCandidate::fail("folder", "This machine has no folder to take a card from.");
    std::error_code ec;
    const bool classic = std::filesystem::is_regular_file(folder / frondMachine::msSibling, ec);
    const bool pro = std::filesystem::is_regular_file(folder / frondMachine::msProSibling, ec);
    if (classic == pro)
        return frondSdCandidate::fail("card", classic ? "The folder has two Memory Stick files." : "The Memory Stick file is missing.");
    if (pro) {
        auto result = openFileCard(folder, frondMachine::kindMsPro, deviceType, identity);
        if (!result.ok) return result;
        if (!attachMsPro(ms, false) || !msCtrlrProtect(ms, writeProtected) || !msCtrlrInsert(ms, true)) {
            detachMsPro(ms);
            return frondSdCandidate::fail("card", "Could not open the Memory Stick file.");
        }
        return {true, "inserted", {}};
    }
    frondSession::CardSwap swap;
    swap.identityGiven = true;
    swap.identity = identity;
    frondMachine::Media media;
    if (!frondSession::swappedInCard(folder, frondMachine::msSibling, swap, media))
        return frondSdCandidate::fail("card", "The Memory Stick file is missing or damaged.");
    if (!deviceProfileMsSizeValid5(deviceType, media.logicalSize >> 20))
        return frondSdCandidate::fail("size", "This machine doesn't take a Memory Stick of that size.");
    if (sdCardInitialized()) return frondSdCandidate::fail("busy", "Take the current card out first.");
    if (!attachClassicFile(ms, folder / frondMachine::msSibling, identity, false, writeProtected) ||
        !msCtrlrInsert(ms, true)) {
        msCtrlrDetach(ms);
        return frondSdCandidate::fail("card", "Could not open the Memory Stick file.");
    }
    msOpened(identity, media.contentGeneration);
    noteTakenOut(frondMachine::msSibling, false);
    return {true, "inserted", {}};
}

}  // namespace frondNative

#endif
