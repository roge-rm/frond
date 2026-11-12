#ifndef FROND_IMX1_CS4_CAPTURE_H
#define FROND_IMX1_CS4_CAPTURE_H

#include <array>
#include "imx1_bootstrap.h"

// Fixed caller-supplied samples, independent of recorded device writes.
class Imx1Cs4IndexedInputs {
   public:
    Imx1Cs4IndexedInputs() { values.fill(-1); }
    bool set(uint32_t bank, uint32_t index, uint32_t value) {
        if (bank > 1 || index > 255 || value > 255 || sample(bank, index) >= 0) return false;
        values[bank * 256 + index] = int16_t(value);
        ++supplied;
        return true;
    }
    int sample(int bank, int index) const {
        return bank < 0 || bank > 1 || index < 0 || index > 255 ? -1 : values[bank * 256 + index];
    }
    bool empty() const { return !supplied; }
    template <class A> void state(A& a) {
        for (auto& value : values) a.Fixed16(reinterpret_cast<uint16_t&>(value));
        a.Fixed32(supplied);
    }
   private:
    std::array<int16_t,512> values{};
    unsigned supplied = 0;
};

// Probe-only raw transport recorder. No device register storage or read side effects.
class Imx1Cs4Capture {
   public:
    explicit Imx1Cs4Capture(uint8_t port0Input, const Imx1Cs4IndexedInputs& samples = {})
        : input(port0Input), indexedInputs(samples) {}
    int selectedIndex() const { return index; }
    int observedBank() const { return bank; }
    Imx1BootstrapAccess access(const Imx1Eim& eim, uint32_t address, uint32_t size,
                              bool write, uint32_t& value) {
        if (address != 0x15000000 && address != 0x15000001) return Imx1BootstrapAccess::unmapped;
        if (size != 1) return Imx1BootstrapAccess::width;
        // Limit capture to the observed enabled CS4 byte-port configuration.
        if (!eim.configurationMatches(4, 0x3e70, 0xffff4301)) return Imx1BootstrapAccess::sequence;
        if (!write) {
            if (address == 0x15000001) {
                const int sample = indexedInputs.sample(bank, index);
                if (sample < 0) return Imx1BootstrapAccess::externalInput;
                value = uint32_t(sample);
            } else value = input;
        } else if (address == 0x15000000) {
            index = value & 0xff;
        } else {
            if (index < 0) return Imx1BootstrapAccess::sequence;
            if (index == 0x0f) {
                // ROM protocol uses common index 0f with values 0/1 to select a bank.
                if (value > 1) return Imx1BootstrapAccess::sideEffect;
                bank = value;
            } else if (bank < 0) return Imx1BootstrapAccess::sequence;
        }
        return Imx1BootstrapAccess::ok;
    }
    // The inputs come from the options.
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.Fixed8(input);
        indexedInputs.state(a);
        a.Do32(reinterpret_cast<uint32_t&>(index)).Do32(reinterpret_cast<uint32_t&>(bank));
        a.Require(index >= -1 && index <= 255 && bank >= -1 && bank <= 1, "cs4 index");
    }

   private:
    uint8_t input;
    Imx1Cs4IndexedInputs indexedInputs;
    int index = -1, bank = -1;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Cs4Capture) == 1040, "update state() and stateVersion");
#endif

struct Imx1ProbeCs4Event {
    uint64_t step;
    uint32_t pc, address, size, value;
    bool write;
    int index, bank;
    Imx1BootstrapAccess status;
};
using Imx1ProbeCs4Observer = void (*)(void*, const Imx1ProbeCs4Event&);

#endif
