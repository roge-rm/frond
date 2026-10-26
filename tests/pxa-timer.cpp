#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "pxa_TIMR.h"

static ArmMemAccessF access;
static void* timer;
static bool lines[4];
static unsigned requests;
static uint32_t elapsedTicks, scheduledTicks;
static uint32_t elapsed(void*) { return elapsedTicks; }
static void reschedule(void* context) { scheduledTicks = pxaTimrNextBatch(static_cast<PxaTimr*>(context)); }
extern "C" void uarmAbort() { std::abort(); }
bool memRegionAdd(ArmMem*, uint32_t base, uint32_t, ArmMemAccessF callback, void* context) {
    assert(base == 0x40a00000);
    access = callback;
    timer = context;
    return true;
}
void pxaIcInt(PxaIc*, uint_fast8_t number, bool raise) {
    assert(number >= PXA_I_TIMR0 && number <= PXA_I_TIMR3);
    lines[number - PXA_I_TIMR0] = raise;
    ++requests;
}
static void write(unsigned reg, uint32_t value) {
    assert(access(timer, 0x40a00000 + 4 * reg, 4, true, &value));
}
static uint32_t read(unsigned reg) {
    uint32_t value;
    assert(access(timer, 0x40a00000 + 4 * reg, 4, false, &value));
    return value;
}
int main() {
    auto timr = pxaTimrInit(nullptr, nullptr);
    pxaTimrTick(timr, 4);
    assert(read(4) == 4 && requests == 0);
    write(0, 10);
    write(7, 1);
    requests = 0;
    pxaTimrTick(timr, 5);
    assert(read(4) == 9 && read(5) == 0 && requests == 0);
    pxaTimrTick(timr, 1);
    assert(read(5) == 1 && lines[0] && !lines[1]);
    requests = 0;
    pxaTimrTick(timr, 1);
    assert(lines[0] && requests == 4); // Preserve pending-level wake-up requests.
    write(5, 1);
    assert(!lines[0] && read(5) == 0);
    requests = 0;
    for (unsigned i = 0; i < 1000; ++i) pxaTimrTick(timr, 1);
    assert(requests == 0);

    write(4, 0xfffffff0);
    write(0, 5);
    write(1, 8);
    write(7, 3);
    pxaTimrTick(timr, 20);
    assert(read(4) == 4 && read(5) == 0);
    pxaTimrTick(timr, 4);
    assert(read(5) == 3 && lines[0] && lines[1]);
    pxaTimrSuspendInterrupts(timr, true);
    pxaTimrSuspendInterrupts(timr, true);
    assert(!lines[0] && !lines[1]);
    pxaTimrTick(timr, 1);
    assert(!lines[0] && !lines[1] && read(5) == 3);
    pxaTimrSuspendInterrupts(timr, false);
    assert(!lines[0]);
    pxaTimrSuspendInterrupts(timr, false);
    assert(lines[0] && lines[1]);
    write(5, 3);
    assert(!lines[0] && !lines[1]);

    // Programming a match at the current counter retains the existing immediate match.
    write(2, read(4));
    write(7, 4);
    assert(read(5) == 4 && lines[2]);
    pxaTimrTick(timr, 1);
    write(5, 4);
    assert(!lines[2]);
    // Batched clocks are still visible during a CPU batch, and a new match
    // reschedules from the original timer phase, including already observed ticks.
    timr = pxaTimrInit(nullptr, nullptr);
    write(0, 10); write(7, 1);
    pxaTimrEnableBatching(timr, elapsed, reschedule, timr);
    elapsedTicks = 3;
    assert(read(4) == 3 && read(5) == 0);
    assert(pxaTimrNextBatch(timr) == 10);
    elapsedTicks = 7;
    assert(read(4) == 7 && !lines[0]);
    pxaTimrTick(timr, 10);
    elapsedTicks = 0;
    assert(read(4) == 10 && lines[0]);
    pxaTimrTick(timr, 1);
    write(5, 1);
    elapsedTicks = 5;
    write(0, 18); // Counter is 16; deadline is seven ticks after last dispatch.
    assert(scheduledTicks == 7 && read(4) == 16 && !lines[0]);
    pxaTimrTick(timr, 7);
    elapsedTicks = 0;
    assert(read(4) == 18 && lines[0]);
    // If a 68k instruction crosses a deadline, retain its extra observed ticks.
    pxaTimrTick(timr, 1); write(5, 1);
    elapsedTicks = 12;
    assert(read(4) == 31);
    pxaTimrTick(timr, 10);
    elapsedTicks = 2;
    assert(read(4) == 31);
    pxaTimrTick(timr, 4);
    elapsedTicks = 0;
    assert(read(4) == 33);
    // A match equal to the batch's starting counter is behind the next tick,
    // not a fresh match. In particular it must not spuriously fire the watchdog.
    write(7, 0); write(3, read(4)); write(6, 1);
    pxaTimrTick(timr, 3);
    write(6, 0);
    // Watchdog deadlines also bound batches when their IRQ is disabled.
    write(7, 0); write(3, read(4) + 5); write(6, 1);
    assert(pxaTimrNextBatch(timr) == 5);
    puts("PASS: timer matches, wrap, pending wake-up, suspend and clock-aware batching");
}
