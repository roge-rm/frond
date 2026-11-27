#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "native/ZaurusPackage.h"

int main() {
    SdCardStore card;
    card.initialize(32 * 1024 * 1024 / SD_SECTOR_SIZE, "zaurus-package-test");
    frondNative::ZaurusCardBlocks blocks(card);
    frondStorage::FatImage::format(blocks);
    const char* path = "/Documents/Install_Files/test.ipk";

    auto queued = frondNative::stageZaurusPackage(card, "test.ipk", 7, [](char* buffer, int64_t n) {
        assert(n == 7);
        std::memcpy(buffer, "package", 7);
        return n;
    });
    assert(queued.ok && queued.code == "queued");
    const auto read = [&] {
        frondStorage::FatImage fat(blocks);
        const auto file = fat.find(path);
        assert(file && !file->directory);
        std::string bytes;
        fat.read(*file, [&](const char* data, int64_t n) { bytes.append(data, size_t(n)); });
        assert(fat.fatCopiesMatch());
        return bytes;
    };
    assert(read() == "package");

    auto interrupted = frondNative::stageZaurusPackage(card, "test.ipk", 9, [](char*, int64_t) { return 0; });
    assert(!interrupted.ok && read() == "package");
    std::cout << "Zaurus package staging and interrupted replacement passed\n";
}
