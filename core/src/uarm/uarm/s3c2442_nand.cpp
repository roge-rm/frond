#include "s3c2442_nand.h"

#include <cstring>

namespace {
    // K9F1208 class 64 MB small page part inside the K5D1258KCM: Samsung, device 0x76.
    constexpr uint8_t chipId[] = {0xec, 0x76, 0xa5, 0xc0};
}

void S3cNand::reset() {
    nfconf = 0x1000;
    nfcont = 0x0384;
    nfstat = 0x1;
    mecc0 = mecc1 = secc = 0;
    meccd[0] = meccd[1] = seccd = 0;
    sblk = eblk = 0;
    mode = Mode::idle;
    command = area = 0;
    addressCycles = 0;
    column = row = idIndex = pageOffset = 0;
    eccCount = 0;
    eccColumn = 0;
    eccLine[0] = eccLine[1] = 0;
    uidPending = uidMode = false;
}

// ASSUMPTION "Samsung unique ID page": 8 copies of the 16 ID bytes each followed by their complement, then
// erased bytes, with the spare starting 55 aa 55 aa 55 aa 55 aa. That's the layout eboot checks for.
uint8_t S3cNand::uidByte(uint32_t offset) const {
    if (offset < 256) return offset & 16 ? uint8_t(~uid[offset & 15]) : uid[offset & 15];
    if (offset >= pageData && offset < pageData + 8) return offset & 1 ? 0xaa : 0x55;
    return 0xff;
}

static unsigned parity8(uint8_t b) { return __builtin_parity(b); }

// The NFMECC0 value from the line parity bits and the XOR of every byte.
static uint32_t packEcc(uint32_t line, uint8_t columns) {
    const uint32_t cp = parity8(columns & 0x55) | parity8(columns & 0xaa) << 1 | parity8(columns & 0x33) << 2 |
                        parity8(columns & 0xcc) << 3 | parity8(columns & 0x0f) << 4 | parity8(columns & 0xf0) << 5;
    const uint32_t packed = (line & 0xffff) | ((((cp << 2) | (line >> 16)) & 0xff) << 16);
    return (~packed & 0xffffff) | 0xff000000;
}

// ASSUMPTION "S3C2442 ECC layout": the SmartMedia ECC over 512 bytes, 18 line parity bits and 6 column
// parity bits, inverted, packed LP7-LP0, LP15-LP8, then CP5-CP0 over LP17-LP16.
uint32_t S3cNand::ecc512(const uint8_t* data) {
    uint32_t line = 0;
    uint8_t columns = 0;
    for (uint32_t i = 0; i < pageData; ++i) {
        columns ^= data[i];
        if (!parity8(data[i])) continue;
        for (unsigned k = 0; k < 9; ++k) line ^= 1u << (2 * k + ((i >> k) & 1));
    }
    return packEcc(line, columns);
}

void S3cNand::addEccByte(uint8_t byte) {
    // Main area ECC runs over the first 512 bytes after InitECC, unless locked (NFCONT bit 5).
    if (nfcont & 0x20 || eccCount >= pageData) return;
    eccColumn ^= byte;
    if (parity8(byte))
        for (unsigned k = 0; k < 9; ++k) eccLine[0] ^= 1u << (2 * k + ((eccCount >> k) & 1));
    ++eccCount;
    if (eccCount == pageData) mecc0 = packEcc(eccLine[0], eccColumn);
}

uint32_t S3cNand::areaOffset() const {
    // Pointer A starts in the first half, B in the second, C in the spare.
    return area == 0x50 ? pageData + (column & 0xf) : area == 0x01 ? 256 + column : column;
}

void S3cNand::startRead() {
    mode = Mode::read;
    pageOffset = areaOffset();
    nfstat |= 0x4;  // RnB went busy and back, which RnB_TransDetect latches
}

const uint8_t* S3cNand::page(uint32_t page) {
    if (page != cachedPage) {
        if (!store->readPage(page, cache)) {
            ++storeFailures;
            std::memset(cache, 0xff, sizeof cache);
        }
        cachedPage = page;
    }
    return cache;
}

void S3cNand::touch(uint32_t page) {
    // With no store and no image the chip starts erased.
    if (!store && flash.empty()) flash.assign(size_t(pages) * pageSize, 0xff);
    if (dirty.size() != blocks) dirty.assign(blocks, false);
    dirty[page / pagesPerBlock] = true;
}

