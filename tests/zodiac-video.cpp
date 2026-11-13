// The W4200 video overlay as a player drives it from its own code (TCPMP's ati4200.c): the ring set-up and
// pointer reads, a display transaction (4d8) around VIDEO_CTRL 434, the window (454, 458), the Y/U/V offsets
// and pitches (43c-450), the overlay format bits of 4d4 and DISP_DEBUG2 bit 30. The frame shows the YUV 4:2:0
// planes over the graphic plane inside the window, mirrored or keyed when asked. The ROM's own overlay driver
// (DAL 200ad8cc-200ad9fc and 200ae09c-200ae7a8), the second and third offset sets (55c-570) and the IDCT block
// (c00-c3c) aren't modelled and still stop; the boot's VIDEO_CTRL clear works.
#include <array>
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "zodiac-graphics-setup.h"
#include "imx1_graphics_frame.h"

using A = Imx1BootstrapAccess;

static constexpr uint32_t player = 0x4053a32c;

static void store(Imx1Cs5Configuration& state, const Imx1Eim& eim, uint32_t pc, uint32_t offset, uint32_t value) {
    assert(state.access(eim, pc, 0x16010000 + offset, 4, true, value) == A::ok);
}
static uint32_t load(Imx1Cs5Configuration& state, const Imx1Eim& eim, uint32_t offset) {
    uint32_t value = 0xdeadbeef;
    assert(state.access(eim, player, 0x16010000 + offset, 4, false, value) == A::ok);
    return value;
}
static A put(Imx1Cs5Configuration& state, const Imx1Eim& eim, uint32_t offset, uint32_t value) {
    return state.access(eim, player, 0x16010000 + offset, 4, true, value);
}
static void transaction(Imx1Cs5Configuration& state, const Imx1Eim& eim, bool begin) {
    store(state, eim, 0x200aeb68, 0x4d8, begin ? 0x78 : 0x7b);
}
// The native mode, as tests/zodiac-graphics-display.cpp sets it up: a 3 by 2 surface at 800000, pitch 8, with
// the ROM's DISP_DEBUG write (200ad3e8).
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
                    {0x200ad3b8, 0x524, 0x102aa}, {0x200ad3e8, 0x4d4, 0x100}, {0x200c40c0, 0x434, 0}})
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
    const uint64_t period = 8 * 6 * Imx1GraphicsDisplay::pixelCycles;

    // The ROM driver's overlay writes stay refused, inside a display transaction as the driver makes them: show
    // and hide (200ae104, 200ad8ec, 200ad9fc), the overlay set up (200ae2c4-200ae2dc, 200ae350-200ae368,
    // 200ae6b0-200ae7a8) and the panel-on restore (200ad8cc, 200ad8d8).
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
    value = 0x00c00001;
    assert(state.access(eim, 0x200c40c0, 0x16010434, 4, true, value) == A::sideEffect);
    transaction(state, eim, false);
    state.advanceCpuCycles(period);
    // Player code still can't reach the second offset set or the IDCT block.
    for (uint32_t offset : {0x55cu, 0xc00u, 0xc3cu}) {
        for (bool write : {true, false}) {
            value = write ? 1 : 0xdeadbeef;
            assert(state.access(eim, player, 0x16010000 + offset, 4, write, value) != A::ok);
            if (!write) assert(value == 0xdeadbeef);
            ++refused;
        }
    }

    // Only code in the RAM the OS maps at 40000000 is a player: not the vectors, the ROM or a null jump.
    for (uint32_t pc : {0u, 0x18u, 0x10000000u, 0x20000000u, 0x30000000u, 0x47400000u}) {
        value = 0xdeadbeef;
        assert(state.access(eim, pc, 0x160104d8, 4, false, value) != A::ok && value == 0xdeadbeef);
        ++refused;
    }

    // What ati4200.c reads first: the ring set-up and pointers, the busy bit and the active area.
    assert(load(state, eim, 0x210) == 0x0800000d && load(state, eim, 0x214) == 0x10150000);
    assert(load(state, eim, 0x21c) == 0 && load(state, eim, 0x220) == 0 && load(state, eim, 0x140) == 0);
    assert(load(state, eim, 0x424) == 0x40001 && load(state, eim, 0x428) == 0x30001);
    assert(put(state, eim, 0x220, 0) != A::ok && put(state, eim, 0x424, 0) != A::ok);
    // Outside a transaction the overlay words don't take writes, and words never written don't read.
    assert(put(state, eim, 0x434, 0) == A::sequence);
    value = 0xdeadbeef;
    assert(state.access(eim, player, 0x1601043c, 4, false, value) == A::externalInput && value == 0xdeadbeef);

    // The conversion: BT.601 studio range, then 565 as the graphic plane.
    constexpr uint32_t white = 0xffffffff, black = 0xff000000, red = 0xff0000ff, green = 0xff00ff00,
                       blue = 0xffff0000, grey = 0xff848284;
    assert(Imx1GraphicsFrame::overlayPixel(235, 128, 128) == white && Imx1GraphicsFrame::overlayPixel(16, 128, 128) == black);
    assert(Imx1GraphicsFrame::overlayPixel(81, 90, 240) == red && Imx1GraphicsFrame::overlayPixel(145, 54, 34) == green);
    assert(Imx1GraphicsFrame::overlayPixel(41, 240, 110) == blue);

    // Graphic plane: red, green, blue / white, black, grey (565, as zodiac-graphics-display).
    Imx1GraphicsMemory memory;
    auto poke = [&](uint32_t address, uint32_t bytes, uint32_t data) {
        assert(memory.accessGpu(state, eim, address, bytes, true, data) == A::ok);
    };
    for (auto pair : {std::array<uint32_t,2>{0x800000,0xf800},{0x800002,0x07e0},{0x800004,0x001f},
            {0x800008,0xffff},{0x80000a,0},{0x80000c,0x8410}}) poke(pair[0], 2, pair[1]);
    // Overlay planes at 900000: Y pitch 16 (white, black / black, white), U and V pitch 8, neutral.
    for (auto pair : {std::array<uint32_t,2>{0x900000,235},{0x900001,16},{0x900010,16},{0x900011,235},
            {0x900100,128},{0x900200,128}}) poke(pair[0], 1, pair[1]);
    Imx1GraphicsFrame frame;
    uint32_t first = 0, last = 0;
    auto check = [&](std::array<uint32_t, 6> expected) {
        state.advanceCpuCycles(period);
        assert(frame.advance(state, memory, eim));
        const uint32_t* image = frame.getPendingFrame(first, last);
        assert(image && frame.width() == 3 && frame.height() == 2);
        for (unsigned n = 0; n < 6; ++n) assert(image[n] == expected[n]);
        frame.acknowledgeFrame();
    };
    check({red, green, blue, white, black, grey});

    // ati4200.c's Update: the window from the active area's start (panel x 1-2, both rows), 4:2:0 with the
    // conversion on, the offsets and pitches, DISP_DEBUG2 bit 30. Then Show sets the two enables.
    const uint32_t control = 1 | (1 << 9) | (1 << 22) | (1 << 23);
    assert(put(state, eim, 0x4d8, 0x78) == A::ok);
    assert(put(state, eim, 0x4d4, load(state, eim, 0x4d4) & ~0x1f0u) == A::ok);
    assert(put(state, eim, 0x434, control) == A::ok);
    assert(put(state, eim, 0x454, 2 | 4 << 16) == A::ok && put(state, eim, 0x458, 1 | 3 << 16) == A::ok);
    for (auto pair : {std::array<uint32_t,2>{0x43c,0x900000},{0x440,16},{0x444,0x900100},{0x448,8},{0x44c,0x900200},
            {0x450,8}}) assert(put(state, eim, pair[0], pair[1]) == A::ok);
    assert(put(state, eim, 0x538, load(state, eim, 0x538) | 0x40000000) == A::ok);
    assert(put(state, eim, 0x4d8, 0x7b) == A::ok && (load(state, eim, 0x4d8) & 2));
    check({red, green, blue, white, black, grey});
    assert(!(load(state, eim, 0x4d8) & 2));
    assert(put(state, eim, 0x4d8, 0x78) == A::ok && put(state, eim, 0x434, load(state, eim, 0x434) | 0x14) == A::ok);
    assert(put(state, eim, 0x4d8, 0x7b) == A::ok);
    check({red, white, black, white, black, white});
    // Within the same frame the shown overlay isn't read again.
    assert(frame.advance(state, memory, eim) && !frame.getPendingFrame(first, last));

    auto edit = [&](std::initializer_list<std::array<uint32_t,2>> words) {
        assert(put(state, eim, 0x4d8, 0x78) == A::ok);
        for (auto pair : words) assert(put(state, eim, pair[0], pair[1]) == A::ok);
        return put(state, eim, 0x4d8, 0x7b);
    };
    // Mirrored left to right: the Y offset points at the window's last column.
    assert(edit({{0x434, control | 0x14 | 1 << 24}, {0x43c, 0x900001}}) == A::ok);
    check({red, black, white, white, white, black});
    // Mirrored top to bottom as well: it points at the last row.
    assert(edit({{0x434, control | 0x14 | 3 << 24}, {0x43c, 0x900011}}) == A::ok);
    check({red, white, black, white, black, white});
    // The keyer: only the green graphic pixel (key 07e0) shows the overlay.
    assert(edit({{0x434, control | 0x16}, {0x438, 0x07e0}, {0x43c, 0x900000}}) == A::ok);
    check({red, white, blue, white, black, grey});

    // Refused: a window outside the active area, the expansions, a plane outside graphics memory, an RGB
    // overlay, the overlay format bits with the overlay shown, another DISP_DEBUG2 or DISP_DEBUG bit.
    assert(edit({{0x454, 2 | 5 << 16}}) == A::sideEffect);
    assert(put(state, eim, 0x434, control | 0x14 | 0x20) == A::sideEffect);
    assert(edit({{0x454, 2 | 4 << 16}, {0x43c, 0xfffff0}}) == A::sideEffect);
    assert(edit({{0x43c, 0x900000}, {0x434, 0x14 | 1 << 22 | 1 << 23}}) == A::sideEffect);
    assert(edit({{0x434, control | 0x14}, {0x4d4, load(state, eim, 0x4d4) | 0x10}}) == A::sideEffect);
    assert(put(state, eim, 0x538, load(state, eim, 0x538) | 1) == A::sideEffect);
    assert(put(state, eim, 0x4d4, load(state, eim, 0x4d4) | 1) == A::sideEffect);
    assert(put(state, eim, 0x4d4, load(state, eim, 0x4d4) & ~0x1f0u) == A::ok);
    assert(put(state, eim, 0x4d8, 0x7b) == A::ok);
    check({red, white, black, white, black, white});
    // Hide clears the enables and the graphic plane shows again, also when it clears only one of them.
    assert(edit({{0x434, control}}) == A::ok);
    check({red, green, blue, white, black, grey});
    assert(edit({{0x434, control | 0x14}}) == A::ok);
    check({red, white, black, white, black, white});
    assert(edit({{0x434, control | 0x10}}) == A::ok);
    check({red, green, blue, white, black, grey});
    assert(edit({{0x434, control | 0x4}}) == A::ok);
    check({red, green, blue, white, black, grey});

    // The ROM's own transactions carry the overlay words on: a mode write keeps the overlay shown.
    assert(edit({{0x434, control | 0x14}}) == A::ok);
    check({red, white, black, white, black, white});
    transaction(state, eim, true);
    store(state, eim, 0x200ad720, 0x418, 0x800000);
    transaction(state, eim, false);
    check({red, white, black, white, black, white});
    const auto& d = state.displayState();
    assert(d.lcdEnabled() && d.activeWord(0x434) == (control | 0x14) && d.activeWord(0x538) == 0x40800000);
    std::printf("PASS: %u ROM overlay driver, offset set and IDCT accesses refused; player overlay shown, "
                "mirrored, keyed and hidden over the graphic plane\n", refused);
}
