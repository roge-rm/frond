#pragma once

// Window sizes are logical pixels; reserve space for the window frame.
inline float fittingWindowScale(int width, int height, int availableWidth, int availableHeight) {
    for (int quarters = 8; quarters > 4; --quarters) {
        const float scale = quarters / 4.f;
        if (width * scale + 16 <= availableWidth && height * scale + 48 <= availableHeight)
            return scale;
    }
    return 1.f;
}
