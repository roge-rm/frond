#ifndef FROND_IMX1_GRAPHICS_DISPLAY_H
#define FROND_IMX1_GRAPHICS_DISPLAY_H

#include <array>
#include "imx1_graphics_windows.h"
#include "imx1_graphics_clocks.h"

// Phase uses 16 units per pixel. Before clock programming, one unit is one CPU cycle.
// Programmed clocks advance phase from nominal guest time without resetting scan position.
class Imx1GraphicsDisplay {
   public:
    struct Surface {
        uint32_t base = 0, pitch = 0, width = 0, height = 0;
        int32_t xStride = 2, yStride = 0;
        uint32_t rangeBase = 0, rangeBytes = 0;
        unsigned bank = 0;
        uint32_t bankOffset = 0;
    };
    static constexpr uint32_t pixelCycles = 16, latchCycles = 32;
    static bool owns(uint32_t address) {
        if (address < 0x16010000) return false;
        const uint32_t offset = address - 0x16010000;
        if (offset == 0x4d8 || offset == 0x54c || offset == 0x4f0) return true;
        // The video overlay words (VIDEO_CTRL 434 to VIDEO_V_POS 458, the Y/U/V offset sets at 55c-570). Player
        // code drives 434-458 through playerAccess; the ROM's own overlay driver isn't modelled, so its accesses
        // (all but the boot's VIDEO_CTRL clear) and the 55c-570 sets stop as an unsupported peripheral.
        if ((offset >= 0x434 && offset <= 0x458) || (offset >= 0x55c && offset <= 0x570)) return !(offset & 3);
        for (const auto& op : operations) if (op.offset == offset) return true;
        return false;
    }
    void reset() { *this = {}; }
    template <class A> static void stateSurface(A& a, Surface& s) {
        a.Do32(s.base).Do32(s.pitch).Do32(s.width).Do32(s.height);
        a.Do32(reinterpret_cast<uint32_t&>(s.xStride)).Do32(reinterpret_cast<uint32_t&>(s.yStride));
        a.Do32(s.rangeBase).Do32(s.rangeBytes).Do32(s.bank).Do32(s.bankOffset);
        a.Require(s.width <= 1024 && s.height <= 1024 && s.bank <= 1, "display surface");
    }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.DoBuffer32(staged.data(), wordCount).DoBuffer32(active.data(), wordCount).DoBuffer32(submitted.data(), wordCount);
        for (auto& k : knownWords) a.DoBool(k);
        stateSurface(a, activeSurface);
        stateSurface(a, submittedSurface);
        a.Do64(latchDelay).Do64(phase).Do64(framePeriod).Do64(submittedPeriod);
        a.Do64(activeRevision).Do64(frames).Do64(blankEvents);
        a.Do32(updateControl).Do32(clockDelay).Do32(clockControl);
        a.DoBool(editing).DoBool(pending).DoBool(submittedEnabled).DoBool(enabled);
        a.DoBool(activeTimingKnown).DoBool(submittedTimingKnown);
        pixelFraction.state(a);
        a.Do64(rateNumerator).Do64(rateDenominator).Require(rateDenominator != 0, "display rate");
        a.DoBool(pixelSource).DoBool(systemSource).DoBool(activeRequested).DoBool(latchAtFrame);
        a.DoBool(pclk).DoBool(sclk);
    }
    uint32_t stagedWord(uint32_t offset) const { return offset >= 0x410 && offset <= 0x59c && !(offset & 3) ? staged[index(offset)] : 0; }
    uint32_t activeWord(uint32_t offset) const { return offset >= 0x410 && offset <= 0x59c && !(offset & 3) ? active[index(offset)] : 0; }
    bool latchPending() const { return pending; }
    uint64_t revision() const { return activeRevision; }
    uint64_t frameNumber() const { return frames; }
    uint64_t verticalBlankEvents() const { return blankEvents; }
    bool lcdEnabled() const { return enabled; }
    bool surface(Surface& value) const {
        if (!enabled) return false;
        value = activeSurface;
        return true;
    }
    // ASSUMPTION "display clock acknowledge 414": bits 25 and 26 follow the requests in bits 23 and 24. The
    // loops in 200ad7cc (200ad80c, 200ad838) wait for each to set and the ones in the panel off (200adaa4,
    // 200adad0) for each to clear, with no bound. A set follows 32 ticks after its request, a clear at once.
    uint32_t clockAcknowledgements() const {
        return (pclk ? 0x2000000u : 0) | (sclk ? 0x4000000u : 0);
    }
    uint64_t scanPhase() const { return phase; }
    bool clockSourcesSupported(Imx1GraphicsClocks::Rate pixel) const {
        const auto n=pixel.numerator*pixelCycles,d=Imx1GraphicsClocks::timeHz*pixel.denominator;
        return pixelFraction.supports(d/imx1Gcd(n,d));
    }
    void setClockSources(Imx1GraphicsClocks::Rate pixel, Imx1GraphicsClocks::Rate system) {
        pixelSource = pixel.numerator != 0;
        systemSource = system.numerator != 0;
        rateNumerator = pixel.numerator * pixelCycles;
        rateDenominator = Imx1GraphicsClocks::timeHz * pixel.denominator;
        const auto divisor = imx1Gcd(rateNumerator, rateDenominator);
        rateNumerator /= divisor;
        rateDenominator /= divisor;
        if (!clockDelay) updateClocks();
        else {
            if (!pixelSource) pclk = false;
            if (!systemSource) sclk = false;
        }
        enabled = activeRequested && pclk && sclk;
    }
    // Ticks until a clock change, a latch, a frame boundary or vertical blank entry. 0 when one is due.
    uint64_t idleQuietTicks() const {
        uint64_t ticks = UINT64_MAX;
        if (clockDelay) ticks = clockDelay;
        if (pending && !latchAtFrame) ticks = std::min(ticks, latchDelay);
        if (enabled && framePeriod && rateNumerator) {
            if (phase >= framePeriod) return 0;
            const uint64_t horizontal = activeWord(0x420) & 0x7ff;
            const uint64_t end = (activeWord(0x428) >> 16) & 0x3ff;
            const uint64_t edge = (end * horizontal * pixelCycles) % framePeriod;
            const uint64_t blank = phase < edge ? edge - phase : edge + framePeriod - phase;
            ticks = std::min(ticks, imx1StepsUntil(pixelFraction.phase, pixelFraction.denominator,
                                                   rateNumerator, rateDenominator, std::min(framePeriod - phase, blank)));
        }
        return ticks;
    }
    void advanceCpuCycles(uint64_t cycles) {
        uint64_t remaining = cycles;
        while (remaining) {
            uint64_t step = std::min<uint64_t>(remaining, 1000000);
            if (clockDelay && clockDelay < step) step = clockDelay;
            if (pending && !latchAtFrame && latchDelay < step) step = latchDelay;
            if (pending && latchAtFrame && enabled && rateNumerator) {
                const uint64_t units = framePeriod - phase;
                const uint64_t whole = units * rateDenominator;
                uint64_t ticks = whole / rateNumerator + (whole % rateNumerator != 0);
                // Fractional phase can move the event one tick earlier.
                if (ticks > 1) --ticks;
                step = std::min(step, ticks);
            }
            bool frameBoundary = false;
            if (enabled && framePeriod) {
                const uint64_t elapsed = phase + pixelFraction.add(step * rateNumerator, rateDenominator);
                const uint64_t horizontal = activeWord(0x420) & 0x7ff;
                const uint64_t end = (activeWord(0x428) >> 16) & 0x3ff;
                const uint64_t edge = (end * horizontal * pixelCycles) % framePeriod;
                // Entry into the vertical blank interval, including end == total.
                blankEvents += (elapsed + framePeriod - edge) / framePeriod -
                               (phase + framePeriod - edge) / framePeriod;
                const uint64_t crossed = elapsed / framePeriod;
                frames += crossed;
                phase = elapsed % framePeriod;
                frameBoundary = crossed != 0;
            }
            remaining -= step;
            const bool clocksDue = clockDelay && step >= clockDelay;
            if (clockDelay) clockDelay -= step;
            if (pending && !latchAtFrame) latchDelay -= step;
            if (clocksDue) updateClocks();
            if (pending && (latchAtFrame ? frameBoundary : latchDelay == 0)) {
                active = submitted;
                activeSurface = submittedSurface;
                activeTimingKnown = submittedTimingKnown;
                activeRequested = submittedEnabled;
                clockControl = activeWord(0x414) & 0x1800000;
                clockDelay = 0;
                updateClocks();
                enabled = activeRequested && pclk && sclk;
                framePeriod = submittedPeriod;
                pending = false;
                if (!frameBoundary) { phase = 0; pixelFraction = {}; }
                ++activeRevision;
                if (enabled && !frameBoundary) ++frames;
            }
        }
    }
    // The scan status (54c) as access reads it after ticks more, for ticks within idleQuietTicks (no clock change,
    // latch or frame boundary on the way). False where access refuses the read.
    // region (when given) is 0 before the status rows, 1 in them and 2 after them; it only grows with ticks.
    bool scanStatusAfter(uint32_t pc, uint64_t ticks, uint32_t& value, unsigned* region = nullptr) const {
        if (pc != 0x200ad7b0 && pc != 0x200adb08 && pc != 0x200adb14) return false;
        uint64_t at = phase;
        if (enabled && framePeriod) {
            if (rateNumerator && ticks > UINT64_MAX / rateNumerator) return false;
            auto fraction = pixelFraction;
            if (!fraction.supports(rateDenominator)) return false;
            at += fraction.add(ticks * rateNumerator, rateDenominator);
            if (at >= framePeriod) return false;
        }
        const uint32_t total = activeWord(0x420) & 0x7ff;
        const uint32_t vertical = activeWord(0x428);
        const uint64_t row = enabled && total ? at / (uint64_t(total) * pixelCycles) : 0;
        value = enabled && row >= (vertical & 0x3ff) && row < (vertical >> 16) ? 0x400 : 0;
        if (region) *region = !enabled || row < (vertical & 0x3ff) ? 0 : row < (vertical >> 16) ? 1 : 2;
        return true;
    }
    Imx1BootstrapAccess access(const Imx1GraphicsWindows& windows, uint32_t pc, uint32_t address,
                              uint32_t size, bool write, uint32_t& value) {
        using A = Imx1BootstrapAccess;
        if (size != 4) return A::width;
        const uint32_t offset = address - 0x16010000;
        if (offset == 0x4f0) {
            // Native panel pulse configuration; electrical output remains unmodelled.
            if (write) {
                if (pc != 0x200869f0) return A::externalInput;
                if ((value & ~0x07ff0000u) != ((stagedWord(offset) & 0xf800ffffu) | 0x40000000u)) return A::sideEffect;
                staged[index(offset)] = active[index(offset)] = value;
            } else {
                if (pc != 0x200869c8 && pc != 0x200869f2 && pc != 0x20086d12) return A::externalInput;
                value = stagedWord(offset);
            }
            return A::ok;
        }
        if (offset == 0x524 && (pc == 0x20086a7c || pc == 0x20086a82 || pc == 0x20086a84)) {
            if (pending || editing || !knownWords[index(offset)]) return A::sequence;
            if (write) {
                if (pc != 0x20086a82) return A::externalInput;
                if (value != (stagedWord(offset) | 0x4001)) return A::sideEffect;
                staged[index(offset)] = active[index(offset)] = value;
            } else {
                if (pc == 0x20086a82) return A::externalInput;
                value = stagedWord(offset);
            }
            return A::ok;
        }
        if (offset == 0x54c) {
            if (write || (pc != 0x200ad7b0 && pc != 0x200adb08 && pc != 0x200adb14)) return A::externalInput;
            const uint32_t total = activeWord(0x420) & 0x7ff;
            const uint32_t vertical = activeWord(0x428);
            const uint64_t row = enabled && total ? phase / (uint64_t(total) * pixelCycles) : 0;
            value = enabled && row >= (vertical & 0x3ff) && row < (vertical >> 16) ? 0x400 : 0;
            return A::ok;
        }
        if (offset == 0x4d8) {
            if (!write) {
                if (pc != 0x200ad74c && pc != 0x200ae9d8) return A::externalInput;
                value = updateControl | (pending ? 2u : 0u);
                return A::ok;
            }
            if (pc != 0x200aeb68) return A::externalInput;
            return transaction(windows, value);
        }
        if (!write) {
            if (offset == 0x414 && (pc == 0x200ad80c || pc == 0x200ad838 || pc == 0x200adaa4 || pc == 0x200adad0)) {
                value = stagedWord(offset) | clockAcknowledgements();
                return A::ok;
            }
            if (offset == 0x434 && pc == 0x200c407c) {
                value = stagedWord(offset);
                return A::ok;
            }
            return A::externalInput;
        }
        bool known = false;
        for (const auto& op : operations)
            if (op.pc == pc && op.offset == offset) { known = true; break; }
        if (!known) return A::externalInput;
        const bool direct = pc == 0x200ad804 || pc == 0x200ad830 || pc == 0x200ada9c || pc == 0x200adac8 ||
                            pc == 0x200ad918 || pc == 0x200ad92c || pc == 0x200ad944 ||
                            pc == 0x200ada54 || pc == 0x200ada68 || pc == 0x200ada80;
        if (!editing && !direct) return A::sequence;
        if (pending && direct && offset != 0x414) return A::sequence;
        const uint32_t previous = stagedWord(offset);
        if (direct && offset == 0x414 && pending && ((value ^ clockControl) & 0x1800000)) return A::sequence;
        if ((pc == 0x200ad804 && value != (previous | 0x800000)) ||
            (pc == 0x200ad830 && value != (previous | 0x1000000)) ||
            (pc == 0x200ada9c && value != (previous & ~0x800000u)) ||
            (pc == 0x200adac8 && value != (previous & ~0x1000000u)) ||
            (pc == 0x200ad8b4 && value != (previous | 0x1800)) ||
            (pc == 0x200ada1c && value != (previous & ~0x1800u)) ||
            (pc == 0x200ad918 && value != (previous | 0x115001)) ||
            (pc == 0x200ad92c && value != (previous | 1)) ||
            (pc == 0x200ad944 && value != (previous | 0x3ffff)) ||
            (pc == 0x200ada54 && value != (previous & 0xffeeaffe)) ||
            (pc == 0x200ada68 && value != (previous & ~1u)) ||
            (pc == 0x200ada80 && value != (previous & 0xfffc0000))) return A::sideEffect;
        if (offset == 0x414) {
            if (value & ~0x1ffffffu) return A::sideEffect;
            if ((value & 7) != 0 && (value & 7) != 6) return A::sideEffect;
            if ((value & 0x18) != 0 && (value & 0x18) != 0x10) return A::sideEffect;
        }
        if (offset == 0x418 && ((value & 1) || value > 0xffffff)) return A::sideEffect;
        if (offset == 0x41c && (!value || value > 0x10000 || (value & 1))) return A::sideEffect;
        if ((offset == 0x420 || offset == 0x424) && (value & 0xf800f800)) return A::sideEffect;
        if (offset == 0x428 && (value & 0xfc00fc00)) return A::sideEffect;
        // Only the native linear RGB565 path is qualified for software scanout.
        if (offset == 0x434 && value != 0) return A::sideEffect;
        if (offset == 0x410 && value != 3) return A::sideEffect;
        if (offset == 0x538 && value != 0x800000) return A::sideEffect;
        if ((offset == 0x540 || offset == 0x578 || offset == 0x594) && value != 0x80604020) return A::sideEffect;
        if ((offset == 0x544 || offset == 0x58c || offset == 0x598) && value != 0xffdfbf9f) return A::sideEffect;
        if ((offset == 0x548 || offset == 0x590 || offset == 0x59c) && value != 0) return A::sideEffect;
        if (offset == 0x524 && value != 0x102aa) return A::sideEffect;
        staged[index(offset)] = value;
        knownWords[index(offset)] = true;
        if (direct && offset == 0x414 && ((clockControl ^ value) & 0x1800000)) {
            clockControl = value & 0x1800000;
            clockDelay = latchCycles;
            // A stopped request immediately loses its acknowledgement.
            if (!(value & 0x800000)) pclk = false;
            if (!(value & 0x1000000)) sclk = false;
            if (!pclk || !sclk) enabled = false;
        }
        if (direct && offset != 0x414) {
            active[index(offset)] = value;
        }
        return A::ok;
    }
    // The video overlay as the latched words set it, in panel pixels (the active area's top left is 0, 0).
    // Planes are Y, U and V; U and V are 4:2:0, a sample for each 2 by 2 pixels. Mirrored planes start at
    // their last column or row and go backwards.
    struct Overlay {
        uint32_t x = 0, y = 0, width = 0, height = 0, key = 0;
        bool keyed = false, mirrorX = false, mirrorY = false;
        struct Plane { uint32_t base = 0, pitch = 0, rangeBase = 0, rangeBytes = 0; } planes[3];
    };
    bool knownOverlay() const {
        for (auto offset : {0x424u,0x428u,0x434u,0x43cu,0x440u,0x444u,0x448u,0x44cu,0x450u,0x454u,0x458u,0x4d4u})
            if (!knownWords[index(offset)]) return false;
        return !(stagedWord(0x434) & 2) || knownWords[index(0x438)];
    }
    // The overlay shows with both enables (434 bits 2 and 4) set; a player hiding it may clear one at a time.
    static bool shown(uint32_t control) { return (control & 0x14) == 0x14; }
    bool overlay(const Imx1GraphicsWindows& windows, Overlay& value) const {
        return enabled && shown(activeWord(0x434)) && decodeOverlay(windows, active, value);
    }
    // Player code in RAM (such as TCPMP, through its own mapping of the chip) reaches the overlay
    // and the words it needs around it: the transaction word 4d8, VIDEO_CTRL 434, the colour key 438, the
    // offsets, pitches and window 43c-458, the overlay format bits of DISP_DEBUG (4d4, bits 4-8) and
    // DISP_DEBUG2 bit 30, and reads of the active area (424, 428). Words never written read as a stop.
    static bool playerWord(uint32_t address) {
        if (address < 0x16010424 || address > 0x16010538 || (address & 3)) return false;
        const uint32_t offset = address - 0x16010000;
        return offset <= 0x458 || offset == 0x4d4 || offset == 0x4d8 || offset == 0x538;
    }
    Imx1BootstrapAccess playerAccess(const Imx1GraphicsWindows& windows, uint32_t address, bool write, uint32_t& value) {
        using A = Imx1BootstrapAccess;
        const uint32_t offset = address - 0x16010000;
        if (offset == 0x4d8) {
            if (!write) { value = updateControl | (pending ? 2u : 0u); return A::ok; }
            return transaction(windows, value);
        }
        if (!write) {
            if (offset != 0x424 && offset != 0x428 && offset < 0x434) return A::externalInput;
            if (!knownWords[index(offset)]) return A::externalInput;
            value = stagedWord(offset);
            return A::ok;
        }
        if (offset < 0x434) return A::externalInput;
        if (!editing) return A::sequence;
        const uint32_t previous = stagedWord(offset);
        // Bits 0-4 (4:2:0, keyer, the two enables), 9-18 (the memory request count, not modelled), 22-23 (YUV to
        // RGB and its option) and 24-25 (mirror). The expansions (5-8) and the rest aren't modelled.
        if (offset == 0x434 && (value & ~0x03c7fe1fu)) return A::sideEffect;
        if (offset == 0x438 && value > 0xffff) return A::sideEffect;
        if ((offset == 0x43c || offset == 0x444 || offset == 0x44c) && value > 0xffffff) return A::sideEffect;
        if ((offset == 0x440 || offset == 0x448 || offset == 0x450) && (!value || value > 4096)) return A::sideEffect;
        if ((offset == 0x454 || offset == 0x458) && (value & 0xf800f800)) return A::sideEffect;
        if ((offset == 0x4d4 || offset == 0x538) && !knownWords[index(offset)]) return A::sequence;
        if (offset == 0x4d4 && ((value ^ previous) & ~0x1f0u)) return A::sideEffect;
        if (offset == 0x538 && ((value ^ previous) & ~0x40000000u)) return A::sideEffect;
        staged[index(offset)] = value;
        knownWords[index(offset)] = true;
        return A::ok;
    }
    // A display transaction: 78 opens one, 7b submits the staged words to latch at the next frame (or after
    // latchCycles with the display off).
    Imx1BootstrapAccess transaction(const Imx1GraphicsWindows& windows, uint32_t value) {
        using A = Imx1BootstrapAccess;
        if (value != 0x78 && value != 0x7b) return A::sideEffect;
        if (value == 0x78) {
            // Opening a new transaction discards an unlatched request.
            editing = true;
            pending = false;
            latchDelay = 0;
            updateControl = 0x78;
            return A::ok;
        }
        if (!editing) return A::sequence;
        Overlay overlayCandidate;
        if (shown(stagedWord(0x434)) && (!knownOverlay() || !decodeOverlay(windows, staged, overlayCandidate)))
            return A::sideEffect;
        Surface candidate;
        const bool requested = (stagedWord(0x414) & 0x1c00) == 0x1c00;
        uint64_t period = 0;
        const bool timingKnown = knownWords[index(0x420)] && knownWords[index(0x424)] && knownWords[index(0x428)];
        uint64_t clockPeriod = 0;
        if ((stagedWord(0x414) & 0x1800000) && (!timingKnown || !timing(staged, clockPeriod))) return A::sideEffect;
        if (requested && !decode(windows, candidate, period)) return A::sideEffect;
        submitted = staged;
        submittedSurface = candidate;
        submittedEnabled = requested;
        submittedTimingKnown = timingKnown;
        submittedPeriod = period;
        pending = true;
        latchAtFrame = enabled && framePeriod;
        latchDelay = latchCycles;
        editing = false;
        updateControl = 0x79;
        return A::ok;
    }
   private:
    static constexpr unsigned wordCount = (0x59c - 0x410) / 4 + 1;
    using Words = std::array<uint32_t, wordCount>;
    static constexpr unsigned index(uint32_t offset) { return (offset - 0x410) / 4; }
    struct Operation { uint32_t pc, offset; };
    inline static constexpr Operation operations[] = {
        {0x200ad720,0x418},{0x200ad72c,0x41c},{0x200ad738,0x414},
        {0x200ae9a8,0x414},{0x200ae9b8,0x418},{0x200ae9c4,0x41c},
        {0x200ad65c,0x42c},{0x200ad668,0x430},
        {0x200ad2e0,0x410},{0x200ad2ec,0x414},{0x200ad2f8,0x420},{0x200ad304,0x424},{0x200ad310,0x428},
        {0x200ad31c,0x48c},{0x200ad328,0x490},{0x200ad334,0x4a0},{0x200ad340,0x4a4},
        {0x200ad34c,0x4a8},{0x200ad358,0x4ac},{0x200ad364,0x494},{0x200ad370,0x49c},
        {0x200ad37c,0x4e0},{0x200ad388,0x4b0},{0x200ad394,0x4b4},{0x200ad3a0,0x4c8},
        {0x200ad3ac,0x4e4},{0x200ad3b8,0x524},{0x200ad3c4,0x4c4},{0x200ad3d0,0x4cc},
        {0x200ad3dc,0x4d0},{0x200ad3e8,0x4d4},{0x200ad404,0x540},{0x200ad410,0x544},
        {0x200ad418,0x548},{0x200ad420,0x578},{0x200ad428,0x58c},{0x200ad430,0x590},
        {0x200ad438,0x594},{0x200ad440,0x598},{0x200ad448,0x59c},{0x200ad528,0x538},
        {0x200c40c0,0x434},
        {0x200ad804,0x414},{0x200ad830,0x414},{0x200ad870,0x418},{0x200ad87c,0x42c},
        {0x200ad888,0x430},{0x200ad894,0x424},{0x200ad8a0,0x428},{0x200ad8b4,0x414},
        {0x200ad918,0x4cc},{0x200ad92c,0x4d0},{0x200ad944,0x4c8},
        {0x200ad9d0,0x4e4},{0x200ada1c,0x414},{0x200ada54,0x4cc},{0x200ada68,0x4d0},
        {0x200ada80,0x4c8},{0x200ada9c,0x414},{0x200adac8,0x414}
    };
    // ASSUMPTION "video overlay": an overlay shows with both enables (bits 2 and 4) set, as TCPMP's Show and the
    // DAL driver set them, and covers the graphic plane inside its window, or with the keyer (bit 1) only where
    // the graphic pixel equals the key (438). Only YUV 4:2:0 with the conversion on (bits 0, 22 and 23, as both
    // writers set them) is modelled; the RGB formats of 4d4 are refused.
    static bool decodeOverlay(const Imx1GraphicsWindows& windows, const Words& words, Overlay& value) {
        const auto word = [&](uint32_t offset) { return words[index(offset)]; };
        const uint32_t control = word(0x434);
        if ((control & 0x14) != 0x14 || (control & 0xc00001) != 0xc00001 || (word(0x4d4) & 0x1f0)) return false;
        const uint32_t h = word(0x454), v = word(0x458), activeH = word(0x424), activeV = word(0x428);
        const uint32_t x0 = h & 0x7ff, x1 = h >> 16, y0 = v & 0x7ff, y1 = v >> 16;
        if (x0 >= x1 || y0 >= y1 || x0 < (activeH & 0x7ff) || x1 > (activeH >> 16) ||
            y0 < (activeV & 0x3ff) || y1 > (activeV >> 16)) return false;
        value.x = x0 - (activeH & 0x7ff);
        value.y = y0 - (activeV & 0x3ff);
        value.width = x1 - x0;
        value.height = y1 - y0;
        value.keyed = control & 2;
        value.key = word(0x438);
        value.mirrorX = control & 0x1000000;
        value.mirrorY = control & 0x2000000;
        for (unsigned plane = 0; plane < 3; ++plane) {
            auto& p = value.planes[plane];
            p.base = word(0x43c + plane * 8);
            p.pitch = word(0x440 + plane * 8);
            const uint32_t columns = plane ? (value.width + 1) / 2 : value.width;
            const uint32_t rows = plane ? (value.height + 1) / 2 : value.height;
            if (!p.pitch || p.pitch < columns) return false;
            const int64_t lastRow = int64_t(rows - 1) * p.pitch * (value.mirrorY ? -1 : 1);
            const int64_t lastColumn = int64_t(columns - 1) * (value.mirrorX ? -1 : 1);
            const int64_t first = int64_t(p.base) + std::min<int64_t>(0, lastRow) + std::min<int64_t>(0, lastColumn);
            const int64_t end = int64_t(p.base) + std::max<int64_t>(0, lastRow) + std::max<int64_t>(0, lastColumn) + 1;
            if (first < 0) return false;
            p.rangeBase = uint32_t(first);
            p.rangeBytes = uint32_t(end - first);
            bool inside = false;
            for (auto range : {windows.internal(), windows.external()})
                inside |= range.known && p.rangeBase >= range.first && uint64_t(end) <= uint64_t(range.last) + 1;
            if (!inside) return false;
        }
        return true;
    }
    static bool timing(const Words& words, uint64_t& period) {
        const uint32_t total = words[index(0x420)], h = words[index(0x424)], v = words[index(0x428)];
        const uint32_t ht = total & 0x7ff, vt = (total >> 16) & 0x7ff;
        if ((total & 0xf800f800) || (h & 0xf800f800) || (v & 0xfc00fc00) ||
            !(ht && vt) || (h & 0x7ff) >= (h >> 16) || (v & 0x3ff) >= (v >> 16) ||
            (h >> 16) > ht || (v >> 16) > vt) return false;
        period = uint64_t(ht) * vt * pixelCycles;
        return true;
    }
    void updateClocks() {
        uint64_t period = 0;
        const bool configured = activeTimingKnown && timing(active, period);
        pclk = configured && pixelSource && (clockControl & 0x800000);
        sclk = configured && systemSource && (clockControl & 0x1000000);
        enabled = activeRequested && pclk && sclk;
    }
    bool decode(const Imx1GraphicsWindows& windows, Surface& value, uint64_t& period) const {
        for (auto offset : {0x420u,0x424u,0x428u}) if (!knownWords[index(offset)]) return false;
        if (!timing(staged, period)) return false;
        for (auto offset : {0x414u,0x418u,0x41cu,0x42cu,0x430u}) if (!knownWords[index(offset)]) return false;
        for (auto offset : {0x410u,0x538u,0x540u,0x544u,0x548u,0x578u,0x58cu,0x590u,0x594u,0x598u,0x59cu})
            if (!knownWords[index(offset)]) return false;
        const uint32_t control = stagedWord(0x414), h = stagedWord(0x42c), v = stagedWord(0x430);
        const uint32_t activeH = stagedWord(0x424), activeV = stagedWord(0x428);
        if ((control & 7) != 6 || ((control & 0x18) != 0 && (control & 0x18) != 0x10) ||
            (control & 0x1800000) != 0x1800000 ||
            h != activeH || v != activeV) return false;
        value.width = (h >> 16) - (h & 0x7ff);
        value.height = (v >> 16) - (v & 0x3ff);
        value.base = stagedWord(0x418);
        value.pitch = stagedWord(0x41c);
        if (!value.width || !value.height || value.width > 1024 || value.height > 1024 ||
            uint64_t(value.width) * value.height > 1024 * 1024) return false;
        const bool rotated = (control & 0x18) == 0x10;
        if (value.pitch < (rotated ? value.height : value.width) * 2) return false;
        if (rotated) {
            // Mode 2 starts at the upper halfword of its word-aligned offset.
            if ((value.base & 3) || value.base > 0xfffffffdu) return false;
            value.base += 2;
            value.xStride = int32_t(value.pitch);
            value.yStride = -2;
        } else value.yStride = int32_t(value.pitch);
        const int64_t lastRow = int64_t(value.height - 1) * value.yStride;
        const int64_t lastColumn = int64_t(value.width - 1) * value.xStride;
        const int64_t first = int64_t(value.base) + std::min<int64_t>(0,lastRow);
        const int64_t end = int64_t(value.base) + std::max<int64_t>(0,lastRow) + lastColumn + 2;
        if (first < 0 || end > 0x100000000ll) return false;
        value.rangeBase = uint32_t(first);
        value.rangeBytes = uint32_t(end - first);
        unsigned bank = 0;
        for (auto range : {windows.internal(),windows.external()}) {
            if (range.known && value.rangeBase >= range.first && uint64_t(value.rangeBase) + value.rangeBytes <= uint64_t(range.last) + 1) {
                value.bank = bank;
                value.bankOffset = value.rangeBase - range.first;
                return true;
            }
            ++bank;
        }
        return false;
    }
    Words staged{}, active{}, submitted{};
    std::array<bool, wordCount> knownWords{};
    Surface activeSurface{}, submittedSurface{};
    uint64_t latchDelay = 0, phase = 0, framePeriod = 0, submittedPeriod = 0;
    uint64_t activeRevision = 0, frames = 0, blankEvents = 0;
    uint32_t updateControl = 0, clockDelay = 0, clockControl = 0;
    bool editing = false, pending = false, submittedEnabled = false, enabled = false;
    bool activeTimingKnown = false, submittedTimingKnown = false;
    Imx1GraphicsFraction pixelFraction;
    uint64_t rateNumerator = 1, rateDenominator = 1;
    bool pixelSource = true, systemSource = true, activeRequested = false, latchAtFrame = false;
    bool pclk = false, sclk = false;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1GraphicsDisplay) == 1504, "update state() and stateVersion");
#endif
#endif
