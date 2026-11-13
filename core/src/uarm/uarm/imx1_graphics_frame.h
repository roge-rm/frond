#ifndef FROND_IMX1_GRAPHICS_FRAME_H
#define FROND_IMX1_GRAPHICS_FRAME_H

#include <algorithm>
#include <vector>
#include "imx1_graphics_memory.h"

// Host pixels and frame acknowledgements are separate from guest control state.
class Imx1GraphicsFrame {
   public:
    Imx1GraphicsFrame() = default;
    Imx1GraphicsFrame(const Imx1GraphicsFrame&) = delete;
    Imx1GraphicsFrame& operator=(const Imx1GraphicsFrame&) = delete;
    void reset() {
        enabled = valid = pending = acquired = false;
        lastFrame = lastRevision = 0;
        ++generation;
    }
    bool lcdEnabled() const { return enabled; }
    // A frame handed out by getPendingFrame and not acknowledged yet.
    bool frameAcquired() const { return acquired; }
    // row is scratch for one advance.
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.DoVector(pixels, 1024 * 1024);
        Imx1GraphicsDisplay::stateSurface(a, surface);
        a.Do64(lastRevision).Do64(lastFrame).Do64(generation).Do64(acquiredGeneration);
        a.Do32(firstRow).Do32(lastRow).DoBool(enabled).DoBool(valid).DoBool(pending).DoBool(acquired);
        a.Require(!acquired, "frame acquired");
        a.Require(!valid || pixels.size() == size_t(surface.width) * surface.height, "frame size");
        a.Require(!pending || (firstRow <= lastRow && lastRow < surface.height), "frame rows");
    }
    // Takes everything from a frame a load has decoded. Borrowed pixels end here.
    void replace(Imx1GraphicsFrame& other) {
        pixels.swap(other.pixels);
        surface = other.surface;
        lastRevision = other.lastRevision; lastFrame = other.lastFrame;
        generation = other.generation; acquiredGeneration = other.acquiredGeneration;
        firstRow = other.firstRow; lastRow = other.lastRow;
        enabled = other.enabled; valid = other.valid; pending = other.pending; acquired = other.acquired;
    }
    uint32_t width() const { return valid ? surface.width : 0; }
    uint32_t height() const { return valid ? surface.height : 0; }
    const uint32_t* peekFrame() const { return valid && enabled ? pixels.data() : nullptr; }
    const uint32_t* getPendingFrame(uint32_t& first, uint32_t& last) {
        if (!pending || !enabled) return nullptr;
        first = firstRow;
        last = lastRow;
        acquiredGeneration = generation;
        acquired = true;
        return pixels.data();
    }
    void acknowledgeFrame() {
        if (acquired && acquiredGeneration == generation) pending = false;
        acquired = false;
    }
    // Tests: directRows off makes advance read every row through copyGpuRange; directRowCount counts the rows
    // read straight from the bank storage.
    static inline bool directRows = true;
    static inline uint64_t directRowCount = 0;
    // advance would read graphics memory now, so a write before it shows in the frame it scans.
    bool scanDue(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim) const {
        const auto& display = configuration.displayState();
        if (!display.lcdEnabled() || !configuration.memoryReady(eim)) return false;
        return !valid || !enabled || display.revision() != (lastRevision & ~overlaidBit) || display.frameNumber() != lastFrame;
    }
    bool advance(const Imx1Cs5Configuration& configuration, Imx1GraphicsMemory& memory, const Imx1Eim& eim) {
        const auto& display = configuration.displayState();
        if (!display.lcdEnabled() || !configuration.memoryReady(eim)) {
            enabled = valid = pending = acquired = false;
            return true;
        }
        Imx1GraphicsDisplay::Overlay overlay;
        const bool overlaid = display.overlay(configuration.memoryWindows(), overlay);
        // The overlay's planes aren't dirty tracked, so each new frame with it shown is read whole, and so is the
        // first one after it goes, which may leave rows the graphic plane never dirties.
        const bool wasOverlaid = lastRevision & overlaidBit;
        const bool full = !valid || !enabled || display.revision() != (lastRevision & ~overlaidBit) ||
                          (overlaid && display.frameNumber() != lastFrame) || (wasOverlaid && !overlaid);
        if (!full && display.frameNumber() == lastFrame) return true;
        Imx1GraphicsDisplay::Surface candidate;
        if (!display.surface(candidate)) return false;
        const uint8_t* planes[3] = {};
        if (overlaid) {
            if (overlay.x + overlay.width > candidate.width || overlay.y + overlay.height > candidate.height) return false;
            for (unsigned plane = 0; plane < 3; ++plane) {
                unsigned bank = 0;
                uint32_t offset = 0;
                planes[plane] = memory.gpuSpan(configuration, eim, overlay.planes[plane].rangeBase,
                                               overlay.planes[plane].rangeBytes, bank, offset);
                if (!planes[plane]) return false;
            }
        }
        unsigned rangeBank = 0;
        uint32_t rangeOffset = 0;
        const uint8_t* range = memory.gpuSpan(configuration, eim, candidate.rangeBase, candidate.rangeBytes, rangeBank,
                                              rangeOffset);
        if (!range) return false;
        // A row inside the checked range passes copyGpuRange's own check when the range lies in one bank's mapped
        // addresses, so it is read straight from the bank storage.
        const uint64_t rangeEnd = uint64_t(candidate.rangeBase) + candidate.rangeBytes;
        const bool direct = directRows && candidate.xStride == 2 && candidate.width &&
            ((candidate.rangeBase >= 0x100000 && rangeEnd <= 0x160000) ||
             (candidate.rangeBase >= 0x800000 && rangeEnd <= 0x1000000));
        enabled = true;
        const auto dirty = memory.dirty(candidate.bank, Imx1GraphicsMemory::Consumer::scanout);
        uint32_t first = candidate.height, last = 0;
        if (full) { first = 0; last = candidate.height - 1; }
        else if (dirty.pending) {
            for (uint32_t y = 0; y < candidate.height; ++y) {
                const uint64_t begin = uint64_t(int64_t(candidate.bankOffset) + candidate.base - candidate.rangeBase +
                                                int64_t(y) * candidate.yStride);
                if (begin < dirty.end && begin + uint64_t(candidate.width - 1) * candidate.xStride + 2 > dirty.first) {
                    first = std::min(first, y);
                    last = y;
                }
            }
        }
        if (first < candidate.height) {
            if (surface.width != candidate.width || surface.height != candidate.height) pending = false;
            if (pixels.size() != candidate.width * candidate.height) pixels.resize(candidate.width * candidate.height);
            row.resize(candidate.width * 2);
            for (uint32_t y = first; y <= last; ++y) {
                const uint32_t start = uint32_t(int64_t(candidate.base) + int64_t(y) * candidate.yStride);
                const uint8_t* source = row.data();
                if (direct && start >= candidate.rangeBase && uint64_t(start) + candidate.width * 2 <= rangeEnd) {
                    source = range + (start - candidate.rangeBase);
                    ++directRowCount;
                } else if (candidate.xStride == 2) {
                    if (!memory.copyGpuRange(configuration, eim, start, candidate.width * 2, row.data())) return false;
                } else {
                    for (uint32_t x = 0; x < candidate.width; ++x)
                        if (!memory.copyGpuRange(configuration, eim, start + x * candidate.xStride,
                                                  2, row.data() + x * 2)) return false;
                }
                uint32_t* out = pixels.data() + size_t(y) * candidate.width;
                for (uint32_t x = 0; x < candidate.width; ++x) {
                    const uint32_t rgb = uint32_t(source[x * 2]) | (uint32_t(source[x * 2 + 1]) << 8);
                    const uint32_t red = (rgb >> 11) & 31, green = (rgb >> 5) & 63, blue = rgb & 31;
                    out[x] = 0xff000000u |
                        ((blue << 3 | blue >> 2) << 16) | ((green << 2 | green >> 4) << 8) | (red << 3 | red >> 2);
                }
                if (overlaid && y >= overlay.y && y < overlay.y + overlay.height)
                    composite(overlay, planes, y - overlay.y, source + overlay.x * 2, out + overlay.x);
            }
            if (pending) { firstRow = std::min(firstRow, first); lastRow = std::max(lastRow, last); }
            else { firstRow = first; lastRow = last; }
            pending = true;
            ++generation;
        }
        memory.acknowledgeDirty(candidate.bank, Imx1GraphicsMemory::Consumer::scanout, dirty.generation);
        surface = candidate;
        lastRevision = display.revision() | (overlaid ? overlaidBit : 0);
        lastFrame = display.frameNumber();
        valid = true;
        return true;
    }
    // YUV to the panel's RGB565 and on to the frame's pixel format.
    // ASSUMPTION "overlay colour": the chip converts BT.601 studio range (Y 16-235), as TCPMP hands it the
    // decoder's planes unchanged, and the result reaches the panel at the graphic plane's 565 depth.
    static uint32_t overlayPixel(int y, int u, int v) {
        const int c = 298 * (y - 16) + 128, d = u - 128, e = v - 128;
        const auto clamp = [](int value) { return uint32_t(std::min(255, std::max(0, value >> 8))); };
        const uint32_t red = clamp(c + 409 * e) >> 3, green = clamp(c - 100 * d - 208 * e) >> 2, blue = clamp(c + 516 * d) >> 3;
        return 0xff000000u |
            ((blue << 3 | blue >> 2) << 16) | ((green << 2 | green >> 4) << 8) | (red << 3 | red >> 2);
    }
    // One row of the overlay over the graphic row it covers. graphic holds that row's 565 pixels for the keyer.
    static void composite(const Imx1GraphicsDisplay::Overlay& overlay, const uint8_t* const planes[3], uint32_t row,
                          const uint8_t* graphic, uint32_t* out) {
        const uint8_t* lines[3];
        for (unsigned plane = 0; plane < 3; ++plane) {
            const auto& p = overlay.planes[plane];
            const int64_t step = int64_t(plane ? row / 2 : row) * p.pitch;
            lines[plane] = planes[plane] + (p.base - p.rangeBase) + (overlay.mirrorY ? -step : step);
        }
        for (uint32_t x = 0; x < overlay.width; ++x) {
            if (overlay.keyed && (uint32_t(graphic[x * 2]) | uint32_t(graphic[x * 2 + 1]) << 8) != overlay.key) continue;
            const int64_t luma = overlay.mirrorX ? -int64_t(x) : int64_t(x);
            const int64_t chroma = overlay.mirrorX ? -int64_t(x / 2) : int64_t(x / 2);
            out[x] = overlayPixel(lines[0][luma], lines[1][chroma], lines[2][chroma]);
        }
    }
   private:
    // The top bit of lastRevision says the last frame read had the overlay shown, so the saved layout stays as it
    // was. The display's revision count never reaches it.
    static constexpr uint64_t overlaidBit = 1ull << 63;
    std::vector<uint32_t> pixels;
    std::vector<uint8_t> row;
    Imx1GraphicsDisplay::Surface surface{};
    uint64_t lastRevision = 0, lastFrame = 0, generation = 0, acquiredGeneration = 0;
    uint32_t firstRow = 0, lastRow = 0;
    bool enabled = false, valid = false, pending = false, acquired = false;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1GraphicsFrame) == 136, "update state() and stateVersion");
#endif
#endif
