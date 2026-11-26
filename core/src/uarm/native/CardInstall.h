#ifndef FROND_CARD_INSTALL_H
#define FROND_CARD_INSTALL_H

// Install on a Windows CE device (SoC::InstallsToCard): the file goes onto the SD card in the slot, which the
// guest sees taken out and put back in. A CAB can start the guest's installer on insertion.

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

#include "CardInstallCopy.h"
#include "CardReinsert.h"
#include "CardSwap.h"
#include "SoC.h"
#include "WinCeAutorun.h"
#include "storage/CardFile.h"
#include "storage/FatImage.h"

namespace frondNative {

struct CardInstallResult {
    bool ok;
    std::string code, message;
};

// A CAB the guest installs from the card, as against a plain file it only finds there.
inline bool cabFile(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    for (auto& c : extension) c = char(std::tolower(static_cast<unsigned char>(c)));
    return extension == ".cab";
}

// Copies source into the card's top folder, over a file of the same name. Needs the card in; it ends up in again
// unless putting it back fails (code card-out).
inline CardInstallResult installToSdCard(SoC& soc, const std::filesystem::path& folder, const std::string& source,
                                          bool startCab = true) {
    if (!sdCardInitialized()) return {false, "no-card", "Put an SD card in first."};
    if (folder.empty()) return {false, "folder", takeOutNeedsFolder};
    if (slotCards().sdProtected) return {false, "protected", "The SD card is write protected."};
    std::error_code error;
    const auto size = std::filesystem::file_size(source, error);
    if (error || !std::filesystem::is_regular_file(source, error)) return {false, "read", "Could not read the file."};
    const std::string name = std::filesystem::path(source).filename().string();

    // A card the user took out stays out.
    if (!soc.SdInserted()) return {false, "ejected", "Put the SD card back in first."};
    soc.SdEject();
    noteEjected(soc);
    CardIdentity identity{};
    const auto out = takeOutFileCard(folder, frondMachine::kindSd, false, identity);
    if (!out.ok) {
        settleEject(soc);
        soc.SdInsert();
        return {false, out.code, out.message};
    }
    CardInstallResult result{true, "copied", {}};
    bool autorun = false;
    try {
        std::ifstream in(source, std::ios::binary);
        if (!in) throw frondStorage::Error("Could not read the file.");
        frondStorage::CardFile card(folder / frondMachine::fileCardSibling(frondMachine::kindSd), true);
        frondStorage::FatImage fat(card);
        clearWinCeAutorun(fat);
        const auto read = [&](char* buffer, int64_t n) {
            in.read(buffer, n);
            return int64_t(in.gcount());
        };
        copyToCardRoot(fat, name, read, int64_t(size));
        if (startCab && cabFile(source)) {
            in.clear();
            in.seekg(0);
            if (in) autorun = stageWinCeAutorun(fat, read, int64_t(size));
        }
    } catch (const std::exception& e) {
        result = {false, "card", e.what()};
    }
    // The card goes back in whether the copy worked or not.
    const auto in = putInSd(folder, soc.GetDeviceType(), identity, false);
    // The card stays out, as the card exchange leaves it; "card-out" tells the frontend to show that.
    if (!in.ok) return {false, "card-out", in.message};
    soc.SetSdWriteProtect(false);
    settleEject(soc);
    soc.SdInsert();
    if (result.ok && autorun)
        return {true, "autorun", "CAB copied. Continue installation on the device."};
    if (result.ok)
        result.message = cabFile(source) ? "Copied to the SD card. Open it in File Explorer to install it."
                                         : "Copied to the SD card.";
    return result;
}

}  // namespace frondNative
#endif
