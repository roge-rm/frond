#include <cassert>
#include <cstdio>
#include <vector>
#include "imx1_boot_probe.h"
#include "machine_state_archive.h"

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

// A board at power-on with the options every machine here has.
Imx1Bootstrap powerOn() {
    Imx1Bootstrap b;
    b.connectCpuInterrupts();
    b.setSystemPllScenario(true); b.setMcuRetuneScenario(true);
    return b;
}
// The boot's clock writes: the MCU PLL programmed and restarted, then the CPU moved to FCLK.
void boot(Imx1Bootstrap& b) {
    w(b, cgm, 0x57000003); w(b, cgm + 4, 0x03e717bf); w(b, cgm, 0x57200003);
    b.advanceFastBusCycles(6000); b.advanceCpuCycles(56, 3, true);
    w(b, cgm, run);
    assert((r(b, cgm) & 3) == 3 && b.cpuClockModeSupported(3, true) && !b.cpuClockHeld(3));
}
// Both PLLs up and qualified, the CPU on FCLK, every HCLK module off.
Imx1Bootstrap running() {
    auto b = powerOn();
    assert(b.setRtcInitialTime({1, 12, 34, 56}));
    boot(b);
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
    // The CPU on FCLK still has no clock until the MCU PLL has locked, then it runs.
    unsigned hclk = 0;
    while (early.cpuClockHeld(3)) { assert(early.mcuWakePending()); early.advanceHclkCycles(1); assert(++hclk < 100000); }
    assert(hclk > 9 && !early.mcuWakePending());
    early.advanceCpuCycles(1000, 3, true);
    w(early, cgm, run, 3);
}
// Saves a board through state() and loads it into a fresh one.
Imx1Bootstrap reload(Imx1Bootstrap& b) {
    std::vector<uint8_t> first, second;
    { StateWriter s(first); b.state(s); }
    auto loaded = powerOn();
    { StateReader s(first.data(), first.size()); loaded.state(s); assert(s.complete()); }
    { StateWriter s(second); loaded.state(s); }
    assert(first == second);
    return loaded;
}
// A board saved during the shutdown count, stopped and locking again goes on the same way once loaded.
void savedAsleep() {
    auto b = running();
    armPin(b, 0, 10);
    w(b, cgm, stop, 3);
    b.advanceHclkCycles(5);
    // Armed, with HCLK periods and the CLK32 edge still to come.
    auto armed = reload(b);
    assert(armed.systemStopArmed());
    unsigned hclk = 0, loadedHclk = 0;
    while (!b.systemStopped()) { b.advanceHclkCycles(1); ++hclk; }
    while (!armed.systemStopped()) { armed.advanceHclkCycles(1); ++loadedHclk; }
    assert(hclk == loadedHclk && hclk >= 11);
    // Stopped.
    b.advanceHclkCycles(100);
    auto stopped = reload(b);
    assert(stopped.systemStopped() && !stopped.systemStopArmed() && !stopped.mcuWakePending());
    assert(r(stopped, rtcSeconds) == r(b, rtcSeconds));
    // Locking again, part way through.
    assert(b.driveGpioInput(0, 0, 1u << 10) == A::ok && b.wakeMcuFromInterrupt());
    b.advanceHclkCycles(4);
    auto waking = reload(b);
    assert(waking.systemStopped() && waking.mcuWakePending());
    for (auto* x : {&b, &waking}) {
        for (unsigned i = 0; i < 5; ++i) { assert(x->systemStopped()); x->advanceHclkCycles(1); }
        assert(!x->systemStopped() && !x->mcuWakePending() && !x->cpuClockHeld(3));
        w(*x, cgm, run, 3);
    }
    assert(r(b, rtcSeconds) == r(waking, rtcSeconds) && r(b, 0x204000) == r(waking, 0x204000));
}
// ASSUMPTION "Reset restarts stopped clocks": Reset in any part of stop mode, the boot runs after it.
Imx1Bootstrap resetAndBoot(Imx1Bootstrap& old) {
    auto b = powerOn();
    b.takeExternalReset(old);
    assert(!b.systemStopped() && !b.systemStopArmed() && !b.mcuWakePending() && (r(b, cgm) & 3) == 3);
    assert(b.resetSource() == Imx1ResetSource::external);
    boot(b);
    b.advanceCpuCycles(1000, 3, true);
    b.advanceHclkCycles(100);
    return b;
}
void resetAsleep() {
    for (unsigned phase = 0; phase < 3; ++phase) {
        auto b = running();
        armPin(b, 0, 10);
        w(b, cgm, stop, 3);
        if (phase > 0) while (!b.systemStopped()) b.advanceHclkCycles(1);
        if (phase == 2) {
            assert(b.driveGpioInput(0, 0, 1u << 10) == A::ok && b.wakeMcuFromInterrupt());
            b.advanceHclkCycles(3);
            assert(b.systemStopped() && b.mcuWakePending());
        }
        auto first = resetAndBoot(b);
        // A second Reset while running, and a third after it sleeps again.
        auto second = resetAndBoot(first);
        armPin(second, 0, 7);
        enter(second);
        auto third = resetAndBoot(second);
        armPin(third, 0, 10);
        enter(third);
        assert(third.driveGpioInput(0, 0, 1u << 10) == A::ok);
        wake(third);
    }
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
        auto copy = reload(b);
        assert(b.driveGpioInput(pin.port, 0, 1u << pin.pin) == A::ok);
        wake(b);
        // A board saved and loaded asleep wakes the same way.
        assert(copy.systemStopped() && copy.driveGpioInput(pin.port, 0, 1u << pin.pin) == A::ok);
        wake(copy);
        assert(r(b, rtcSeconds) == r(copy, rtcSeconds) && r(b, 0x204000) == r(copy, 0x204000));
    }
}
}
int main() {
    entryRules(); rtcKeepsTime(); pinWakes(); savedAsleep(); resetAsleep();
    std::puts("Zodiac stop mode: entry rules, CLK32 only time, wake on A10, A7, D22, B14, A14 and RTC, saved asleep, Reset asleep: PASS");
}
