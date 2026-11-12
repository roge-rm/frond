#include "imx1_watchdog.h"

#include <cassert>
#include <limits>
#include <numeric>

bool Imx1Watchdog::referenceRatioSupported(uint64_t denominator) const {
    if (!denominator) return false;
    if (!enabled() || uncertain) return true;
    const auto factor = denominator / imx1Gcd(denominator, referenceDenominator);
    return factor <= (std::numeric_limits<uint64_t>::max() / 2) / referenceDenominator;
}
void Imx1Watchdog::reloadBound() {
    // Unknown CLK2HZ phase permits one immediate edge. WT edges can reach zero.
    // Using WT also covers the manual's longer (WT + 1) half-second range.
    remainingTicks = ((control >> 8) - 1) * uint64_t(16384);
    referencePhase = 0;
    referenceDenominator = 1;
}
void Imx1Watchdog::advanceReference(uint64_t numerator, uint64_t denominator) {
    assert(referenceRatioSupported(denominator));
    if (!enabled() || uncertain || !numerator) return;
    const auto common = referenceDenominator * (denominator / imx1Gcd(denominator, referenceDenominator));
    const auto fraction = referencePhase * (common / referenceDenominator) +
                          (numerator % denominator) * (common / denominator);
    referencePhase = fraction % common;
    referenceDenominator = common;
    const auto ticks = numerator / denominator + fraction / common;
    if (ticks >= remainingTicks) uncertain = true;
    else remainingTicks -= ticks;
}

const char* Imx1Watchdog::registerName(uint32_t address) {
    switch (address & ~3u) {
        case 0x00201000: return "WDOG.WCR";
        case 0x00201004: return "WDOG.WSR";
        case 0x00201008: return "WDOG.WSTR";
        default: return nullptr;
    }
}

Imx1WatchdogAccess Imx1Watchdog::access(uint32_t address, uint32_t size, bool write, uint32_t& value) {
    if (!registerName(address)) return Imx1WatchdogAccess::unmapped;
    if (size != 4 || (address & 3)) return Imx1WatchdogAccess::width;
    if (uncertain) return Imx1WatchdogAccess::possibleTimeout;
    if (!write) {
        switch (address) {
            case 0x00201000: value = control; break;
            case 0x00201004: value = service; break;
            case 0x00201008: value = 0; break;
        }
        return Imx1WatchdogAccess::ok;
    }
    if (address == 0x00201008) return Imx1WatchdogAccess::sideEffect;
    if (address == 0x00201004) {
        if (value & 0xffff0000) return Imx1WatchdogAccess::reservedBits;
        if (enabled() && serviceArmed && value == 0xaaaa) reloadBound();
        // Repeated prefixes conservatively invalidate the sequence until another value.
        if (enabled() && value == 0x5555) {
            serviceRepeated = serviceArmed || serviceRepeated;
            serviceArmed = !serviceRepeated;
        } else serviceArmed = serviceRepeated = false;
        service = value;
        return Imx1WatchdogAccess::ok;
    }
    if (value & ~0xff1fu) return Imx1WatchdogAccess::reservedBits;
    // WDEC controls WDE write access. A write-once WDE retains its previous value.
    const uint32_t next = (value & 2) || !enableWritten ? value : (value & ~1u) | (control & 1);
    if (next & 4) return Imx1WatchdogAccess::resetRequest;
    if (next & 1) {
        // WT must be programmed before enable. Active retiming, halt and IRQ mode are guarded.
        if ((next & 0x8018) || (next >> 8) < 2 || !enableWritten ||
            ((next ^ control) & 0x7f00)) return Imx1WatchdogAccess::sideEffect;
    }
    const bool starting = (next & 1) && !enabled();
    const bool stopping = !(next & 1) && enabled();
    control = next;
    if (starting) reloadBound();
    if (starting || stopping) serviceArmed = serviceRepeated = false;
    enableWritten = true;
    return Imx1WatchdogAccess::ok;
}
