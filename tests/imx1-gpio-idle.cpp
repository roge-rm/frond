#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "imx1_bootstrap.h"

namespace {
using Access = Imx1GpioAccess;
constexpr uint32_t b = 0x0021c100, bit = 1u << 14;
uint32_t access(Imx1Gpio& gpio, uint32_t address, bool write, uint32_t value = 0,
                Access expected = Access::ok) {
    const uint32_t supplied = value;
    assert(gpio.access(address, 4, write, value) == expected);
    if (write || expected != Access::ok) assert(value == supplied);
    return value;
}
void reject(Imx1Gpio& gpio, uint32_t address, uint32_t value) {
    const auto before = access(gpio, address, false);
    access(gpio, address, true, value, Access::sideEffect);
    assert(access(gpio, address, false) == before);
    assert(access(gpio, b + 0x30, false) == bit);
    assert(gpio.interruptOutput(1) == Imx1GpioLine::clear);
}
void levels() {
    // Section 32.1: A0..31, B8..31, C3..17/19..31, D6..31 are bonded.
    for (unsigned port = 0; port < 4; ++port) for (unsigned pin = 0; pin < 32; ++pin)
        for (unsigned mode = 0; mode < 4; ++mode) for (bool high : {false, true}) {
            Imx1Gpio gpio;
            const uint32_t base = 0x0021c000 + port * 0x100, mask = 1u << pin;
            assert(gpio.setInput(port, high ? mask : 0));
            access(gpio, base + 0x20, true, mask);
            access(gpio, base + 0x28 + (pin / 16) * 4, true, mode << (2 * (pin % 16)));
            const bool bonded = port == 0 || (port == 1 && pin >= 8) ||
                (port == 2 && pin >= 3 && pin != 18) || (port == 3 && pin >= 6);
            const bool active = mode >= 2 && high == (mode == 2);
            access(gpio, base + 0x30, true, mask, bonded ? Access::ok : Access::sideEffect);
            assert(access(gpio, base + 0x30, false) == (bonded ? mask : 0));
            assert(gpio.interruptOutput(port) == (bonded && active ? Imx1GpioLine::asserted : Imx1GpioLine::clear));
        }
}
void controlsAndHistory() {
    Imx1Gpio gpio;
    assert(gpio.setInput(1, 0x08000000));
    assert(!gpio.setInput(1, 0x08004000));
    access(gpio, b + 0x20, true, 0x3fffc0ff);
    access(gpio, b + 0x28, true, 0x20000000);
    // Reset ISR=0 and IMR=0 supply the clear history, W1C is not required.
    access(gpio, b + 0x30, true, bit);
    for (uint32_t clear : {0u, bit, ~bit, 0xffffffffu}) {
        access(gpio, b + 0x34, true, clear);
        assert(access(gpio, b + 0x34, false) == 0);
        assert(access(gpio, b + 0x30, false) == bit);
        assert(gpio.interruptOutput(1) == Imx1GpioLine::clear);
    }
    // GPIO input ignores its output data, output source and peripheral selector.
    for (uint32_t offset : {0x1cu, 4u, 8u, 0x38u, 0x40u}) {
        access(gpio, b + offset, true, 0xffffffff);
        assert(gpio.interruptOutput(1) == Imx1GpioLine::clear);
    }
    assert(access(gpio, b + 0x24, false) == 0x08000000);
    assert(gpio.setInput(1, 0x08000000));
    assert(!gpio.setInput(1, 0x08004000));
    reject(gpio, b, bit);                           // Output direction needs pin physics.
    reject(gpio, b + 0x20, 0x3fff80ff);             // B14 peripheral mux.
    for (unsigned mode : {0u, 1u, 2u}) {
        access(gpio, b + 0x28, true, mode << 28);
        assert(gpio.interruptOutput(1) == Imx1GpioLine::clear);
    }
    reject(gpio, b + 0x30, bit | 1u);        // Unbonded extra request.
    access(gpio, b + 0x30, true, 0);
    access(gpio, b + 0x28, true, 0x30000000);
    access(gpio, b + 0x34, true, bit);
    access(gpio, b + 0x30, true, bit);
    assert(gpio.interruptOutput(1) == Imx1GpioLine::asserted);

    Imx1Gpio late;
    access(late, b + 0x24, false, 0xfeedface, Access::externalInput);
    assert(!late.setInput(1, 0));                    // A new level supplies no earlier history.
    access(late, b + 0x28, true, 0x20000000);
    access(late, b + 0x34, true, 0xffffffff);
    access(late, b + 0x30, true, bit);
    assert(late.interruptOutput(1) == Imx1GpioLine::unknown);
    access(late, b + 0x34, false, 0xfeedface, Access::externalInput);
    assert(!late.setInput(4, 0));

    Imx1Gpio masked;
    assert(masked.setInput(1, 0));
    // Masked polarity/mux/output writes cannot set ISR (sections 32.5.8/.9).
    access(masked, b, true, bit);
    access(masked, b + 0x1c, true, bit);
    access(masked, b + 0x20, true, 0);
    access(masked, b + 0x28, true, 0x30000000);
    access(masked, b + 0x30, true, bit, Access::sideEffect);
    access(masked, b, true, 0);
    access(masked, b + 0x20, true, bit);
    access(masked, b + 0x28, true, 0x20000000);
    access(masked, b + 0x30, true, bit);
    assert(masked.interruptOutput(1) == Imx1GpioLine::clear);
}
void interruptController() {
    using Status = Imx1BootstrapAccess;
    Imx1Bootstrap soc;
    assert(soc.setGpioInput(1, 0x08000000));
    auto op = [&](uint32_t address, bool write, uint32_t value, Status expected = Status::ok) {
        const uint32_t supplied = value;
        assert(soc.access(address, 4, write, value) == expected);
        if (write || expected != Status::ok) assert(value == supplied);
        return value;
    };
    op(0x00223008, true, 12);                        // Known inactive with IMR=0.
    op(b + 0x28, true, 0x20000000);
    op(b + 0x30, true, bit);
    assert(op(0x00223014, false, 0) == 1u << 12);
    for (uint32_t pending : {0x58u, 0x5cu, 0x60u, 0x64u})
        assert(op(0x00223000 + pending, false, 0) == 0);
    assert(op(0x00223044, false, 0) == 0xffffffff);
    op(0x0022304c, false, 0xdeadbeef, Status::externalInput); // Other sources still unknown.
    assert(op(b + 0x34, false, 0) == 0);
    op(0x00223054, true, 1u << 12, Status::sideEffect);
    assert(op(0x00223054, false, 0) == 0);
    op(0x00223004, true, 0);                        // Priority zero masked, then forced.
    op(0x00223054, true, 1u << 12);
    op(0x00223004, true, 0x1f, Status::sideEffect);
    assert(op(0x00223004, false, 0) == 0);
    op(0x0022301c, true, 1u << 12, Status::sideEffect); // FIQ delivery remains gated.
    assert(op(0x0022301c, false, 0) == 0);
    op(0x00223054, true, 0);
    op(0x00223004, true, 0x1f);
    op(b, true, bit, Status::sideEffect);
    assert(op(b, false, 0) == 0);
    assert(op(b + 0x30, false, 0) == bit);
    assert(!soc.setGpioInput(1, 0x08004000));
    assert(op(b + 0x24, false, 0) == 0x08000000);
}
}
int main() {
    levels(); controlsAndHistory(); interruptController();
    std::puts("PASS: held GPIO idle levels, masked/reset/W1C history, transactional controls and gated AITC delivery");
}
