// The W4200 video overlay (VIDEO_CTRL 434 to VIDEO_V_POS 458, the colour key 438, the overlay pixel format in
// DISP_DEBUG 4d4, DISP_DEBUG2 538 bit 30 and the second and third Y/U/V offset sets 55c-570) and the IDCT
// block (c00-c3c) aren't modelled yet. Every write the ROM's overlay driver makes (DAL 200ad8cc-200ad9fc and
// 200ae09c-200ae7a8) and every access a player makes from its own code, as TCPMP's ati4200.c does, is refused,
// so the run stops (an unsupported peripheral; the IDCT block an unmapped access) instead of drawing without
// the overlay. The boot's own
// VIDEO_CTRL clear still works and the display keeps running.
#include <array>
#include <cassert>
#include <cstdio>
#include "zodiac-graphics-setup.h"
#include "imx1_graphics_frame.h"

using A = Imx1BootstrapAccess;

static void store(Imx1Cs5Configuration& state, const Imx1Eim& eim, uint32_t pc, uint32_t offset, uint32_t value) {
    assert(state.access(eim, pc, 0x16010000 + offset, 4, true, value) == A::ok);
}
static void transaction(Imx1Cs5Configuration& state, const Imx1Eim& eim, bool begin) {
    store(state, eim, 0x200aeb68, 0x4d8, begin ? 0x78 : 0x7b);
}
// The native mode, as tests/zodiac-graphics-display.cpp sets it up.
static void display(Imx1Cs5Configuration& state, const Imx1Eim& eim) {
    uint32_t value = 0;
    assert(state.access(eim, 0x200acb98, 0x160113e8, 4, false, value) == A::ok);
    for (auto op : {std::array<uint32_t, 3>{0x200acba8, 0x13e8, 0x400}, {0x200acbb4, 0x144, 0x2440},
                    {0x200acbc4, 0x10a8, 0x1fff1fff}, {0x200acbd0, 0x4dc, 0xc0000}, {0x200b8e80, 0x214, 0x10150000},
                    {0x200b8e88, 0x220, 0}, {0x200b8f04, 8, 0x04150000}, {0x200b8f10, 0x230, 0x80000000},
                    {0x200b8f20, 0x210, 0x0800000d}})
        store(state, eim, op[0], op[1], op[2]);
    transaction(state, eim, true);
    store(state, eim, 0x200ad720, 0x418, 0x800000);
    store(state, eim, 0x200ad72c, 0x41c, 8);
    store(state, eim, 0x200ad738, 0x414, 6);
    store(state, eim, 0x200ad65c, 0x42c, 0x40001);
    store(state, eim, 0x200ad668, 0x430, 0x30001);
    for (auto op : {std::array<uint32_t, 3>{0x200ad2e0, 0x410, 3}, {0x200ad404, 0x540, 0x80604020},
                    {0x200ad410, 0x544, 0xffdfbf9f}, {0x200ad418, 0x548, 0}, {0x200ad420, 0x578, 0x80604020},
                    {0x200ad428, 0x58c, 0xffdfbf9f}, {0x200ad430, 0x590, 0}, {0x200ad438, 0x594, 0x80604020},
                    {0x200ad440, 0x598, 0xffdfbf9f}, {0x200ad448, 0x59c, 0}, {0x200ad528, 0x538, 0x800000},
                    {0x200ad3b8, 0x524, 0x102aa}, {0x200c40c0, 0x434, 0}})
        store(state, eim, op[0], op[1], op[2]);
    store(state, eim, 0x200ad2ec, 0x414, 0x1800406);
    store(state, eim, 0x200ad2f8, 0x420, 0x60008);
    store(state, eim, 0x200ad304, 0x424, 0x40001);
    store(state, eim, 0x200ad310, 0x428, 0x30001);
    transaction(state, eim, false);
    state.advanceCpuCycles(32);
    transaction(state, eim, true);
    store(state, eim, 0x200ad8b4, 0x414, 0x1801c06);
    transaction(state, eim, false);
    state.advanceCpuCycles(32);
    assert(state.displayState().lcdEnabled());
}

