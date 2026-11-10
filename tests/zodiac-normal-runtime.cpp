// The Zodiac's view, pen, stick and EEPROM identity as the normal runtime works them out.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "zodiac_panel.h"

namespace {
void rotation() {
    std::vector<uint32_t> panel(size_t(zodiacPanelWidth) * zodiacPanelHeight), view(size_t(zodiacViewWidth) * zodiacViewHeight, ~0u);
    for (uint32_t sy = 0; sy < zodiacPanelHeight; ++sy)
        for (uint32_t sx = 0; sx < zodiacPanelWidth; ++sx) panel[size_t(sy) * zodiacPanelWidth + sx] = sy << 16 | sx;
    zodiacRotateFrame(panel.data(), view.data());
    // A quarter turn clockwise: the panel's top left is the view's top right, its top row the right column.
    auto at = [&](uint32_t x, uint32_t y) { return view[size_t(y) * zodiacViewWidth + x]; };
    assert(at(479, 0) == 0 && at(479, 319) == 319 && at(0, 0) == (479u << 16) && at(0, 319) == (479u << 16 | 319));
    for (uint32_t y = 0; y < zodiacViewHeight; ++y)
        for (uint32_t x = 0; x < zodiacViewWidth; ++x) assert(at(x, y) == ((479 - x) << 16 | y));
}

void pen() {
    uint32_t x = 0, y = 0;
    // The corners of the virtual panel, and the tap that pressed Next at Welcome.
    zodiacPenRaw(0, 0, x, y); assert(x == 366 && y == 32);
    zodiacPenRaw(480, 320, x, y); assert(x == 36 && y == 382);
    zodiacPenRaw(440, 305, x, y); assert(x == 51 && y == 353);
    zodiacPenRaw(-50, 900, x, y); assert(x == 36 && y == 32);
    const auto down = zodiacPenInputs(true, 440, 305), up = zodiacPenInputs(false, 440, 305);
    assert(down.contactKnown && down.normalKnown && down.referenceKnown && down.contactDown && !up.contactDown);
    assert(down.normal[0] == 0x1100 && down.normal[1] == 0x1100 + (51 << 7) && down.normal[2] == 0x1100 + (353 << 7) &&
           down.normal[3] == 0x1234);
    assert(down.reference[0] == 0x0180 && down.reference[1] == 0x3280 && down.reference[2] == 0x7100);
    assert(up.normal == down.normal && up.reference == down.reference);
}

void stick() {
    assert(zodiacStickOffset(0) == 0 && zodiacStickOffset(1) == 511 && zodiacStickOffset(-1) == -511);
    assert(zodiacStickOffset(7) == 511 && zodiacStickOffset(-7) == -511 && zodiacStickOffset(0.5f) == 256);
    assert(zodiacStickOffset(0.0f / 0.0f) == 0);
}

void put32le(std::vector<uint8_t>& rom, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) rom[at + i] = uint8_t(v >> (8 * i)); }
void put32be(std::vector<uint8_t>& rom, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) rom[at + i] = uint8_t(v >> (24 - 8 * i)); }

// A made-up ROM with the layout the identity is read from: packed globals at 0x400 and their
// metadata at 0x300 past the big card, the data in literal blocks.
std::vector<uint8_t> rom() {
    constexpr size_t card = 0x20000;
    std::vector<uint8_t> rom(card + 0x1000);
    put32le(rom, card + 0x78, 0x400);
    put32le(rom, card + 0xac, 0x300);
    put32be(rom, card + 0x300 + 4, 0x9a24);
    put32be(rom, card + 0x400 + 4, 0u - 0x9a24);
    size_t at = card + 0x408, value = 0;
    for (size_t length : {100, 128, 128, 128, 92}) {
        rom[at++] = uint8_t(0x80 | (length - 1));
        for (size_t i = 0; i < length; ++i, ++value) rom[at++] = uint8_t(value * 7 + 3);
    }
    return rom;
}

void eeprom() {
    auto good = rom();
    std::array<uint8_t, 256> image{};
    assert(zodiacEepromIdentity(good.data(), good.size(), image));
    for (size_t i = 0; i < 256; ++i)
        assert(image[i] == (i >= 0x50 && i < 0xd0 ? uint8_t((0x1c0 + i - 0x50) * 7 + 3) : 0xff));
    // Anything else is refused and the image left alone.
    const auto kept = image;
    auto refused = [&](const std::vector<uint8_t>& bad) {
        return !zodiacEepromIdentity(bad.data(), bad.size(), image) && image == kept;
    };
    assert(!zodiacEepromIdentity(nullptr, 0, image));
    auto bad = good; bad.resize(0x20000 + 0x40); assert(refused(bad));
    bad = good; put32be(bad, 0x20000 + 0x304, 0x9a28); assert(refused(bad));
    bad = good; put32be(bad, 0x20000 + 0x404, 0x9a24); assert(refused(bad));
    bad = good; put32le(bad, 0x20000 + 0x78, 0x7fffffff); assert(refused(bad));
    bad = good; put32le(bad, 0x20000 + 0xac, 0xfffffff0); assert(refused(bad));
    // A block that isn't a literal, and literal blocks that run off the end.
    bad = good; bad[0x20000 + 0x408] = 0x20; assert(refused(bad));
    bad = good; bad.resize(0x20000 + 0x408 + 200); assert(refused(bad));
}
}  // namespace

int main() {
    rotation();
    pen();
    stick();
    eeprom();
    std::puts("PASS: the Zodiac's view turn, pen samples, stick offsets and EEPROM identity");
    return 0;
}
