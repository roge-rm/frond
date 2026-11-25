// Pure checks of the fitted window layout and the window/output size reconciliation.
#include <cassert>
#include <cmath>
#include <cstdio>
#include "WindowLayout.h"

namespace {
struct Device { const char* name; int width, height; };
// Device sizes in device pixels: the Zodiac around its screen, a TG50 with its keyboard, a plain 320x320 and a 68k 160x225.
const Device devices[] = {{"zodiac2", 780, 320}, {"zodiac2-below", 480, 534}, {"tg50", 320, 544}, {"plain", 320, 320}, {"m515", 160, 225}};
const int sizes[][2] = {{1050, 620}, {1051, 611}, {913, 479}, {801, 401}, {640, 700}, {333, 777}, {1, 1}, {2561, 1441}};
const float dprs[] = {1.f, 1.1f, 1.25f, 1.5f, 1.75f, 2.f};

void checkFit(WindowFit fit, const Device& d, int ww, int wh) {
    const auto l = fitWindowLayout(fit, d.width, d.height, ww, wh);
    assert(l.x >= 0 && l.y >= 0 && l.width <= ww && l.height <= wh);
    assert(l.x + l.width <= ww && l.y + l.height <= wh);
    // Centred to within a pixel.
    assert(std::abs((ww - l.width) - 2 * l.x) <= 1 && std::abs((wh - l.height) - 2 * l.y) <= 1);
    if (fit != WindowFit::fill) assert(l.scaleX == l.scaleY);
    // The drawn device fills the viewport to within the rounding of its size.
    assert(std::fabs(d.width * l.scaleX - l.width) <= 1.f && std::fabs(d.height * l.scaleY - l.height) <= 1.f);
    // Touching one side of the window.
    if (fit == WindowFit::fit) assert(l.width == ww || l.height == wh);
}
}  // namespace

int main() {
    for (const auto& d : devices)
        for (const auto& s : sizes)
            for (auto fit : {WindowFit::fit, WindowFit::whole, WindowFit::fill}) checkFit(fit, d, s[0], s[1]);

    // A window and output that agree on a ratio, at whole and fractional scales and at odd sizes, keep it on both axes.
    for (float dpr : dprs)
        for (const auto& s : sizes) {
            const int ow = int(std::lround(s[0] * dpr)), oh = int(std::lround(s[1] * dpr));
            const auto m = consistentWindowMetrics(s[0], s[1], ow, oh);
            assert(m.width == s[0] && m.height == s[1]);
            assert(m.pixelsX == m.pixelsY);
            if (s[0] > 20 && s[1] > 20) assert(std::fabs(m.pixelsX - dpr) < 0.06f);
            // Drawn at that ratio, the layout stays inside the output on both axes.
            for (const auto& d : devices) {
                const auto l = fitWindowLayout(WindowFit::fit, d.width, d.height, m.width, m.height);
                assert(std::lround((l.x + l.width) * m.pixelsX) <= ow + 1 && std::lround((l.y + l.height) * m.pixelsY) <= oh + 1);
            }
        }

    // Windows with display scaling: the child's physical size is whole pixels, SDL reports it divided by the scale and
    // floored on each axis. Each pair is physical size and scale; the logical size is what SDL says.
    struct Scaled { int pw, ph; float dpr; } windows[] = {{1313, 775, 1.25f}, {1301, 655, 1.5f}, {1111, 777, 1.25f}, {901, 777, 1.5f}, {1457, 893, 1.25f}, {399, 301, 1.5f}};
    for (const auto& w : windows) {
        const int lw = int(std::floor(w.pw / w.dpr)), lh = int(std::floor(w.ph / w.dpr));
        const auto m = consistentWindowMetrics(lw, lh, w.pw, w.ph);
        assert(m.pixelsX == m.pixelsY);
        for (const auto& d : devices) {
            const auto l = fitWindowLayout(WindowFit::fit, d.width, d.height, m.width, m.height);
            assert(l.scaleX == l.scaleY);
            assert(std::lround((l.x + l.width) * m.pixelsX) <= w.pw + 1 && std::lround((l.y + l.height) * m.pixelsY) <= w.ph + 1);
        }
        // SDL's size is physical on one axis only (the other scaled, or stale from before the resize): the output wins.
        const auto bad = consistentWindowMetrics(w.pw, lh, w.pw, w.ph);
        assert(bad.width == w.pw && bad.height == w.ph && bad.pixelsX == 1.f && bad.pixelsY == 1.f);
        const auto stale = consistentWindowMetrics(lw, int(std::floor(w.ph / w.dpr)) + 120, w.pw, w.ph);
        assert(stale.width == w.pw && stale.pixelsY == 1.f);
    }

    // A window size that disagrees with the output (one axis stale or scaled on its own) gives way to the output.
    struct Case { int ww, wh, ow, oh; } stretched[] = {{1050, 620, 1312, 775}, {1050, 620, 1313, 620}, {700, 500, 700, 800}, {531, 677, 797, 677}};
    for (const auto& c : stretched) {
        const auto m = consistentWindowMetrics(c.ww, c.wh, c.ow, c.oh);
        if (m.width == c.ww && m.height == c.wh) { assert(std::fabs(float(c.ow) / c.ww - float(c.oh) / c.wh) < 0.02f); continue; }
        assert(m.width == c.ow && m.height == c.oh && m.pixelsX == 1.f && m.pixelsY == 1.f);
        for (const auto& d : devices) {
            const auto l = fitWindowLayout(WindowFit::fit, d.width, d.height, m.width, m.height);
            assert(l.scaleX == l.scaleY && l.x + l.width <= c.ow && l.y + l.height <= c.oh);
        }
    }
    // No output size: the window size stands.
    const auto none = consistentWindowMetrics(640, 480, 0, 0);
    assert(none.width == 640 && none.height == 480 && none.pixelsX == 1.f);
    std::puts("window-layout: ok");
    return 0;
}
