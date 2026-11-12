#include "imx1_pwm.h"

#include <cassert>
#include <limits>
#include <numeric>

const char* Imx1Pwm::registerName(uint32_t address) {
    switch (address & ~3u) {
        case 0x00208000: return "PWM.PWMC";
        case 0x00208004: return "PWM.PWMS";
        case 0x00208008: return "PWM.PWMP";
        case 0x0020800c: return "PWM.PWMCNT";
        default: return nullptr;
    }
}

void Imx1Pwm::advance(uint64_t hclkCycles) {
    resetRemaining = hclkCycles >= resetRemaining ? 0 : resetRemaining - hclkCycles;
}

void Imx1Pwm::freeze() {
    counter = sourcePhase = 0;
    head = count = repeatsRemaining = 0;
    irq = false;
    referencePhase = 0;
    referenceDenominator = 1;
}

bool Imx1Pwm::referenceRatioSupported(uint64_t denominator) const {
    if (!denominator) return false;
    const auto factor = denominator / imx1Gcd(denominator, referenceDenominator);
    return factor <= (std::numeric_limits<uint64_t>::max() / 2) / referenceDenominator;
}

void Imx1Pwm::advanceReference(uint64_t numerator, uint64_t denominator) {
    assert(referenceRatioSupported(denominator));
    if (!enabled() || !(control & 0x8000) || !numerator) return;
    const auto common = referenceDenominator * (denominator / imx1Gcd(denominator, referenceDenominator));
    const auto fraction = referencePhase * (common / referenceDenominator) +
                          (numerator % denominator) * (common / denominator);
    referencePhase = fraction % common;
    referenceDenominator = common;
    advanceSource(numerator / denominator + fraction / common);
}

void Imx1Pwm::advancePerclk1(uint64_t cycles) {
    if (usesPerclk1()) advanceSource(cycles);
}

void Imx1Pwm::advanceSource(uint64_t cycles) {
    const uint32_t divisor = (2u << (control & 3)) * (((control >> 8) & 127) + 1);
    const uint32_t phase = sourcePhase + cycles % divisor;
    sourcePhase = phase % divisor;
    advanceCounter(cycles / divisor + phase / divisor);
}

void Imx1Pwm::newPeriod() {
    if (repeatsRemaining > 1) --repeatsRemaining;
    else if (count) {
        activeSample = fifo[head];
        activeKnown = true;
        head = (head + 1) % fifo.size();
        --count;
        repeatsRemaining = 1u << ((control >> 2) & 3);
    } else repeatsRemaining = 0;
    if (count <= 1) irq = true;
}

void Imx1Pwm::advanceCounter(uint64_t ticks) {
    const uint32_t length = periodLength();
    const uint32_t remaining = length - counter;
    if (ticks < remaining) { counter += ticks; return; }
    ticks -= remaining;
    counter = 0;
    newPeriod();
    // At most four queued samples and eight periods per sample need individual work.
    while (ticks >= length && (count || repeatsRemaining > 1)) {
        ticks -= length;
        newPeriod();
    }
    if (ticks >= length) {
        irq = true;
        repeatsRemaining = 0;
    }
    counter = ticks % length;
}

int Imx1Pwm::output() const {
    if (!enabled()) return 0;
    if (!activeKnown) return -1;
    return counter < activeSample ? 1 : 0;
}

Imx1PwmAccess Imx1Pwm::access(uint32_t address, uint32_t size, bool write, uint32_t& value) {
    if (!registerName(address)) return Imx1PwmAccess::unmapped;
    if (size != 4 || (address & 3)) return Imx1PwmAccess::width;
    if (!write) {
        switch (address) {
            case 0x00208000:
                value = (control & ~0xcu) | (count < fifo.size() ? 0x20 : 0) |
                        (irq ? 0x80 : 0) | (resetting() ? 0x10000 : 0);
                irq = false;
                break;
            // Table 22-5 leaves SAMPLE unknown at reset.
            case 0x00208004:
                if (!sampleKnown) return Imx1PwmAccess::externalInput;
                value = sample;
                break;
            case 0x00208008: value = period; break;
            case 0x0020800c: value = counter; break;
        }
        return Imx1PwmAccess::ok;
    }
    if (address == 0x00208000) {
        if (value & 0x10000) {
            if (resetting()) return Imx1PwmAccess::sideEffect;
            // Whole-module reset takes priority over EN and the other control fields.
            *this = Imx1Pwm{};
            resetRemaining = 5;
            return Imx1PwmAccess::ok;
        }
        if (resetting()) return (value & 0x10) ? Imx1PwmAccess::sideEffect : Imx1PwmAccess::ok;
        if (!(value & 0x10)) {
            if (enabled()) { control &= ~0x10u; freeze(); }
            return Imx1PwmAccess::ok;
        }
        // Live clock and sample-repeat changes have no qualified phase boundary.
        if (enabled() && ((value ^ control) & 0xff0f)) return Imx1PwmAccess::sideEffect;
        if (!enabled()) { freeze(); irq = true; }
        control = value & 0x6ff5f;
        if (value & 0x80) irq = true; // Table 22-3 permits a software debug request.
        return Imx1PwmAccess::ok;
    }
    // Disabled data/configuration writes are ignored (table 22-3).
    if (!enabled()) return Imx1PwmAccess::ok;
    if (address == 0x00208004 && count < fifo.size()) {
        const auto data = (control & 0x40000) ? value >> 16 : value;
        sample = data;
        sampleKnown = true;
        const uint16_t swapped = (control & 0x20000) ? (sample >> 8) | (sample << 8) : sample;
        fifo[(head + count) % fifo.size()] = swapped;
        ++count;
    } else if (address == 0x00208008) {
        const auto next = value & 0xffff;
        const auto length = (next == 0xffff ? 0xfffe : next) + 2;
        if (counter >= length) return Imx1PwmAccess::sideEffect;
        period = next;
    }
    // PWMCNT is read-only.
    return Imx1PwmAccess::ok;
}
