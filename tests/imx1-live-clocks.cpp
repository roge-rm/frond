#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "imx1_bootstrap.h"

namespace {
using A = Imx1BootstrapAccess;
constexpr uint32_t cgm = 0x0021b000, rtc = 0x00204000, watchdog = 0x00201000;
constexpr uint32_t timer1 = 0x00202000, timer2 = 0x00203000, pwm = 0x00208000;
uint32_t read(Imx1Bootstrap& b, uint32_t address) {
    uint32_t value = 0xdeadbeef;
    assert(b.access(address, 4, false, value) == A::ok);
    return value;
}
void write(Imx1Bootstrap& b, uint32_t address, uint32_t value, A expected = A::ok, uint32_t mode = 3) {
    const auto original = value;
    assert(b.access(address, 4, true, value, mode) == expected && value == original);
}
void configure(Imx1Bootstrap& b) {
    assert(b.setRtcInitialTime({1, 12, 34, 56}));
    b.setSystemPllScenario(true);
    write(b, cgm, 0x57000003, A::ok, 0);
    write(b, cgm + 4, 0x03e717bf, A::ok, 0);
    write(b, cgm, 0x57200003, A::ok, 0);
    b.advanceFastBusCycles(6000);
    assert(b.cpuClockModeSupported(3, true));
    write(b, cgm + 12, 0x123638ad);
    write(b, cgm, 0x57400003);
    assert(b.finishSystemPllBusStall() == 2 * 762752);
    write(b, cgm + 32, 2, A::ok, 0);
    write(b, timer1 + 4, 31); write(b, timer1, 0x103);
    write(b, timer2 + 4, 6); write(b, timer2, 0x103);
}
void compare(Imx1Bootstrap& a, Imx1Bootstrap& b) {
    for (uint32_t address : {cgm, cgm + 4, cgm + 12, cgm + 32, timer1 + 16, timer2 + 16,
                             rtc, rtc + 4, rtc + 20, rtc + 32, watchdog, watchdog + 8, pwm})
        assert(read(a, address) == read(b, address));
    assert(a.clockStatus() == b.clockStatus());
}
void liveDividersAndFractions() {
    Imx1Bootstrap b; configure(b);
    write(b, watchdog, 0x7f02); write(b, watchdog, 0x7f03);
    b.advanceCpuCycles(3, 3, true);
    auto unchanged = b;
    write(b, cgm, 0x5700ac03);
    assert(read(b, cgm) == 0x5700ac03 && b.cpuClockModeSupported(3, true));
    assert(read(b, timer1 + 16) == 0 && read(b, timer2 + 16) == 0);
    uint64_t weighted = 3;
    for (unsigned i = 0; i < 300; ++i) {
        b.advanceCpuCycles(1, 3, true); weighted += 2;
        const uint64_t edges = weighted * 125 * 8305664 / (uint64_t(762752) * 2835);
        assert(read(b, timer1 + 16) == edges / 96);
        assert(read(b, timer2 + 16) == edges / 21);
    }
    // The same elapsed reference duration without changing either divider gives identical timers.
    unchanged.advanceCpuCycles(600, 3, true);
    assert(read(b, timer1 + 16) == read(unchanged, timer1 + 16));
    assert(read(b, timer2 + 16) == read(unchanged, timer2 + 16));
    for (auto address : {rtc, rtc + 4, rtc + 20, rtc + 32}) assert(read(b, address) == read(unchanged, address));
    auto chunks = b;
    b.advanceCpuCycles(1000000, 3, true);
    for (unsigned i = 0; i < 1000; ++i) chunks.advanceCpuCycles(1000, 3, true);
    compare(b, chunks); weighted += 2000000;
    // BCLK / 12 FastBus time and subsequent PRESC changes retain unrelated divider fractions.
    b.advanceFastBusCycles(17); chunks.advanceFastBusCycles(17);
    write(b, cgm, 0x57002c03); write(chunks, cgm, 0x57002c03);
    for (unsigned i = 0; i < 500; ++i) {
        b.advanceCpuCycles(1, 3, true); chunks.advanceCpuCycles(1, 3, true); ++weighted;
        const uint64_t edges = weighted * 125 * 8305664 / (uint64_t(762752) * 2835) + 17 * 12;
        assert(read(b, timer1 + 16) == edges / 96);
        assert(read(b, timer2 + 16) == edges / 21);
        compare(b, chunks);
    }
    write(b, cgm, 0x57002c03); // Unchanged writes cannot reset BCLK phase.
    b.advanceCpuCycles(19, 3, true); chunks.advanceCpuCycles(19, 3, true); compare(b, chunks);
    // Maximum charge retains the same rational remainder as split charges.
    b.advanceCpuCycles(UINT32_MAX, 3, true);
    chunks.advanceCpuCycles(UINT32_MAX - 71, 3, true); chunks.advanceCpuCycles(71, 3, true);
    compare(b, chunks);
}
void prospectiveRatiosAndTransactions() {
    Imx1Bootstrap equal;
    assert(equal.setRtcInitialTime({1, 12, 34, 56}));
    write(equal, cgm, 0x57000003, A::ok, 0);
    equal.advanceCpuCycles(1, 3, false);
    auto untouched = equal;
    write(equal, cgm, 0x57008003, A::sideEffect); // FCLK = BCLK after PRESC / 2.
    compare(equal, untouched);
    // A divided bus admits that prescaler, then removing the bus divider is unsafe.
    write(equal, cgm, 0x57008403);
    untouched = equal;
    write(equal, cgm, 0x57008003, A::sideEffect);
    compare(equal, untouched);
    // A slower staged SPLL cannot hide an unsafe active output.
    Imx1Bootstrap active;
    active.setSystemPllScenario(true);
    write(active, cgm, 0x57000003, A::ok, 0);
    write(active, cgm + 12, 0x073f1437); // Staged / CLK32 = 1500; active = 3000.
    write(active, cgm, 0x57008003, A::sideEffect);
    assert(read(active, cgm) == 0x57000003);
    // A safe active ratio cannot hide an unsafe staged output.
    Imx1Bootstrap staged;
    staged.setSystemPllScenario(true);
    write(staged, cgm, 0x57000403, A::ok, 0);
    write(staged, cgm + 12, 0x003f1437); // Staged = 6000; candidate BCLK / 1 equals FCLK.
    write(staged, cgm, 0x57000003, A::sideEffect);
    assert(read(staged, cgm) == 0x57000403);
    Imx1Bootstrap b; configure(b); b.advanceCpuCycles(37, 3, true); untouched = b;
    write(b, cgm + 32, 3, A::sideEffect);
    write(b, cgm, 0x57000403, A::sideEffect, 0); // Existing FastBus live-timer guard.
    write(b, cgm, 0x57010003, A::externalInput);
    write(b, cgm, 0x57000002, A::sideEffect);
    write(b, cgm, 0x5720ac03, A::sideEffect);
    write(b, cgm, 0x5700ec03, A::reservedBits);
    compare(b, untouched);
    for (unsigned i = 0; i < 300; ++i) {
        b.advanceCpuCycles(1, 3, true); untouched.advanceCpuCycles(1, 3, true); compare(b, untouched);
    }
}
void hclkConsumers() {
    Imx1Bootstrap b; configure(b);
    write(b, cgm, 0x5700ac03);
    b.advanceCpuCycles(5, 3, true); // Nonzero /12 BCLK phase before starting reset.
    write(b, pwm, 0x10010);
    auto unchanged = b;
    write(b, cgm, 0x5700a803, A::sideEffect);
    compare(b, unchanged);
    // PRESC changes are safe while reset is pending; five HCLK edges still finish it.
    write(b, cgm, 0x57002c03);
    for (unsigned n = 0;; ++n) {
        const uint64_t systemEdges = uint64_t(10 + n) * 125 * 8305664 / (uint64_t(762752) * 2835);
        assert(bool(read(b, pwm) & 0x10000) == (systemEdges / 12 < 5));
        if (systemEdges / 12 >= 5) break;
        b.advanceCpuCycles(1, 3, true);
        assert(n < 200);
    }
    write(b, cgm, 0x57002803);
    // I2C uses HCLK. Its active transfer prevents BCLK retiming, including combined PRESC writes.
    b.setI2cScenario(true);
    write(b, 0x0021c020, 0); write(b, 0x00217008, 0x80); write(b, 0x00217008, 0xb0);
    unchanged = b;
    write(b, cgm, 0x5700ac03, A::sideEffect);
    compare(b, unchanged);
    write(b, cgm, 0x5700a803); // PRESC alone changes elapsed CPU duration, no bus divider change.
    write(b, 0x00217008, 0x80);
    write(b, cgm, 0x5700ac03);
}
void referenceTimeAndWatchdog() {
    Imx1Bootstrap b; configure(b);
    write(b, watchdog, 0x0302); write(b, watchdog, 0x0303);
    b.advanceCpuCycles(37, 3, true);
    write(b, cgm, 0x5700ac03);
    b.advanceCpuCycles(100003, 3, true);
    write(b, cgm, 0x57002c03);
    // The watchdog bound is 32768 CLK32 periods from enable, unaffected by divider writes.
    constexpr uint64_t bound = uint64_t(32768) * 762752;
    constexpr uint64_t used = uint64_t(37 + 2 * 100003) * 125;
    const uint32_t remaining = (bound - used - 1) / 125;
    b.advanceCpuCycles(remaining, 3, true);
    assert(b.clockStatus() == A::ok);
    Imx1Rtc expected;
    assert(expected.initialise({1, 12, 34, 56}));
    expected.advanceReference(4, 1); // MCU restart and explicit System PLL restart.
    expected.advanceReference(used + uint64_t(remaining) * 125, 762752);
    for (unsigned off : {0u, 4u, 20u, 32u}) {
        uint32_t value = 0; assert(expected.access(rtc + off, 4, false, value) == Imx1RtcAccess::ok);
        assert(read(b, rtc + off) == value);
    }
    b.advanceCpuCycles(1, 3, true);
    assert(b.clockStatus() == A::possibleWatchdogTimeout);
}
void manualUClockPhases() {
    constexpr uint32_t adc = 0x00215000;
    Imx1Bootstrap b(true, true, 0x8000); configure(b);
    write(b, adc + 0x2c, 3); write(b, adc + 0x14, 0x1814);
    write(b, adc + 0x10, 0x02003001); write(b, adc + 0x10, 0x02003003);
    auto chunks = b;
    b.advanceCpuCycles(137, 3, true);
    for (unsigned n = 0; n < 137; ++n) chunks.advanceCpuCycles(1, 3, true);
    assert(read(b, adc + 0x1c) == 0 && read(chunks, adc + 0x1c) == 0);
    write(b, cgm, 0x5700ac03, A::sideEffect);
    write(chunks, cgm, 0x5700ac03, A::sideEffect);
    write(b, cgm, 0x57008003); write(chunks, cgm, 0x57008003);
    b.advanceCpuCycles(10000, 3, true);
    for (unsigned n = 0; n < 100; ++n) chunks.advanceCpuCycles(100, 3, true);
    b.advanceFastBusCycles(1000);
    for (unsigned n = 0; n < 1000; ++n) chunks.advanceFastBusCycles(1);
    compare(b, chunks);
    assert(read(b, adc + 0x1c) == read(chunks, adc + 0x1c));
    write(b, cgm, 0x57000003); write(chunks, cgm, 0x57000003);
    b.advanceCpuCycles(200000, 3, true);
    for (unsigned n = 0; n < 2000; ++n) chunks.advanceCpuCycles(100, 3, true);
    compare(b, chunks);
    assert(read(b, adc + 0x1c) == 1 && read(chunks, adc + 0x1c) == 1);
    assert(read(b, adc) == 0x8000 && read(chunks, adc) == 0x8000);
}

}
int main() {
    manualUClockPhases();
    liveDividersAndFractions(); prospectiveRatiosAndTransactions(); hclkConsumers(); referenceTimeAndWatchdog();
    std::puts("PASS: asynchronous PRESC/BCLK changes, prospective ratios, timer/reference phases and HCLK guards");
}
