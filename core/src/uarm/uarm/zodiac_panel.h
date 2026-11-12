#ifndef FROND_ZODIAC_PANEL_H
#define FROND_ZODIAC_PANEL_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "imx1_asp.h"

// The Tapwave Zodiac 2 as the normal runtime shows it: the 320x480 panel a quarter turn clockwise,
// the pen as the fixed virtual touch panel's samples, the stick and the EEPROM identity.

inline constexpr uint32_t zodiacPanelWidth = 320, zodiacPanelHeight = 480;
inline constexpr uint32_t zodiacViewWidth = 480, zodiacViewHeight = 320;

// Panel pixel (sx, sy) goes to view pixel (479 - sy, sx). The frontend makes the same turn on the GPU.
inline void zodiacRotateFrame(const uint32_t* panel, uint32_t* view) {
    for (uint32_t sy = 0; sy < zodiacPanelHeight; ++sy) {
        const uint32_t* row = panel + size_t(sy) * zodiacPanelWidth;
        uint32_t* column = view + (zodiacViewWidth - 1 - sy);
        for (uint32_t sx = 0; sx < zodiacPanelWidth; ++sx) column[size_t(sx) * zodiacViewWidth] = row[sx];
    }
}

// Raw 9 bit touch values for a view position. This is the virtual panel the ROM's own calibration
// was done on: raw X falls with view y and raw Y rises with view x.
inline void zodiacPenRaw(int x, int y, uint32_t& rawX, uint32_t& rawY) {
    x = std::clamp(x, 0, int(zodiacViewWidth));
    y = std::clamp(y, 0, int(zodiacViewHeight));
    rawX = uint32_t(std::lround(36.0 + 330.0 * (1.0 - double(y) / zodiacViewHeight)));
    rawY = uint32_t(std::lround(32.0 + 350.0 * double(x) / zodiacViewWidth));
}

// ASP samples for the pen at a view position, down or just lifted from there. Z, U and the
// reference words are the fixed values of the virtual panel.
inline Imx1AspPenInputs zodiacPenInputs(bool down, int x, int y) {
    uint32_t rawX = 0, rawY = 0;
    zodiacPenRaw(x, y, rawX, rawY);
    Imx1AspPenInputs pen{};
    pen.contactKnown = pen.normalKnown = pen.referenceKnown = true;
    pen.contactDown = down;
    pen.normal = {0x1100, uint16_t(0x1100 + (rawX << 7)), uint16_t(0x1100 + (rawY << 7)), 0x1234};
    pen.reference = {0x0180, 0x3280, 0x7100};
    return pen;
}

// A host axis (-1 to 1) as the stick's offset from rest, -511 to 511.
inline int zodiacStickOffset(float axis) {
    if (!(axis == axis)) return 0;
    return int(std::lround(std::clamp(axis, -1.0f, 1.0f) * 511.0f));
}

// The 256 byte identity EEPROM the ROM expects, worked out from the ROM itself: the DAL's packed
// globals (resource amdd, found through the big card's boot header) hold the 128 byte key at
// 1c0, and the EEPROM carries it at 50..cf. Everything else reads ff. Only the layout this was
// established on is taken (globals of 9a24 bytes that start with literal blocks); false otherwise.
inline bool zodiacEepromIdentity(const uint8_t* rom, size_t size, std::array<uint8_t, 256>& image) {
    constexpr size_t card = 0x20000, keyAt = 0x1c0, keyBytes = 128;
    auto le32 = [&](size_t at) {
        return uint32_t(rom[at]) | uint32_t(rom[at + 1]) << 8 | uint32_t(rom[at + 2]) << 16 | uint32_t(rom[at + 3]) << 24;
    };
    auto be32 = [&](size_t at) {
        return uint32_t(rom[at]) << 24 | uint32_t(rom[at + 1]) << 16 | uint32_t(rom[at + 2]) << 8 | uint32_t(rom[at + 3]);
    };
    if (!rom || size < card + 0xb0) return false;
    const uint64_t packed = card + uint64_t(le32(card + 0x78)), metadata = card + uint64_t(le32(card + 0xac));
    if (packed + 8 > size || metadata + 8 > size) return false;
    const uint32_t globals = be32(size_t(metadata) + 4);
    if (globals != 0x9a24 || be32(size_t(packed) + 4) != uint32_t(0) - globals) return false;
    std::array<uint8_t, keyAt + keyBytes> data{};
    size_t have = 0, at = size_t(packed) + 8;
    while (have < data.size()) {
        if (at >= size || !(rom[at] & 0x80)) return false;
        const size_t length = (rom[at] & 0x7f) + 1;
        if (length > size - at - 1) return false;
        const size_t take = std::min(length, data.size() - have);
        std::copy(rom + at + 1, rom + at + 1 + take, data.begin() + have);
        have += take;
        at += 1 + length;
    }
    image.fill(0xff);
    std::copy(data.begin() + keyAt, data.end(), image.begin() + 0x50);
    return true;
}

#endif
