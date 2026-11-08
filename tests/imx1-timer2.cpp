#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "imx1_bootstrap.h"

namespace {
constexpr uint32_t timer1 = 0x00202000, timer2 = 0x00203000;
constexpr uint32_t cgm = 0x0021b000, aitc = 0x00223000;
constexpr uint32_t timerBits = (1u << 26) | (1u << 27);
void write(Imx1Bootstrap& soc, uint32_t address, uint32_t value,
           Imx1BootstrapAccess expected = Imx1BootstrapAccess::ok, unsigned mode = 0) {
    const auto actual = soc.access(address, 4, true, value, mode);
    if (actual != expected) std::fprintf(stderr, "%08x=%08x: %s expected %s\n", address, value,
        imx1BootstrapAccessName(actual), imx1BootstrapAccessName(expected));
    assert(actual == expected);
}
uint32_t read(Imx1Bootstrap& soc, uint32_t address) {
    uint32_t value = 0xdeadbeef;
    assert(soc.access(address, 4, false, value) == Imx1BootstrapAccess::ok);
    return value;
}
void clocks(Imx1Bootstrap& soc, uint32_t divider = 0) {
    write(soc, cgm, 0x57000003);
    write(soc, cgm + 32, divider);
}
void resetAndIsolation() {
    Imx1Bootstrap soc;
    for (auto base : {timer1, timer2}) {
        for (uint32_t offset = 0; offset < 24; offset += 4)
            assert(read(soc, base + offset) == (offset == 8 ? 0xffffffff : 0));
        uint32_t value = 0xdeadbeef;
        for (auto size : {1u, 2u, 8u})
            assert(soc.access(base, size, false, value) == Imx1BootstrapAccess::width);
        assert(soc.access(base + 1, 4, true, value) == Imx1BootstrapAccess::width);
        assert(soc.access(base + 24, 4, true, value) == Imx1BootstrapAccess::unmapped);
        assert(soc.access(base + 0x100, 4, false, value) == Imx1BootstrapAccess::unmapped);
        assert(value == 0xdeadbeef && !soc.instructionWord(base + 16, value));
    }
    assert(!std::strcmp(Imx1Bootstrap::registerName(timer1 + 8), "TIMER1.TCMP"));
    assert(!std::strcmp(Imx1Bootstrap::registerName(timer2 + 8), "TIMER2.TCMP"));
    assert(!Imx1Bootstrap::registerName(0x0020100c));
    assert(!Imx1Bootstrap::registerName(timer2 + 24));
    write(soc, timer2, 0x32); // Disabled output-mode configuration has no active pin effect.
    assert(read(soc, timer2) == 0x32);
    write(soc, timer2, 0x33); // Reset GPIO D31 is an input, isolating TMR2OUT.
    assert(read(soc, timer2) == 0x33 && read(soc, timer2 + 16) == 0);
    write(soc, timer2, 0);
    clocks(soc, 2); // One PERCLK1 edge per three charged FastBus cycles.
    write(soc, timer1 + 4, 1); write(soc, timer1 + 8, 3); write(soc, timer1, 0x103);
    soc.advanceFastBusCycles(5);
    write(soc, timer2 + 4, 2); write(soc, timer2 + 8, 2); write(soc, timer2, 3);
    soc.advanceFastBusCycles(1);
    assert(read(soc, timer1 + 16) == 1 && read(soc, timer2 + 16) == 0);
    soc.advanceFastBusCycles(6);
    assert(read(soc, timer1 + 16) == 2 && read(soc, timer2 + 16) == 1);
    for (unsigned i = 0; i < 100; ++i) {
        assert(read(soc, timer1 + 16) == 2 && read(soc, timer2 + 16) == 1);
    }
    soc.advanceFastBusCycles(3);
    assert(read(soc, timer1 + 16) == 2 && read(soc, timer2 + 16) == 1);
    write(soc, timer1, 0); // Timer2 keeps its prescaler phase across Timer1 disable.
    soc.advanceFastBusCycles(6);
    assert(read(soc, timer1 + 16) == 0 && read(soc, timer2 + 16) == 0);
    assert(read(soc, timer1 + 20) == 0 && read(soc, timer2 + 20) == 1);
    write(soc, timer1 + 20, 0); assert(read(soc, timer2 + 20) == 1);
    write(soc, timer2 + 20, 0); assert(read(soc, timer2 + 20) == 0);
    // Rejected divider and source writes retain the second timer's fractional progress.
    soc.advanceFastBusCycles(4);
    write(soc, cgm, 0x57000403, Imx1BootstrapAccess::sideEffect);
    write(soc, cgm + 32, 3, Imx1BootstrapAccess::sideEffect);
    write(soc, timer2 + 4, 3, Imx1BootstrapAccess::sideEffect);
    write(soc, timer2, 5, Imx1BootstrapAccess::sideEffect);
    write(soc, timer2, 0x8003, Imx1BootstrapAccess::sideEffect);
    for (auto control : {0x43u, 7u, 9u, 15u})
        write(soc, timer2, control, Imx1BootstrapAccess::externalInput);
    write(soc, timer2, 0x203, Imx1BootstrapAccess::reservedBits);
    write(soc, timer2 + 4, 256, Imx1BootstrapAccess::reservedBits);
    write(soc, timer2 + 20, 4, Imx1BootstrapAccess::reservedBits);
    write(soc, timer2 + 12, 1, Imx1BootstrapAccess::sideEffect);
    write(soc, timer2 + 16, 1, Imx1BootstrapAccess::sideEffect);
    assert(read(soc, timer2) == 3 && read(soc, timer2 + 4) == 2);
    assert(read(soc, timer2 + 12) == 0 && read(soc, timer2 + 8) == 2);
    soc.advanceFastBusCycles(5); assert(read(soc, timer2 + 16) == 1);
    write(soc, cgm + 32, 0x12); // PERCLK2 changes leave both Timer2 phases in place.
    soc.advanceFastBusCycles(9); assert(read(soc, timer2 + 16) == 0);
}
void largeAndChunkedCharges() {
    Imx1Bootstrap whole, chunks;
    for (auto soc : {&whole, &chunks}) {
        clocks(*soc, 2);
        write(*soc, timer1 + 4, 255); write(*soc, timer1, 0x105);
        write(*soc, timer2 + 4, 31); write(*soc, timer2 + 8, 37); write(*soc, timer2, 3);
    }
    whole.advanceFastBusCycles(0xffffffff);
    for (auto charge : {1u, 0x70000000u, 0x80000000u, 0xffffffeu})
        chunks.advanceFastBusCycles(charge);
    assert(read(whole, timer1 + 16) == 0xffffffffu / (3 * 256 * 16));
    assert(read(whole, timer2 + 16) == (0xffffffffu / 96) % 37);
    for (auto base : {timer1, timer2})
        for (auto offset : {0u, 4u, 8u, 12u, 16u, 20u})
            assert(read(whole, base + offset) == read(chunks, base + offset));
    // PERCLK1 can be faster than HCLK, independently for both timer prescalers.
    Imx1Bootstrap fast;
    clocks(fast, 2); write(fast, cgm, 0x57000c03);
    write(fast, timer1, 0x103); write(fast, timer2, 0x103);
    for (auto count : {1u, 2u, 4u}) {
        fast.advanceFastBusCycles(1);
        assert(read(fast, timer1 + 16) == count && read(fast, timer2 + 16) == count);
    }
}
void requestsAndTransactions() {
    for (auto base : {timer1, timer2}) {
        const unsigned source = base == timer1 ? 59 : 58;
        const uint32_t bit = 1u << (source - 32);
        Imx1Bootstrap soc;
        clocks(soc);
        write(soc, aitc + 8, source); // Known inactive even before timer configuration.
        assert(read(soc, aitc + 0x58) == 0 && soc.clockStatus() == Imx1BootstrapAccess::ok);
        write(soc, aitc + 0x50, bit, Imx1BootstrapAccess::sideEffect);
        assert(read(soc, aitc + 0x50) == 0);
        write(soc, base + 8, 3); write(soc, base, 3);
        soc.advanceFastBusCycles(3); // COMP pending while IRQEN is clear.
        assert(soc.clockStatus() == Imx1BootstrapAccess::ok && read(soc, base + 16) == 0);
        write(soc, base, 0x13, Imx1BootstrapAccess::sideEffect);
        assert(read(soc, base) == 3 && read(soc, aitc + 0x58) == 0);
        // Failed enable preserves the pending bit and read-before-clear state.
        write(soc, base + 20, 0); assert(read(soc, base + 20) == 1);
        write(soc, base + 20, 1); assert(read(soc, base + 20) == 1);
        write(soc, base + 20, 0); write(soc, base, 0x13);
        soc.advanceFastBusCycles(2); assert(soc.clockStatus() == Imx1BootstrapAccess::ok);
        soc.advanceFastBusCycles(1);
        assert(soc.clockStatus() == Imx1BootstrapAccess::sideEffect);
        assert(read(soc, base + 20) == 1 && read(soc, aitc + 0x58) == bit);
        assert(read(soc, aitc + 0x40) == source << 16);
        write(soc, base, 0x10, Imx1BootstrapAccess::sideEffect); // TEN does not clear COMP.
        assert(read(soc, base) == 0x13);
        write(soc, base, 0); // IRQEN clear deasserts the request, preserving pending status.
        assert(soc.clockStatus() == Imx1BootstrapAccess::ok && read(soc, base + 20) == 1);
        write(soc, base + 20, 0); assert(read(soc, base + 20) == 0);
        write(soc, aitc + 0x18, bit); write(soc, base, 0x13);
        soc.advanceFastBusCycles(3); // FIQ delivery has the same guard.
        assert(soc.clockStatus() == Imx1BootstrapAccess::sideEffect);
        assert(read(soc, aitc + 0x60) == bit && read(soc, aitc + 0x44) == source);
    }
    Imx1Bootstrap masked;
    clocks(masked); write(masked, aitc + 4, 0); write(masked, aitc + 0x10, timerBits);
    write(masked, timer2 + 8, 2); write(masked, timer2, 0x113);
    masked.advanceFastBusCycles(2);
    assert(masked.clockStatus() == Imx1BootstrapAccess::ok && read(masked, timer2 + 16) == 2);
    assert(read(masked, aitc + 0x58) == 1u << 26);
    write(masked, aitc + 4, 31, Imx1BootstrapAccess::sideEffect);
    write(masked, aitc + 0x18, 1u << 26, Imx1BootstrapAccess::sideEffect);
    assert(read(masked, aitc + 4) == 0 && read(masked, aitc + 0x18) == 0);
    write(masked, timer2, 0x103); write(masked, aitc + 4, 31);
    write(masked, aitc + 12, 58); write(masked, timer2, 0x113);
    write(masked, aitc + 8, 58, Imx1BootstrapAccess::sideEffect);
    assert(read(masked, aitc + 0x10) == 1u << 27);
    assert(read(masked, timer2 + 20) == 1);
    write(masked, timer2 + 20, 0); write(masked, aitc + 8, 58);
    assert(masked.clockStatus() == Imx1BootstrapAccess::ok);
}
void isolatedOutputMode() {
    constexpr uint32_t gpioD = 0x0021c300;
    Imx1Bootstrap soc;
    clocks(soc);
    write(soc, timer2, 0x32); write(soc, timer2 + 8, 5);
    write(soc, gpioD, 0x80000000); // GPIO output with reset OCR=A_IN is unqualified.
    for (auto output : {0u, 0x40000000u, 0x80000000u}) {
        write(soc, gpioD + 8, output);
        write(soc, timer2, 0x33, Imx1BootstrapAccess::sideEffect);
        assert(read(soc, timer2) == 0x32 && read(soc, timer2 + 16) == 0);
    }
    write(soc, gpioD + 8, 0xc0000000); write(soc, timer2, 0x33);
    soc.advanceFastBusCycles(4); assert(read(soc, timer2 + 16) == 4);
    write(soc, gpioD + 28, 0x80000000); // DR changes do not connect the timer output.
    assert(read(soc, gpioD + 28) == 0x80000000 && read(soc, timer2 + 16) == 4);
    write(soc, gpioD + 28, 0);
    for (auto output : {0u, 0x40000000u, 0x80000000u})
        write(soc, gpioD + 8, output, Imx1BootstrapAccess::sideEffect);
    assert(read(soc, gpioD + 8) == 0xc0000000);
    write(soc, gpioD + 32, 0x7fffffff, Imx1BootstrapAccess::sideEffect);
    assert(read(soc, gpioD + 32) == 0xffffffff);
    write(soc, gpioD + 56, 0x80000000); // GPR is irrelevant while GIUS selects GPIO.
    write(soc, gpioD + 32, 0x7fffffff, Imx1BootstrapAccess::sideEffect);
    soc.advanceFastBusCycles(1); // Counter and compare IRQ work with the output isolated.
    assert(read(soc, timer2 + 16) == 0 && read(soc, timer2 + 20) == 1);
    assert(soc.clockStatus() == Imx1BootstrapAccess::ok);
    write(soc, gpioD, 0); write(soc, gpioD + 8, 0); // Input direction disconnects OCR.
    write(soc, gpioD, 0x80000000, Imx1BootstrapAccess::sideEffect);
    assert(read(soc, gpioD) == 0 && read(soc, gpioD + 8) == 0);
    write(soc, gpioD + 32, 0x7fffffff, Imx1BootstrapAccess::sideEffect);
    write(soc, timer2, 0x32); // Disabled timer permits configuring an exposed route.
    write(soc, gpioD, 0x80000000); write(soc, gpioD + 32, 0x7fffffff);
    write(soc, timer2, 0x33, Imx1BootstrapAccess::sideEffect);
    assert(read(soc, timer2) == 0x32);
    write(soc, gpioD + 56, 0); // Primary peripheral output is also guarded.
    write(soc, timer2, 0x33, Imx1BootstrapAccess::sideEffect);
}
void asynchronousAndHeldOutput() {
    Imx1Bootstrap soc;
    clocks(soc, 2); soc.setSystemPllScenario(true);
    write(soc, cgm + 4, 0x03e717bf); write(soc, cgm, 0x57200003);
    soc.advanceFastBusCycles(6000);
    write(soc, timer1 + 4, 31); write(soc, timer1, 0x103);
    soc.advanceCpuCycles(97, 3, true);
    write(soc, timer2 + 4, 31, Imx1BootstrapAccess::ok, 3);
    write(soc, timer2, 0x103, Imx1BootstrapAccess::ok, 3);
    soc.advanceCpuCycles(762752, 3, true);
    assert(read(soc, timer1 + 16) == 3906 && read(soc, timer2 + 16) == 3906);
    // Hold with Timer1 off isolates the Timer2 divider guard and its retained phase.
    write(soc, timer1, 0, Imx1BootstrapAccess::ok, 3);
    write(soc, cgm + 12, 0x123638ad, Imx1BootstrapAccess::ok, 3);
    write(soc, cgm, 0x57400003, Imx1BootstrapAccess::ok, 3);
    const auto before = read(soc, timer2 + 16);
    soc.advanceCpuCycles(12000, 3, true);
    assert(read(soc, timer2 + 16) == before);
    auto continued = soc;
    assert(soc.finishSystemPllBusStall() != 0);
    assert(read(soc, timer2 + 16) == before);
    soc.advanceCpuCycles(37, 3, true);
    assert(read(soc, timer2 + 16) == before); // Retained prescaler phase, below next edge.
    soc.advanceCpuCycles(100000, 3, true);
    assert(read(soc, timer2 + 16) > before);
    // Splitting an activation inside a maximum charge must give identical final timers.
    auto chunks = continued;
    continued.advanceCpuCycles(0xffffffff, 3, true);
    chunks.advanceCpuCycles(200, 3, true);
    chunks.advanceCpuCycles(0xffffffff - 200, 3, true);
    for (auto base : {timer1, timer2})
        for (auto offset : {16u, 20u}) assert(read(continued, base + offset) == read(chunks, base + offset));
    const auto count = read(continued, timer2 + 16);
    continued.advanceCpuCycles(762752, 3, true);
    // New System PLL output supplies floor(8305664 * 125 / 2835) edges per this charge.
    assert(read(continued, timer2 + 16) - count >= 3814);
}
}
int main() {
    resetAndIsolation(); largeAndChunkedCharges(); requestsAndTransactions(); isolatedOutputMode(); asynchronousAndHeldOutput();
    std::puts("PASS: two timers, independent phases, PERCLK1 ratios, held output, pending requests and guarded CPU delivery");
}
