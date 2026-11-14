#ifndef FROND_S3C2442_NAND_H
#define FROND_S3C2442_NAND_H

// The S3C2442's NAND flash controller at 0x4E000000 (manual chapter 6) and a small page NAND chip behind
// it, the K5D1258KCM's 64 MB part: 4096 blocks of 32 pages, each 512 bytes plus 16 spare. R1 reads only:
// reset, read ID, status, the three read commands and Samsung's unique ID page, with the main area ECC the
// controller makes as data passes. Program and erase answer unsupported.

#include <cstdint>
#include <vector>

#include "s3c2442_blocks.h"

class S3cNand {
   public:
    static constexpr uint32_t pageData = 512, pageSpare = 16, pageSize = pageData + pageSpare;
    static constexpr uint32_t pagesPerBlock = 32, blocks = 4096;
    static constexpr uint32_t pages = pagesPerBlock * blocks;

    // image holds every page, data then spare, pages * pageSize bytes. Empty reads as erased.
    void reset();
    void setImage(std::vector<uint8_t> image) { flash = std::move(image); }
    S3cAccess read(uint32_t offset, unsigned size, uint32_t& value);
    S3cAccess write(uint32_t offset, unsigned size, uint32_t value, S3cIntc& intc);
    // Why the last unsupported answer was given.
    const char* refusal = "";
    // The Samsung SmartMedia style ECC of 512 bytes, as NFMECC0 gives it: bytes 0 to 2 the line parity
    // halves and the column parity, both inverted, byte 3 0xff.
    static uint32_t ecc512(const uint8_t* data);

    // The chip's unique ID, which the 30h 65h command pair reads; eboot makes the device's UUID from it.
    // It starts with Samsung's maker code, which eboot checks for.
    uint8_t uid[16] = {0xec, 0x00, 0x46, 0x52, 0x4f, 0x4e, 0x44, 0x52, 0x58, 0x31, 0x39, 0x35, 0x30, 0x00, 0x00, 0x01};

    uint32_t nfconf, nfcont, nfstat, mecc0, mecc1, secc, meccd[2], seccd, sblk, eblk;
    std::vector<uint8_t> flash;

   private:
    enum class Mode : uint8_t { idle, id, status, read };
    uint8_t dataByte();
    void startRead();
    void addEccByte(uint8_t byte);
    uint8_t uidByte(uint32_t offset) const;

    Mode mode = Mode::idle;
    uint8_t command = 0, area = 0;
    // 30h then 65h: reads give the unique ID page until a reset.
    bool uidPending = false, uidMode = false;
    unsigned addressCycles = 0;
    uint32_t column = 0, row = 0, idIndex = 0;
    uint32_t pageOffset = 0;  // the next byte in the current page, data then spare
    // The main area ECC being made, over 512 bytes from InitECC.
    uint32_t eccCount = 0;
    uint8_t eccColumn = 0;
    uint32_t eccLine[2] = {0, 0};
};

#endif
