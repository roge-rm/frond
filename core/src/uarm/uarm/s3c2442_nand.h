#ifndef FROND_S3C2442_NAND_H
#define FROND_S3C2442_NAND_H

// The S3C2442's NAND flash controller at 0x4E000000 (manual chapter 6) and a small page NAND chip behind
// it, the K5D1258KCM's 64 MB part: 4096 blocks of 32 pages, each 512 bytes plus 16 spare. Commands: reset,
// read ID, status, the three reads (A, B and C, which also point programs), page program, block erase and
// Samsung's unique ID page, with the main area ECC the controller makes as data passes either way. Program
// and erase are immediate (busy for no time) and never fail; blocks they change are marked dirty for the
// machine's NAND store file.

#include <cstdint>
#include <vector>

#include "s3c2442_blocks.h"

class S3cNand {
   public:
    static constexpr uint32_t pageData = 512, pageSpare = 16, pageSize = pageData + pageSpare;
    static constexpr uint32_t pagesPerBlock = 32, blocks = 4096;
    static constexpr uint32_t pages = pagesPerBlock * blocks;

    // Where the pages are kept when not in flash: the board's NAND file, a page at a time. Erase sets a whole
    // block to ones.
    struct Store {
        virtual ~Store() = default;
        virtual bool readPage(uint32_t page, uint8_t* out) = 0;
        virtual bool writePage(uint32_t page, const uint8_t* in) = 0;
        virtual bool eraseBlock(uint32_t firstPage, uint32_t count) = 0;
    };
    // Pages go to store instead of flash from now on; null goes back to flash. The page the chip last read
    // is kept, so forget it (dropCache) whenever something else changes the store.
    void setStore(Store* pages) {
        store = pages;
        cachedPage = ~0u;
    }
    void dropCache() { cachedPage = ~0u; }
    // A store's read or write failed; the chip then reads ones and programs nothing.
    uint64_t storeFailures = 0;

    // image holds every page, data then spare, pages * pageSize bytes. Empty reads as erased.
    void reset();
    void setImage(std::vector<uint8_t> image) {
        flash = std::move(image);
        dirty.assign(blocks, false);
    }
    // Erases count blocks from first as the chip's erase does (store or flash, marked dirty), for a hard reset
    // done by the host. The controller and chip registers are left alone. False if the store failed.
    bool eraseBlocks(uint32_t first, uint32_t count);
    // Blocks programmed or erased since the image was set or clearDirty.
    bool blockDirty(uint32_t block) const { return block < dirty.size() && dirty[block]; }
    void clearDirty() { dirty.assign(blocks, false); }
    uint64_t programs = 0, erases = 0;
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

    // The controller and chip registers for a saved state; the pages are saved by their owner.
    template <class A> void state(A& a) {
        a.Do32(nfconf).Do32(nfcont).Do32(nfstat).Do32(mecc0).Do32(mecc1).Do32(secc).DoBuffer32(meccd, 2).Do32(seccd);
        a.Do32(sblk).Do32(eblk).Do64(programs).Do64(erases).DoBuffer(uid, sizeof uid).DoEnum(mode, 6);
        a.Do8(command).Do8(area).DoBool(uidPending).DoBool(uidMode);
        uint32_t cycles = addressCycles;
        a.Do32(cycles).Do32(column).Do32(row).Do32(idIndex).Do32(pageOffset).Do32(eccCount).Do8(eccColumn);
        addressCycles = cycles;
        a.DoBuffer32(eccLine, 2).DoBuffer(pageBuffer, sizeof pageBuffer);
        a.Require(pageOffset < pageSize && addressCycles < 8, "nand pointer");
        cachedPage = ~0u;
    }

   private:
    enum class Mode : uint8_t { idle, id, status, read, program, erase };
    uint8_t dataByte();
    void startRead();
    // The byte offset in a page the A, B or C pointer starts at, from the column.
    uint32_t areaOffset() const;
    void program();
    void erase();
    void touch(uint32_t page);
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
    // The page register a program fills, all ones where nothing was written.
    uint8_t pageBuffer[pageSize];
    std::vector<bool> dirty;
    // A page out of the store: the one being read, or programmed.
    const uint8_t* page(uint32_t page);
    Store* store = nullptr;
    uint32_t cachedPage = ~0u;
    uint8_t cache[pageSize];
};

#endif
