#ifndef FROND_ZAURUS_PACKAGE_H
#define FROND_ZAURUS_PACKAGE_H

#include "CardInstall.h"

namespace frondNative {

// FAT edits go into the ejected card's overlay. The next machine save journals the overlay
// with the session, just like guest writes. An unsuccessful edit rolls back to the pin.
class ZaurusCardBlocks final : public frondStorage::Blocks {
public:
    explicit ZaurusCardBlocks(SdCardStore& store) : store(store) {}

    int64_t length() const override { return int64_t(store.sectorCount()) * SD_SECTOR_SIZE; }
    void read(int64_t offset, char* data, int64_t n) override {
        if (offset < 0 || n < 0 || !store.readBytes(offset, n, data))
            throw frondStorage::Error("Couldn't read the card.");
    }
    void write(int64_t offset, const char* data, int64_t n) override {
        if (offset < 0 || n < 0 || !store.writeBytes(offset, n, data))
            throw frondStorage::Error("Couldn't write the card.");
    }
    void sync() override {}
    void commit(const frondStorage::Sectors& sectors) override {
        for (const auto& sector : sectors)
            write(sector.first, reinterpret_cast<const char*>(sector.second.data()), sector.second.size());
    }

private:
    SdCardStore& store;
};

inline CardInstallResult stageZaurusPackage(SdCardStore& store, const std::string& name, int64_t size,
                                             const std::function<int64_t(char*, int64_t)>& input) {
    const auto pin = store.snapshot();
    if (!pin) return {false, "card", "Could not prepare the card for editing."};
    try {
        ZaurusCardBlocks blocks(store);
        frondStorage::FatImage fat(blocks);
        const std::string queue = "/Documents/Install_Files";
        if (!fat.find("/Documents")) fat.makeFolder("/", "Documents");
        if (!fat.find(queue)) fat.makeFolder("/Documents", "Install_Files");
        copyToCardFolder(fat, queue, name, input, size);
    } catch (const std::exception& e) {
        if (!store.restore(pin)) return {false, "card", "Could not restore the staged card after the edit failed."};
        return {false, "card", e.what()};
    }
    return {true, "queued", "Reinsert the card and open Add/Remove Software in Qtopia."};
}

inline CardInstallResult queueZaurusPackage(SocPXA& soc, const std::string& request) {
    const auto space = request.find(' ');
    const auto slot = request.substr(0, space);
    if (space == std::string::npos || (slot != "sd" && slot != "cf") ||
        !frondPosix::IsAbsolutePath(request.substr(space + 1)))
        return {false, "argument", "Choose SD or CF and an absolute IPK file path."};
    const std::filesystem::path source = request.substr(space + 1);
    auto extension = source.extension().string();
    for (auto& c : extension) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (extension != ".ipk") return {false, "argument", "Choose an IPK package."};

    const bool cf = slot == "cf";
    auto& store = sdCardStore(cf ? 1 : 0);
    if (!store.initialized()) return {false, "no-card", "Insert a FAT card first."};
    if (cf ? soc.CompactFlashInserted() : soc.SdInserted())
        return {false, "busy", "Eject the card in Qtopia and then in Frond."};
    const auto& slots = slotCards();
    if (cf ? slots.cfProtected : slots.sdProtected)
        return {false, "protected", "The card is write protected."};

    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec))
        return {false, "read", "Could not read the package."};
    const auto size = std::filesystem::file_size(source, ec);
    std::ifstream in(source, std::ios::binary);
    if (ec || !in || !size || size > uint64_t(UINT32_MAX))
        return {false, "read", "The package is empty, unreadable or too large."};
    return stageZaurusPackage(store, source.filename().string(), int64_t(size),
                              [&](char* data, int64_t n) { in.read(data, n); return int64_t(in.gcount()); });
}

}  // namespace frondNative

#endif
