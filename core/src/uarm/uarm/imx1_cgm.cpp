#include "imx1_cgm.h"

#include <algorithm>
#include <numeric>
#include <cassert>
#include <limits>

static Imx1PllRatio pllRatioOf(uint32_t control);

// The last two control words and their ratios, as the MCU and System PLL words are asked for in turn.
// The ratio depends on the word alone.
static Imx1PllRatio pllRatio(uint32_t control) {
    struct Entry { uint32_t control; bool known; Imx1PllRatio ratio; };
    static thread_local Entry entries[2]{};
    for (auto& entry : entries)
        if (entry.known && entry.control == control) return entry.ratio;
    static thread_local unsigned next = 0;
    auto& entry = entries[next];
    next ^= 1;
    entry = {control, true, pllRatioOf(control)};
    return entry.ratio;
}

static Imx1PllRatio pllRatioOf(uint32_t control) {
    // Equation 12-1 and table 12-9: internal reference = 512 * CLK32, MFN unsigned.
    const uint32_t pd = ((control >> 26) & 15) + 1;
    const uint32_t mfd = ((control >> 16) & 1023) + 1;
    const uint32_t mfi = std::max(5u, (control >> 10) & 15);
    const uint32_t mfn = control & 1023;
    const uint64_t numerator = uint64_t(1024) * (mfi * mfd + mfn);
    const uint32_t denominator = pd * mfd;
    const uint32_t common = std::gcd(numerator, uint64_t(denominator));
    return {numerator / common, denominator / common};
}

Imx1PllRatio Imx1Cgm::mcuPllTargetRatio() const { return pllRatio(mpctl0); }
Imx1PllRatio Imx1Cgm::systemPllTargetRatio() const { return pllRatio(spctl0); }
Imx1PllRatio Imx1Cgm::systemPllActiveRatio() const { return pllRatio(activeSpctl0); }

bool Imx1Cgm::mcuReferenceSupported(uint64_t denominator) const {
    if (!denominator) return false;
    const auto supports = [](uint64_t d, uint64_t old) {
        return d / imx1Gcd(d, old) <= (std::numeric_limits<uint64_t>::max() / 2) / old;
    };
    if (mcuWakeRemaining && !supports(denominator, mcuWakeDenominator)) return false;
    if (mcuReferenceRemaining && !supports(denominator, mcuReferenceDenominator)) return false;
    if (nominalMcuRetune && mcuOutputQualified && mcuWriteDelay) {
        const auto rate = mcuPllTargetRatio();
        const auto divider = uint64_t(rate.denominator) * mcuPrescaler();
        if (denominator > std::numeric_limits<uint64_t>::max() / std::max(rate.numerator, divider) ||
            !supports(denominator * divider, mcuWriteDenominator)) return false;
    }
    return true;
}

void Imx1Cgm::wakePlls() {
    const bool mcu = !mcuEnabled();
    cscr |= 2;
    if (mcu) { wakeMcuPll(); return; }
    // The System PLL alone: the same nominal lock time, with the MCU PLL's state left as it is.
    mcuWakeRemaining = 9;
    mcuWakePhase = 0;
    mcuWakeDenominator = 1;
}

void Imx1Cgm::wakeMcuPll() {
    if (mcuEnabled()) return;
    cscr |= 1;
    mcuTimingUnknown = true;
    mcuRestartComplete = false;
    mcuContinuous = false;
    // MC9328MX1RM 12.4.2: nominal nine CLK32 periods (274.66 us), within the 300 us wake bound.
    mcuWakeRemaining = 9;
    mcuWakePhase = 0;
    mcuWakeDenominator = 1;
}

void Imx1Cgm::advanceMcuReference(uint64_t numerator, uint64_t denominator) {
    if (!numerator) return;
    assert(mcuReferenceSupported(denominator));
    if (mcuWakeRemaining) {
        const auto common = mcuWakeDenominator * (denominator / imx1Gcd(denominator, mcuWakeDenominator));
        const auto fraction = mcuWakePhase * (common / mcuWakeDenominator) +
                              (numerator % denominator) * (common / denominator);
        const auto elapsed = numerator / denominator + fraction / common;
        mcuWakePhase = fraction % common;
        mcuWakeDenominator = common;
        mcuWakeRemaining -= std::min<uint64_t>(elapsed, mcuWakeRemaining);
        if (!mcuWakeRemaining) mcuRestartComplete = true;
    }
    if (!mcuEnabled()) return;
    if (nominalMcuRetune && mcuOutputQualified && mcuWriteDelay) {
        // Every legal internal-source MCU ratio supplies at least 160 FCLK periods per CLK32.
        if (numerator / denominator) mcuWriteDelay = 0;
        else {
            const auto rate = mcuPllTargetRatio();
            const auto d = denominator * rate.denominator * mcuPrescaler();
            const auto n = numerator * rate.numerator;
            const auto common = mcuWriteDenominator * (d / imx1Gcd(d, mcuWriteDenominator));
            const auto fraction = mcuWritePhase * (common / mcuWriteDenominator) + (n % d) * (common / d);
            mcuWriteDelay -= std::min<uint64_t>(n / d + fraction / common, mcuWriteDelay);
            mcuWritePhase = fraction % common;
            mcuWriteDenominator = common;
        }
    }
    if (!mcuReferenceRemaining) return;
    const auto common = mcuReferenceDenominator * (denominator / imx1Gcd(denominator, mcuReferenceDenominator));
    const auto fraction = mcuReferencePhase * (common / mcuReferenceDenominator) +
                          (numerator % denominator) * (common / denominator);
    const auto elapsed = numerator / denominator + fraction / common;
    mcuReferencePhase = fraction % common;
    mcuReferenceDenominator = common;
    mcuReferenceRemaining -= std::min<uint64_t>(elapsed, mcuReferenceRemaining);
    if (!mcuReferenceRemaining) {
        mcuRestartComplete = true;
        cscr &= ~0x00200000u;
    }
}

