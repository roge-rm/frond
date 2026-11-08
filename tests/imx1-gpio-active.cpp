#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "imx1_bootstrap.h"

namespace {
using G = Imx1GpioAccess;
using B = Imx1BootstrapAccess;
constexpr uint32_t base = 0x0021c000, bit = 1u << 13;
uint32_t op(Imx1Gpio& gpio, unsigned port, unsigned offset, bool write, uint32_t value = 0, G status = G::ok) {
    const auto supplied = value;
    assert(gpio.access(base + port * 0x100 + offset, 4, write, value) == status);
    if (write || status != G::ok) assert(value == supplied);
    return value;
}
void levels() {
    for (unsigned port = 0; port < 4; ++port) for (unsigned mode : {2u, 3u}) {
        Imx1Gpio gpio;
        const auto active = mode == 2 ? bit : 0u, inactive = active ^ bit;
        assert(gpio.updateInput(port, inactive, 0xffffffff) == G::ok);
        op(gpio, port, 0x20, true, bit);
        op(gpio, port, 0x28, true, mode << 26);
        assert(op(gpio, port, 0x34, false) == 0);
        op(gpio, port, 0x30, true, bit);
        assert(gpio.interruptOutput(port) == Imx1GpioLine::clear);
        assert(gpio.updateInput(port, active, 0xffffffff) == G::ok);
        assert(gpio.interruptOutput(port) == Imx1GpioLine::asserted);
        assert(op(gpio, port, 0x34, false) == bit);
        op(gpio, port, 0x34, true, bit);
        assert(op(gpio, port, 0x34, false) == bit); // Active level immediately reasserts.
        assert(gpio.updateInput(port, inactive, 0xffffffff) == G::ok);
        assert(op(gpio, port, 0x34, false) == bit);
        op(gpio, port, 0x30, true, 0);
        assert(gpio.interruptOutput(port) == Imx1GpioLine::clear);
        assert(op(gpio, port, 0x34, false) == bit); // Masking preserves pending status.
        op(gpio, port, 0x34, true, bit);
        assert(gpio.updateInput(port, active, 0xffffffff) == G::ok);
        assert(op(gpio, port, 0x34, false) == 0); // Masked events cannot latch.
        op(gpio, port, 0x30, true, bit);
        assert(op(gpio, port, 0x34, false) == bit);
        assert(gpio.updateInput(port, inactive, 0xffffffff) == G::ok);
        op(gpio, port, 0x34, true, 0xffffffff);
        assert(op(gpio, port, 0x34, false) == 0);
    }
}
void edges() {
    for (unsigned port = 0; port < 4; ++port) for (unsigned mode : {0u, 1u}) {
        Imx1Gpio gpio;
        const auto firing = mode == 0 ? bit : 0u, idle = firing ^ bit;
        assert(gpio.updateInput(port, firing, 0xffffffff) == G::ok);
        op(gpio, port, 0x20, true, bit);
        op(gpio, port, 0x28, true, mode << 26);
        op(gpio, port, 0x30, true, bit);
        assert(op(gpio, port, 0x34, false) == 0); // Enabling is not an edge.
        assert(gpio.updateInput(port, idle, 0xffffffff) == G::ok);
        assert(op(gpio, port, 0x34, false) == 0);
        assert(gpio.updateInput(port, firing, 0xffffffff) == G::ok);
        assert(op(gpio, port, 0x34, false) == bit);
        op(gpio, port, 0x34, true, bit);
        assert(op(gpio, port, 0x34, false) == 0); // A held edge polarity does not reassert.
        op(gpio, port, 0x30, true, 0);
        assert(gpio.updateInput(port, idle, 0xffffffff) == G::ok);
        assert(gpio.updateInput(port, firing, 0xffffffff) == G::ok);
        op(gpio, port, 0x30, true, bit);
        assert(op(gpio, port, 0x34, false) == 0);
        op(gpio, port, 0x28, true, (mode ^ 1) << 26);
        assert(op(gpio, port, 0x34, false) == 0); // Mode changes seed a baseline.
    }
}
void knownnessAndMux() {
    Imx1Gpio gpio;
    assert(gpio.updateInput(0, 0, 0) == G::ok);
    op(gpio, 0, 0x20, true, bit);
    op(gpio, 0, 0x28, true, 2u << 26);
    op(gpio, 0, 0x30, true, bit);
    assert(gpio.interruptOutput(0) == Imx1GpioLine::unknown);
    op(gpio, 0, 0x34, false, 0xfeedface, G::externalInput);
    assert(gpio.updateInput(0, 0, 0xffffffff) == G::ok);
    assert(gpio.interruptOutput(0) == Imx1GpioLine::unknown); // Inactive now does not disprove an earlier event.
    op(gpio, 0, 0x34, true, bit);
    assert(op(gpio, 0, 0x34, false) == 0);
    op(gpio, 0, 0x28, true, 0);
    assert(gpio.updateInput(0, 0, 0) == G::ok);
    assert(gpio.interruptOutput(0) == Imx1GpioLine::unknown);
    op(gpio, 0, 0x34, true, bit);
    assert(gpio.updateInput(0, bit, 0xffffffff) == G::ok);
    assert(op(gpio, 0, 0x34, false) == 0); // Unknown-to-known seeds, without a fake edge.
    assert(gpio.updateInput(0, 0, 0xffffffff) == G::ok);
    assert(gpio.updateInput(0, bit, 0xffffffff) == G::ok);
    assert(op(gpio, 0, 0x34, false) == bit);
    for (unsigned offset : {0u, 0x20u}) {
        const auto old = op(gpio, 0, offset, false);
        op(gpio, 0, offset, true, offset ? 0 : bit, G::sideEffect);
        assert(op(gpio, 0, offset, false) == old && op(gpio, 0, 0x34, false) == bit);
    }
    op(gpio, 0, 0x30, true, 0);
    op(gpio, 0, 0x20, true, 0);
    op(gpio, 0, 0x20, true, bit);
    op(gpio, 0, 0x34, true, bit);
    op(gpio, 0, 0x30, true, bit);
    assert(op(gpio, 0, 0x34, false) == 0);
    assert(!gpio.setInput(0, bit));
    Imx1Gpio fixed;
    assert(fixed.setInput(0, 0x80));
    assert(fixed.updateInput(0, 0x2080, 0xffffffff) == G::sideEffect);
    assert(fixed.setInput(0, 0x80) && !fixed.setInput(0, 0x2080));
}
void mixedKnownness() {
    Imx1Gpio gpio;
    constexpr uint32_t other = bit << 1;
    assert(gpio.updateInput(0, bit, bit) == G::ok);
    op(gpio, 0, 0x20, true, bit | other);
    op(gpio, 0, 0x28, true, (2u << 26) | (2u << 28));
    op(gpio, 0, 0x30, true, bit | other);
    assert(gpio.interruptOutput(0) == Imx1GpioLine::asserted);
    op(gpio, 0, 0x34, false, 0xfeedface, G::externalInput);
    op(gpio, 0, 0x30, true, bit);
    op(gpio, 0, 0x34, false, 0xfeedface, G::externalInput);
    op(gpio, 0, 0x34, true, other);
    assert(op(gpio, 0, 0x34, false) == bit);
    assert(gpio.interruptOutput(0) == Imx1GpioLine::asserted);
}
uint32_t bus(Imx1Bootstrap& soc, uint32_t address, bool write, uint32_t value = 0, B status = B::ok) {
    const auto supplied = value;
    assert(soc.access(address, 4, write, value) == status);
    if (write || status != B::ok) assert(value == supplied);
    return value;
}
void transactionalDelivery() {
    Imx1Bootstrap soc;
    assert(soc.updateGpioInput(0, 0, 0xffffffff) == B::ok);
    bus(soc, base + 0x20, true, bit);
    bus(soc, base + 0x28, true, 3u << 26);
    bus(soc, 0x00223008, true, 11);
    bus(soc, base + 0x30, true, bit, B::sideEffect);
    assert(bus(soc, base + 0x30, false) == 0 && bus(soc, base + 0x34, false) == 0);
    soc.connectCpuInterrupts();
    bus(soc, base + 0x30, true, bit);
    assert(soc.interruptLines().irq == Imx1AitcLine::asserted);
    assert(bus(soc, 0x0022305c, false) == 1u << 11);
    bus(soc, 0x00223008, true, 1, B::externalInput); // Known source 11 cannot hide source 1 uncertainty.
    assert(bus(soc, 0x00223014, false) == 1u << 11);
    bus(soc, base + 0x30, true, 0);
    bus(soc, base + 0x34, true, bit);
    bus(soc, base + 0x28, true, 0);
    bus(soc, base + 0x30, true, bit);
    assert(soc.updateGpioInput(0, 0, 0) == B::externalInput);
    assert(bus(soc, base + 0x24, false) == 0 && bus(soc, base + 0x34, false) == 0);
    assert(soc.updateGpioInput(0, bit, 0xffffffff) == B::ok);
    assert(bus(soc, base + 0x34, false) == bit); // Failed update preserved the low baseline.
    bus(soc, base + 0x34, true, bit);
    assert(soc.interruptLines().irq == Imx1AitcLine::clear);
}
}
int main() {
    levels(); edges(); knownnessAndMux(); mixedKnownness(); transactionalDelivery();
    std::puts("PASS: GPIO levels, edges, masked status, W1C, unknown history, immutable samples and transactional AITC delivery");
}
