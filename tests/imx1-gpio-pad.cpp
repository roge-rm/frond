#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "imx1_bootstrap.h"

namespace {
using Access = Imx1GpioAccess;
constexpr uint32_t a = 0x0021c000, bit = 1u << 14;
uint32_t op(Imx1Gpio& gpio, uint32_t offset, bool write, uint32_t value = 0,
            Access expected = Access::ok) {
    const uint32_t supplied = value;
    assert(gpio.access(a + offset, 4, write, value) == expected);
    if (write || expected != Access::ok) assert(value == supplied);
    return value;
}
void write(Imx1Gpio& gpio, uint32_t offset, uint32_t value) { op(gpio, offset, true, value); }
void reject(Imx1Gpio& gpio, uint32_t offset, uint32_t value) {
    const auto previous = op(gpio, offset, false);
    op(gpio, offset, true, value, Access::sideEffect);
    assert(op(gpio, offset, false) == previous);
    assert(op(gpio, 0x30, false) == bit);
    assert(gpio.interruptOutput(0) == Imx1GpioLine::clear);
}
void exactA14History() {
    Imx1Gpio gpio;
    assert(gpio.setInput(0, 0x80));
    // Actual A14 history, all output transitions occur with IMR_A=0.
    write(gpio, 0, 0x6f98);
    op(gpio, 0x24, false, 0xfeedface, Access::externalInput); // OCR=AIN unknown.
    write(gpio, 4, 0xffffffff);
    assert(op(gpio, 0x24, false) == 0);                    // A7 also drives DR=0.
    write(gpio, 0x20, 0x001c7ffa);
    write(gpio, 0x28, 0x003fc000);
    write(gpio, 0x34, 0xffffffff);
    write(gpio, 0x1c, 0x14782);
    assert(op(gpio, 0x24, false) == 0x4780);               // Resolved GPIO outputs.
    write(gpio, 0, 0x2818);
    assert(op(gpio, 0x24, false) == 0x80);                 // A14 returns to external low.
    write(gpio, 0x30, bit);
    assert(gpio.interruptOutput(0) == Imx1GpioLine::clear);
    assert(op(gpio, 0x30, false) == bit);
    op(gpio, 0x34, false, 0xfeedface, Access::externalInput);
    assert(gpio.setInput(0, 0x80));
    assert(!gpio.setInput(0, 0x4080));
    // Unsupported enabled mux/direction/detector changes preserve both registers and baseline.
    reject(gpio, 0, 0x6818);
    reject(gpio, 0x20, 0x001c3ffa);
    reject(gpio, 0x28, 0x103fc000);
    reject(gpio, 0x28, 0x203fc000);
    reject(gpio, 0x30, bit | (1u << 15));
    write(gpio, 0x1c, 0);                                 // DR/OCR do not drive A14 input.
    write(gpio, 4, 0);
    assert(gpio.interruptOutput(0) == Imx1GpioLine::clear);
    op(gpio, 0x24, false, 0xfeedface, Access::externalInput); // Other outputs now unknown.
    write(gpio, 0x30, 0);
    write(gpio, 0, 0x6818);
    op(gpio, 0x30, true, bit, Access::sideEffect);
    assert(op(gpio, 0x30, false) == 0);
}
void maskedTransitions() {
    for (unsigned mode : {0u, 1u}) for (bool high : {false, true}) {
        Imx1Gpio gpio;
        assert(gpio.setInput(0, high ? bit : 0));
        write(gpio, 0x20, bit);
        write(gpio, 0x28, mode << 28);
        write(gpio, 4, 3u << 28);
        write(gpio, 0, bit);
        // Rising, unchanged and falling logical output, all masked.
        for (uint32_t data : {0u, bit, bit, 0u}) {
            write(gpio, 0x1c, data);
            assert(op(gpio, 0x24, false) == data);
            assert(gpio.interruptOutput(0) == Imx1GpioLine::clear);
        }
        // Unmasking while still an output remains guarded even with known drive.
        op(gpio, 0x30, true, bit, Access::sideEffect);
        write(gpio, 0, 0);
        const bool idle = mode == unsigned(high);
        op(gpio, 0x30, true, bit, idle ? Access::ok : Access::sideEffect);
        assert(op(gpio, 0x30, false) == (idle ? bit : 0));
        if (idle) {
            reject(gpio, 0, bit);
            reject(gpio, 0x20, 0);
            reject(gpio, 0x28, (mode ^ 1) << 28);
            write(gpio, 0x34, bit);
            assert(gpio.interruptOutput(0) == Imx1GpioLine::clear);
        }
    }
    // Every unsupported output source stays unknown, without falling back to stimulus.
    for (unsigned source : {0u, 1u, 2u}) {
        Imx1Gpio gpio;
        assert(gpio.setInput(0, 0));
        write(gpio, 0x20, bit);
        write(gpio, 4, source << 28);
        write(gpio, 0x1c, bit);
        write(gpio, 0, bit);
        op(gpio, 0x24, false, 0xdeadbeef, Access::externalInput);
        op(gpio, 0x30, true, bit, Access::sideEffect);
        write(gpio, 4, 3u << 28);
        assert(op(gpio, 0x24, false) == bit);
        write(gpio, 0, 0);                              // Known external low reestablishes baseline.
        write(gpio, 0x30, bit);
    }
    // Both OCR halves decode their own pin pairs, with DR feedback on high pins.
    for (unsigned pin : {15u, 16u, 31u}) {
        Imx1Gpio gpio;
        const uint32_t mask = 1u << pin, offset = 4 + (pin / 16) * 4;
        const unsigned shift = 2 * (pin % 16);
        assert(gpio.setInput(0, 0));
        write(gpio, 0x20, mask);
        write(gpio, 0, mask);
        write(gpio, 0x1c, mask);
        for (unsigned source : {0u, 1u, 2u, 3u}) {
            write(gpio, offset, source << shift);
            if (source == 3) assert(op(gpio, 0x24, false) == mask);
            else op(gpio, 0x24, false, 0xdeadbeef, Access::externalInput);
        }
        write(gpio, 0x1c, 0);
        assert(op(gpio, 0x24, false) == 0);
    }
    Imx1Gpio unknown;
    write(unknown, 0x20, bit);
    write(unknown, 4, 3u << 28);
    write(unknown, 0, bit);
    write(unknown, 0, 0);                               // Output history cannot supply external input.
    write(unknown, 0x34, bit);
    op(unknown, 0x30, true, bit, Access::sideEffect);
    assert(!unknown.setInput(0, 0));
    op(unknown, 0x24, false, 0xdeadbeef, Access::externalInput);
}
void source11() {
    Imx1Bootstrap soc;
    assert(soc.setGpioInput(0, 0x80));
    auto bus = [&](uint32_t address, bool write, uint32_t value = 0,
                   Imx1BootstrapAccess expected = Imx1BootstrapAccess::ok) {
        const uint32_t supplied = value;
        assert(soc.access(address, 4, write, value) == expected);
        if (write || expected != Imx1BootstrapAccess::ok) assert(value == supplied);
        return value;
    };
    bus(a + 0x30, true, bit);
    bus(0x00223008, true, 11);
    assert(bus(0x00223014, false) == 1u << 11);
    for (uint32_t offset : {0x58u, 0x5cu, 0x60u, 0x64u})
        assert(bus(0x00223000 + offset, false) == 0);
    assert(bus(0x00223044, false) == 0xffffffff);
    bus(0x00223054, true, 1u << 11, Imx1BootstrapAccess::sideEffect);
    assert(bus(0x00223054, false) == 0);
    bus(0x00223004, true, 0);
    bus(0x00223054, true, 1u << 11);
    bus(0x00223004, true, 0x1f, Imx1BootstrapAccess::sideEffect);
    assert(bus(0x00223004, false) == 0);
    bus(0x0022301c, true, 1u << 11, Imx1BootstrapAccess::sideEffect);
    assert(bus(0x0022301c, false) == 0);
    bus(0x00223054, true, 0);
    bus(0x00223004, true, 0x1f);
    bus(a, true, bit, Imx1BootstrapAccess::sideEffect);
    assert(bus(a, false) == 0);
    assert(bus(a + 0x30, false) == bit);
    bus(a + 0x34, false, 0xdeadbeef, Imx1BootstrapAccess::externalInput);
}
}
int main() {
    exactA14History(); maskedTransitions(); source11();
    std::puts("PASS: GPIO masked pad history, edge baselines, SSR feedback and gated AITC source 11");
}
