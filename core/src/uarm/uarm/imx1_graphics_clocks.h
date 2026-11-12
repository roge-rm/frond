#ifndef FROND_IMX1_GRAPHICS_CLOCKS_H
#define FROND_IMX1_GRAPHICS_CLOCKS_H

#include <array>
#include <algorithm>
#include <cassert>
#include <numeric>
#include <limits>
#include "imx1_bootstrap.h"

// Proper fractions retain elapsed time across charges and source changes.
struct Imx1GraphicsFraction {
    uint64_t phase = 0, denominator = 1;
    // A d that divides the denominator, as nearly every charge's does, skips the gcd: the common denominator
    // stays the same.
    bool supports(uint64_t d) const {
        if (d && denominator % d == 0) return denominator <= std::numeric_limits<uint64_t>::max() / 2;
        return d && d / imx1Gcd(d, denominator) <= (std::numeric_limits<uint64_t>::max() / 2) / denominator;
    }
    uint64_t add(uint64_t n, uint64_t d) {
        assert(supports(d));
        if (denominator % d == 0) {
            const auto fraction = phase + (n % d) * (denominator / d);
            phase = fraction % denominator;
            return n / d + fraction / denominator;
        }
        const auto common = denominator * (d / imx1Gcd(d, denominator));
        const auto fraction = phase * (common / denominator) + (n % d) * (common / d);
        phase = fraction % common;
        denominator = common;
        return n / d + fraction / common;
    }
    // add(units * scale, d) in one step where d divides the denominator: the phase keeps the exact remainder, so
    // this gives the ticks and phase that adding the same total in any pieces would. False, adding nothing, where
    // d doesn't divide it or the total doesn't fit.
    bool addWhole(uint64_t units, uint64_t scale, uint64_t d, uint64_t& ticks) {
        constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
        if (!d || denominator % d || denominator > max / 2 || (scale && units > max / scale)) return false;
        ticks += add(units * scale, d);
        return true;
    }
    template <class A> void state(A& a) {
        a.Do64(phase).Do64(denominator).Require(denominator && phase < denominator, "graphics fraction");
    }
};

