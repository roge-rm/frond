#include "s3c2442_blocks.h"

// DMA, IIS and IIC: the blocks the audio driver sets up, as registers only for now.

void S3cDma::reset() {
    for (auto& c : channel) c = {};
}

S3cAccess S3cDma::read(uint32_t offset, uint32_t& value) {
    if (offset >= 0x100 || offset & 3 || (offset & 0x3f) > 0x20) return S3cAccess::unknown;
    const auto& c = channel[offset / 0x40];
    switch (offset & 0x3f) {
        case 0x00: value = c.disrc; break;
        case 0x04: value = c.disrcc; break;
        case 0x08: value = c.didst; break;
        case 0x0c: value = c.didstc; break;
        case 0x10: value = c.dcon; break;
        case 0x14: value = c.dcon & 0xfffff; break;  // DSTAT: ready, the whole count left
        case 0x18: value = c.disrc; break;           // DCSRC
        case 0x1c: value = c.didst; break;           // DCDST
        case 0x20: value = c.dmasktrig; break;
    }
    return S3cAccess::ok;
}

S3cAccess S3cDma::write(uint32_t offset, uint32_t value) {
    if (offset >= 0x100 || offset & 3 || (offset & 0x3f) > 0x20) return S3cAccess::unknown;
    auto& c = channel[offset / 0x40];
    switch (offset & 0x3f) {
        case 0x00: c.disrc = value & 0x7fffffff; break;
        case 0x04: c.disrcc = value & 3; break;
        case 0x08: c.didst = value & 0x7fffffff; break;
        case 0x0c: c.didstc = value & 7; break;
        case 0x10: c.dcon = value; break;
        case 0x14: case 0x18: case 0x1c: break;  // read only
        case 0x20:
            // SW_TRIG on a channel set to software requests would move memory now.
            if ((value & 1) && !(c.dcon & (1u << 23))) return S3cAccess::unsupported;
            c.dmasktrig = value & 6;  // STOP and ON; a stop turns the channel off at once
            if (value & 4) c.dmasktrig = 0;
            break;
    }
    return S3cAccess::ok;
}

void S3cIis::reset() { iiscon = 0x100; iismod = iispsr = iisfcon = 0; }

S3cAccess S3cIis::read(uint32_t offset, uint32_t& value) {
    switch (offset) {
        // IISCON: left channel, FIFOs ready; IISFCON: both FIFOs empty.
        case 0x00: value = iiscon | 0xc0; return S3cAccess::ok;
        case 0x04: value = iismod; return S3cAccess::ok;
        case 0x08: value = iispsr; return S3cAccess::ok;
        case 0x0c: value = iisfcon & ~0xfffu; return S3cAccess::ok;
        case 0x10: value = 0; return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}

S3cAccess S3cIis::write(uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0x00: iiscon = value & 0x3f; return S3cAccess::ok;
        case 0x04: iismod = value & 0x3ff; return S3cAccess::ok;
        case 0x08: iispsr = value & 0x3ff; return S3cAccess::ok;
        case 0x0c: iisfcon = value & 0xf000; return S3cAccess::ok;
        case 0x10: return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}

namespace {
    constexpr uint32_t pending = 1u << 4, busy = 1u << 5, txrx = 1u << 4, nack = 1u << 0;
}

void S3cIic::reset() { iiccon = 0; iicstat = 0; iicadd = 0; iicds = 0; iiclc = 0; }

S3cAccess S3cIic::read(uint32_t offset, uint32_t& value) {
    switch (offset) {
        case 0x00: value = iiccon; return S3cAccess::ok;
        case 0x04: value = iicstat & ~nack; return S3cAccess::ok;
        case 0x08: value = iicadd; return S3cAccess::ok;
        case 0x0c: value = iicds; return S3cAccess::ok;
        case 0x10: value = iiclc; return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}

S3cAccess S3cIic::write(uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0x00: {
            // Clearing the pending bit lets the next byte go, which finishes at once.
            const bool resume = (iiccon & pending) && !(value & pending);
            iiccon = value & 0xff;
            if (resume && (iicstat & busy) && (iicstat & txrx)) {
                ++bytes;
                if (!(iicstat & (1u << 6))) iicds = 0;  // master receive: nothing to read
                iiccon |= pending;
            }
            return S3cAccess::ok;
        }
        case 0x04: {
            const bool start = (value & busy) && (value & txrx);
            iicstat = value & 0xf0;
            if (start) {
                ++bytes;  // the address byte
                iiccon |= pending;
            }
            return S3cAccess::ok;
        }
        case 0x08: iicadd = value & 0xfe; return S3cAccess::ok;
        case 0x0c: iicds = value & 0xff; return S3cAccess::ok;
        case 0x10: iiclc = value & 7; return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}