int main() {
    Imx1Eim eim;
    uint32_t value = 0x3e00;
    assert(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
    value = 0x11111e01;
    assert(eim.access(0x0022002c, 4, true, value) == Imx1EimAccess::ok);
    Imx1Cs5Configuration state(Imx1GraphicsProfile::virtual5744Compatible8MiB);
    configureGraphicsInterface(state, eim);
    configureGraphicsController(state, eim);
    display(state, eim);
    const uint64_t revision = state.displayState().revision();

    // The ROM driver's overlay writes, inside a display transaction as the driver makes them: show and hide
    // (200ae104, 200ad8ec, 200ad9fc), the overlay set up (200ae2c4-200ae2dc, 200ae350-200ae368, 200ae6b0-200ae7a8)
    // and the panel-on restore (200ad8cc, 200ad8d8). Values are what a YUV 4:2:0 overlay would write.
    static constexpr std::array<uint32_t, 3> driver[] = {
        {0x200ae104, 0x434, 0x14}, {0x200ad8ec, 0x434, 0x1c}, {0x200ad9fc, 0x434, 0}, {0x200ad8cc, 0x454, 0x01400000},
        {0x200ad8d8, 0x458, 0x00f00000}, {0x200ae2c4, 0x434, 0x00c00001}, {0x200ae2d0, 0x454, 0x01400000},
        {0x200ae2dc, 0x458, 0x00f00000}, {0x200ae350, 0x43c, 0}, {0x200ae35c, 0x444, 0}, {0x200ae368, 0x44c, 0},
        {0x200ae6b0, 0x4d4, 0}, {0x200ae6d4, 0x438, 0xf81f}, {0x200ae6ec, 0x434, 0x00c00001},
        {0x200ae6fc, 0x55c, 0x880000}, {0x200ae70c, 0x43c, 0x880000}, {0x200ae710, 0x568, 0x880000},
        {0x200ae718, 0x440, 320}, {0x200ae74c, 0x560, 0x8a5800}, {0x200ae754, 0x564, 0x8b3c00},
        {0x200ae768, 0x444, 0x8a5800}, {0x200ae76c, 0x56c, 0x8a5800}, {0x200ae774, 0x44c, 0x8b3c00},
        {0x200ae778, 0x570, 0x8b3c00}, {0x200ae784, 0x448, 160}, {0x200ae790, 0x450, 160},
        {0x200ae7a8, 0x538, 0x40800000}};
    transaction(state, eim, true);
    unsigned refused = 0;
    for (auto op : driver) {
        value = op[2];
        const A got = state.access(eim, op[0], 0x16010000 + op[1], 4, true, value);
        assert(got != A::ok && got != A::unmapped);
        ++refused;
    }
    // The boot's clear (200c40c0) takes only 0.
    value = 0x00c00001;
    assert(state.access(eim, 0x200c40c0, 0x16010434, 4, true, value) == A::sideEffect);
    transaction(state, eim, false);
    state.advanceCpuCycles(32);

    // A player's own code (a RAM PC) writing and reading the overlay, the IDCT control and the ring pointer.
    for (uint32_t offset : {0x434u, 0x43cu, 0x454u, 0x4d4u, 0x538u, 0xc00u, 0xc3cu}) {
        for (bool write : {true, false}) {
            value = write ? 0x00c00001 : 0xdeadbeef;
            const A got = state.access(eim, 0x00031000, 0x16010000 + offset, 4, write, value);
            // The overlay words stop as an unsupported peripheral; the IDCT block isn't mapped yet, a stop too.
            assert(got != A::ok && (offset >= 0xc00 || got != A::unmapped));
            if (!write) assert(value == 0xdeadbeef);
            ++refused;
        }
    }

    // Nothing reached the display: VIDEO_CTRL is still 0 and the mode runs on.
    const auto& d = state.displayState();
    assert(d.stagedWord(0x434) == 0 && d.activeWord(0x434) == 0 && d.stagedWord(0x538) == 0x800000);
    assert(d.lcdEnabled() && d.revision() >= revision);
    std::printf("PASS: %u W4200 overlay and IDCT accesses (ROM driver and player code) refused, the boot's "
                "VIDEO_CTRL clear kept, the display unchanged\n", refused);
}
