#include "rx1950_nand_image.h"

#include <algorithm>
#include <cstring>

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
        // dwDbgMsgFlags at 16, where bit 1 set turns the kernel's debug text off.
        if (page == settingsPage && options.debugMessages) std::memset(p + 16, 0, 4);
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
