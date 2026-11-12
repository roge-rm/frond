#include "imx1_gpio.h"

Imx1Gpio::Imx1Gpio() {
    constexpr uint32_t inUseReset[] = {0x00c3fffe, 0xffffffff, 0x0007ffff, 0xffffffff};
    for (unsigned port = 0; port < 4; ++port) {
        auto& r = ports[port].registers;
        for (unsigned i = 3; i <= 6; ++i) r[i] = 0xffffffff;
        r[8] = inUseReset[port];
        r[16] = port == 2 ? 0xf910ffff : 0xffffffff;
    }
}

bool Imx1Gpio::setInputMasked(unsigned port, uint32_t sample, uint32_t mask) {
    if (port >= 4 || !mask || (sample & ~mask)) return false;
    auto& p = ports[port];
    if (p.boardInput || (mask & p.drivenMask)) return false;
    if (p.sampleKnown) return p.fixedMask == mask && (p.sample & mask) == sample;
    if (p.accessed) return false;
    p.sample = (p.sample & ~mask) | sample;
    p.sampleKnown = true;
    p.fixedMask = mask;
    p.inputKnown |= mask;
    resolvePad(p);
    return true;
}

bool Imx1Gpio::reserveInput(unsigned port, uint32_t mask) {
    if (port >= 4 || !mask) return false;
    auto& p = ports[port];
    if (p.accessed || p.boardInput || (p.fixedMask & mask)) return false;
    if (p.drivenMask) return p.drivenMask == mask;
    p.drivenMask = mask;
    return true;
}

Imx1GpioAccess Imx1Gpio::driveInput(unsigned index, uint32_t sample, uint32_t known, bool baseline) {
    if (index >= 4) return Imx1GpioAccess::unmapped;
    auto& port = ports[index];
    if (!port.drivenMask || ((sample | known) & ~port.drivenMask)) return Imx1GpioAccess::sideEffect;
    auto next = port;
    next.sample = (next.sample & ~next.drivenMask) | sample;
    next.inputKnown = (next.inputKnown & ~next.drivenMask) | known;
    resolvePad(next);
    auto old = port;
    if (baseline) {
        old.pad = (old.pad & ~port.drivenMask) | (next.pad & port.drivenMask);
        old.padKnown = (old.padKnown & ~port.drivenMask) | (next.padKnown & port.drivenMask);
    }
    const auto status = updateInterrupts(next, old, index, true);
    if (status == Imx1GpioAccess::ok) port = next;
    return status;
}

void Imx1Gpio::resolvePad(Port& port) {
    const auto& r = port.registers;
    port.pad = port.sample;
    port.padKnown = port.inputKnown;
    const uint32_t outputs = r[8] & r[0];
    port.padKnown &= ~outputs;
    for (unsigned pin = 0; pin < 32; ++pin) {
        const uint32_t bit = 1u << pin;
        const unsigned source = (r[1 + pin / 16] >> (2 * (pin % 16))) & 3;
        if (!(outputs & bit) || source != 3) continue;
        // Figures 32-1/2: ideal digital GPIO drive, without external contention.
        port.pad = (port.pad & ~bit) | (r[7] & bit);
        port.padKnown |= bit;
    }
}

Imx1GpioAccess Imx1Gpio::transitionInput(unsigned index, uint32_t sample, uint32_t mask) {
    if (index >= 4) return Imx1GpioAccess::unmapped;
    auto& port = ports[index];
    if (!mask || (sample & ~mask) || (mask & ~port.fixedMask)) return Imx1GpioAccess::sideEffect;
    auto next = port;
    next.sample = (next.sample & ~mask) | sample;
    resolvePad(next);
    const auto status = updateInterrupts(next, port, index, true);
    if (status == Imx1GpioAccess::ok) port = next;
    return status;
}

Imx1GpioAccess Imx1Gpio::updateInput(unsigned index, uint32_t sample, uint32_t known) {
    if (index >= 4) return Imx1GpioAccess::unmapped;
    auto& port = ports[index];
    if (port.sampleKnown || port.drivenMask) return Imx1GpioAccess::sideEffect;
    auto next = port;
    next.boardInput = true;
    next.sample = sample;
    next.inputKnown = known;
    resolvePad(next);
    const auto status = updateInterrupts(next, port, index, true);
    if (status == Imx1GpioAccess::ok) port = next;
    return status;
}

Imx1GpioAccess Imx1Gpio::updateInterrupts(Port& next, const Port& old, unsigned index, bool sampleEvent) {
    constexpr uint32_t usable[] = {0xffffffff, 0xffffff00, 0xfffbfff8, 0xffffffc0};
    const auto& r = next.registers;
    const uint32_t inputs = usable[index] & r[8] & ~r[0];
    const uint32_t oldInputs = usable[index] & old.registers[8] & ~old.registers[0];
    const uint32_t enabled = r[12];
    // Interrupts on peripheral muxes and outputs remain outside this input model.
    if (enabled & ~inputs) return Imx1GpioAccess::sideEffect;
    for (unsigned pin = 0; pin < 32; ++pin) {
        const uint32_t bit = 1u << pin;
        // Sections 32.5.8/.9: only unmasked events can set status.
        if (!(enabled & bit)) continue;
        const unsigned shift = 2 * (pin % 16), icr = 10 + pin / 16;
        const unsigned mode = (r[icr] >> shift) & 3;
        bool event = false, unknown = false;
        if (mode >= 2) {
            // Levels are sampled on enable and after W1C as well as pad changes.
            unknown = !(next.padKnown & bit);
            event = !unknown && bool(next.pad & bit) == (mode == 2);
        } else if ((old.registers[12] & oldInputs & bit) &&
                   mode == ((old.registers[icr] >> shift) & 3)) {
            // Enabling/reconfiguring an edge detector seeds its current baseline.
            if ((old.padKnown & next.padKnown & bit) != 0)
                event = ((old.pad ^ next.pad) & bit) && bool(next.pad & bit) == (mode == 0);
            else if (!(next.padKnown & bit) &&
                     (sampleEvent || ((old.padKnown ^ next.padKnown) & bit))) unknown = true;
        } else if (!(next.padKnown & bit)) {
            unknown = true;
        }
        if (event) {
            next.registers[13] |= bit;
            next.statusKnown |= bit;
        } else if (unknown && !((next.registers[13] & next.statusKnown) & bit)) {
            next.registers[13] &= ~bit;
            next.statusKnown &= ~bit;
        }
    }
    return Imx1GpioAccess::ok;
}

