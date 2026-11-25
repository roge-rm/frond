#include "rx1950_nand_image.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "s3c2442_nand.h"

namespace {
    constexpr size_t nbfHeader = 0xa5a, nbfSector = 512 + 8;
    // ASSUMPTION "rx1950 store at block 21": 21 blocks sit outside the store (R0: 4096 blocks, 4075 in
    // Dan's capture), block 0 the IPL, then eboot from block 1. They're marked reserved, which BootPart
    // and the FAL skip, so the store starts in the first block after them.
    constexpr uint32_t reservedBlocks = 21, ebootBlock = 1, settingsPage = 9;

    // Spare bytes 0-7 are the SectorInfo (logical sector, OEM flags, bad block byte at 5, two reserved),
    // 8-10 the main area ECC. ASSUMPTION "rx1950 spare layout": the small page layout of Samsung's
    // S3C24x0 FMD; R2 checks it against FLASHDRV.
    void setSpare(uint8_t* page, const uint8_t sectorInfo[8]) {
        uint8_t* spare = page + S3cNand::pageData;
        std::memcpy(spare, sectorInfo, 8);
        const uint32_t ecc = S3cNand::ecc512(page);
        spare[8] = uint8_t(ecc);
        spare[9] = uint8_t(ecc >> 8);
        spare[10] = uint8_t(ecc >> 16);
    }
}

bool rx1950NandImage(const uint8_t* eboot, size_t ebootSize, const uint8_t* nbf, size_t nbfSize,
                     std::vector<uint8_t>& flash, std::string& error, const Rx1950NandOptions& options) {
    if (nbfSize < nbfHeader || (nbfSize - nbfHeader) % nbfSector ||
        (nbfSize - nbfHeader) / nbfSector > size_t(S3cNand::blocks - reservedBlocks) * S3cNand::pagesPerBlock) {
        error = "not an NBF of 512 byte sectors with SectorInfo that fits the NAND";
        return false;
    }
    const size_t sectors = (nbfSize - nbfHeader) / nbfSector;
    // Each sector's logical sector number is erased (0xffffffff) or inside the NAND; a raw dump isn't.
    for (size_t i = 0; i < sectors; ++i) {
        const uint8_t* info = nbf + nbfHeader + i * nbfSector + S3cNand::pageData;
        const uint32_t logical = info[0] | info[1] << 8 | info[2] << 16 | uint32_t(info[3]) << 24;
        if (logical != 0xffffffffu && logical >= uint32_t(S3cNand::blocks - reservedBlocks) * S3cNand::pagesPerBlock) {
            error = "not an NBF of 512 byte sectors with SectorInfo that fits the NAND";
            return false;
        }
    }
    if (ebootSize > size_t(reservedBlocks - ebootBlock) * S3cNand::pagesPerBlock * S3cNand::pageData) {
        error = "eboot doesn't fit the reserved blocks";
        return false;
    }
    flash.assign(size_t(S3cNand::pages) * S3cNand::pageSize, 0xff);
    // Reserved blocks: OEM byte with OEM_BLOCK_RESERVED (bit 0) and OEM_BLOCK_READONLY (bit 1) clear.
    const uint8_t reservedInfo[8] = {0xff, 0xff, 0xff, 0xff, 0xfc, 0xff, 0xff, 0xff};
    for (uint32_t page = 0; page < reservedBlocks * S3cNand::pagesPerBlock; ++page) {
        uint8_t* p = &flash[size_t(page) * S3cNand::pageSize];
        if (page >= ebootBlock * S3cNand::pagesPerBlock) {
            const size_t at = size_t(page - ebootBlock * S3cNand::pagesPerBlock) * S3cNand::pageData;
            if (at < ebootSize) std::memcpy(p, eboot + at, std::min<size_t>(S3cNand::pageData, ebootSize - at));
        }
        // Settings page: bKitlTransport at 0, the RUU signature at 4, dwDbgMsgSignature at 12 and
        // dwDbgMsgFlags at 16, where bit 1 set turns the kernel's debug text off. ASSUMPTION "rx1950
        // settings page": transport 0 (eboot's "No Debug"), RUU signature 0 (what eboot writes once an
        // update is done; anything but "UUR " boots the OS), the debug message signature (0x1a2b3c4d when
        // set from eboot's menu) left erased and the flags all ones, so a retail kernel keeps no text.
        if (page == settingsPage) {
            std::memset(p, 0, 8);
            if (options.debugMessages) std::memset(p + 16, 0, 4);
        }
        setSpare(p, reservedInfo);
    }
    for (size_t i = 0; i < sectors; ++i) {
        uint8_t* p = &flash[(size_t(reservedBlocks) * S3cNand::pagesPerBlock + i) * S3cNand::pageSize];
        const uint8_t* s = nbf + nbfHeader + i * nbfSector;
        std::memcpy(p, s, S3cNand::pageData);
        setSpare(p, s + S3cNand::pageData);
    }
    return true;
}

