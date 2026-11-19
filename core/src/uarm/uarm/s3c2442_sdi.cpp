#include <algorithm>
#include <cstring>

#include "s3c2442_blocks.h"

namespace {
    enum { sdicon = 0, sdipre, sdicarg, sdiccon, sdicsta, sdirsp0, sdirsp1, sdirsp2, sdirsp3, sdidtimer, sdibsize,
           sdidcon, sdidcnt, sdidsta, sdifsta, sdiimsk, sdidat };
    // SDICON
    constexpr uint32_t conByteOrder = 1u << 4, conReset = 1u << 8;
    // SDICCON
    constexpr uint32_t cmdStart = 1u << 8, cmdWaitRsp = 1u << 9;
    // SDICSTA
    constexpr uint32_t cstaRspFin = 1u << 9, cstaTimeout = 1u << 10, cstaSent = 1u << 11, cstaCrc = 1u << 12;
    // SDIDCON
    constexpr uint32_t dconBusyAfterCmd = 1u << 18, dconRxAfterCmd = 1u << 19, dconTxAfterRsp = 1u << 20;
    // A change in these (re)starts a transfer: BACMD, RACMD, TARSP, DTST, DatMode and BlkNum.
    constexpr uint32_t dconStartBits = 0x1c7fff;
    // SDIDSTA
    constexpr uint32_t dstaRxOn = 1u << 0, dstaTxOn = 1u << 1, dstaBusyFin = 1u << 3, dstaFin = 1u << 4,
                       dstaTimeout = 1u << 5, dstaCrcSta = 1u << 7;
    // SDIFSTA
    constexpr uint32_t fstaRfHalf = 1u << 7, fstaRfFull = 1u << 8, fstaRfLast = 1u << 9, fstaTfEmpty = 1u << 10,
                       fstaTfHalf = 1u << 11, fstaRfDet = 1u << 12, fstaTfDet = 1u << 13;
    constexpr uint32_t fifoBytes = 64;
}

void S3cSdi::reset() {
    for (auto& r : regs) r = 0;
    regs[sdipre] = 1;
    regs[sdidtimer] = 0x10000;
    clearRun();
    std::memset(block, 0, sizeof block);
}

void S3cSdi::cardRemoved() {
    if (!active && !armed) return;
    // The card stopped answering: the data never comes or goes.
    active = armed = loaded = false;
    regs[sdidsta] = (regs[sdidsta] & ~(dstaRxOn | dstaTxOn)) | dstaTimeout;
}

bool S3cSdi::receiving() const { return ((regs[sdidcon] >> 12) & 3) == 2; }

uint32_t S3cSdi::available() const {
    if (!active) return 0;
    if (receiving()) return loaded ? std::min(fifoBytes, blockSize - position) : 0;
    return std::min(fifoBytes, blockSize - position);
}

// The FIFO flags show only during a transfer.
uint32_t S3cSdi::fifoStatus() const {
    const uint32_t unit = 1u << std::min(2u, (regs[sdidcon] >> 22) & 3);
    if (!active) return 0;
    const uint32_t n = available();
    if (receiving()) {
        uint32_t v = n;
        if (n >= fifoBytes / 2) v |= fstaRfHalf;
        if (n == fifoBytes) v |= fstaRfFull;
        if (n && blocksLeft == 1 && n == blockSize - position) v |= fstaRfLast;
        if (n >= unit) v |= fstaRfDet;
        return v;
    }
    // The transmit FIFO drains to the card at once, so it's empty with room for what's left of the block.
    return fstaTfEmpty | fstaTfHalf | (n >= unit ? fstaTfDet : 0);
}