// Nominal virtual analogue policy. Frequencies and deadlines use guest time only.
class Imx1GraphicsClocks {
   public:
    struct Rate { uint64_t numerator = 0, denominator = 1; };
    static constexpr uint64_t timeHz = 200000000, referenceHz = 16000000;
    static constexpr uint32_t testPrescaler = 4;
    static bool owns(uint32_t address) { return address >= 0x16010080 && address <= 0x160100b0 && !(address & 3); }
    void reset() { *this = {}; }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.DoBuffer32(words.data(), words.size());
        countFraction.state(a);
        a.Do64(lockRemaining).Do64(countRemaining).Do64(automaticRemaining).Do16(counter);
        a.DoBool(started).DoBool(gpuBusy).DoBool(normal);
    }
    bool programmed() const { return started; }
    uint32_t word(uint32_t offset) const { return words[(offset - 0x80) / 4]; }
    uint16_t count() const { return counter; }
    uint64_t nextEvent() const {
        uint64_t next=0;
        for (auto event : {lockRemaining,countRemaining,automaticRemaining})
            if (event && (!next || event<next)) next=event;
        return next;
    }
    bool automaticNormal() const { return automaticRouted() && normal; }
    bool gpuBusyValue() const { return gpuBusy; }
    void setGpuBusy(bool value) {
        if (gpuBusy==value) return;
        gpuBusy=value;
        scheduleAutomatic();
    }
    uint64_t measurementTicks() const {
        return (uint64_t((word(0x84) >> 24) + 1) * testPrescaler * timeHz + referenceHz - 1) / referenceHz;
    }
    Rate pllRate() const {
        const auto c = word(0x88), div = word(0x84);
        if (!started || !(word(0x80) & 1) || (c & 0x80000003)) return {};
        const uint32_t dac = (c >> 20) & 15;
        if (dac) {
            const uint32_t voltage = dac <= 5 ? 0 : dac - 5;
            return {vcoBase() + voltage * 3200000u, vcoDivisor()};
        }
        if ((c & 0x40) || lockRemaining) return {};
        const Rate target{referenceHz * (((div >> 8) & 63) * 8 + 8 + ((div >> 16) & 7)),
                          uint64_t((div & 15) + 1) * 8};
        if (target.numerator * vcoDivisor() < vcoBase() * target.denominator ||
            target.numerator * vcoDivisor() > (vcoBase() + 32000000u) * target.denominator) return {};
        return target;
    }
    Rate pixelConfiguredRate() const {
        const auto control=word(0x90), div=word(0x84);
        const uint64_t post=((control>>4)&15)+1;
        if (!(control&15)) return {referenceHz,post};
        const auto dac=(word(0x88)>>20)&15;
        if (dac) return {vcoBase()+(dac<=5?0:dac-5)*3200000u,vcoDivisor()*post};
        return {referenceHz * (((div>>8)&63)*8+8+((div>>16)&7)), ((div&15)+1)*8*post};
    }
    Rate systemRate() const {
        if ((word(0x98) & 12) == 12) return {};
        const auto control = word(0x8c);
        if (automaticRouted() && normal)
            return {(word(0x80)&1)?referenceHz:0,((control>>11)&15)+1};
        auto r = output(control);
        if (!automaticRouted() && (word(0x98) & 12) == 4) {
            r.denominator /= ((control >> 4) & 15) + 1;
            r.denominator *= ((control >> 11) & 15) + 1;
        }
        return r;
    }
    Rate pixelRate() const { return (word(0x98) & 12) == 12 ? Rate{} : output(word(0x90)); }
    void advance(uint64_t ticks) {
        assert(!nextEvent() || ticks <= nextEvent());
        if ((word(0x94) & 0x300) == 0x100 && countRemaining) {
            const auto rate = testRate(word(0x94) & 31);
            // Callers bound each charge, so the frequency product fits uint64_t.
            const auto d = timeHz * rate.denominator * testPrescaler;
            const auto common = imx1Gcd(rate.numerator,d);
            counter = uint16_t(counter + countFraction.add(ticks * (rate.numerator/common),d/common));
        }
        if (countRemaining) countRemaining -= ticks;
        if (lockRemaining) lockRemaining -= ticks;
        if (automaticRemaining) {
            automaticRemaining-=ticks;
            if (!automaticRemaining) normal=!normal;
        }
    }
    Imx1BootstrapAccess access(uint32_t pc, uint32_t address, uint32_t size, bool write, uint32_t& value) {
        using A = Imx1BootstrapAccess;
        if (size != 4) return A::width;
        const uint32_t offset = address - 0x16010000;
        bool known = false;
        for (const auto& op : operations)
            if (op.pc == pc && op.offset == offset && op.write == write) { known = true; break; }
        if (!known) return A::externalInput;
        if (!write) { value = word(offset) | (offset == 0x94 ? uint32_t(counter) << 16 : 0); return A::ok; }
        if (offset == 0x94) {
            if ((value & 0xfce0) || (value & 31) > 3) return A::sideEffect;
            const uint32_t next = value & 0x31f;
            if (!(next & 0x200) && !fractionalSourcesSupported()) return A::sideEffect;
            if (next & 0x200) { counter = 0; countFraction = {}; countRemaining = 0; }
            else if ((next & 0x100) && !(word(0x94) & 0x100)) countRemaining = measurementTicks();
            if (!(next & 0x100)) countRemaining = 0;
            words[(offset - 0x80) / 4] = next;
            return A::ok;
        }
        if (offset >= 0xa4) {
            uint32_t expected = 0;
            if (pc == 0x200c4e24 || pc == 0x200c639c) expected = (word(0xa4) & 0xfff00) | 0x1100020;
            else if (pc == 0x200c4f9c || pc == 0x200c6374) expected = (word(0xa8) & 0x30cc000) | 0x4b000200;
            else if (pc == 0x200c501c || pc == 0x200c6348) expected = (word(0xac) & 0xfffcfe00) | 2;
            else if (pc == 0x200c5060) expected = 0x7f;
            else return A::externalInput;
            if (value != expected) return A::sideEffect;
        }
        if ((pc == 0x200c14e4 && value != (word(0x80) | 0x80u)) ||
            (pc == 0x200c165c && value != (word(0x80) & 0x7fffffu)) ||
            (pc == 0x200c16ac && value != (word(0x88) & ~3u)) ||
            (pc == 0x200c1550 && ((value ^ word(0x98)) & ~0xf1u)) ||
            (pc == 0x200c11c4 && value != (word(0x98) | 0xc0u)) ||
            (pc == 0x200c15c0 && value != ((word(0x80) & 0x7fffffu) | 0xa000000u)) ||
            (pc == 0x200c15f0 && value != ((word(0x8c) & ~0xfu) | 1))) return A::sideEffect;
        if ((pc == 0x200c1148 && value != (word(0x9c) & ~0x80u)) ||
            (pc == 0x200c1174 && value != (word(0x88) & ~4u))) return A::sideEffect;
        // The second half of the ROM's chip power down (clock handler 50002, mode 2, 200c11d0). Each word is
        // written whole from the driver's RAM copy and none is read back or tested.
        if ((pc == 0x200c128c && value != (word(0x80) | 0x80u)) ||
            (pc == 0x200c12b8 && value != (word(0x88) | 4u)) ||
            (pc == 0x200c12fc && value != (word(0x88) & ~4u)) ||
            (pc == 0x200c133c && value != ((word(0x98) | 1u) & ~0xc0u)) ||
            (pc == 0x200c1368 && value != (word(0x9c) | 0x80u))) return A::sideEffect;
        if ((pc == 0x200c54c0 && ((value ^ word(0x90)) & ~0xfu)) ||
            (pc == 0x200c54e0 && ((value ^ word(0x90)) & ~0xf0u))) return A::sideEffect;
        if (offset == 0x98) {
            // Bits 5:4 are stored and change nothing here. The power down sets bit 4 (200c1550 with byte 11,
            // kept by 200c133c) and the way back writes the word whole again (200c4c00).
            const uint32_t stored = pc == 0x200c1550 || pc == 0x200c133c ? 0x30u : 0;
            if ((value & 0x32 & ~stored) || (value & 12) == 8) return A::sideEffect;
            // Only the native condition pair is qualified for automatic activity.
            if ((pc == 0x200c4c00 || (value & 0xc0)) && (value & 0xff00) != 0x4000) return A::sideEffect;
            if (pc != 0x200c4c00 && ((value ^ word(0x98)) & 0xff00)) return A::sideEffect;
        }
        // Pump current and auxiliary PLL controls retain the native nominal settings.
        // Bit 2 of 88 is stored and changes nothing here: the power down sets it at 200c12b8 and clears it
        // again in its next write (200c12fc).
        if (offset == 0x88 && (value & ~(0x80f0fc43u | (pc == 0x200c12b8 ? 4u : 0u))) != 0x4b000200u) return A::sideEffect;
        if (offset == 0x9c && pc == 0x200c4cac && value != 0x6e) return A::sideEffect;
        if (offset == 0xa0 && value != 0xffff0000u) return A::sideEffect;
        if ((offset == 0x8c || offset == 0x90) && (value & 15) > 1) return A::sideEffect;
        if ((offset == 0x80 && (value & ~0x0a0000bfu)) ||
            (offset == 0x84 && (value & 0x0008c0f0u)) ||
            (offset == 0x8c && (value & ~0x7fffu)) ||
            (offset == 0x90 && (value & ~0xffu))) return A::sideEffect;
        auto candidate=*this;
        candidate.words[(offset - 0x80) / 4]=value;
        if (!candidate.fractionalSourcesSupported()) return A::sideEffect;
        const uint32_t previous = word(offset);
        words[(offset - 0x80) / 4] = value;
        started = true;
        if ((offset == 0x80 && ((previous ^ value) & 0xa000000u)) ||
            (offset == 0x98 && ((previous ^ value) & 0xcdu))) {
            normal=(word(0x98)&12)==4;
            scheduleAutomatic();
        } else if (offset == 0x98 && previous != value) scheduleAutomatic();
        if (offset == 0x84 || (offset == 0x88 && ((previous ^ value) & 0x80f0fc43))) {
            const auto c = word(0x88);
            lockRemaining = (c & 0x80000043) || (c & 0xf00000) ? 0 :
                ((uint64_t((word(0x84) >> 24) + 1) * 16 + ((word(0x84) >> 20) & 15) + 1) * timeHz + referenceHz - 1) / referenceHz;
        }
        return A::ok;
    }
   private:
    struct Operation { uint32_t pc, offset; bool write; };
    inline static constexpr Operation operations[] = {
        {0x200c45e4,0x80,true},{0x200c47b0,0x8c,true},{0x200c4920,0x90,true},
        {0x200c49ac,0x84,true},{0x200c4b38,0x88,true},{0x200c4c00,0x98,true},
        {0x200c4cac,0x9c,true},{0x200c4cdc,0xa0,true},
        {0x200c4da4,0xa4,false},{0x200c4e24,0xa4,true},{0x200c4e2c,0xa8,false},
        {0x200c4f9c,0xa8,true},{0x200c501c,0xac,true},{0x200c5060,0xb0,true},
        {0x200c56c0,0x8c,true},{0x200c56f8,0x80,true},{0x200c573c,0x98,true},{0x200c57a8,0x88,true},
        {0x200c632c,0xac,false},{0x200c6348,0xac,true},
        {0x200c6350,0xa8,false},{0x200c6374,0xa8,true},
        {0x200c637c,0xa4,false},{0x200c639c,0xa4,true},
        {0x200c11c4,0x98,true},{0x200c15c0,0x80,true},{0x200c15f0,0x8c,true},
        {0x200c14e4,0x80,true},{0x200c1550,0x98,true},{0x200c165c,0x80,true},{0x200c16ac,0x88,true},
        {0x200c1148,0x9c,true},{0x200c1174,0x88,true},
        {0x200c128c,0x80,true},{0x200c12b8,0x88,true},{0x200c12fc,0x88,true},{0x200c133c,0x98,true},{0x200c1368,0x9c,true},
        {0x200c54a8,0x90,false},{0x200c54c0,0x90,true},
        {0x200c54c8,0x90,false},{0x200c54e0,0x90,true},
        {0x200c55c0,0x8c,true},{0x200c5f50,0x8c,true},
        {0x200c5d58,0x98,true},{0x200c5e0c,0x84,true},{0x200c5e38,0x98,true},{0x200c5e9c,0x98,true},
        {0x200c5f18,0x88,true},{0x200c6068,0x88,true},
        {0x200c5be4,0x88,true},{0x200c5c48,0x88,true},{0x200c5c7c,0x88,true},{0x200c5ca8,0x88,true},
        {0x200c5930,0x88,true},{0x200c5978,0x88,true},{0x200c59d8,0x88,true},{0x200c5af8,0x88,true},
        {0x200c5824,0x94,true},{0x200c5850,0x94,true},{0x200c587c,0x94,true},
        {0x200c588c,0x94,false},{0x200c58c4,0x94,true}
    };
    bool automaticRouted() const {
        return (word(0x98)&1) && (word(0x98)&0xc0) && (word(0x98)&12)!=12 && (word(0x80)&0xa000000u)==0xa000000u;
    }
    void scheduleAutomatic() {
        automaticRemaining=0;
        if (!automaticRouted()) return;
        if ((!gpuBusy && !normal && (word(0x98)&0x40)) ||
            (gpuBusy && normal && (word(0x98)&0x80))) {
            const auto timer=(word(0x98)>>(gpuBusy?24:16))&255;
            automaticRemaining=((uint64_t(timer)+1)*16*timeHz+referenceHz-1)/referenceHz;
        }
    }
    bool fractionalSourcesSupported() const {
        const auto div=word(0x84),dac=(word(0x88)>>20)&15;
        for (uint64_t post : {uint64_t(1),uint64_t(((word(0x8c)>>4)&15)+1),
                              uint64_t(((word(0x8c)>>11)&15)+1),uint64_t(((word(0x90)>>4)&15)+1)}) {
            const uint64_t n=referenceHz * (((div>>8)&63)*8+8+((div>>16)&7));
            const uint64_t d=timeHz*((div&15)+1)*8*post*testPrescaler;
            const uint64_t referenceD=timeHz*post*testPrescaler;
            const uint64_t forcedN=vcoBase()+(dac<=5?0:dac-5)*3200000u;
            const uint64_t forcedD=timeHz*vcoDivisor()*post*testPrescaler;
            if (!countFraction.supports(d/imx1Gcd(n,d)) ||
                !countFraction.supports(referenceD/imx1Gcd(referenceHz,referenceD)) ||
                !countFraction.supports(forcedD/imx1Gcd(forcedN,forcedD))) return false;
        }
        return true;
    }
    uint64_t vcoBase() const {
        const auto c = word(0x88);
        return 8000000u + ((c >> 10) & 7) * 16000000u + ((c >> 14) & 3) * 4000000u;
    }
    uint64_t vcoDivisor() const { return (word(0x88) & 0x2000) ? 4 : 1; }
    Rate output(uint32_t control) const {
        Rate r = (control & 15) == 1 ? pllRate() : Rate{(word(0x80) & 1) ? referenceHz : 0,1};
        r.denominator *= ((control >> 4) & 15) + 1;
        return r;
    }
    Rate testRate(uint32_t selector) const {
        if (selector == 1) return pllRate();
        if (selector == 2) return systemRate();
        if (selector == 3) return pixelRate();
        return {};
    }
    std::array<uint32_t,13> words{};
    Imx1GraphicsFraction countFraction;
    uint64_t lockRemaining = 0, countRemaining = 0, automaticRemaining = 0;
    uint16_t counter = 0;
    bool started = false, gpuBusy = false, normal = false;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1GraphicsClocks) == 104, "update state() and stateVersion");
#endif
#endif
