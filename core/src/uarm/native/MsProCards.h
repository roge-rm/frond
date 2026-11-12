#ifndef FROND_MS_PRO_CARDS_H
#define FROND_MS_PRO_CARDS_H

#include "MsProImage.h"
#include "SdCards.h"
#include "device_profile5.h"
#include "mmiodev_MemoryStickController.h"
#include "sdcard.h"

#include <filesystem>
#include <string>
#include <sys/stat.h>

// Memory Stick Pro create, import and attach. A Pro stick's sectors live in the same card store as
// an SD card (sdcard.h), as a candidate sidecar until a save publishes ms.frondmsp, so it shares
// the SD card's journals, generations and exports. Only devices without an SD slot take one.
namespace frondNative {

inline bool msProCreateAllowed(int32_t deviceType, unsigned mib) {
    return frondMsPro::validCardMib(mib) && deviceProfileMsProSizeValid5(deviceType, mib);
}

// Sizes and labels for messages: "256 MB, 512 MB, 1 GB or 2 GB".
inline std::string msProSizeLabel(unsigned mib) {
    return mib >= 1024 ? std::to_string((mib + 512) / 1024) + " GB" : std::to_string(mib) + " MB";
}
inline std::string msProSizes(int32_t deviceType) {
    std::vector<unsigned> sizes;
    for (unsigned mib : frondMsPro::cardMibs)
        if (msProCreateAllowed(deviceType, mib)) sizes.push_back(mib);
    std::string out;
    for (size_t i = 0; i < sizes.size(); ++i) {
        if (i) out += i + 1 == sizes.size() ? " or " : ", ";
        out += msProSizeLabel(sizes[i]);
    }
    return out;
}

// Puts the card store's stick in the Memory Stick slot as a Pro.
inline bool attachMsPro(MemoryStickController* ms, bool inserted) {
    const uint64_t bytes = uint64_t(sdCardSectorCount()) * SD_SECTOR_SIZE;
    const unsigned mib = frondMsPro::mibOfBytes(bytes);
    if (!ms || !sdCardInitialized() || !mib) return false;
    MsProStore store;
    store.read = [](uint32_t sector, uint8_t* out) { return sdCardRead(sector, out); };
    store.write = [](uint32_t sector, const uint8_t* in) { return sdCardWrite(sector, in); };
    store.sectors = uint32_t(sdCardSectorCount());
    const char* id = sdCardGetId();
    store.attribute = frondMsPro::attribute(mib, id ? id : "");
    store.release = [] { sdCardReset(); frondSdCandidate::releaseUnused(); };
    store.format = [mib] { return frondMsPro::format(mib, [](uint32_t sector, const uint8_t* in) { return sdCardWrite(sector, in); }); };
    return msCtrlrAttachPro(ms, std::move(store), inserted);
}

// Takes the stick out of the slot and drops its card store and any candidate file.
inline void detachMsPro(MemoryStickController* ms) {
    if (ms && msCtrlrPro(ms)) msCtrlrDetach(ms);
    sdCardReset();
    frondSdCandidate::releaseUnused();
}

constexpr const char* msProNeedsFolder = "Memory Stick Pro cards need a saved session.";

// A new formatted Pro stick, inserted. It is always a candidate sidecar in `folder` on a random
// identity. The old stick stays if the file can't be made.
inline SdResult createMsProCard(MemoryStickController* ms, const std::filesystem::path& folder, int32_t deviceType,
                                unsigned mib, const frondSdCandidate::Progress& progress = {}) {
    if (!msProCreateAllowed(deviceType, mib))
        return frondSdCandidate::fail("size", "Choose a Memory Stick Pro of " + msProSizes(deviceType) + ".");
    if (folder.empty()) return frondSdCandidate::fail("folder", msProNeedsFolder);
    const uint64_t bytes = uint64_t(mib) << 20;
    const auto layout = frondSd::layout(mib);
    std::vector<frondSdCandidate::LayoutSector> sectors;
    for (const auto& sector : layout) sectors.push_back({sector.index, sector.bytes.data()});
    frondSdCandidate::Candidate candidate;
    auto result = frondSdCandidate::create(folder, bytes, sectors, candidate, progress);
    if (!result.ok) return result;
    if (!frondSdCandidate::attach(candidate, bytes, frondSdCandidate::randomIdentity()))
        return frondSdCandidate::fail("write", "Could not open the card file.");
    // The new stick goes on first, and replaces the old one only once that worked.
    if (!attachMsPro(ms, true)) return frondSdCandidate::fail("write", "Could not open the card file.");
    return {true, "inserted", {}};
}

// A raw image of one of the Pro sizes, copied into a candidate and inserted.
inline SdResult loadMsProImage(MemoryStickController* ms, const std::filesystem::path& folder, int32_t deviceType,
                               const std::filesystem::path& source, const frondSdCandidate::Progress& progress = {}) {
    struct stat st{};
    if (::stat(source.c_str(), &st) < 0 || !S_ISREG(st.st_mode))
        return frondSdCandidate::fail("read", "Could not read the Memory Stick image.");
    const uint64_t bytes = uint64_t(st.st_size);
    if (!msProCreateAllowed(deviceType, frondMsPro::mibOfBytes(bytes)))
        return frondSdCandidate::fail("size", "Use a raw Memory Stick Pro image of " + msProSizes(deviceType) + ".");
    if (folder.empty()) return frondSdCandidate::fail("folder", msProNeedsFolder);
    frondSdCandidate::Candidate candidate;
    auto result = frondSdCandidate::importImage(folder, source, bytes, candidate, progress);
    if (!result.ok) return result;
    if (!frondSdCandidate::attach(candidate, bytes, frondSdCandidate::randomIdentity()))
        return frondSdCandidate::fail("write", "Could not open the card file.");
    // The new stick goes on first, and replaces the old one only once that worked.
    if (!attachMsPro(ms, true)) return frondSdCandidate::fail("write", "Could not open the card file.");
    return {true, "inserted", {}};
}

}  // namespace frondNative

#endif