bool Imx1Cgm::advanceSystemRestart(uint64_t units) {
    if (!systemRestartUnits) return false;
    if (units < systemRestartUnits) {
        systemRestartUnits -= units;
        return false;
    }
    systemRestartUnits = 0;
    activeSpctl0 = spctl0;
    // A new output period discards incomplete spacing credit conservatively.
    systemWritePhase = 0;
    cscr &= ~0x00400000u;
    return true;
}

void Imx1Cgm::advanceSystemWriteFraction(uint64_t numerator, uint64_t denominator) {
    // Write-relative progress cannot borrow the output phase from before the write.
    // The bootstrap denominator is below 2^38, so adding two remainders fits uint64_t.
    const uint64_t fraction = numerator % denominator + systemWritePhase;
    const uint64_t periods = numerator / denominator + fraction / denominator;
    systemWritePhase = fraction % denominator;
    systemWriteDelay -= std::min<uint64_t>(periods, systemWriteDelay);
}

void Imx1Cgm::advanceSystemPllCycles(uint64_t cycles, bool wholeWritePeriods) {
    if (wholeWritePeriods) systemWriteDelay -= std::min<uint64_t>(cycles, systemWriteDelay);
    if (!mcuRestartCycles) return;
    if (cycles >= mcuRestartCycles) {
        mcuRestartCycles = 0;
        mcuRestartComplete = true;
        cscr &= ~0x00200000u;
    } else {
        mcuRestartCycles -= cycles;
    }
}

const char* Imx1Cgm::registerName(uint32_t address) {
    switch (address & ~3u) {
        case 0x0021b000: return "CGM.CSCR";
        case 0x0021b004: return "CGM.MPCTL0";
        case 0x0021b008: return "CGM.MPCTL1";
        case 0x0021b00c: return "CGM.SPCTL0";
        case 0x0021b010: return "CGM.SPCTL1";
        case 0x0021b020: return "CGM.PCDR";
        default: return nullptr;
    }
}