bool S3cSdi::irq() const {
    const uint32_t mask = regs[sdiimsk], c = regs[sdicsta], d = regs[sdidsta], f = fifoStatus();
    const struct { unsigned bit; uint32_t reg, status; } sources[] = {
        {17, c, cstaCrc}, {16, c, cstaSent}, {15, c, cstaTimeout}, {14, c, cstaRspFin}, {13, d, 1u << 10},
        {12, d, 1u << 9}, {10, d, dstaCrcSta}, {9, d, 1u << 6}, {8, d, dstaTimeout}, {7, d, dstaFin},
        {6, d, dstaBusyFin}, {4, f, fstaTfHalf}, {3, f, fstaTfEmpty}, {2, f, fstaRfLast}, {1, f, fstaRfFull},
        {0, f, fstaRfHalf},
    };
    for (const auto& s : sources)
        if ((mask & (1u << s.bit)) && (s.reg & s.status)) return true;
    return false;
}

bool S3cSdi::dmaWants(unsigned bytes) const {
    return active && (regs[sdidcon] & (1u << 15)) && bytes && available() >= bytes;
}

void S3cSdi::finish() {
    active = armed = loaded = false;
    position = 0;
    regs[sdidsta] = (regs[sdidsta] & ~(dstaRxOn | dstaTxOn)) | dstaFin;
}

void S3cSdi::loadBlock() {
    position = 0;
    loaded = false;
    if (card && card->readBlock(block, blockSize)) {
        loaded = true;
        ++blocksRead;
        return;
    }
    ++cardErrors;
    active = false;
    regs[sdidsta] = (regs[sdidsta] & ~dstaRxOn) | dstaTimeout;
}

void S3cSdi::start() {
    armed = false;
    const uint32_t dcon = regs[sdidcon], mode = (dcon >> 12) & 3;
    blocksLeft = dcon & 0xfff;
    blockSize = regs[sdibsize] & 0xfff;
    position = 0;
    loaded = false;
    if (mode == 1) {
        // Busy check: the card is never busy. ASSUMPTION "SDI busy finish sets DatFin": the driver waits for
        // DatFin after CMD12 and CMD38 and then clears BusyFin.
        regs[sdidsta] |= dstaBusyFin | dstaFin;
        return;
    }
    if (!blocksLeft || !blockSize) {
        regs[sdidsta] |= dstaFin;
        return;
    }
    active = true;
    if (mode == 2) {
        regs[sdidsta] |= dstaRxOn;
        loadBlock();
    } else {
        regs[sdidsta] |= dstaTxOn;
    }
}

uint32_t S3cSdi::pop(unsigned bytes) {
    uint32_t value = 0;
    const bool typeB = regs[sdicon] & conByteOrder;
    for (unsigned i = 0; i < bytes; ++i) {
        if (!active || !receiving() || !loaded) break;
        const uint32_t b = block[position++];
        // Type A puts the first byte in bits 7:0, type B in the access's top byte.
        value |= typeB ? b << (8 * (bytes - 1 - i)) : b << (8 * i);
        if (position < blockSize) continue;
        if (--blocksLeft) loadBlock();
        else finish();
    }
    return value;
}

void S3cSdi::push(unsigned bytes, uint32_t value) {
    const bool typeB = regs[sdicon] & conByteOrder;
    for (unsigned i = 0; i < bytes; ++i) {
        if (!active || receiving()) break;
        block[position++] = uint8_t(typeB ? value >> (8 * (bytes - 1 - i)) : value >> (8 * i));
        if (position < blockSize) continue;
        position = 0;
        if (card && card->writeBlock(block, blockSize)) {
            ++blocksWritten;
        } else {
            ++cardErrors;
            active = false;
            regs[sdidsta] = (regs[sdidsta] & ~dstaTxOn) | dstaCrcSta;
            break;
        }
        if (!--blocksLeft) finish();
    }
}

uint32_t S3cSdi::fifoRead(unsigned bytes) { return pop(bytes); }

void S3cSdi::fifoWrite(unsigned bytes, uint32_t value) { push(bytes, value); }

