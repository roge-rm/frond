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

// The window size in the units the layout is made in, and output pixels per unit on each axis.
struct WindowMetrics {
    int width{0}, height{0};
    float pixelsX{1}, pixelsY{1};
};

// SDL's window size and its renderer's output size can disagree when a window is resized from outside
// (embedded in another toolkit's window, or a DPI scale applied to one of them). When the two agree on one
// ratio, that ratio is used for both axes. When they don't, the output size is the one that is drawn on, so
// it becomes the window size and one unit is one pixel. That keeps the device's scale equal on both axes.
inline WindowMetrics consistentWindowMetrics(int windowWidth, int windowHeight, int outputWidth, int outputHeight) {
    WindowMetrics m;
    if (outputWidth <= 0 || outputHeight <= 0) {
        m.width = windowWidth;
        m.height = windowHeight;
        return m;
    }
    if (windowWidth > 0 && windowHeight > 0) {
        const float rx = float(outputWidth) / windowWidth, ry = float(outputHeight) / windowHeight;
        // Rounding of a fractional DPI scale moves a size by under a pixel on each axis.
        const float slackX = (0.5f * rx + 1.f) / outputWidth, slackY = (0.5f * ry + 1.f) / outputHeight;
        if (std::fabs(rx - ry) <= std::max(rx, ry) * std::max(slackX, slackY) + 1e-4f) {
            m.width = windowWidth;
            m.height = windowHeight;
            m.pixelsX = m.pixelsY = std::min(rx, ry);
            return m;
        }
    }
    m.width = outputWidth;
    m.height = outputHeight;
    return m;
}
