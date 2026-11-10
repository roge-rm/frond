#include <cassert>
#include <cstdio>
#include "imx1_graphics_interrupts.h"

using A = Imx1BootstrapAccess;
static uint32_t access(Imx1GraphicsInterrupts& state, bool status, bool write, uint32_t value = 0,
                       A expected = A::ok) {
    const auto supplied = value;
    const auto pc = status ? (write ? 0x20087824u : 0x200868ccu) : (write ? 0x20086e08u : 0x20086deeu);
    assert(state.access(pc, status ? 0x16010204 : 0x16010200, 4, write, value) == expected);
    if (write || expected != A::ok) assert(value == supplied);
    return value;
}
int main() {
    Imx1GraphicsInterrupts state;
    assert(access(state, true, false) == 0);
    state.observeDisplay(true, true);
    assert(access(state, true, false) == 1);
    assert(state.interruptOutput() == Imx1GpioLine::clear);
    access(state, false, true, 1);
    assert(state.interruptOutput() == Imx1GpioLine::asserted);
    access(state, false, true, 0);
    assert(access(state, true, false) == 1);
    access(state, true, true, 0x50);
    assert(access(state, true, false) == 1);
    access(state, true, true, 1);
    assert(access(state, true, false) == 0);
    access(state, false, true, 1);
    state.observeDisplay(true, false);
    assert(state.interruptOutput() == Imx1GpioLine::clear);
    state.observeDisplay(true, true);
    assert(state.interruptOutput() == Imx1GpioLine::asserted);
    for (auto mask : {0x50u, 0x02000000u, 0x02000051u}) {
        access(state, false, true, mask, A::externalInput);
        assert(access(state, false, false) == 1 && access(state, true, false) == 1);
    }
    access(state, false, true, 2, A::reservedBits);
    access(state, true, true, 2, A::reservedBits);
    state.reset();
    state.observeDisplay(false, false);
    assert(state.knownMask() == 0xfffffffe);
    assert(state.interruptOutput() == Imx1GpioLine::clear);
    access(state, true, false, 0xfeedface, A::externalInput);
    access(state, false, true, 1, A::externalInput);
    state.observeDisplay(true, false);
    access(state, true, false, 0xfeedface, A::externalInput);
    state.observeDisplay(true, true);
    assert(access(state, true, false) == 1);
    access(state, true, true, 1);
    assert(access(state, true, false) == 0);
    access(state, false, true, 1);
    state.observeDisplay(false, false);
    assert(state.interruptOutput() == Imx1GpioLine::unknown);
    access(state, true, true, 1);
    assert(state.interruptOutput() == Imx1GpioLine::clear);
    state.reset();
    assert(state.enableValue() == 0 && state.pendingValue() == 0 && state.knownMask() == 0xffffffff);
    uint32_t resetEnable = 1;
    assert(state.access(0x20086970, 0x16010200, 4, true, resetEnable) == A::sideEffect);
    assert(state.enableValue() == 0);
    // The shutdown write at 20086c06 turns every source off, and only takes 0.
    uint32_t shutdownEnable = 1;
    assert(state.access(0x20086c06, 0x16010200, 4, true, shutdownEnable) == A::sideEffect);
    assert(state.access(0x20086e08, 0x16010200, 4, true, shutdownEnable) == A::ok && state.enableValue() == 1);
    shutdownEnable = 0;
    assert(state.access(0x20086c06, 0x16010200, 4, true, shutdownEnable) == A::ok && state.enableValue() == 0);
    uint32_t value = 0xfeedface;
    assert(state.access(0, 0x16010204, 4, false, value) == A::externalInput && value == 0xfeedface);
    assert(state.access(0x200868cc, 0x16010204, 2, false, value) == A::width);
    std::puts("PASS: GPU masked pending, enable, W1C, source knownness, reset and unsupported source guards");
}