// Programming can only clear bits, so the page register is ANDed in. The B pointer goes back to A after a
// program; A and C stay.
void S3cNand::program() {
    const uint32_t page = row % pages;
    touch(page);
    if (store) {
        uint8_t bytes[pageSize];
        std::memcpy(bytes, this->page(page), pageSize);
        for (uint32_t i = 0; i < pageSize; ++i) bytes[i] &= pageBuffer[i];
        if (store->writePage(page, bytes)) std::memcpy(cache, bytes, pageSize);
        else {
            ++storeFailures;
            cachedPage = ~0u;
        }
    } else {
        uint8_t* p = &flash[size_t(page) * pageSize];
        for (uint32_t i = 0; i < pageSize; ++i) p[i] &= pageBuffer[i];
    }
    ++programs;
    if (area == 0x01) area = 0x00;
    mode = Mode::idle;
    nfstat |= 0x4;
}

void S3cNand::erase() {
    const uint32_t block = (row % pages) / pagesPerBlock;
    touch(block * pagesPerBlock);
    if (store) {
        if (!store->eraseBlock(block * pagesPerBlock, pagesPerBlock)) ++storeFailures;
        cachedPage = ~0u;
    } else {
        std::memset(&flash[size_t(block) * pagesPerBlock * pageSize], 0xff, size_t(pagesPerBlock) * pageSize);
    }
    ++erases;
    mode = Mode::idle;
    nfstat |= 0x4;
}

bool S3cNand::eraseBlocks(uint32_t first, uint32_t count) {
    bool ok = true;
    for (uint32_t block = first; block < blocks && block - first < count; ++block) {
        touch(block * pagesPerBlock);
        if (store) {
            if (!store->eraseBlock(block * pagesPerBlock, pagesPerBlock)) { ++storeFailures; ok = false; }
        } else {
            std::memset(&flash[size_t(block) * pagesPerBlock * pageSize], 0xff, size_t(pagesPerBlock) * pageSize);
        }
    }
    cachedPage = ~0u;
    return ok;
}

uint8_t S3cNand::dataByte() {
    switch (mode) {
        case Mode::id: return idIndex < sizeof chipId ? chipId[idIndex++] : 0;
        case Mode::status: return 0xc0;  // ready, not write protected
        case Mode::read: {
            if (uidMode) {
                const uint8_t byte = uidByte(pageOffset);
                if (++pageOffset >= pageSize) pageOffset = 0;
                return byte;
            }
            // The chip ignores row bits above its size, for reads, program and erase alike.
            const size_t at = size_t(row % pages) * pageSize + pageOffset;
            const uint8_t byte = store ? page(row % pages)[pageOffset] : at < flash.size() ? flash[at] : 0xff;
            if (pageOffset < pageData) addEccByte(byte);
            // Sequential reads run on into the next page.
            if (++pageOffset >= pageSize) {
                pageOffset = area == 0x50 ? pageData : 0;
                ++row;
            }
            return byte;
        }
        default: return 0xff;
    }
}

