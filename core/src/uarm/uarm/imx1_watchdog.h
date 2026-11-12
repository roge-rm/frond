#ifndef FROND_IMX1_WATCHDOG_H
#define FROND_IMX1_WATCHDOG_H

#include <cstdint>
#include "imx1_reference_time.h"

enum class Imx1WatchdogAccess { ok, unmapped, width, reservedBits, sideEffect, resetRequest, possibleTimeout };

// MC9328MX1RM chapter 14. Cold reset and a conservative interval before any possible normal-clock timeout.
class Imx1Watchdog {
   public:
    Imx1WatchdogAccess access(uint32_t address, uint32_t size, bool write, uint32_t& value);
    bool enabled() const { return control & 1; }
    bool possibleTimeout() const { return uncertain; }
    bool referenceRatioSupported(uint64_t denominator) const;
    void advanceReference(uint64_t numerator, uint64_t denominator);
    // Steps of numerator/denominator CLK32 periods until the timeout bound is reached.
    uint64_t idleQuietSteps(uint64_t numerator, uint64_t denominator) const {
        if (!enabled() || uncertain) return UINT64_MAX;
        return imx1StepsUntil(referencePhase, referenceDenominator, numerator, denominator, remainingTicks);
    }
    static const char* registerName(uint32_t address);
    static constexpr uint64_t interruptSource = uint64_t(1) << 63;
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.Do32(control).Do32(service).DoBool(enableWritten).DoBool(serviceArmed).DoBool(serviceRepeated).DoBool(uncertain);
        a.Do64(remainingTicks).Do64(referencePhase).Do64(referenceDenominator);
        a.Require(referenceDenominator != 0, "watchdog denominator");
    }

   private:
    uint32_t control = 0, service = 0;
    bool enableWritten = false, serviceArmed = false, serviceRepeated = false, uncertain = false;
    uint64_t remainingTicks = 0, referencePhase = 0, referenceDenominator = 1;
    void reloadBound();
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Watchdog) == 40, "update state() and stateVersion");
#endif

#endif
