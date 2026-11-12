#ifndef FROND_IMX1_CS5_RESET_H
#define FROND_IMX1_CS5_RESET_H

#include "imx1_bootstrap.h"

// Diagnostic echo for the ROM reset helper, with an explicit opaque status sample.
class Imx1Cs5Reset {
   public:
    explicit Imx1Cs5Reset(uint8_t statusInput) : input(statusInput), reset(statusInput & 8) {}
    uint8_t statusValue() const { return (input & 0xf7) | (reset ? 8 : 0); }
    unsigned phaseValue() const { return phase; }
    Imx1BootstrapAccess access(const Imx1Eim& eim, uint32_t pc, uint32_t address,
                              uint32_t size, bool write, uint32_t& value, bool virtualTransport = false) {
        if (address != 0x16000018) return Imx1BootstrapAccess::unmapped;
        if (size != 1) return Imx1BootstrapAccess::width;
        if (!(virtualTransport ? eim.cs5DeviceConfigured() : eim.configurationMatches(5, 0x3e00, 0x11111e01))) return Imx1BootstrapAccess::sequence;
        // Other callers may consume status fields or rely on read side effects.
        const uint32_t pcs[] = {0x200bac84, 0x200bac9c, 0x200baca4, 0x200bace8, 0x200bacf0};
        if (pc != pcs[phase] || write != (phase == 1 || phase == 3))
            return Imx1BootstrapAccess::externalInput;
        if (write) {
            const uint32_t expected = (input & 0xf7) | (phase == 1 ? 8 : 0);
            if (value != expected) return Imx1BootstrapAccess::sideEffect;
            reset = phase == 1;
        } else {
            value = statusValue();
        }
        phase = (phase + 1) % 5;
        return Imx1BootstrapAccess::ok;
    }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.Fixed8(input).DoBool(reset).Do32(phase).Require(phase < 5, "cs5 reset phase");
    }

   private:
    uint8_t input;
    bool reset = false;
    unsigned phase = 0;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Cs5Reset) == 8, "update state() and stateVersion");
#endif

struct Imx1ProbeCs5ResetEvent {
    uint64_t step;
    uint32_t pc, address, size, value;
    bool write;
    unsigned phase;
    Imx1BootstrapAccess status;
};
using Imx1ProbeCs5ResetObserver = void (*)(void*, const Imx1ProbeCs5ResetEvent&);

#endif
