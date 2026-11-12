#ifndef FROND_IMX1_ASP_H
#define FROND_IMX1_ASP_H

#include <cstdint>
#include <array>

enum class Imx1AspAccess { ok, unmapped, width, reservedBits, sideEffect, externalInput };

// Explicit post-filter words, with no assigned voltage or calibration coefficients.
struct Imx1AspPenInputs {
    bool contactKnown = false, contactDown = false;
    bool normalKnown = false, referenceKnown = false;
    std::array<uint16_t, 4> normal{};  // Z, X, Y, U
    std::array<uint16_t, 3> reference{};  // ACAL Z, X, Y
};

// MC9328MX1RM chapter 15. Qualified manual U and native pen sampling modes.
class Imx1Asp {
   public:
    explicit Imx1Asp(bool px1HighFromReset = false, bool uSampleKnown = false, uint16_t uSample = 0)
        : px1HighFromReset(px1HighFromReset), uSampleKnown(uSampleKnown), uSample(uSample) {
        penInputs.contactKnown = px1HighFromReset;
    }
    // Execution-thread transaction. In-flight slots keep their captured input.
    Imx1AspAccess replacePenInputs(const Imx1AspPenInputs& inputs);
    const Imx1AspPenInputs& currentPenInputs() const { return penInputs; }
    // The pen ADC clock is divided from PERCLK2 (manual table 12-6), so HCLK and BCLK_DIV don't time it.
    void advance(uint64_t perclk2Cycles);
    uint64_t futureInterrupts() const;
    // PERCLK2 cycles until the running slot or pause ends, 0 when nothing runs.
    uint64_t idleQuietPerclk2() const {
        const uint64_t divide = clockDivider();
        if (!remaining || !divide) return 0;
        return phase < divide ? remaining * divide - phase : 1;
    }
    bool converting() const { return remaining != 0; }
    Imx1AspAccess access(uint32_t address, uint32_t size, bool write, uint32_t& value);
    static const char* registerName(uint32_t address);
    static constexpr uint64_t interruptSources = (uint64_t(1) << 5) | (uint64_t(1) << 9) |
                                                (uint64_t(1) << 33) | (uint64_t(1) << 46);
    // Only known active-low level contact detection is admitted.
    uint64_t inactiveInterrupts() const {
        uint64_t known = interruptSources & ~(uint64_t(1) << 46);
        if (!(interrupts & 0x10) || qualifiedPenDetection(interrupts)) known |= uint64_t(1) << 46;
        return known & ~assertedInterrupts();
    }
    uint64_t assertedInterrupts() const {
        uint64_t requests = 0;
        if ((interrupts & 0x10) && pen) requests |= uint64_t(1) << 46;
        if ((interrupts & 0x400) && penUp) requests |= uint64_t(1) << 5;
        if ((compare & 0x30000) && (compare & 0x80000)) requests |= uint64_t(1) << 9;
        if ((count && (interrupts & 1)) || (count == 12 && (interrupts & 2))) requests |= uint64_t(1) << 33;
        return requests;
    }
    unsigned clockDivider() const { return (control & (1u << 25)) && divider ? divider + 1 : 0; }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.FixedBool(px1HighFromReset).FixedBool(uSampleKnown).Fixed16(uSample);
        a.DoBuffer16(fifo.data(), fifo.size()).Do32(readPointer).Do32(writePointer).Do32(count);
        a.DoBool(penInputs.contactKnown).DoBool(penInputs.contactDown);
        a.DoBool(penInputs.normalKnown).DoBool(penInputs.referenceKnown);
        a.DoBuffer16(penInputs.normal.data(), 4).DoBuffer16(penInputs.reference.data(), 3);
        a.DoBool(overflow).DoBool(armed).DoBool(pen).DoBool(penUp).DoBool(idle).DoBool(capturedUp);
        a.Do32(slot).Do16(captured).Do32(remaining).Do32(phase);
        a.Do32(control).Do32(sampleRate).Do32(compare).Do32(interrupts).Do32(divider);
        a.Require(readPointer < fifo.size() && writePointer < fifo.size() && count <= fifo.size(), "asp fifo");
        a.Require(slot < (calibration() ? 3u : 4u), "asp slot");
    }

   private:
    // Software reset preserves external inputs.
    bool px1HighFromReset, uSampleKnown;
    uint16_t uSample;
    std::array<uint16_t, 12> fifo{};
    unsigned readPointer = 0, writePointer = 0, count = 0;
    Imx1AspPenInputs penInputs{};
    bool overflow = false, armed = false, pen = false, penUp = false;
    bool idle = false, capturedUp = false;
    unsigned slot = 0;
    uint16_t captured = 0;
    uint32_t remaining = 0;
    unsigned phase = 0;
    bool qualifiedU(uint32_t nextControl, uint32_t nextRate) const {
        return uSampleKnown && (nextControl & 0x3001) == 0x3001 &&
               !(nextControl & ((1u << 21) | (1u << 14))) && (nextRate & 0xc00) == 0x800;
    }
    bool qualifiedAuto(uint32_t nextControl, uint32_t nextRate) const {
        const auto mode = nextControl & ~((1u << 25) | 2u);
        return nextRate == 0x3814 &&
            ((mode == 0x0400d201 && penInputs.referenceKnown) ||
             (mode == 0x0000e001 && penInputs.normalKnown && contactKnown()));
    }
    bool qualifiedSampling(uint32_t nextControl, uint32_t nextRate) const {
        return qualifiedU(nextControl, nextRate) || qualifiedAuto(nextControl, nextRate);
    }
    bool automatic() const { return control & 0x4000; }
    bool calibration() const { return control & (1u << 26); }
    bool contactKnown() const { return penInputs.contactKnown; }
    bool contactDown() const { return penInputs.contactKnown && penInputs.contactDown; }
    bool qualifiedPenDetection(uint32_t nextInterrupts) const {
        return contactKnown() && !(nextInterrupts & 0x60);
    }
    void refreshPen() {
        if ((interrupts & 0x10) && qualifiedPenDetection(interrupts) && contactDown()) pen = true;
    }
    // ASSUMPTION "ASP one-write restore". Table 15-7 only says PADE "must be set after all other
    // control bits are set" and gives no result for setting it in the same write as the mode. The
    // Zodiac ROM's battery routine (20088588) needs that write to start sampling: with the pen down
    // it resets the module (SWRST), writes CLKDIV 99, PSMPLRG 3814 and ICNTLR 10, then puts its saved
    // control word 0200e003 back in one write at 200885fc and never touches PADE again. Only that
    // word, on a module in the state those writes leave, is taken as the mode write followed by the
    // PADE write: Auto ZXYU starts from an empty FIFO. Not checked on a device.
    bool oneWriteRestore(uint32_t written) const {
        return written == 0x0200e003 && control == 0x02000000 && sampleRate == 0x3814 && interrupts == 0x10 &&
               divider == (99 & 0x1f) && !compare && !count && !readPointer && !writePointer && !remaining &&
               !overflow && !penUp && !armed && !idle;
    }
    void startSlot();
    void complete();
    uint32_t control = 0x02000000, sampleRate = 0, compare = 0, interrupts = 0, divider = 0;
    bool samplingRequested(uint32_t nextControl, uint32_t nextDivider) const {
        return (nextControl & (1u << 25)) && nextDivider && (nextControl & 2) && (nextControl & 0x3000);
    }
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Asp) == 100, "update state() and stateVersion");
#endif

#endif