void S3cSdi::command(uint32_t value) {
    regs[sdiccon] = value;
    if (!(value & cmdStart)) return;
    ++commands;
    regs[sdiccon] &= ~cmdStart;
    uint8_t reply[17] = {};
    bool busy = false;
    const unsigned bits = card ? card->command(uint8_t(value & 0x3f), regs[sdicarg], reply, busy) : 0;
    regs[sdicsta] = (regs[sdicsta] & ~0x1ffu) | cstaSent;
    const bool wantsResponse = value & cmdWaitRsp;
    if (wantsResponse) {
        if (!bits) {
            regs[sdicsta] |= cstaTimeout;
        } else {
            auto word = [&](unsigned at) {
                return uint32_t(reply[at]) << 24 | uint32_t(reply[at + 1]) << 16 | uint32_t(reply[at + 2]) << 8 | reply[at + 3];
            };
            if (bits == 136) {
                // RSP0 to RSP3 hold bits 127:0, the CRC and end bit in RSP3's low byte.
                for (unsigned i = 0; i < 4; ++i) regs[sdirsp0 + i] = word(1 + 4 * i);
            } else {
                // RSP0 holds bits 39:8 (the card status or OCR), RSP1's top byte the CRC and end bit.
                regs[sdirsp0] = word(1);
                regs[sdirsp1] = uint32_t(reply[5]) << 24;
                regs[sdirsp2] = regs[sdirsp3] = 0;
            }
            // RspIndex: the response's command index, 0x3f for a long one.
            regs[sdicsta] |= cstaRspFin | (bits == 136 ? 0x3fu : reply[0] & 0x3fu);
        }
    }
    // A transfer waiting on the command starts now; it needs the response first when it waits for one.
    if (armed && (!wantsResponse || bits)) start();
}

S3cAccess S3cSdi::read(uint32_t offset, unsigned size, uint32_t& value) {
    if (offset >= 0x40 && offset < 0x44) {
        value = pop(size);
        return S3cAccess::ok;
    }
    if (offset & 3 || offset > 0x40) return S3cAccess::unknown;
    switch (offset / 4) {
        case sdidcnt: value = ((active ? blockSize - position : 0) << 12) | (active ? blocksLeft : 0); break;
        case sdifsta: value = fifoStatus(); break;
        default: value = regs[offset / 4]; break;
    }
    return S3cAccess::ok;
}

S3cAccess S3cSdi::write(uint32_t offset, uint32_t value, unsigned size) {
    if (offset >= 0x40 && offset < 0x44) {
        push(size, value);
        return S3cAccess::ok;
    }
    if (offset & 3 || offset > 0x40) return S3cAccess::unknown;
    if (size != 4) return S3cAccess::unsupported;
    const unsigned r = offset / 4;
    switch (r) {
        case sdicon:
            if (value & conReset) {
                // SDRESET clears the command and data state and then itself.
                for (unsigned i = sdiccon; i <= sdidsta; ++i) regs[i] = 0;
                regs[sdidtimer] = 0x10000;
                active = armed = loaded = false;
                value &= ~conReset;
            }
            regs[r] = value;
            break;
        case sdicsta:
            regs[r] &= ~(value & 0x1e00);  // status bits clear by writing one
            break;
        case sdidsta:
            regs[r] &= ~(value & 0x7fc);
            break;
        case sdirsp0: case sdirsp1: case sdirsp2: case sdirsp3: case sdidcnt:
            break;  // read only
        case sdifsta:
            break;  // FRST and the FIFO fail bits: the FIFO never holds data between accesses
        case sdiccon:
            command(value);
            break;
        case sdidcon: {
            const uint32_t old = regs[r];
            regs[r] = value;
            // ASSUMPTION "SDI restart on change": a write that leaves the start bits as they were (the driver
            // clearing DMAEn after a transfer) doesn't start another transfer.
            if (!((old ^ value) & dconStartBits)) break;
            active = armed = loaded = false;
            regs[sdidsta] &= ~(dstaRxOn | dstaTxOn);
            const uint32_t mode = (value >> 12) & 3;
            if (!mode) break;
            if ((mode == 1 && (value & dconBusyAfterCmd)) || (mode == 2 && (value & dconRxAfterCmd)) ||
                (mode == 3 && (value & dconTxAfterRsp)))
                armed = true;
            else
                start();
            break;
        }
        default:
            regs[r] = value;
            break;
    }
    return S3cAccess::ok;
}
