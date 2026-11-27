#ifndef FROND_CARD_INSTALL_COPY_H
#define FROND_CARD_INSTALL_COPY_H

// The card edit behind a Windows CE install: a file into the card's top folder, over a file of the same name.

#include <cstdint>
#include <functional>
#include <string>

#include "storage/FatImage.h"

namespace frondNative {

// input fills the buffer and returns the bytes given, as FatImage::add takes it. An old file of the same name
// is replaced in one change, so its clusters stay used until the new data is in: a failed copy leaves it whole.
inline void copyToCardFolder(frondStorage::FatImage& fat, const std::string& folder, const std::string& name,
                           const std::function<int64_t(char*, int64_t)>& input, int64_t size) {
    const std::string path = (folder == "/" ? folder : folder + "/") + name;
    const auto add = [&] { fat.add(folder, name, input, size); };
    if (const auto old = fat.find(path)) {
        if (old->directory) throw frondStorage::Error("The card has a folder named " + name + ".");
        fat.replace(path, add);
    } else {
        add();
    }
}

inline void copyToCardRoot(frondStorage::FatImage& fat, const std::string& name,
                           const std::function<int64_t(char*, int64_t)>& input, int64_t size) {
    copyToCardFolder(fat, "/", name, input, size);
}

}  // namespace frondNative
#endif
