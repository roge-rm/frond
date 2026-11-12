#ifndef FROND_IMX1_CGM_H
#define FROND_IMX1_CGM_H

#include <cstdint>
#include "imx1_reference_time.h"

enum class Imx1CgmAccess { ok, unmapped, width, reservedBits, sideEffect, externalInput };

struct Imx1PllRatio {
    uint64_t numerator;
    uint32_t denominator;
};

// MC9328MX1RM chapter 12. Internal-source FastBus timing subset.
class Imx1Cgm {
   public:
    Imx1CgmAccess access(uint32_t address, uint32_t size, bool write, uint32_t& value);
    static const char* registerName(uint32_t address);
    void advanceSystemPllCycles(uint64_t cycles, bool wholeWritePeriods = true);
    void advanceSystemWriteFraction(uint64_t numerator, uint64_t denominator);
    void setSystemPllScenario(bool enabled) { nominalSystemPll = enabled; }
    void setMcuRetuneScenario(bool enabled) { nominalMcuRetune = enabled; }
    void qualifyMcuOutput() { mcuOutputQualified = true; }
    bool mcuReferenceSupported(uint64_t denominator) const;
    void advanceMcuReference(uint64_t numerator, uint64_t denominator);
    Imx1PllRatio systemPllTargetRatio() const;
    Imx1PllRatio systemPllActiveRatio() const;
    uint64_t systemRestartRemaining() const { return systemRestartUnits; }
    // Units are 1 / MCU PLL numerator CLK32 periods, independent of System PLL edges.
    bool advanceSystemRestart(uint64_t units);
    // Nominal programmed ratio to CLK32, without asserting a valid or locked output.
    Imx1PllRatio mcuPllTargetRatio() const;
    // Reset output is known; programmed output needs restart completion and an explicit assumption.
    // The equal-frequency retune scenario keeps that qualified nominal output continuous.
    bool mcuEnabled() const { return cscr & 1; }
    // SPEN, and the CLK32 edges the System PLL goes on for once it is cleared (SD_CNT, table 12-5).
    bool systemPllEnabled() const { return cscr & 2; }
    uint32_t systemShutdownEdges() const { return ((cscr >> 24) & 3) + 1; }
    // Both PLLs back on after an interrupt (table 12-3), locked after the wake time of 12.4.2.
    void wakePlls();
    bool mcuWakePending() const { return mcuWakeRemaining != 0; }
    void wakeMcuPll();
    // Both PLLs on and locked, as a wake from stop mode leaves them once the lock time has passed.
    void restartAfterStop() {
        wakePlls();
        mcuWakeRemaining = 0;
        mcuWakePhase = 0;
        mcuWakeDenominator = 1;
        mcuRestartComplete = true;
    }
    // Steps of numerator/denominator CLK32 periods until a clock state changes. 1 while a restart or write delay runs.
    uint64_t idleQuietSteps(uint64_t numerator, uint64_t denominator) const {
        if (systemWriteDelay || mcuRestartCycles || systemRestartUnits ||
            (mcuEnabled() && (mcuReferenceRemaining || (nominalMcuRetune && mcuOutputQualified && mcuWriteDelay)))) return 1;
        if (!mcuWakeRemaining) return UINT64_MAX;
        return imx1StepsUntil(mcuWakePhase, mcuWakeDenominator, numerator, denominator, mcuWakeRemaining);
    }
    bool mcuPowerQualified() const { return mcuOutputQualified; }
    bool mcuClockReady(bool assumeSettled) const {
        if (!mcuEnabled() || mcuWakeRemaining) return false;
        return !mcuTimingUnknown || (assumeSettled && (mcuContinuous ||
            (mcuRestartComplete && !mcuRestartCycles)));
    }
    uint32_t mcuPrescaler() const { return (cscr & 0x8000) ? 2 : 1; }
    uint32_t busDivider() const { return ((cscr >> 10) & 15) + 1; }
    uint32_t perclk2Divider() const { return ((pcdr >> 4) & 15) + 1; }
    uint32_t perclk3Divider() const { return ((pcdr >> 16) & 127) + 1; }
    uint32_t perclk1Divider() const { return (pcdr & 15) + 1; }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.Do32(cscr).Do32(pcdr).Do32(mpctl0).Do32(mpctl1).Do32(spctl0).Do32(spctl1);
        a.Do32(mcuRestartCycles).Do32(mcuWakeRemaining).Do64(mcuWakePhase).Do64(mcuWakeDenominator);
        a.Do32(activeSpctl0).Do32(systemWriteDelay).Do64(systemWritePhase).Do64(systemRestartUnits);
        a.FixedBool(nominalSystemPll).FixedBool(nominalMcuRetune).DoBool(mcuOutputQualified).DoBool(mcuContinuous);
        a.Do32(mcuWriteDelay).Do32(mcuReferenceRemaining).Do64(mcuReferencePhase).Do64(mcuReferenceDenominator);
        a.Do64(mcuWritePhase).Do64(mcuWriteDenominator).DoBool(mcuTimingUnknown).DoBool(mcuRestartComplete);
        a.Require(mcuWakeDenominator && mcuReferenceDenominator && mcuWriteDenominator, "cgm denominator");
    }

   private:
    uint32_t cscr = 0x0f00ac03, pcdr = 0x000b00bb;
    uint32_t mpctl0 = 0x003f1437, mpctl1 = 0;
    uint32_t spctl0 = 0x043f1437, spctl1 = 0;
    uint32_t mcuRestartCycles = 0, mcuWakeRemaining = 0;
    uint64_t mcuWakePhase = 0, mcuWakeDenominator = 1;
    uint32_t activeSpctl0 = 0x043f1437;
    uint32_t systemWriteDelay = 0;
    uint64_t systemWritePhase = 0;
    uint64_t systemRestartUnits = 0;
    bool nominalSystemPll = false;
    bool nominalMcuRetune = false, mcuOutputQualified = false, mcuContinuous = false;
    uint32_t mcuWriteDelay = 0, mcuReferenceRemaining = 0;
    uint64_t mcuReferencePhase = 0, mcuReferenceDenominator = 1;
    uint64_t mcuWritePhase = 0, mcuWriteDenominator = 1;
    // No MCU lock flag or qualified settling interval is available after programming.
    bool mcuTimingUnknown = false;
    bool mcuRestartComplete = false;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Cgm) == 128, "update state() and stateVersion");
#endif

#endif
