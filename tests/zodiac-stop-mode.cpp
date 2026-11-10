#include <cassert>
#include <cstdio>
#include "imx1_boot_probe.h"

namespace {
using A = Imx1BootstrapAccess;
constexpr uint32_t cgm = 0x21b000, aitcEnable = 0x223008, rtcSeconds = 0x204004, rtcEnables = 0x204018;
constexpr uint32_t run = 0x57003c03, stop = 0x57003c00;
void w(Imx1Bootstrap& b, uint32_t a, uint32_t v, unsigned mode = 0, A status = A::ok) {
    const auto actual = b.access(a, 4, true, v, mode);
    if (actual != status) std::fprintf(stderr, "%08x=%08x mode%u: %s\n", a, v, mode, imx1BootstrapAccessName(actual));
    assert(actual == status);
}
uint32_t r(Imx1Bootstrap& b, uint32_t a) { uint32_t v = 0; assert(b.access(a, 4, false, v) == A::ok); return v; }
uint32_t gpioBase(unsigned port) { return 0x21c000 + port * 0x100; }

// Both PLLs up and qualified, the CPU on FCLK, every HCLK module off.
Imx1Bootstrap running() {
    Imx1Bootstrap b;
    b.connectCpuInterrupts();
    assert(b.setRtcInitialTime({1, 12, 34, 56}));
    b.setSystemPllScenario(true); b.setMcuRetuneScenario(true);
    w(b, cgm, 0x57000003); w(b, cgm + 4, 0x03e717bf); w(b, cgm, 0x57200003);
    b.advanceFastBusCycles(6000); b.advanceCpuCycles(56, 3, true);
    w(b, cgm, run);
    return b;
}
// A pin armed as a low level interrupt, idle high.
void armPin(Imx1Bootstrap& b, unsigned port, unsigned pin) {
    const uint32_t bit = 1u << pin, base = gpioBase(port);
    assert(b.setGpioInputMasked(port, 0, ~bit) && b.reserveGpioInput(port, bit));
    assert(b.driveGpioInput(port, bit, bit, true, true) == A::ok);
    w(b, base + 0x20, r(b, base + 0x20) | bit);
    const uint32_t icr = base + (pin < 16 ? 0x28 : 0x2c);
    w(b, icr, r(b, icr) | 3u << (pin & 15) * 2);
    w(b, base + 0x30, bit);
    w(b, aitcEnable, port == 0 ? 11 : port == 1 ? 12 : port == 2 ? 13 : 62);
}
void enter(Imx1Bootstrap& b) {
    w(b, cgm, stop, 3);
    assert(b.systemStopArmed() && !b.systemStopped());
    // An asynchronous CPU has lost its clock already, one on HCLK has not.
    assert(b.cpuClockHeld(3) && !b.cpuClockHeld(0));
    // Nothing in the clock module is written during the count.
    w(b, cgm, run, 3, A::sideEffect);
    unsigned hclk = 0;
    while (!b.systemStopped()) { b.advanceHclkCycles(1); assert(++hclk < 100000); }
    assert(hclk >= 16 && b.cpuClockHeld(0) && b.cpuClockHeld(3));
    assert(!(r(b, cgm) & 3));
    w(b, cgm, run, 3, A::sideEffect);
    assert(b.idleWakePossible());
}
void wake(Imx1Bootstrap& b) {
    assert(b.wakeMcuFromInterrupt());
    assert(b.systemStopped() && b.mcuWakePending());
    // Nine CLK32 periods to lock, then HCLK and the CPU run again.
    for (unsigned i = 0; i < 9; ++i) { assert(b.systemStopped()); b.advanceHclkCycles(1); }
    assert(!b.systemStopped() && !b.systemStopArmed() && !b.mcuWakePending());
    assert(!b.cpuClockHeld(3) && (r(b, cgm) & 3) == 3);
    assert(b.cpuClockModeSupported(3, true));
    w(b, cgm, run, 3);
}
void entryRules() {
    auto b = running();
    // SPEN alone, a changed field, or an HCLK module still running: refused, nothing changed.
    w(b, cgm, 0x57003c01, 3, A::sideEffect);
    w(b, cgm, 0x57002c00, 3, A::sideEffect);
    auto timer = b;
    w(timer, 0x202004, 7); w(timer, 0x202000, 0x103);
    w(timer, cgm, stop, 3, A::sideEffect);
    assert(!timer.systemStopArmed() && (r(timer, cgm) & 3) == 3);
    // MPEN first on HCLK, then SPEN: the same stop.
    auto twoStep = b;
    w(twoStep, cgm, 0x57003c02);
    w(twoStep, cgm, stop);
    assert(twoStep.systemStopArmed() && !twoStep.cpuClockHeld(0));
    while (!twoStep.systemStopped()) twoStep.advanceHclkCycles(1);
    // An interrupt during the count keeps the System PLL on.
    auto early = b;
    armPin(early, 0, 10);
    w(early, cgm, stop, 3);
    assert(early.driveGpioInput(0, 0, 1u << 10) == A::ok);
    assert(early.wakeMcuFromInterrupt() && !early.systemStopArmed() && !early.systemStopped());
    assert((r(early, cgm) & 3) == 3);
}
void rtcKeepsTime() {
    auto b = running();
    w(b, 0x202004, 7); w(b, 0x202000, 0x103); b.advanceHclkCycles(100);
    w(b, 0x202000, 0);
    const uint32_t count = r(b, 0x202010);
    enter(b);
    assert(!b.wakeMcuFromInterrupt());
    const uint32_t before = r(b, rtcSeconds);
    // Stopped, a step is one CLK32 period: three seconds for the RTC, nothing for a timer.
    assert(b.busReferencePeriod().numerator == 1 && b.busReferencePeriod().denominator == 1);
    b.advanceHclkCycles(3 * 32768);
    assert((r(b, rtcSeconds) + 60 - before) % 60 == 3 && r(b, 0x202010) == count);
    assert(b.systemStopped() && !b.wakeMcuFromInterrupt());
    // The RTC's once a second interrupt, source 17, wakes it.
    w(b, rtcEnables, 0x10); w(b, aitcEnable, 17);
    unsigned ticks = 0;
    while (!b.wakeMcuFromInterrupt()) { b.advanceHclkCycles(1); assert(++ticks <= 32768); }
    wake(b);
}
void pinWakes() {
    // Power A10, Home A7, cradle D22 and the power lines B14 and A14.
    struct Pin { unsigned port, pin; };
    for (auto pin : {Pin{0, 10}, {0, 7}, {3, 22}, {1, 14}, {0, 14}}) {
        auto b = running();
        armPin(b, pin.port, pin.pin);
        enter(b);
        b.advanceHclkCycles(5000);
        assert(b.systemStopped() && !b.wakeMcuFromInterrupt());
        auto copy = b;
        assert(b.driveGpioInput(pin.port, 0, 1u << pin.pin) == A::ok);
        wake(b);
        // A copy taken asleep wakes the same way.
        assert(copy.systemStopped() && copy.driveGpioInput(pin.port, 0, 1u << pin.pin) == A::ok);
        wake(copy);
        assert(r(b, rtcSeconds) == r(copy, rtcSeconds) && r(b, 0x204000) == r(copy, 0x204000));
    }
}
}
int main() {
    entryRules(); rtcKeepsTime(); pinWakes();
    std::puts("Zodiac stop mode: entry rules, CLK32 only time, wake on A10, A7, D22, B14, A14 and RTC: PASS");
}
