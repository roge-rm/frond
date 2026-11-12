#ifndef FROND_DANA_WIRELESS5_H
#define FROND_DANA_WIRELESS5_H

#include <cstddef>
#include <cstdint>

// The Dana Wireless runs the Dana ROM. Its 'KeyWalker Ext' sets feature WTAP when the 'snum' ROM
// token reads "x040..." (a Wireless serial number), so the core makes a Dana dump read that way in
// its own copy of the ROM: digits 1 to 3 become "040", the rest stays. The token store is where the
// card header's word at 0x64 points (the ROM is mapped at 0x10000000); tokens are a four-byte ID, a
// big-endian length and the data, padded to an even length, until an erased ID.
// The offset of the 'snum' token's data, or 0 when the ROM has none of at least four bytes.
inline size_t danaSerialToken5(const uint8_t* rom, size_t size) {
    constexpr uint32_t base = 0x10000000;
    auto be32 = [&](size_t at) {
        return uint32_t(rom[at]) << 24 | uint32_t(rom[at + 1]) << 16 | uint32_t(rom[at + 2]) << 8 | rom[at + 3];
    };
    if (!rom || size < 0x100) return 0;
    const uint32_t store = be32(0x64);
    if (store < base || store - base >= size) return 0;
    size_t at = store - base;
    // A real store is a few dozen bytes; the bound only stops a damaged one running on.
    const size_t end = size - at > 4096 ? at + 4096 : size;
    while (end - at >= 6) {
        const uint32_t id = be32(at);
        if (id == 0xffffffff) return 0;
        const size_t length = size_t(rom[at + 4]) << 8 | rom[at + 5];
        if (length > end - at - 6) return 0;
        if (id == 0x736e756d /* snum */) return length >= 4 && rom[at + 6] != 0xff ? at + 6 : 0;
        at += 6 + length + (length & 1);
    }
    return 0;
}

// Makes the serial number read as a Wireless one; false, leaving the ROM alone, without a token.
inline bool danaWirelessSerial5(uint8_t* rom, size_t size) {
    const size_t at = danaSerialToken5(rom, size);
    if (!at) return false;
    rom[at + 1] = '0'; rom[at + 2] = '4'; rom[at + 3] = '0';
    return true;
}

#endif
