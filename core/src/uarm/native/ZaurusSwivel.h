#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

struct ZaurusViewState {
    bool folded;
    bool portrait;

    bool operator==(const ZaurusViewState& other) const {
        return folded == other.folded && portrait == other.portrait;
    }
};

class ZaurusSwivel {
   public:
    ZaurusSwivel(bool folded, unsigned width, unsigned height)
        : width(width), height(height), target(folded), shown{folded, folded} {}

    void Observe(bool folded, uint64_t now) {
        if (folded == target) return;
        target = folded;
        waiting = true;
        litTime = markerSince = 0;
        previousTime = now;
        sawMatchingMarker = false;
        lastPaint.reset();
    }

    bool Portrait() const { return shown.portrait; }

    bool HoldFrame() const {
        return waiting && !initialFrame && orientation &&
               (lastPaint || MixedPaint() || (*orientation != target && litTime < frameHold));
    }

    std::optional<ZaurusViewState> Update(uint64_t now, const uint32_t* frame, bool lcdEnabled) {
        if (waiting && lcdEnabled && wasLit && previousTime && now >= *previousTime)
            litTime += now - *previousTime;
        previousTime = now;
        wasLit = lcdEnabled;
        if (!waiting) return {};

        ZaurusViewState next = shown;
        if (!lcdEnabled) {
            lastPaint.reset();
        } else if (frame) {
            const auto corners = TaskbarCorners(frame);
            if (corners != marker) markerSince = litTime;
            marker = corners;
            std::optional<bool> seen;
            if (marker == Taskbar::landscape || marker == Taskbar::portrait)
                seen = marker == Taskbar::portrait;
            sawMatchingMarker = sawMatchingMarker || seen == target;
            if (seen) {
                orientation = seen;
                // A saved guest may still be drawing the other way when it opens.
                if (initialFrame) next.portrait = *seen;
                initialFrame = false;
            }

            if (seen == target && next.portrait != target) {
                const auto* end = frame + std::size_t(width) * height;
                if (!lastPaint) repaintStarted = litTime;
                if (!lastPaint || !std::equal(frame, end, painting.begin(), painting.end())) {
                    painting.assign(frame, end);
                    lastPaint = litTime;
                }
            } else {
                lastPaint.reset();
            }
        }

        constexpr uint64_t settle = 650'000'000;  // guest ns with the LCD on
        constexpr uint64_t animatedSettle = 2'000'000'000;
        if (lastPaint && (litTime - *lastPaint >= settle || litTime - repaintStarted >= animatedSettle))
            next.portrait = target;
        // A guest without the stock taskbar still follows the hinge after a bounded wait.
        const bool known = marker == Taskbar::landscape || marker == Taskbar::portrait;
        const bool fallback = !known && !MixedPaint() && litTime >= frameHold;
        if (fallback) next.portrait = target;
        if ((known && sawMatchingMarker && orientation == target && next.portrait == target) || fallback) {
            waiting = false;
            initialFrame = false;
        }
        if (!waiting || !lcdEnabled || litTime >= controlsDelay) next.folded = target;
        if (next == shown) return {};
        shown = next;
        return shown;
    }

   private:
    enum class Taskbar { absent, landscape, portrait, both };

    bool MixedPaint() const {
        constexpr uint64_t overlap = 4'000'000'000;  // guest ns
        return marker == Taskbar::both && orientation && litTime - markerSince < overlap;
    }

    Taskbar TaskbarCorners(const uint32_t* frame) const {
        constexpr unsigned corner = 36;
        if (width < corner || height < 2 * corner) return Taskbar::absent;
        // Stock Qtopia puts its green Start button in the bottom left in portrait.
        // With its landscape transform the button moves to the raw LCD's top left.
        // Count the green cap and its dark mark, allowing for backlight brightness.
        const auto startButton = [&](unsigned firstRow) {
            unsigned green = 0, dark = 0;
            for (unsigned y = firstRow; y < firstRow + corner; y += 2) {
                for (unsigned x = 0; x < corner; x += 2) {
                    const uint32_t pixel = frame[y * width + x];
                    const unsigned r = pixel & 255, g = (pixel >> 8) & 255, b = (pixel >> 16) & 255;
                    green += g >= 12 && g * 4 >= r * 5 && g >= b * 2;
                    dark += std::max({r, g, b}) <= 64;
                }
            }
            return green >= 84 && dark >= 24;
        };
        const bool top = startButton(0), bottom = startButton(height - corner);
        if (top) return bottom ? Taskbar::both : Taskbar::landscape;
        return bottom ? Taskbar::portrait : Taskbar::absent;
    }

    static constexpr uint64_t controlsDelay = 5'000'000'000;
    static constexpr uint64_t frameHold = 12'000'000'000;
    const unsigned width, height;
    bool target;
    ZaurusViewState shown;
    bool waiting{true}, initialFrame{true}, wasLit{true};
    bool sawMatchingMarker{false};
    Taskbar marker{Taskbar::absent};
    uint64_t litTime{0}, markerSince{0}, repaintStarted{0};
    std::optional<bool> orientation;
    std::optional<uint64_t> previousTime, lastPaint;
    std::vector<uint32_t> painting;
};
