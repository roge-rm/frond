#include <cstring>

#include "camera_input.h"

const uint8_t cameraTestPatternBars[8][3] = {
    {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
    {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0},
};

// 3x5 digits, one row a byte, bit 2 the leftmost column.
static const uint8_t cameraPrvDigits[10][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
};

void cameraTestPattern(uint32_t width, uint32_t height, uint64_t frame, uint8_t *rgb) {
    if (!width || !height) return;
    const uint32_t barsEnd = height * 7 / 10, bandEnd = height * 8 / 10;
    const uint32_t movingStart = uint32_t(uint64_t(frame % 64) * width / 64);
    const uint32_t movingEnd = movingStart + (width + 15) / 16;
    // Six digits, each 3x5 cells on a 4x7 pitch, as high as the bottom strip allows.
    const uint32_t strip = height - bandEnd, cell = strip / 7 ? strip / 7 : 1;
    char digits[6];
    uint64_t n = frame;
    for (int i = 5; i >= 0; --i) { digits[i] = char(n % 10); n /= 10; }
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = rgb + size_t(y) * width * 3;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t *c;
            static const uint8_t grey[3] = {64, 64, 64}, white[3] = {255, 255, 255}, black[3] = {0, 0, 0};
            if (y < barsEnd) {
                c = cameraTestPatternBars[uint64_t(x) * 8 / width];
            } else if (y < bandEnd) {
                c = x >= movingStart && x < movingEnd ? white : grey;
            } else {
                c = black;
                const uint32_t cy = (y - bandEnd) / cell, cx = x / cell;
                if (cy >= 1 && cy <= 5 && cx >= 1 && cx < 1 + 6 * 4) {
                    const uint32_t digit = (cx - 1) / 4, column = (cx - 1) % 4;
                    if (column < 3 && (cameraPrvDigits[int(digits[digit])][cy - 1] >> (2 - column) & 1)) c = white;
                }
            }
            memcpy(row + x * 3, c, 3);
        }
    }
}
