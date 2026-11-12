#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

// How a resizable window holds the device. none keeps the fixed window scale in the
// top left; fit keeps the device's shape at the largest size that fits; whole uses the
// largest whole multiple that fits; fill stretches the device to the whole window.
enum class WindowFit { none, fit, whole, fill };

// Where the device sits in the window, in window coordinates.
struct WindowLayout {
    float scaleX{1}, scaleY{1};
    int x{0}, y{0}, width{0}, height{0};
};

inline bool parseWindowFit(const std::string& value, WindowFit& fit) {
    if (value == "fit") fit = WindowFit::fit;
    else if (value == "whole") fit = WindowFit::whole;
    else if (value == "fill") fit = WindowFit::fill;
    else return false;
    return true;
}

// Parses RRGGBB into 0xRRGGBB.
inline bool parseWindowColour(const std::string& value, uint32_t& colour) {
    if (value.size() != 6 || value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        return false;
    colour = uint32_t(std::stoul(value, nullptr, 16));
    return true;
}

// Centres the device of deviceWidth x deviceHeight in a window of windowWidth x windowHeight.
inline WindowLayout fitWindowLayout(WindowFit fit, int deviceWidth, int deviceHeight, int windowWidth,
                                    int windowHeight) {
    WindowLayout layout;
    if (fit == WindowFit::none || deviceWidth <= 0 || deviceHeight <= 0 || windowWidth <= 0 ||
        windowHeight <= 0) {
        layout.width = deviceWidth;
        layout.height = deviceHeight;
        return layout;
    }
    const float scaleX = float(windowWidth) / deviceWidth, scaleY = float(windowHeight) / deviceHeight;
    if (fit == WindowFit::fill) {
        layout.scaleX = scaleX;
        layout.scaleY = scaleY;
    } else {
        float scale = std::min(scaleX, scaleY);
        // A window smaller than the device falls back to fitting.
        if (fit == WindowFit::whole && scale >= 1) scale = std::floor(scale);
        layout.scaleX = layout.scaleY = scale;
    }
    layout.width = std::min(windowWidth, int(std::lround(deviceWidth * layout.scaleX)));
    layout.height = std::min(windowHeight, int(std::lround(deviceHeight * layout.scaleY)));
    layout.x = (windowWidth - layout.width) / 2;
    layout.y = (windowHeight - layout.height) / 2;
    return layout;
}
