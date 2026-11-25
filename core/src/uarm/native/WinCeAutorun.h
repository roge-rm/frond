#ifndef FROND_WINCE_AUTORUN_H
#define FROND_WINCE_AUTORUN_H

#include <cstring>

#include "WinCeAutorunData.h"
#include "storage/FatImage.h"

namespace frondNative {

// Cancel an earlier Frond request on the next Install. Its original CAB is still in the top folder.
inline void clearWinCeAutorun(frondStorage::FatImage& fat) {
    const auto directory = fat.find("/2577");
    if (!directory || !directory->directory) return;
    const auto helper = fat.find("/2577/autorun.exe");
    if (!helper || helper->directory || helper->size != int64_t(sizeof(winCeAutorun))) return;
    size_t offset = 0;
    bool ours = true;
    fat.read(*helper, [&](const char* buffer, int64_t count) {
        if (std::memcmp(buffer, winCeAutorun + offset, size_t(count)) != 0) ours = false;
        offset += size_t(count);
    });
    if (!ours) return;
    const auto pending = fat.find("/2577/FrondPending.cab");
    fat.batch([&] {
        if (pending && !pending->directory) fat.remove("/2577/FrondPending.cab");
        fat.remove("/2577/autorun.exe");
    });
}

// The original CAB stays in the card's top folder. The helper consumes its private copy once.
inline bool stageWinCeAutorun(frondStorage::FatImage& fat,
                             const std::function<int64_t(char*, int64_t)>& input, int64_t size) {
    const auto directory = fat.find("/2577");
    if (directory && (!directory->directory || fat.find("/2577/autorun.exe") ||
                      fat.find("/2577/FrondPending.cab"))) return false;
    const auto neutral = fat.find("/0");
    if (neutral && neutral->directory && fat.find("/0/autorun.exe")) return false;

    try {
        fat.batch([&] {
            if (!directory) fat.makeFolder("/", "2577");
            fat.add("/2577", "FrondPending.cab", input, size);
            size_t offset = 0;
            fat.add("/2577", "autorun.exe", [&](char* buffer, int64_t n) {
                const auto count = std::min(size_t(n), sizeof(winCeAutorun) - offset);
                std::memcpy(buffer, winCeAutorun + offset, count);
                offset += count;
                return int64_t(count);
            }, sizeof(winCeAutorun));
        });
        return true;
    } catch (const frondStorage::Error&) {
        return false;
    }
}

}  // namespace frondNative
#endif
