#ifndef FROND_IMX1_PWM_H
#define FROND_IMX1_PWM_H

#include <cstdint>
#include <array>
#include "imx1_reference_time.h"

enum class Imx1PwmAccess { ok, unmapped, width, sideEffect, externalInput };

// MC9328MX1RM chapter 22. Functional timing, without a board output connection.
class Imx1Pwm {
   public:
    Imx1PwmAccess access(uint32_t address, uint32_t size, bool write, uint32_t& value);
    static const char* registerName(uint32_t address);
    static constexpr uint64_t interruptSource = uint64_t(1) << 34;
    void advance(uint64_t hclkCycles);
    void advancePerclk1(uint64_t cycles);
    bool referenceRatioSupported(uint64_t denominator) const;
    void advanceReference(uint64_t numerator, uint64_t denominator);
    bool resetting() const { return resetRemaining != 0; }
    bool enabled() const { return control & 0x10; }
    bool usesPerclk1() const { return enabled() && !(control & 0x8000); }
    bool interruptOutput() const { return irq && (control & 0x40); }
    bool usesReference() const { return enabled() && (control & 0x8000); }
    // The duty the CPU last wrote: the last sample's high clocks per period and the period, false while off
    // or with no sample written. The output takes the sample up at a later period boundary; this follows the
    // writes only, so it changes at the same guest time however far the board is advanced at once.
    bool writtenDuty(uint32_t& high, uint32_t& length) const {
        if (!enabled() || !sampleKnown) return false;
        length = periodLength();
        const uint32_t s = (control & 0x20000) ? uint16_t((sample >> 8) | (sample << 8)) : sample;
        high = s < length ? s : length;
        return true;
    }
    // Source clocks until the current period ends, 0 when disabled.
    uint64_t idleQuietCycles() const {
        if (!enabled()) return 0;
        const uint64_t divisor = uint64_t(2u << (control & 3)) * (((control >> 8) & 127) + 1);
        if (counter >= periodLength() || sourcePhase >= divisor) return 1;
        return uint64_t(periodLength() - counter) * divisor - sourcePhase;
    }
    uint64_t idleQuietReferenceSteps(uint64_t numerator, uint64_t denominator) const {
        return imx1StepsUntil(referencePhase, referenceDenominator, numerator, denominator, idleQuietCycles());
    }
    // -1 means the active sample has not been supplied.
    int output() const;
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.Do32(resetRemaining).Do32(control).Do32(period).Do32(counter).Do32(sourcePhase);
        a.Do16(sample).Do16(activeSample).DoBool(sampleKnown).DoBool(activeKnown).DoBool(irq);
        a.DoBuffer16(fifo.data(), fifo.size()).Do32(head).Do32(count).Do32(repeatsRemaining);
        a.Do64(referencePhase).Do64(referenceDenominator);
        a.Require(head < fifo.size() && count <= fifo.size() && referenceDenominator, "pwm fifo");
    }

   private:
    uint32_t resetRemaining = 0;
    uint32_t control = 0, period = 0xfffe, counter = 0, sourcePhase = 0;
    uint16_t sample = 0, activeSample = 0;
    bool sampleKnown = false, activeKnown = false, irq = false;
    std::array<uint16_t, 4> fifo{};
    unsigned head = 0, count = 0, repeatsRemaining = 0;
    uint64_t referencePhase = 0, referenceDenominator = 1;
    uint32_t periodLength() const { return (period == 0xffff ? 0xfffe : period) + 2; }
    void advanceSource(uint64_t cycles);
    void advanceCounter(uint64_t ticks);
    void newPeriod();
    void freeze();
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Pwm) == 64, "update state() and stateVersion");
#endif

#endif
