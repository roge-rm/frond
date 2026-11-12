#ifndef FROND_FRAME_BOUNDS_H
#define FROND_FRAME_BOUNDS_H

#include <cstddef>
#include <cstdint>

// Check the whole destination, row pixels and guest range before dirty-line arithmetic.
inline bool FrameBoundsValid(uint32_t width, uint32_t lines, uint32_t rowBytes,
                             uint32_t margin, uint32_t bpp, size_t capacity, uint32_t base) {
    if (!width || !lines || !rowBytes || (bpp != 1 && bpp != 2 && bpp != 4)) return false;
    const uint64_t rowBits = (uint64_t(width) + margin) * bpp;
    const uint64_t bytes = uint64_t(rowBytes) * lines;
    return rowBits <= uint64_t(rowBytes) * 8 && bytes <= capacity &&
           uint64_t(base) + bytes <= (uint64_t(1) << 32);
}

inline bool FrameDirtyLinesValid(uint32_t lines, uint32_t first, uint32_t last) {
    return first <= last && last < lines;
}

#endif
