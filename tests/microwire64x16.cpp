#include "Microwire64x16.h"
#include <cassert>
#include <cstdio>

static void edge(Microwire64x16& e, bool bit) {
    e.Pins(true, false, true, bit);
    e.Pins(true, true, true, bit);
    assert(Microwire64x16::Valid(e.Save()));
}
static void bits(Microwire64x16& e, unsigned value, unsigned count) {
    for (unsigned n = count; n; --n) edge(e, (value >> (n - 1)) & 1);
}
static void command(Microwire64x16& e, unsigned op, unsigned address) {
    e.Pins(false, false, false, false);
    e.Pins(true, false, true, false);
    bits(e, 0x100 | op << 6 | address, 9);
}
static uint16_t readWord(Microwire64x16& e) {
    uint16_t result = 0;
    for (unsigned n = 0; n < 16; ++n) {
        e.Pins(true, false, false, false);
        e.Pins(true, true, false, false);
        const auto output = e.DataOut();
        assert(output.driven);
        result = uint16_t(result << 1 | output.high);
    }
    return result;
}
static void program(Microwire64x16& e, unsigned address, uint16_t value) {
    command(e, 1, address);
    bits(e, value, 16);
    e.Pins(false, false, false, false);
}
int main() {
    Microwire64x16 e;
    assert(Microwire64x16::Valid(e.Save()));
    // The original Sony routine sends leading zero clocks, 110, A5..A0, then samples 16 bits.
    e.Pins(true, false, true, false); bits(e, 0, 6); bits(e, 0x180 | 63, 9);
    assert(e.DataOut().driven && !e.DataOut().high);
    assert(readWord(e) == 0xffff && readWord(e) == 0xffff);
    program(e, 7, 0x1234); e.Advance(Microwire64x16::writeTimeNs);
    assert(e.Save().words[7] == 0xffff);
    command(e, 0, 48); // EWEN.
    program(e, 7, 0x1234);
    e.Pins(true, false, false, false);
    assert(e.DataOut().driven && !e.DataOut().high);
    e.Advance(2000000);
    Microwire64x16 fresh;
    assert(fresh.Load(e.Save()));
    fresh.Advance(4000000);
    assert(fresh.DataOut().driven && fresh.DataOut().high);
    command(fresh, 2, 7); assert(readWord(fresh) == 0x1234);
    fresh.ResetPins(); assert(fresh.Save().words[7] == 0x1234);

    command(fresh, 1, 63); bits(fresh, 0x69, 8);
    Microwire64x16 resumed; assert(resumed.Load(fresh.Save()));
    bits(resumed, 0xa5, 8); resumed.ResetPins(); resumed.Advance(6000000);
    command(resumed, 2, 63); assert(readWord(resumed) == 0x69a5 && readWord(resumed) == 0xffff);
    auto bad = resumed.Save(); bad.commandBits = 0xffffffff;
    assert(!resumed.Load(bad)); assert(readWord(resumed) == 0xffff);

    command(resumed, 1, 7); bits(resumed, 0xab, 8); resumed.ResetPins();
    assert(resumed.Save().words[7] == 0x1234);
    program(resumed, 7, 0xbeef); resumed.PowerCycle(); resumed.Advance(6000000);
    assert(resumed.Save().words[7] == 0x1234 && !resumed.Save().enabled);
    command(resumed, 0, 48); command(resumed, 3, 7); resumed.ResetPins(); resumed.Advance(6000000);
    assert(resumed.Save().words[7] == 0xffff);
    command(resumed, 0, 16); bits(resumed, 0xa55a, 16); resumed.ResetPins();
    resumed.Advance(14999999); assert(resumed.Save().words[0] == 0xffff);
    resumed.Advance(1); for (auto word : resumed.Save().words) assert(word == 0xa55a);
    command(resumed, 0, 32); resumed.ResetPins(); resumed.Advance(6000000);
    for (auto word : resumed.Save().words) assert(word == 0xffff);
    command(resumed, 0, 0); program(resumed, 7, 1); resumed.Advance(6000000);
    assert(resumed.Save().words[7] == 0xffff);
    // Invalid continuation fields never replace a valid running device.
    for (unsigned field = 0; field < 9; ++field) {
        auto invalid = resumed.Save();
        switch (field) {
            case 0: invalid.selected = 2; break;
            case 1: invalid.phase = 99; break;
            case 2: invalid.remainingNs = 1; break;
            case 3: invalid.pending = 1; break;
            case 4: invalid.address = 64; break;
            case 5: invalid.readBit = 17; break;
            case 6: invalid.commandValue = 512; break;
            case 7: invalid.data = 65536; break;
            case 8: invalid.queued = 5; break;
        }
        assert(!resumed.Load(invalid));
    }
    command(resumed, 1, 7); bits(resumed, 0x55, 8);
    resumed.Pins(true, false, false, false);
    resumed.Pins(true, true, false, false);
    assert(Microwire64x16::Valid(resumed.Save()));
    resumed.ResetPins(); resumed.Advance(6000000);
    assert(resumed.Save().words[7] == 0xffff);
    // A snapshot at every partial command, write and sequential read clock is portable.
    for (unsigned count = 0; count <= 9; ++count) {
        Microwire64x16 partial;
        partial.Pins(true, false, true, false);
        bits(partial, 0x187 >> (9 - count), count);
        Microwire64x16 copy;
        assert(copy.Load(partial.Save()));
        bits(copy, 0x187, 9 - count);
        assert(readWord(copy) == 0xffff);
    }
    std::puts("Microwire64x16 protocol, interrupted write and portable continuation passed");
}
