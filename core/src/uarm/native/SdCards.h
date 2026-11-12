#ifndef FROND_SD_CARDS_H
#define FROND_SD_CARDS_H

#include "MachineFolder.h"
#include "SdCandidate.h"
#include "SdImage.h"
#include "device_profile5.h"
#include "md5.h"
#include "sdcard.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <sys/stat.h>
#include <vector>

// SD card create, import and export for the CLI and the desktop control socket.
namespace frondNative {

using SdResult = frondSdCandidate::Result;

// Sizes sd-create makes for this device: the SdImage.h layouts within the profile's bounds.
inline bool sdCreateAllowed(int32_t deviceType, unsigned mib) {
    return frondSd::validCardMib(mib) && deviceProfileSdSizeValid5(deviceType, uint64_t(mib) << 20);
}

inline std::string sdCreateSizes(int32_t deviceType) {
    std::vector<unsigned> sizes;
    for (unsigned mib : frondSd::cardMibs)
        if (sdCreateAllowed(deviceType, mib)) sizes.push_back(mib);
    std::string out = "Choose ";
    for (size_t i = 0; i < sizes.size(); ++i) {
        if (i) out += i + 1 == sizes.size() ? " or " : ", ";
        out += std::to_string(sizes[i]);
    }
    return out + " MiB.";
}

// A raw image this device takes: whole sectors within the profile's bounds.
inline bool sdImageFits(int32_t deviceType, uint64_t bytes) {
    return frondSdFile::validPayloadSize(bytes) && deviceProfileSdSizeValid5(deviceType, bytes);
}

inline std::string sdImageSizes(int32_t deviceType) {
    const auto* profile = deviceProfile5(deviceType);
    const uint64_t most = std::min<uint64_t>(profile ? profile->sdMax : frondSdFile::maxPayloadBytes,
                                             frondSdFile::maxPayloadBytes);
    return "Use a raw SD image of 32 to " + std::to_string(most >> 20) + " MiB, aligned to 512 bytes.";
}

inline SdResult sdInRam(std::vector<uint8_t>& image) {
    auto* owned = static_cast<uint8_t*>(malloc(image.size()));
    if (!owned) throw std::bad_alloc();
    std::memcpy(owned, image.data(), image.size());
    const auto id = md5(owned, image.size());
    sdCardInitializeWithData(image.size() / SD_SECTOR_SIZE, owned, id.c_str());
    frondSdCandidate::releaseUnused();
    return {true, "inserted", {}};
}

inline constexpr const char* sdNeedsFolder = "SD cards above 128 MiB need a saved session.";

// Replaces the card with a new formatted one. Up to 128 MiB it lives in RAM; above that it is a
// candidate sidecar in `folder` on a random identity. The old card stays if this fails.
inline SdResult createSdCard(const std::filesystem::path& folder, int32_t deviceType, unsigned mib,
                             const frondSdCandidate::Progress& progress = {}) {
    if (!sdCreateAllowed(deviceType, mib)) return frondSdCandidate::fail("size", sdCreateSizes(deviceType));
    const uint64_t bytes = uint64_t(mib) << 20;
    if (bytes <= frondSdCandidate::ramMaxBytes) {
        auto image = frondSd::create(mib);
        return sdInRam(image);
    }
    if (folder.empty()) return frondSdCandidate::fail("folder", sdNeedsFolder);
    const auto layout = frondSd::layout(mib);
    std::vector<frondSdCandidate::LayoutSector> sectors;
    for (const auto& sector : layout) sectors.push_back({sector.index, sector.bytes.data()});
    frondSdCandidate::Candidate candidate;
    auto result = frondSdCandidate::create(folder, bytes, sectors, candidate, progress);
    if (!result.ok) return result;
    if (!frondSdCandidate::attach(candidate, bytes, frondSdCandidate::randomIdentity()))
        return frondSdCandidate::fail("write", "Could not open the card file.");
    return {true, "inserted", {}};
}

// Replaces the card with a copy of a raw image. Images above 128 MiB stream into a candidate.
inline SdResult loadSdImage(const std::filesystem::path& folder, int32_t deviceType,
                            const std::filesystem::path& source, const frondSdCandidate::Progress& progress = {}) {
    struct stat st{};
    if (::stat(source.c_str(), &st) < 0 || !S_ISREG(st.st_mode))
        return frondSdCandidate::fail("read", "Could not read the card image.");
    const uint64_t bytes = uint64_t(st.st_size);
    if (!sdImageFits(deviceType, bytes)) return frondSdCandidate::fail("size", "sd card image has bad size");
    if (bytes <= frondSdCandidate::ramMaxBytes) {
        std::vector<uint8_t> image;
        if (!frondMachine::readBytes(source, image) || image.size() != bytes)
            return frondSdCandidate::fail("read", "Could not read the card image.");
        return sdInRam(image);
    }
    if (folder.empty()) return frondSdCandidate::fail("folder", sdNeedsFolder);
    frondSdCandidate::Candidate candidate;
    auto result = frondSdCandidate::importImage(folder, source, bytes, candidate, progress);
    if (!result.ok) return result;
    if (!frondSdCandidate::attach(candidate, bytes, frondSdCandidate::randomIdentity()))
        return frondSdCandidate::fail("write", "Could not open the card file.");
    return {true, "inserted", {}};
}

// Writes the card's raw sectors to `dest` from a pinned snapshot.
inline SdResult exportSd(const std::filesystem::path& dest, const frondSdCandidate::Progress& progress = {}) {
    auto pin = sdCardSnapshot();
    if (!pin) return frondSdCandidate::fail("empty", "This session has no card.");
    return frondSdCandidate::exportRaw(*pin, dest, progress);
}

}  // namespace frondNative

#endif