S3cAccess S3cNand::read(uint32_t offset, unsigned size, uint32_t& value) {
    switch (offset) {
        case 0x00: value = nfconf; return S3cAccess::ok;
        case 0x04: value = nfcont; return S3cAccess::ok;
        case 0x08: value = command; return S3cAccess::ok;
        case 0x10:
            if (nfcont & 2) { refusal = "data read with the chip not selected"; return S3cAccess::unsupported; }
            value = 0;
            for (unsigned i = 0; i < size; ++i) value |= uint32_t(dataByte()) << (8 * i);
            return S3cAccess::ok;
        case 0x14: value = meccd[0]; return S3cAccess::ok;
        case 0x18: value = meccd[1]; return S3cAccess::ok;
        case 0x1c: value = seccd; return S3cAccess::ok;
        case 0x20: value = nfstat; return S3cAccess::ok;
        case 0x24: {
            // Main area check against the ECC software wrote into NFMECCD0/1: 0 no error, 2 more than one bit.
            const uint32_t stored = (meccd[0] & 0xff) | ((meccd[0] >> 8) & 0xff00) | ((meccd[1] & 0xff) << 16);
            value = (stored & 0xffffff) == (mecc0 & 0xffffff) ? 0 : 2;
            return S3cAccess::ok;
        }
        case 0x28: value = 0; return S3cAccess::ok;
        case 0x2c: value = mecc0; return S3cAccess::ok;
        case 0x30: value = mecc1; return S3cAccess::ok;
        case 0x34: value = secc; return S3cAccess::ok;
        case 0x38: value = sblk; return S3cAccess::ok;
        case 0x3c: value = eblk; return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}

S3cAccess S3cNand::write(uint32_t offset, unsigned size, uint32_t value, S3cIntc&) {
    switch (offset) {
        case 0x00: nfconf = value & 0x777f; return S3cAccess::ok;
        case 0x04:
            if (value & 0x10) {
                eccCount = 0;
                eccColumn = 0;
                eccLine[0] = eccLine[1] = 0;
                mecc0 = 0xffffffff;
            }
            nfcont = value & 0x37e7;
            return S3cAccess::ok;
        case 0x08:
            command = uint8_t(value);
            if (command != 0x65) uidPending = false;
            switch (command) {
                case 0xff: mode = Mode::idle; area = 0; uidMode = false; nfstat |= 0x4; break;
                case 0x30: uidPending = true; break;
                case 0x65:
                    if (!uidPending) {
                        refusal = "NAND command 65h without 30h before it";
                        return S3cAccess::unsupported;
                    }
                    uidPending = false;
                    uidMode = true;
                    break;
                case 0x90: mode = Mode::id; idIndex = 0; addressCycles = 0; break;
                case 0x70: mode = Mode::status; break;
                case 0x00: case 0x01: case 0x50: area = command; addressCycles = 0; column = row = 0; mode = Mode::idle; break;
                case 0x80:
                    mode = Mode::program;
                    addressCycles = 0;
                    column = row = 0;
                    std::memset(pageBuffer, 0xff, sizeof pageBuffer);
                    break;
                case 0x10:
                    if (mode != Mode::program || addressCycles < 4) {
                        refusal = "NAND program confirm (10h) without 80h and a full address";
                        return S3cAccess::unsupported;
                    }
                    program();
                    break;
                case 0x60: mode = Mode::erase; addressCycles = 0; row = 0; break;
                case 0xd0:
                    if (mode != Mode::erase || addressCycles < 3) {
                        refusal = "NAND erase confirm (D0h) without 60h and a full row address";
                        return S3cAccess::unsupported;
                    }
                    erase();
                    break;
                default:
                    refusal = "NAND command the K9F1208 part doesn't have or Frond doesn't model";
                    return S3cAccess::unsupported;
            }
            return S3cAccess::ok;
        case 0x0c:
            if (mode == Mode::id) return S3cAccess::ok;  // read ID takes one address byte, 0
            // Erase takes the three row bytes only.
            if (mode == Mode::erase) {
                if (addressCycles < 3) row |= (value & 0xff) << (8 * addressCycles);
                ++addressCycles;
                return S3cAccess::ok;
            }
            // Small page: one column byte, then three row bytes.
            if (addressCycles == 0) column = value & 0xff;
            else if (addressCycles <= 3) row |= (value & 0xff) << (8 * (addressCycles - 1));
            ++addressCycles;
            if (addressCycles == 4) {
                if (mode == Mode::program) pageOffset = areaOffset();
                else startRead();
            }
            return S3cAccess::ok;
        case 0x10:
            if (nfcont & 2) { refusal = "data write with the chip not selected"; return S3cAccess::unsupported; }
            if (mode != Mode::program || addressCycles < 4) {
                refusal = "NAND data write outside a page program";
                return S3cAccess::unsupported;
            }
            for (unsigned i = 0; i < size; ++i) {
                const uint8_t byte = uint8_t(value >> (8 * i));
                if (pageOffset < pageData) addEccByte(byte);
                if (pageOffset < pageSize) pageBuffer[pageOffset++] = byte;
            }
            return S3cAccess::ok;
        case 0x14: meccd[0] = value; return S3cAccess::ok;
        case 0x18: meccd[1] = value; return S3cAccess::ok;
        case 0x1c: seccd = value; return S3cAccess::ok;
        case 0x20: nfstat &= ~(value & 0xc); return S3cAccess::ok;
        case 0x38: sblk = value & 0xffffff; return S3cAccess::ok;
        case 0x3c: eblk = value & 0xffffff; return S3cAccess::ok;
        case 0x24: case 0x28: case 0x2c: case 0x30: case 0x34: return S3cAccess::ok;  // read only
    }
    return S3cAccess::unknown;
}
