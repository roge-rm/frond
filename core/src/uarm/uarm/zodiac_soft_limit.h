#ifndef FROND_ZODIAC_SOFT_LIMIT_H
#define FROND_ZODIAC_SOFT_LIMIT_H

#include <cstdint>

// A level in the 16-bit sample range with the part above 3/4 of full scale bent towards full scale, so a boosted
// volume (Zodiac gain over unity) doesn't clip hard. Integer only; never past +-32767.
inline int32_t zodiacSoftLimit(int64_t v) {
    constexpr int64_t knee = 24576, room = 32767 - knee;
    const int64_t a = v < 0 ? -v : v;
    if (a <= knee) return int32_t(v);
    const int64_t over = a - knee;
    const int64_t out = knee + room * over / (over + room);
    return int32_t(v < 0 ? -out : out);
}

#endif
