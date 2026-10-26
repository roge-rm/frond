#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "pxa_TIMR.h"

static ArmMemAccessF access;
static void* timer;
static bool lines[4];
static unsigned requests;
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
    puts("PASS: timer idle ticks, match boundaries, wrap, clear, pending wake-up and suspend");
}
