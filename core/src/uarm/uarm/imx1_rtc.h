#ifndef FROND_IMX1_RTC_H
#define FROND_IMX1_RTC_H

#include <cstdint>
#include "imx1_reference_time.h"

enum class Imx1RtcAccess { ok, unmapped, width, reservedBits, sideEffect, externalInput };
struct Imx1RtcTime { uint32_t day = 0, hour = 0, minute = 0, second = 0; };

// MC9328MX1RM chapter 23. Retained TOD is unknown until explicitly supplied.
class Imx1Rtc {
   public:
    bool initialise(Imx1RtcTime time);
    static bool validTime(Imx1RtcTime time);
    static const char* registerName(uint32_t address);
    Imx1RtcAccess access(uint32_t address, uint32_t size, bool write, uint32_t& value);
    bool referenceRatioSupported(uint64_t denominator) const;
    // Elapsed CLK32 periods, independent of System PLL output edges.
    void advanceReference(uint64_t numerator, uint64_t denominator);
    // Steps of numerator/denominator CLK32 periods until a status bit can next be set.
    uint64_t idleQuietSteps(uint64_t numerator, uint64_t denominator) const;
    static constexpr uint64_t interruptSources = (uint64_t(1) << 17) | (uint64_t(1) << 18);
    uint64_t assertedInterrupts() const;
    uint64_t futureInterrupts() const {
        if (!known || !(control & 0x80)) return 0;
        return ((enable & 0x003f) ? uint64_t(1) << 17 : 0) |
               ((enable & 0xff00) ? uint64_t(1) << 18 : 0);
    }
    // The normal watchdog input is absent or at most CLK32 / 16384 in this subset.
    bool watchdogClockBoundSupported() const { return (control & 0x80) && divisor() == 32768 && !(enable & 0x80); }
    bool preservesWatchdogClock(const Imx1Rtc& next, uint32_t address) const;
    // DAYR, HOURMIN and SECONDS "cannot be reset" (MC9328MX1RM 23.2.1 to 23.2.3): a board that has
    // just been reset goes on with the time of the board as it was. The fraction of a CLK32 period is dropped.
    // ASSUMPTION "RTC prescaler kept across reset": the count inside the second is kept with the time.
    // The manual says nothing about the prescaler on a reset.
    void keepTime(const Imx1Rtc& before) {
        known = before.known; used = before.used; elapsed = before.elapsed;
        tod = before.tod;
        prescaler = before.prescaler % divisor();
    }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.DoBool(known).DoBool(used).DoBool(elapsed).Do32(tod).Do32(alarm).Do32(control).Do32(status).Do32(enable);
        a.Do32(prescaler).Do32(samplingPhase).Do64(referencePhase).Do64(referenceDenominator);
        a.Require(referenceDenominator != 0, "rtc denominator");
    }

   private:
    bool known = false, used = false, elapsed = false;
    uint32_t tod = 0, alarm = 0, control = 0x80, status = 0, enable = 0;
    uint32_t prescaler = 0, samplingPhase = 0;
    uint64_t referencePhase = 0, referenceDenominator = 1;
    uint32_t divisor() const { return ((control >> 5) & 3) == 1 ? 32000 : 32768; }
    void advanceTicks(uint64_t ticks);
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Rtc) == 48, "update state() and stateVersion");
#endif
#endif
