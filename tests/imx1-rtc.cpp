#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include "imx1_bootstrap.h"

constexpr uint32_t base = 0x00204000;
using A = Imx1RtcAccess;
using B = Imx1BootstrapAccess;
static uint32_t read(Imx1Rtc& rtc, unsigned offset, A expected = A::ok) {
    uint32_t value = 0xdeadbeef;
    assert(rtc.access(base + offset, 4, false, value) == expected);
    if (expected != A::ok) assert(value == 0xdeadbeef);
    return value;
}
static void write(Imx1Rtc& rtc, unsigned offset, uint32_t value, A expected = A::ok) {
    const auto actual = rtc.access(base + offset, 4, true, value);
    if (actual != expected) std::fprintf(stderr, "RTC offset=%x value=%x actual=%d expected=%d\n", offset, value, int(actual), int(expected));
    assert(actual == expected);
}
static uint32_t read(Imx1Bootstrap& rtc, uint32_t address) {
    uint32_t value = 0xdeadbeef;
    assert(rtc.access(address, 4, false, value) == B::ok);
    return value;
}
static void write(Imx1Bootstrap& rtc, uint32_t address, uint32_t value, B expected = B::ok, unsigned mode = 0) {
    const auto result = rtc.access(address, 4, true, value, mode);
    if (result != expected) std::fprintf(stderr, "%08x=%08x: %s expected %s\n", address, value, imx1BootstrapAccessName(result), imx1BootstrapAccessName(expected));
    assert(result == expected);
}
static void same(Imx1Rtc& a, Imx1Rtc& b) {
    for (unsigned offset = 0; offset < 40; offset += 4) assert(read(a, offset) == read(b, offset));
}
static void resetAndRegisters() {
    Imx1Rtc rtc;
    for (unsigned offset : {0u, 4u, 32u}) read(rtc, offset, A::externalInput);
    assert(read(rtc, 8) == 0 && read(rtc, 12) == 0 && read(rtc, 36) == 0);
    assert(read(rtc, 16) == 0x80 && read(rtc, 20) == 0 && read(rtc, 24) == 0 && read(rtc, 28) == 63);
    rtc.advanceReference(1000000000, 1);
    for (unsigned offset : {0u, 4u, 32u}) read(rtc, offset, A::externalInput);
    assert(!rtc.initialise({1, 2, 3, 4}));
    write(rtc, 24, 0x10, A::externalInput);
    assert(read(rtc, 24) == 0 && rtc.assertedInterrupts() == 0);
    write(rtc, 0, 0, A::externalInput);
    write(rtc, 16, 0x81, A::sideEffect);
    write(rtc, 16, 0, A::sideEffect);
    write(rtc, 16, 0xc0, A::reservedBits);
    write(rtc, 16, 0xa0, A::sideEffect);
    write(rtc, 24, 0x80, A::sideEffect);
    write(rtc, 24, 1, A::sideEffect);
    write(rtc, 28, 4, A::sideEffect);
    for (unsigned offset = 0; offset < 40; offset += 4) {
        for (unsigned size : {1u, 2u, 8u}) {
            uint32_t value = 0xdeadbeef;
            assert(rtc.access(base + offset, size, false, value) == A::width && value == 0xdeadbeef);
            assert(rtc.access(base + offset + 1, 4, true, value) == A::width);
        }
    }
    uint32_t value = 0;
    assert(!Imx1Rtc::registerName(base - 4) && !Imx1Rtc::registerName(base + 40));
    assert(rtc.access(base + 40, 4, true, value) == A::unmapped);
    Imx1Rtc seeded;
    for (auto time : {Imx1RtcTime{512, 0, 0, 0}, {0, 24, 0, 0}, {0, 0, 60, 0}, {0, 0, 0, 60}})
        assert(!seeded.initialise(time));
    assert(seeded.initialise({511, 23, 59, 59}) && !seeded.initialise({0, 0, 0, 0}));
    assert(read(seeded, 0) == 0x173b && read(seeded, 4) == 59 && read(seeded, 32) == 511);
    for (unsigned offset : {0u, 8u}) {
        for (unsigned value : {0x183bu, 0x173cu, 0x1740u, 0x2359u, 0x10000u}) write(seeded, offset, value, A::reservedBits);
    }
    for (unsigned offset : {4u, 12u}) for (unsigned value : {60u, 63u, 64u}) write(seeded, offset, value, A::reservedBits);
    for (unsigned offset : {32u, 36u}) write(seeded, offset, 512, A::reservedBits);
    write(seeded, 20, 64, A::reservedBits); write(seeded, 24, 64, A::reservedBits);
    assert(read(seeded, 0) == 0x173b && read(seeded, 4) == 59 && read(seeded, 32) == 511);
    write(seeded, 0, 0x0c22); write(seeded, 4, 56); write(seeded, 32, 1);
    write(seeded, 8, 0x173b); write(seeded, 12, 59); write(seeded, 36, 511);
    for (unsigned i = 0; i < 100; ++i) assert(read(seeded, 4) == 56);
    assert(read(seeded, 0) == 0x0c22 && read(seeded, 32) == 1);
    assert(read(seeded, 8) == 0x173b && read(seeded, 12) == 59 && read(seeded, 36) == 511);
}
static void elapsedAndEvents() {
    for (unsigned crystal : {0u, 1u, 3u}) {
        const unsigned divisor = crystal == 1 ? 32000 : 32768;
        Imx1Rtc a, b;
        assert(a.initialise({511, 23, 59, 59}) && b.initialise({511, 23, 59, 59}));
        for (auto* rtc : {&a, &b}) {
            write(*rtc, 16, 0x80 | crystal << 5);
            write(*rtc, 24, 0xff3e);
        }
        for (unsigned i = 0; i < divisor - 1; ++i) a.advanceReference(1, 1);
        b.advanceReference(divisor - 1, 1);
        same(a, b);
        assert(read(a, 4) == 59 && read(a, 20) == 0xff00);
        a.advanceReference(1, 1); b.advanceReference(1, 1); same(a, b);
        assert(read(a, 0) == 0 && read(a, 4) == 0 && read(a, 32) == 0 && read(a, 20) == 0xff3e);
        assert(a.assertedInterrupts() == Imx1Rtc::interruptSources);
        write(a, 20, 0xff00); assert(read(a, 20) == 0x3e && a.assertedInterrupts() == (uint64_t(1) << 17));
        write(a, 20, 0x3e); assert(read(a, 20) == 0 && a.assertedInterrupts() == 0);
        // A W1C read cannot generate an event, and disabled events do not latch.
        write(a, 24, 0); a.advanceReference(uint64_t(divisor) * 86400, 1);
        assert(read(a, 20) == 0 && read(a, 32) == 1);
        write(a, 16, 0x80 | (crystal == 1 ? 0u : 0x20u), A::sideEffect);
    }
    // XTL 00 and 11 select the same divider, including an existing fraction.
    Imx1Rtc alias, original;
    for (auto* rtc : {&alias, &original}) {
        assert(rtc->initialise({1, 12, 34, 56}));
        rtc->advanceReference(uint64_t(32768) * 3000 - 1, 3000);
        write(*rtc, 24, 0x10);
    }
    write(alias, 16, 0xe0);
    for (auto* rtc : {&alias, &original}) rtc->advanceReference(1, 3000);
    for (unsigned offset : {0u, 4u, 20u, 32u}) assert(read(alias, offset) == read(original, offset));
    assert(read(alias, 4) == 57 && read(alias, 20) == 0x10);
    write(alias, 16, 0x80);
    Imx1Rtc matching;
    assert(matching.initialise({0, 0, 0, 0}));
    write(matching, 24, 4, A::sideEffect); assert(read(matching, 24) == 0);
    write(matching, 12, 1); write(matching, 24, 4);
    write(matching, 12, 0, A::sideEffect); write(matching, 4, 1, A::sideEffect);
    assert(read(matching, 4) == 0 && read(matching, 12) == 1);
    matching.advanceReference(32768, 1); assert(read(matching, 20) == 4);
    write(matching, 24, 0); assert(read(matching, 20) == 4 && matching.assertedInterrupts() == 0);
    write(matching, 24, 4); assert(matching.assertedInterrupts() == (uint64_t(1) << 17));
    write(matching, 20, 4); assert(read(matching, 20) == 0);
    matching.advanceReference(uint64_t(512) * 86400 * 32768, 1);
    assert(read(matching, 20) == 4 && read(matching, 4) == 1);
    // Each sample bit uses its own tap, including the 32 kHz divisor.
    for (unsigned bit = 8; bit < 16; ++bit) {
        Imx1Rtc sample;
        assert(sample.initialise({1, 12, 34, 56}));
        write(sample, 16, 0xa0); write(sample, 24, 1u << bit);
        sample.advanceReference((1u << (21 - bit)) - 1, 1); assert(read(sample, 20) == 0);
        sample.advanceReference(1, 1); assert(read(sample, 20) == (1u << bit));
    }
}
static void fractionsAndBounds() {
    Imx1Rtc a, b;
    assert(a.initialise({1, 12, 34, 56}) && b.initialise({1, 12, 34, 56}));
    for (auto* rtc : {&a, &b}) write(*rtc, 24, 0xff3e);
    for (unsigned i = 0; i < 32000; ++i) a.advanceReference(125, 762752);
    b.advanceReference(uint64_t(32000) * 125, 762752);
    same(a, b);
    for (auto* rtc : {&a, &b}) {
        rtc->advanceReference(17, 3000);
        rtc->advanceReference(uint64_t(73) * 2835, 8305664);
    }
    a.advanceReference(58, 1); b.advanceReference(58, 1);
    for (unsigned i = 0; i < 10000; ++i) {
        write(a, 20, 0xff3e); write(b, 20, 0xff3e);
        a.advanceReference(125, 762752); b.advanceReference(125, 762752); same(a, b);
    }
    a.advanceReference(UINT64_MAX, 16384);
    b.advanceReference(UINT64_MAX - 17, 16384); b.advanceReference(17, 16384); same(a, b);
    assert(!a.referenceRatioSupported(0));
    assert(!a.referenceRatioSupported(UINT64_MAX));
    // Adding a small new ratio after large elapsed charges keeps all subperiods.
    a.advanceReference(32768 * 3000, 3000); b.advanceReference(32768 * 3000, 3000); same(a, b);
}
static void configure(Imx1Bootstrap& soc) {
    assert(soc.setRtcInitialTime({1, 12, 34, 56}));
    soc.setSystemPllScenario(true);
    write(soc, 0x0021b000, 0x57000003);
    write(soc, 0x0021b004, 0x03e717bf);
    write(soc, 0x0021b000, 0x57200003);
    soc.advanceFastBusCycles(6000);
    assert(soc.cpuClockModeSupported(3, true));
}
static void clocksAndInterrupts() {
    Imx1Bootstrap a;
    configure(a);
    for (auto* soc : {&a}) {
        write(*soc, base + 24, 0x10);
        soc->advanceCpuCycles(37, 3, true);
        write(*soc, 0x0021b00c, 0x123638ad, B::ok, 3);
        write(*soc, 0x0021b000, 0x57400003, B::ok, 3);
    }
    a.advanceCpuCycles(17, 3, true);
    assert(a.finishSystemPllBusStall() == 2 * 762752 - 17 * 125);
    a.advanceCpuCycles(762751, 3, true);
    a.advanceFastBusCycles(73);
    // Compare stalled progression against an independently computed exact reference clock.
    Imx1Rtc expected;
    assert(expected.initialise({1, 12, 34, 56}));
    write(expected, 24, 0x10);
    expected.advanceReference(6000, 3000);
    expected.advanceReference(37 * 125 + 2 * 762752 + uint64_t(762751) * 125, 762752);
    expected.advanceReference(73 * 2835, 8305664);
    for (unsigned off : {0u, 4u, 20u, 32u}) assert(read(a, base + off) == read(expected, off));
    // Max core charges and chunked charges retain subsecond phase across CPU modes.
    auto chunks = a;
    a.advanceCpuCycles(UINT32_MAX, 3, true);
    chunks.advanceCpuCycles(UINT32_MAX - 17, 3, true); chunks.advanceCpuCycles(17, 3, true);
    for (unsigned i = 0; i < 500; ++i) {
        a.advanceCpuCycles(1, 3, true); chunks.advanceCpuCycles(1, 3, true);
        for (unsigned off : {0u, 4u, 20u, 32u}) assert(read(a, base + off) == read(chunks, base + off));
    }
    // The RTC seed cannot qualify a programmed, unrestarted or unsettled CPU PLL.
    Imx1Bootstrap unqualified;
    assert(unqualified.setRtcInitialTime({1, 12, 34, 56}));
    write(unqualified, 0x0021b000, 0x57000003);
    write(unqualified, 0x0021b004, 0x03e717bf);
    assert(!unqualified.cpuClockModeSupported(3, true));
    write(unqualified, 0x0021b000, 0x57200003);
    assert(!unqualified.cpuClockModeSupported(3, true));
    unqualified.advanceFastBusCycles(6000);
    assert(!unqualified.cpuClockModeSupported(3, false));
    assert(unqualified.cpuClockModeSupported(3, true));
    write(unqualified, 0x0021b00c, 0x123638ad, B::sideEffect, 3);
    assert(read(unqualified, 0x0021b00c) == 0x043f1437);
    // Rejected clock writes leave both clock registers and elapsed RTC phase unchanged.
    auto untouched = a;
    write(a, 0x0021b004, 0x003f1437, B::sideEffect, 3);
    write(a, 0x0021b020, 3, B::sideEffect, 3);
    assert(read(a, 0x0021b000) == read(untouched, 0x0021b000));
    assert(read(a, 0x0021b004) == read(untouched, 0x0021b004));
    for (unsigned i = 0; i < 500; ++i) {
        a.advanceCpuCycles(1, 3, true); untouched.advanceCpuCycles(1, 3, true);
        assert(read(a, base + 4) == read(untouched, base + 4));
    }
    // A two-reference-period SPLL bus stall crosses a sampling boundary.
    Imx1Bootstrap edge;
    configure(edge);
    write(edge, base + 24, 0x8000);
    edge.advanceCpuCycles(366121, 3, true);
    assert(read(edge, base + 20) == 0);
    write(edge, 0x0021b00c, 0x123638ad, B::ok, 3);
    write(edge, 0x0021b000, 0x57400003, B::ok, 3);
    assert(read(edge, base + 20) == 0);
    edge.finishSystemPllBusStall();
    assert(read(edge, base + 20) == 0x8000);
    // Reset RTC requests are known clear, even with retained TOD unknown.
    Imx1Bootstrap irq;
    write(irq, 0x00223008, 17); write(irq, 0x00223008, 18);
    assert(irq.clockStatus() == B::ok);
    uint32_t value = 0x10;
    assert(irq.access(base + 24, 4, true, value) == B::externalInput);
    assert(read(irq, base + 24) == 0);
    Imx1Bootstrap pending;
    assert(pending.setRtcInitialTime({1, 12, 34, 56}));
    write(pending, 0x00223008, 17); write(pending, base + 24, 0x10);
    pending.advanceFastBusCycles(32768 * 3000 / 12);
    assert(read(pending, base + 20) == 0x10 && pending.clockStatus() == B::sideEffect);
    write(pending, base + 24, 0); assert(pending.clockStatus() == B::ok);
    write(pending, base + 24, 0x10, B::sideEffect); assert(read(pending, base + 24) == 0);
    write(pending, base + 20, 0x10); write(pending, base + 24, 0x10);
    assert(pending.clockStatus() == B::ok);
    Imx1Bootstrap sampled;
    assert(sampled.setRtcInitialTime({1, 12, 34, 56}));
    write(sampled, base + 24, 0x8000); sampled.advanceFastBusCycles(64 * 3000 / 12);
    assert(read(sampled, base + 20) == 0x8000);
    write(sampled, 0x00223008, 18, B::sideEffect);
    assert(read(sampled, 0x00223014) == 0);
    write(sampled, base + 20, 0x8000); write(sampled, 0x00223008, 18);
}
int main() {
    resetAndRegisters(); elapsedAndEvents(); fractionsAndBounds(); clocksAndInterrupts();
    std::puts("PASS: RTC retained domain, explicit time, binary ranges, elapsed CLK32, events, phases, rollover, SPLL stall and IRQ guards");
}
