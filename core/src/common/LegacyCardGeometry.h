#pragma once
#include <cstdint>

// Fixed 512-byte blocks, CSD v1 C_SIZE and C_SIZE_MULT, shared with the SPI encoder.
inline bool legacyCardLayout(uint64_t blocks, uint32_t& mult, uint32_t& size) {
    if (blocks < 4) return false;
    for (mult = 0; mult < 8; ++mult)
        if ((blocks >> (mult + 2)) < 0x1000) break;
    if (mult > 7) return false;
    size = (blocks >> (mult + 2)) - 1;
    return (uint64_t(1) << (mult + 2)) * (size + 1) == blocks;
}
