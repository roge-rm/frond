// The W4200 video overlay as a player drives it from its own code (TCPMP's ati4200.c): the ring set-up and
// pointer reads, a display transaction (4d8) around VIDEO_CTRL 434, the window (454, 458), the Y/U/V offsets
// and pitches (43c-450), the overlay format bits of 4d4 and DISP_DEBUG2 bit 30. The frame shows the YUV 4:2:0
// planes over the graphic plane inside the window, mirrored or keyed when asked. The ROM's own overlay driver
// (DAL 200ad8cc-200ad9fc and 200ae09c-200ae7a8), the second and third offset sets (55c-570) and CPU access to the
// IDCT block (c00-c3c) aren't modelled and still stop; the boot's VIDEO_CTRL clear works. playerEngine covers
// the player's own ring packets.
#include <array>
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "zodiac-graphics-setup.h"
#include "imx1_graphics_frame.h"
#include "imx1_graphics_commands.h"
#include <vector>

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

// A player's own ring packets (ati4200.c): NOPs and register writes, the reads of the words it saves, the plane
// stretch, the overlay words its flip sends and IDCT macroblocks.
static void playerEngine(Imx1Cs5Configuration& state, const Imx1Eim& eim, Imx1GraphicsMemory& memory) {
    Imx1GraphicsCommands commands;
    auto poke = [&](uint32_t address, uint32_t bytes, uint32_t data) {
        assert(memory.accessGpu(state, eim, address, bytes, true, data) == A::ok);
    };
    auto peek = [&](uint32_t address) {
        uint32_t data = 0;
        assert(memory.accessGpu(state, eim, address, 1, false, data) == A::ok);
        return data;
    };
    // Writes the words at the producer and submits them as the player; the engine then runs them to the end.
    auto submit = [&](const std::vector<uint32_t>& words, bool run = true) {
        uint32_t at = state.engineState().producer();
        for (uint32_t word : words) {
            poke(0x150000 + at * 4, 4, word);
            at = (at + 1) & 16383;
        }
        auto candidate = state;
        auto work = commands;
        A status = candidate.access(eim, player, 0x16010220, 4, true, at);
        if (status == A::ok) status = work.prepare(candidate, eim, memory);
        if (status != A::ok) return status;
        candidate.startEngine(work.duration());
        state = candidate;
        commands = std::move(work);
        if (!run) return A::ok;
        assert(state.advanceCpuCycles(uint32_t(commands.duration())));
        return commands.complete(state, eim, memory);
    };
    auto reg = [](uint32_t index, uint32_t value) { return std::vector<uint32_t>{index, value}; };
    auto join = [](std::initializer_list<std::vector<uint32_t>> parts) {
        std::vector<uint32_t> all;
        for (const auto& part : parts) all.insert(all.end(), part.begin(), part.end());
        return all;
    };
    uint32_t value = 0;
    // Saved words read nothing before a packet sets them, and only from 16011000 on and c3c.
    assert(commands.playerRead(0x1601106c, value) == A::sequence);
    // NOP2, NOP3, the type 3 NOP, then the backup words as RingEnd restores them.
    const uint32_t saved[][2] = {{0x41b, 0x5798b4df}, {0x472, 0x3f}, {0x4b2, 0x00cc0b00}, {0x4b1, 0x40030d04},
        {0x401, 0x800000}, {0x402, 8}, {0x46b, 0x800000}, {0x46c, 8}, {0x488, 4}, {0x30f, 0}, {0x42e, 0},
        {0x42f, 0}, {0x430, 0}, {0x431, 0}, {0x432, 0}, {0x433, 0}};
    std::vector<uint32_t> restore = {0, 0xdead, 0x40000800, 1, 2, 0xc0011000, 3, 4, 0x30e, 0, 0x46f, 0,
                                      0x470, 0x1fff1fff, 0x471, 0x1fff1fff};
    for (auto& pair : saved) { restore.push_back(pair[0]); restore.push_back(pair[1]); }
    assert(submit(restore, false) == A::ok);
    assert(commands.playerRead(0x1601106c, value) == A::sequence);
    assert(state.advanceCpuCycles(uint32_t(commands.duration())) && commands.complete(state, eim, memory) == A::ok);
    for (auto& pair : saved) {
        value = 0xdeadbeef;
        assert(commands.playerRead(0x16010000 + pair[0] * 4, value) == A::ok && value == pair[1]);
    }
    assert(commands.playerRead(0x16011284, value) == A::externalInput);
    // Words outside the player's set, a type 3 packet other than NOP and IDCT, 30e other than 0: refused.
    assert(submit(reg(0x423, 0)) == A::sideEffect && submit(reg(0x30e, 1)) == A::sideEffect);
    assert(submit({0xc0001500, 0, 0}) == A::sideEffect);

    // The plane stretch: a 4 by 2 source at 900000 (pitch 8) doubled into 900100 (pitch 16), smooth.
    for (uint32_t n = 0; n < 24; ++n) poke(0x900000 + n, 1, n < 8 ? n * 16 : n < 16 ? 128 : 0);
    auto stretch = [&](uint32_t sx, uint32_t width, uint32_t height, uint32_t ratio) {
        return join({reg(0x41b, 0xd598b2df), reg(0x472, 0x3f), reg(0x4b2, 0x01cc1200), reg(0x4b1, 0x40037d02),
                     reg(0x488, 0x02020008), reg(0x46f, 0), reg(0x470, 0x00100010), reg(0x401, 0x900100),
                     reg(0x402, 16), reg(0x46b, 0x900000), reg(0x46c, 8), reg(0x418, 0x900000), reg(0x419, 8),
                     reg(0x410, width + 1), reg(0x411, height), reg(0x412, ratio), reg(0x420, width + 1),
                     reg(0x421, height), reg(0x422, ratio), reg(0x464, sx << 18), reg(0x416, sx << 18 | 4),
                     reg(0x465, 0), reg(0x466, width << 16 | height)});
    };
    assert(submit(stretch(0, 6, 2, 0x00800080)) == A::ok);
    // Row 0: 0, 8, 16, 24, ... halfway steps across; row 1 halfway to the 128 row below.
    for (uint32_t x = 0; x < 6; ++x) {
        assert(peek(0x900100 + x) == x * 8);
        assert(peek(0x900110 + x) == (x * 8 + 128 + 1) / 2);
    }
    // Ratios outside 10-1ff, a source past its pitch and the ROM's own 16 bit datatype are refused.
    assert(submit(stretch(0, 6, 2, 0x00800008)) == A::sideEffect);
    assert(submit(stretch(5, 6, 2, 0x00800080)) == A::sideEffect);
    assert(submit(join({stretch(0, 6, 2, 0x00800080), reg(0x4b1, 0x40037d04), reg(0x466, 6 << 16 | 2)})) == A::sideEffect);

    // The flip: 4d8 = 78, the Y offset, 4d8 = 7b through the ring, set when the engine gets there.
    assert(submit(join({reg(0x136, 0x78), reg(0x10f, 0x900040), reg(0x136, 0x7b)}), false) == A::ok);
    assert(load(state, eim, 0x43c) != 0x900040);
    assert(state.advanceCpuCycles(uint32_t(commands.duration())) && commands.complete(state, eim, memory) == A::ok);
    assert(load(state, eim, 0x43c) == 0x900040);
    // A display word the display refuses (a Y offset past 24 bits) refuses the whole batch at the 220 write,
    // and the plane stretch before it in the batch writes nothing.
    for (uint32_t x = 0; x < 6; ++x) poke(0x900100 + x, 1, 0xee);
    assert(submit(join({stretch(0, 6, 2, 0x00800080), reg(0x136, 0x78), reg(0x10f, 0x1000000), reg(0x136, 0x7b)}),
                  false) == A::sideEffect);
    for (uint32_t x = 0; x < 6; ++x) assert(peek(0x900100 + x) == 0xee);

    // An intra macroblock with only DC coefficients: luma blocks 100 to 103, U 50, V 60. Y at a00000 pitch 16,
    // U at a00100 and V at a00200 pitch 8.
    auto block = [](bool first, int dc) {
        std::vector<uint32_t> words = first ? std::vector<uint32_t>{7, 0} : std::vector<uint32_t>{2};
        words.insert(words.end(), {1, 0x00ffffff, uint32_t(dc * 8) << 16, 0});
        return words;
    };
    auto macroblock = [](const std::vector<uint32_t>& body) {
        std::vector<uint32_t> words = {0x4a1, 0, 0xc0003200 | uint32_t(body.size() - 1) << 16};
        words.insert(words.end(), body.begin(), body.end());
        return words;
    };
    assert(submit(reg(0x402, 8)) == A::ok);
    const auto intra = join({block(true, 100), block(false, 101), block(false, 102), block(false, 103),
        block(false, 50), block(false, 60), {5, 0x20f, 0x2da00003, 0, 0, 8 << 16, 32 << 16, 8, 32, 8 << 16 | 8,
        32 << 16 | 32, 0, 0, 0x2ca00103, 0, 0, 0x2ca00203}});
    assert(submit(macroblock(intra)) == A::ok);
    for (uint32_t y = 0; y < 16; ++y)
        for (uint32_t x = 0; x < 16; ++x) assert(peek(0xa00000 + y * 16 + x) == 100u + (x >= 8) + 2 * (y >= 8));
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) assert(peek(0xa00100 + y * 8 + x) == 50 && peek(0xa00200 + y * 8 + x) == 60);
    // An AC coefficient: one horizontal cosine cycle over the block on a DC of 80.
    const auto wave = join({{7, 0, 1, 0x0000ffff, uint32_t(80 * 8) << 16 | uint32_t(uint16_t(-40)), 0}, block(false, 0),
        block(false, 0), block(false, 0), block(false, 0), block(false, 0), {5, 0x20f, 0x2da00403, 0, 0, 8 << 16,
        32 << 16, 8, 32, 8 << 16 | 8, 32 << 16 | 32, 0, 0, 0x2ca00503, 0, 0, 0x2ca00603}});
    assert(submit(macroblock(wave)) == A::ok);
    const uint32_t row[8] = {73, 74, 76, 79, 81, 84, 86, 87};
    for (uint32_t x = 0; x < 8; ++x) assert(peek(0xa00400 + x) == row[x] && peek(0xa00470 + x) == row[x]);

    // A copy from the intra picture (REF1-3) half a pixel to the right: mode b, no coefficients.
    assert(submit(join({reg(0x42e, 16 << 19 | 0xa0000), reg(0x42f, 8 << 19 | 0xa0010), reg(0x430, 8 << 19 | 0xa0020)})) == A::ok);
    auto inter = [](uint32_t mv) {
        return std::vector<uint32_t>{5, 0x20b, 0x2da00800, 0, mv, 8 << 16, (32 << 16) + mv, 8, 32 + mv, 8 << 16 | 8,
                                     (32 << 16 | 32) + mv, 0, mv, 0x30a00901, 0, mv, 0x34a00a02};
    };
    assert(submit(macroblock(inter(2 << 16))) == A::ok);
    for (uint32_t y = 0; y < 16; ++y) {
        // Column 7 averages 100 or 102 with the block to its right, rounding up; column 15 with the 0 past it.
        assert(peek(0xa00800 + y * 16 + 6) == 100u + 2 * (y >= 8));
        assert(peek(0xa00800 + y * 16 + 7) == 101u + 2 * (y >= 8));
        assert(peek(0xa00800 + y * 16 + 8) == 101u + 2 * (y >= 8));
    }
    assert(peek(0xa00900) == 50 && peek(0xa00a00) == 60);
    // Quarter pixel sources, an unknown mode and a plane word for the wrong plane are refused.
    assert(submit(macroblock(inter(1 << 16))) == A::sideEffect);
    auto badMode = inter(0);
    badMode[1] = 0x20a;
    assert(submit(macroblock(badMode)) == A::sideEffect);
    auto badPlane = inter(0);
    badPlane[13] = 0x34a00901;
    assert(submit(macroblock(badPlane)) == A::sideEffect);
    // A display word the display took at the 220 write but refuses when the engine reaches it (the player's
    // own transaction closed in between) leaves the batch unwritten, the plane stretch before it as well.
    for (uint32_t x = 0; x < 6; ++x) poke(0x900100 + x, 1, 0xee);
    assert(put(state, eim, 0x4d8, 0x78) == A::ok);
    assert(submit(join({stretch(0, 6, 2, 0x00800080), reg(0x10f, 0x900040)}), false) == A::ok);
    assert(put(state, eim, 0x4d8, 0x7b) == A::ok);
    assert(state.advanceCpuCycles(uint32_t(commands.duration())) && commands.complete(state, eim, memory) == A::sequence);
    for (uint32_t x = 0; x < 6; ++x) assert(peek(0x900100 + x) == 0xee);
    std::printf("PASS: player ring NOPs, register writes and saved word reads, plane stretch, ring overlay flip, "
                "intra and half pixel inter IDCT macroblocks, refusals\n");
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
    assert(load(state, eim, 0x210) == 0x0800000d && load(state, eim, 0x214) == 0x150000);
    assert(load(state, eim, 0x21c) == 0 && load(state, eim, 0x220) == 0 && load(state, eim, 0x140) == 0);
    assert(load(state, eim, 0x424) == 0x40001 && load(state, eim, 0x428) == 0x30001);
    assert(put(state, eim, 0x220, 0x4000) != A::ok && put(state, eim, 0x214, 0) != A::ok && put(state, eim, 0x424, 0) != A::ok);
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
    playerEngine(state, eim, memory);
}
