#include "imx1_timer.h"

const char* Imx1Timer::registerName(uint32_t address) {
    static const char* names[2][6] = {
        {"TIMER1.TCTL", "TIMER1.TPRER", "TIMER1.TCMP", "TIMER1.TCR", "TIMER1.TCN", "TIMER1.TSTAT"},
        {"TIMER2.TCTL", "TIMER2.TPRER", "TIMER2.TCMP", "TIMER2.TCR", "TIMER2.TCN", "TIMER2.TSTAT"}
    };
    const uint32_t base = address & ~0xfffu;
    if ((base != 0x00202000 && base != 0x00203000) || address - base >= 0x18) return nullptr;
    return names[base == 0x00203000][(address - base) >> 2];
}

Imx1TimerAccess Imx1Timer::access(uint32_t offset, uint32_t size, bool write, uint32_t& value,
                                   bool outputIsolated) {
    if (offset >= 0x18) return Imx1TimerAccess::unmapped;
    if (size != 4 || (offset & 3)) return Imx1TimerAccess::width;
    if (!write) {
        switch (offset) {
            case 0x00: value = control; break;
            case 0x04: value = prescaler; break;
            case 0x08: value = compare; break;
            case 0x0c: value = 0; break;  // Capture is disabled in every accepted configuration.
            case 0x10: value = counter; break;
            case 0x14: value = status; statusRead |= status; break;
        }
        return Imx1TimerAccess::ok;
    }
    switch (offset) {
        case 0x00: {
            if (value & ~0x81ffu) return Imx1TimerAccess::reservedBits;
            // Reset has a three-SystemCLK hold; capture and output-pin effects are unqualified.
            if (value & 0x8000) return Imx1TimerAccess::sideEffect;
            if (value & 0xc0) return Imx1TimerAccess::externalInput;
            if ((value & 0x21) == 0x21 && !outputIsolated) return Imx1TimerAccess::sideEffect;
            const unsigned source = (value >> 1) & 7;
            if (source >= 3) return Imx1TimerAccess::externalInput;
            // Live source changes need the prescaler handoff qualified.
            if (enabled() && (value & 1) && ((value ^ control) & 0xe))
                return Imx1TimerAccess::sideEffect;
            control = value;
            if (!enabled()) { counter = 0; phase = 0; }
            break;
        }
        case 0x04:
            if (value & ~0xffu) return Imx1TimerAccess::reservedBits;
            if (enabled() && value != prescaler) return Imx1TimerAccess::sideEffect;
            prescaler = value;
            break;
        case 0x08: compare = value; break;
        case 0x0c: case 0x10: return Imx1TimerAccess::sideEffect;
        case 0x14: {
            if (value & ~3u) return Imx1TimerAccess::reservedBits;
            const uint32_t cleared = statusRead & ~value;
            status &= ~cleared;
            statusRead &= ~cleared;
            break;
        }
    }
    return Imx1TimerAccess::ok;
}

uint64_t Imx1Timer::idleQuietCycles() const {
    const unsigned source = (control >> 1) & 7;
    if (!enabled() || !source) return 0;
    const uint64_t divisor = uint64_t(prescaler + 1) * (source == 2 ? 16 : 1);
    uint64_t distance = uint32_t(compare - counter);
    if (!distance) distance = uint64_t(1) << 32;
    return phase < divisor ? distance * divisor - phase : 1;
}

void Imx1Timer::advance(uint64_t perclk1Cycles) {
    const unsigned source = (control >> 1) & 7;
    if (!enabled() || !source) return;
    const uint32_t divisor = (prescaler + 1) * (source == 2 ? 16 : 1);
    uint64_t ticks = perclk1Cycles / divisor;
    const uint64_t remainder = perclk1Cycles % divisor + phase;
    ticks += remainder / divisor;
    phase = remainder % divisor;
    if (!ticks) return;
    uint64_t distance = uint32_t(compare - counter);
    if (!distance) distance = uint64_t(1) << 32;
    if (ticks >= distance) {
        status |= 1;
        if (!(control & 0x100)) {
            const uint64_t period = compare ? compare : uint64_t(1) << 32;
            counter = (ticks - distance) % period;
            return;
        }
    }
    counter += uint32_t(ticks);
}