Imx1CgmAccess Imx1Cgm::access(uint32_t address, uint32_t size, bool write, uint32_t& value) {
    if (!registerName(address)) return Imx1CgmAccess::unmapped;
    if (size != 4 || (address & 3)) return Imx1CgmAccess::width;
    uint32_t* reg = nullptr;
    uint32_t mask = 0;
    switch (address) {
        case 0x0021b000: reg = &cscr; mask = 0xff67bc03; break;
        case 0x0021b004: reg = &mpctl0; mask = 0x3fff3fff; break;
        case 0x0021b008: reg = &mpctl1; mask = 0x00000040; break;
        case 0x0021b00c: reg = &spctl0; mask = 0x3fff3fff; break;
        case 0x0021b010: reg = &spctl1; mask = 0x00008040; break;
        case 0x0021b020: reg = &pcdr; mask = 0x007f00ff; break;
    }
    if (!write) {
        // LF depends on elapsed reference clocks, not on register reads.
        if (address == 0x0021b010) {
            if (!nominalSystemPll) return Imx1CgmAccess::externalInput;
            value = spctl1 | (systemRestartUnits ? 0 : 0x8000);
            return Imx1CgmAccess::ok;
        }
        value = *reg;
        return Imx1CgmAccess::ok;
    }
    if (value & ~mask) return Imx1CgmAccess::reservedBits;
    if (address != 0x0021b000 && !systemPllEnabled()) return Imx1CgmAccess::sideEffect;
    if ((address == 0x0021b004 || address == 0x0021b00c) &&
        (!(value & 0x03ff0000) || (value & 1023) == 1023))
        return Imx1CgmAccess::reservedBits;
    if (address == 0x0021b000) {
        if ((value >> 29) > 5) return Imx1CgmAccess::reservedBits;
        // Stop mode (12.4.3): SPEN and MPEN cleared, in one write or MPEN first, and nothing else changed.
        // The board model times the shutdown and holds everything but CLK32 while it lasts.
        if (!(value & 2)) {
            if ((value & 1) || ((value ^ cscr) & ~3u) || !systemPllEnabled() || !mcuOutputQualified ||
                mcuRestartCycles || mcuReferenceRemaining || mcuWakeRemaining || systemRestartUnits || systemWriteDelay)
                return Imx1CgmAccess::sideEffect;
            *reg = value;
            return Imx1CgmAccess::ok;
        }
        if (!systemPllEnabled()) return Imx1CgmAccess::sideEffect;
        const bool powerChanged = (value ^ cscr) & 1;
        if (powerChanged && (!mcuOutputQualified || mcuRestartCycles || mcuReferenceRemaining ||
            mcuWakeRemaining || systemRestartUnits || (value & 0x00600000) ||
            ((value ^ cscr) & ~1u))) return Imx1CgmAccess::sideEffect;
        if (!mcuEnabled() && !powerChanged && (value & 0x00600000)) return Imx1CgmAccess::sideEffect;
        if (powerChanged && (value & 1)) wakeMcuPll();
        if ((value ^ cscr) & 0x00070000) return Imx1CgmAccess::externalInput;
        if ((value & 0x00400000) && (!nominalSystemPll || systemRestartUnits || mcuReferenceRemaining))
            return Imx1CgmAccess::sideEffect;
        if (value & 0x00200000) {
            if (nominalMcuRetune && mcuOutputQualified && !mcuTimingUnknown)
                return Imx1CgmAccess::sideEffect;
            if (mcuRestartCycles || mcuReferenceRemaining) return Imx1CgmAccess::sideEffect;
            if (nominalMcuRetune && mcuOutputQualified && (mcuContinuous || mcuRestartComplete)) {
                if (systemRestartUnits || (value & 0x00400000)) return Imx1CgmAccess::sideEffect;
                // The explicit equal-frequency policy keeps the qualified nominal output continuous.
                mcuContinuous = true;
                mcuReferenceRemaining = 2;
                mcuReferencePhase = 0;
                mcuReferenceDenominator = 1;
            } else {
                // Initial internal-source reset SPCTL0 gives System PLL / CLK32 = 3000.
                mcuRestartCycles = 2 * 3000;
            }
            // Choose the documented maximum of two CLK32 periods, without claiming lock.
            mcuRestartComplete = false;
            mcuTimingUnknown = true;
        }
        if (value & 0x00400000) systemRestartUnits = 2 * mcuPllTargetRatio().numerator;
        // Writing zero has no effect on a pending self-clearing restart request.
        *reg = (value & ~0x00600000u) | ((mcuRestartCycles || mcuReferenceRemaining) ? 0x00200000u : 0) |
               (systemRestartUnits ? 0x00400000u : 0);
        return Imx1CgmAccess::ok;
    } else if (address == 0x0021b004) {
        if (!mcuEnabled() || mcuWakeRemaining) return Imx1CgmAccess::sideEffect;
        // Repeat writes need 56 FCLK cycles. Unqualified FCLK cannot establish that delay.
        if (nominalMcuRetune && mcuOutputQualified && !mcuTimingUnknown)
            return Imx1CgmAccess::sideEffect;
        if (mcuTimingUnknown) {
            if (!nominalMcuRetune || !mcuOutputQualified) return Imx1CgmAccess::externalInput;
            const auto old = mcuPllTargetRatio(), next = pllRatio(value);
            if (!mcuRestartComplete || mcuWriteDelay || mcuReferenceRemaining || systemRestartUnits ||
                old.numerator != next.numerator || old.denominator != next.denominator)
                return Imx1CgmAccess::sideEffect;
            mcuContinuous = true;
        }
        mcuWriteDelay = 56;
        mcuWritePhase = 0;
        mcuWriteDenominator = 1;
        mcuRestartComplete = false;
        mcuTimingUnknown = true;
    } else if (address == 0x0021b00c && nominalSystemPll) {
        if (systemRestartUnits || systemWriteDelay) return Imx1CgmAccess::sideEffect;
        systemWriteDelay = 30;
        systemWritePhase = 0;
    } else if (address == 0x0021b010 && nominalSystemPll) {
        // LF is driven by the nominal output event; changing LF or BRMO is unqualified.
        if (value != (spctl1 | (systemRestartUnits ? 0 : 0x8000))) return Imx1CgmAccess::sideEffect;
        return Imx1CgmAccess::ok;
    } else if (address != 0x0021b020) {
        // BRMO timing and System PLL programming remain unqualified.
        return Imx1CgmAccess::sideEffect;
    }
    *reg = value;
    return Imx1CgmAccess::ok;
}
