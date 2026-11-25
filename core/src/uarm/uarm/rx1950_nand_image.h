#ifndef FROND_RX1950_NAND_IMAGE_H
#define FROND_RX1950_NAND_IMAGE_H

// The rx1950's NAND contents (S3cNand's page layout) made from the bootloader and CEOS.nbf, the firmware
// Frond accepts for it, and the machine's NAND store file.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class S3cNand;

// Block 0 page 9 holds the board's settings, which eboot copies into the driver globals. Neither dump has
// it; rx1950NandImage writes the values a retail unit is assumed to hold (KITL off, no RUU update pending,
// the kernel's debug text off). debugMessages turns the debug text on (see tools/rx1950-boot.cpp).
struct Rx1950NandOptions {
    bool debugMessages = false;
};

// The first physical block of the user store (the FAT partition's FILESYS region): 21 reserved blocks, then the
// MSFLSH50 XIP region (233 blocks) and READONLY region (1518 blocks), R0. The guest writes nothing below it.
// A hard reset erases from here to the end.
// This is HP's WM5 value, the fallback when logical sector 0 holds no usable partition table.
constexpr uint32_t rx1950UserStoreBlock = 21 + 233 + 1518;

// The user store's first block from logical sector 0 (512 bytes, the MBR): the first FAT partition (type 04,
// 06, 0b or 0c) start sector / 32 + 21 reserved blocks. rx1950UserStoreBlock when there's no 0x55aa signature
// or no such partition. WM5 gives 1772, a WM6.1 image 1775.
// A start that isn't on a 32-sector block boundary rounds up.
uint32_t rx1950UserStoreBlockFromMbr(const uint8_t* sector0);

// The same from the raw page at physical block 21 page 0 (512 data bytes, then the spare): it counts as the
// MBR only when its SectorInfo logical sector (spare bytes 0 to 3) is 0, else the fallback.
uint32_t rx1950UserStoreBlockFromPage(const uint8_t* page);

bool rx1950NandImage(const uint8_t* eboot, size_t ebootSize, const uint8_t* nbf, size_t nbfSize,
                     std::vector<uint8_t>& flash, std::string& error, const Rx1950NandOptions& options = {});

// The firmware Frond runs on the rx1950: HP's WM5 ROM (OS 5.1.1702, ROM 1.01.00 ENG) as the CEBL.nbf
// bootloader and CEOS.nbf OS pair. Dan's logical store capture has no bootloader, spare data or reserved
// blocks and holds user data, so it isn't accepted.
struct Rx1950Firmware {
    const char* name;
    size_t bytes;
    const char* sha256;  // lower case hex
};
// The frontends and tools check the hash at import (frondZodiacRom::sha256Hex).
extern const Rx1950Firmware rx1950Cebl, rx1950Ceos;

// The NAND store file: every page, data then spare, as S3cNand holds it (69 206 016 bytes). Load fails on
// a missing or wrong sized file. Save writes the whole flash to a new file, or only the dirty blocks into
// an existing one, then clears the dirty marks.
bool rx1950NandLoad(const std::string& path, std::vector<uint8_t>& flash, std::string& error);
bool rx1950NandSave(const std::string& path, S3cNand& nand, std::string& error);

#endif
