#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "imx1_bootstrap.h"

int main() {
    Imx1Timer timer;
    auto read = [&](uint32_t offset, uint32_t expected) {
        uint32_t value = 0xdeadbeef;
        assert(timer.access(offset, 4, false, value) == Imx1TimerAccess::ok);
        if (value != expected) std::fprintf(stderr, "read %x: got %08x expected %08x\n", offset, value, expected);
        assert(value == expected);
    };
    auto write = [&](uint32_t offset, uint32_t value,
                     Imx1TimerAccess expected = Imx1TimerAccess::ok) {
        assert(timer.access(offset, 4, true, value) == expected);
    };
    for (uint32_t offset : {0u, 4u, 12u, 16u, 20u}) read(offset, 0);
    read(8, 0xffffffff);
    timer.advance(10000); read(16, 0);
    write(4, 31); write(8, 3); write(0, 0x103);
    timer.advance(31); read(16, 0);
    for (unsigned i = 0; i < 100; ++i) read(16, 0);
    timer.advance(1); read(16, 1);
    timer.advance(64); read(16, 3);
    // A write without a preceding set-status read cannot clear the event.
    write(20, 0); read(20, 1); write(20, 1); read(20, 1);
    write(20, 0); read(20, 0);
    timer.advance(32); read(16, 4); read(20, 0);
    // Reading clear status must not arm clearing an event that arrives later.
    write(8, 5); read(20, 0); timer.advance(32);
    write(20, 0); read(20, 1); write(20, 0); read(20, 0);
    // Full-width rollover and a compare at zero, without per-tick host loops.
    timer.advance((uint64_t(1) << 32) * 32 - 5 * 32); read(16, 0);
    write(8, 0); read(20, 0); write(20, 0);
    timer.advance((uint64_t(1) << 32) * 32); read(16, 0); read(20, 1);
    write(20, 0); write(0, 0); read(16, 0);
    // Restart mode resets on the compare edge and continues through many periods.
    write(4, 0); write(8, 3); write(0, 3);
    timer.advance(2); read(16, 2); read(20, 0);
    timer.advance(1); read(16, 0); read(20, 1);
    timer.advance(11); read(16, 2);
    write(20, 0); read(20, 0);
    write(0, 0); write(0, 0x105); // PERCLK1 / 16, free run
    timer.advance(15); read(16, 0); timer.advance(1); read(16, 1);
    write(4, 1, Imx1TimerAccess::sideEffect); read(4, 0);
    write(0, 0x103, Imx1TimerAccess::sideEffect); read(0, 0x105);
    for (uint32_t value : {0x8001u, 0x115u, 0x125u})
        write(0, value, Imx1TimerAccess::sideEffect);
    for (uint32_t value : {0x145u, 0x107u, 0x109u, 0x10fu})
        write(0, value, Imx1TimerAccess::externalInput);
    read(0, 0x105);
    write(0, 0x10000, Imx1TimerAccess::reservedBits);
    write(0, 0x200, Imx1TimerAccess::reservedBits);
    write(4, 256, Imx1TimerAccess::reservedBits);
    write(20, 4, Imx1TimerAccess::reservedBits);
    write(12, 1, Imx1TimerAccess::sideEffect);
    write(16, 1, Imx1TimerAccess::sideEffect);
    uint32_t untouched = 0xdeadbeef;
    for (uint32_t size : {1u, 2u, 8u})
        assert(timer.access(0, size, false, untouched) == Imx1TimerAccess::width);
    assert(timer.access(1, 4, true, untouched) == Imx1TimerAccess::width);
    assert(timer.access(24, 4, false, untouched) == Imx1TimerAccess::unmapped);
    assert(untouched == 0xdeadbeef);
    write(0, 0); write(0, 0x101); // Stop-count source holds the count.
    timer.advance(100); read(16, 0);
    write(0, 0); write(4, 255); write(0, 0x103);
    timer.advance(255); read(16, 0); timer.advance(1); read(16, 1);

    Imx1Bootstrap soc;
    auto busWrite = [&](uint32_t address, uint32_t value,
                        Imx1BootstrapAccess expected = Imx1BootstrapAccess::ok) {
        assert(soc.access(address, 4, true, value) == expected);
    };
    auto busRead = [&](uint32_t address, uint32_t expected) {
        uint32_t value = 0xdeadbeef;
        assert(soc.access(address, 4, false, value) == Imx1BootstrapAccess::ok);
        assert(value == expected);
    };
    busWrite(0x0021b000, 0x57000003); busWrite(0x0021b020, 0x00100012);
    busWrite(0x00202004, 31); busWrite(0x00202000, 0x103);
    soc.advanceFastBusCycles(95); busRead(0x00202010, 0);
    for (unsigned i = 0; i < 100; ++i) busRead(0x00202010, 0);
    soc.advanceFastBusCycles(1); busRead(0x00202010, 1);
    for (unsigned i = 0; i < 96; ++i) soc.advanceFastBusCycles(1);
    busRead(0x00202010, 2);
    busWrite(0x0021b000, 0x57000403, Imx1BootstrapAccess::sideEffect);
    busWrite(0x0021b020, 0x00100013, Imx1BootstrapAccess::sideEffect);
    busRead(0x0021b000, 0x57000003); busRead(0x0021b020, 0x00100012);
    // Other peripheral divider changes do not alter Timer 1's input clock.
    busWrite(0x0021b020, 0x00100022);
    soc.advanceFastBusCycles(96); busRead(0x00202010, 3);
    busWrite(0x00202000, 0); busRead(0x00202010, 0);
    // BCLK may be slower than PERCLK1; keep both integer ratios and phase exact.
    busWrite(0x0021b000, 0x57000c03); busWrite(0x0021b020, 0x00100012);
    busWrite(0x00202004, 0); busWrite(0x00202000, 0x103);
    soc.advanceFastBusCycles(1); busRead(0x00202010, 1);
    soc.advanceFastBusCycles(1); busRead(0x00202010, 2);
    soc.advanceFastBusCycles(1); busRead(0x00202010, 4);
    assert(!soc.instructionWord(0x00202010, untouched));
    assert(soc.access(0x00203000, 4, false, untouched) == Imx1BootstrapAccess::unmapped);
    assert(!std::strcmp(Imx1Bootstrap::registerName(0x00202010), "TIMER1.TCN"));
    std::puts("PASS: Timer 1 reset, elapsed clocks, prescaling, compare/restart/rollover, read-before-w0c and explicit gates");
}
