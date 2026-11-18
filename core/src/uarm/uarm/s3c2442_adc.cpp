#include "s3c2442_blocks.h"

namespace {
    constexpr uint32_t ecflg = 1u << 15, enableStart = 1u << 0, readStart = 1u << 1;
    constexpr uint32_t stylusUp = 1u << 15;
}

void S3cAdc::reset() {
    adccon = 0x3fc4;  // prescaler 0xff, standby
    adctsc = 0x58;
    adcdly = 0xff;
    adcdat0 = adcdat1 = adcupdn = 0;
    for (auto& c : channel) c = 0x200;
}

S3cAccess S3cAdc::read(uint32_t offset, uint32_t& value) {
    switch (offset) {
        case 0x00: value = adccon; return S3cAccess::ok;
        case 0x04: value = adctsc; return S3cAccess::ok;
        case 0x08: value = adcdly; return S3cAccess::ok;
        case 0x0c:
        case 0x10: {
            value = (offset == 0x0c ? adcdat0 : adcdat1) | stylusUp | (adctsc & 3) << 12;
            // READ_START: reading the data starts the next conversion.
            if (adccon & readStart) {
                ++conversions;
                adccon |= ecflg;
            }
            return S3cAccess::ok;
        }
        case 0x14: value = adcupdn; return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}

S3cAccess S3cAdc::write(uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0x00: {
            adccon = (adccon & ecflg) | (value & 0x7fff);
            if (value & (enableStart | readStart)) {
                const unsigned mux = (value >> 3) & 7;
                adcdat0 = adcdat1 = channel[mux] & 0x3ff;
                channelsRead |= 1u << mux;
                ++conversions;
                adccon = (adccon & ~enableStart) | ecflg;
            } else {
                adccon &= ~ecflg;
            }
            return S3cAccess::ok;
        }
        case 0x04: adctsc = value & 0x1ff; return S3cAccess::ok;
        case 0x08: adcdly = value & 0xffff; return S3cAccess::ok;
        case 0x14: adcupdn &= ~value; return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}