Imx1GpioLine Imx1Gpio::interruptOutput(unsigned port) const {
    if (port >= 4) return Imx1GpioLine::unknown;
    const auto& p = ports[port];
    const uint32_t enabled = p.registers[12];
    if (enabled & p.registers[13] & p.statusKnown) return Imx1GpioLine::asserted;
    return enabled & ~p.statusKnown ? Imx1GpioLine::unknown : Imx1GpioLine::clear;
}

const char* Imx1Gpio::registerName(uint32_t address) {
    if (address < 0x0021c000 || address >= 0x0021c400 || (address & 0xfc) > 0x40) return nullptr;
    static const char* const names[4][17] = {
        {"GPIO.DDIR_A", "GPIO.OCR1_A", "GPIO.OCR2_A", "GPIO.ICONFA1_A", "GPIO.ICONFA2_A",
         "GPIO.ICONFB1_A", "GPIO.ICONFB2_A", "GPIO.DR_A", "GPIO.GIUS_A", "GPIO.SSR_A",
         "GPIO.ICR1_A", "GPIO.ICR2_A", "GPIO.IMR_A", "GPIO.ISR_A", "GPIO.GPR_A", "GPIO.SWR_A", "GPIO.PUEN_A"},
        {"GPIO.DDIR_B", "GPIO.OCR1_B", "GPIO.OCR2_B", "GPIO.ICONFA1_B", "GPIO.ICONFA2_B",
         "GPIO.ICONFB1_B", "GPIO.ICONFB2_B", "GPIO.DR_B", "GPIO.GIUS_B", "GPIO.SSR_B",
         "GPIO.ICR1_B", "GPIO.ICR2_B", "GPIO.IMR_B", "GPIO.ISR_B", "GPIO.GPR_B", "GPIO.SWR_B", "GPIO.PUEN_B"},
        {"GPIO.DDIR_C", "GPIO.OCR1_C", "GPIO.OCR2_C", "GPIO.ICONFA1_C", "GPIO.ICONFA2_C",
         "GPIO.ICONFB1_C", "GPIO.ICONFB2_C", "GPIO.DR_C", "GPIO.GIUS_C", "GPIO.SSR_C",
         "GPIO.ICR1_C", "GPIO.ICR2_C", "GPIO.IMR_C", "GPIO.ISR_C", "GPIO.GPR_C", "GPIO.SWR_C", "GPIO.PUEN_C"},
        {"GPIO.DDIR_D", "GPIO.OCR1_D", "GPIO.OCR2_D", "GPIO.ICONFA1_D", "GPIO.ICONFA2_D",
         "GPIO.ICONFB1_D", "GPIO.ICONFB2_D", "GPIO.DR_D", "GPIO.GIUS_D", "GPIO.SSR_D",
         "GPIO.ICR1_D", "GPIO.ICR2_D", "GPIO.IMR_D", "GPIO.ISR_D", "GPIO.GPR_D", "GPIO.SWR_D", "GPIO.PUEN_D"}
    };
    return names[(address >> 8) & 3][(address & 0xfc) / 4];
}

Imx1GpioAccess Imx1Gpio::access(uint32_t address, uint32_t size, bool write, uint32_t& value) {
    if (!registerName(address)) return Imx1GpioAccess::unmapped;
    if (size != 4 || (address & 3)) return Imx1GpioAccess::width;
    const unsigned portIndex = (address >> 8) & 3;
    auto& port = ports[portIndex];
    port.accessed = true;
    const unsigned index = (address & 0xff) / 4;
    auto& reg = port.registers[index];
    if (index == 9) {
        if (write) return Imx1GpioAccess::sideEffect;
        if (port.padKnown != 0xffffffff) return Imx1GpioAccess::externalInput;
        value = port.pad;
        return Imx1GpioAccess::ok;
    }
    if (!write) {
        if (index == 13 && port.statusKnown != 0xffffffff) return Imx1GpioAccess::externalInput;
        value = reg;
        return Imx1GpioAccess::ok;
    }
    if (index == 15) {
        if (value & ~1u) return Imx1GpioAccess::reservedBits;
        // Software reset holds for three system clocks, requiring clock progression.
        if (value) return Imx1GpioAccess::sideEffect;
    }
    auto next = port;
    if (index == 13) {
        next.registers[index] &= ~value;
        next.statusKnown |= value;
    } else next.registers[index] = value;
    resolvePad(next);
    const auto status = updateInterrupts(next, port, portIndex, false);
    if (status != Imx1GpioAccess::ok) return status;
    port = next;
    return Imx1GpioAccess::ok;
}