const Rx1950Firmware rx1950Cebl{"CEBL.nbf", 264794, "33807007a49ab40e8190f2fdb20080d1d7efe6d3a56a6b551422373271a94b3f"};
const Rx1950Firmware rx1950Ceos{"CEOS.nbf", 67095130, "764807c7aacd780876462b62a9ca2555226d2f11acccc69e0785fc159be20f6e"};

bool rx1950NandLoad(const std::string& path, std::vector<uint8_t>& flash, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "can't read " + path; return false; }
    flash.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (flash.size() != size_t(S3cNand::pages) * S3cNand::pageSize) {
        error = path + " isn't an rx1950 NAND store (wrong size)";
        flash.clear();
        return false;
    }
    return true;
}

bool rx1950NandSave(const std::string& path, S3cNand& nand, std::string& error) {
    const size_t blockBytes = size_t(S3cNand::pagesPerBlock) * S3cNand::pageSize;
    if (nand.flash.size() != size_t(S3cNand::pages) * S3cNand::pageSize) { error = "no NAND to save"; return false; }
    FILE* f = std::fopen(path.c_str(), "r+b");
    const bool whole = !f;
    if (whole) f = std::fopen(path.c_str(), "wb");
    if (!f) { error = "can't write " + path; return false; }
    bool ok = true;
    if (whole) ok = std::fwrite(nand.flash.data(), 1, nand.flash.size(), f) == nand.flash.size();
    else
        for (uint32_t block = 0; ok && block < S3cNand::blocks; ++block) {
            if (!nand.blockDirty(block)) continue;
            ok = std::fseek(f, long(block * blockBytes), SEEK_SET) == 0 &&
                 std::fwrite(&nand.flash[block * blockBytes], 1, blockBytes, f) == blockBytes;
        }
    ok = std::fclose(f) == 0 && ok;
    if (!ok) { error = "writing " + path + " failed"; return false; }
    nand.clearDirty();
    return true;
}

uint32_t rx1950UserStoreBlockFromMbr(const uint8_t* sector0) {
    if (!sector0 || sector0[510] != 0x55 || sector0[511] != 0xaa) return rx1950UserStoreBlock;
    constexpr uint32_t reservedBlocks = 21;
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t* entry = sector0 + 0x1be + 16 * i;
        const uint8_t type = entry[4];
        if (type != 0x04 && type != 0x06 && type != 0x0b && type != 0x0c) continue;
        const uint32_t start = uint32_t(entry[8]) | uint32_t(entry[9]) << 8 | uint32_t(entry[10]) << 16 | uint32_t(entry[11]) << 24;
        // Rounded up, so the erase never reaches into the block before the store.
        const uint32_t block = (start + S3cNand::pagesPerBlock - 1) / S3cNand::pagesPerBlock + reservedBlocks;
        if (start == 0 || block >= S3cNand::blocks) return rx1950UserStoreBlock;
        return block;
    }
    return rx1950UserStoreBlock;
}

uint32_t rx1950UserStoreBlockFromPage(const uint8_t* page) {
    if (!page || page[512] || page[513] || page[514] || page[515]) return rx1950UserStoreBlock;
    return rx1950UserStoreBlockFromMbr(page);
}
