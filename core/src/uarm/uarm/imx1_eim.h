#ifndef FROND_IMX1_EIM_H
#define FROND_IMX1_EIM_H

#include <cstdint>

enum class Imx1Cs5Mode { unsupported, fixed, dtack, wait };

enum class Imx1EimAccess { ok, unmapped, width, reservedBits, sideEffect, externalInput };

// Asynchronous bootstrap configuration; attached devices and bus timing are separate.
class Imx1Eim {
   public:
    Imx1EimAccess access(uint32_t address, uint32_t size, bool write, uint32_t& value);
    static const char* registerName(uint32_t address);
    bool configurationMatches(unsigned chip, uint32_t hi, uint32_t lo) const {
        return chip < 6 && upper[chip] == hi && lower[chip] == lo;
    }

    Imx1Cs5Mode cs5Mode() const {
        if (lower[5] != 0x11111e01 && lower[5] != 0x11501e01) return Imx1Cs5Mode::unsupported;
        if (upper[5] == 0x3e00 && lower[5] == 0x11111e01) return Imx1Cs5Mode::fixed;
        if (upper[5] == 0x3f00) return Imx1Cs5Mode::dtack;
        if (upper[5] == 0x80003f00) return Imx1Cs5Mode::wait;
        return Imx1Cs5Mode::unsupported;
    }
    bool cs5DeviceConfigured() const { return cs5Mode() != Imx1Cs5Mode::unsupported; }
    // AN2422 section 3: external DTACK never splits a transfer across narrower ports.
    Imx1EimAccess cs5Transfer(uint32_t address, uint32_t size, bool write, bool user = false) const {
        if ((size != 1 && size != 2 && size != 4) || (address & (size - 1))) return Imx1EimAccess::width;
        if (!(lower[5] & 1) || (write && (lower[5] & 16)) || (user && (lower[5] & 64))) return Imx1EimAccess::sideEffect;
        const auto width = (lower[5] >> 8) & 7;
        const auto bytes = width < 4 ? 1u : width < 6 ? 2u : 4u;
        if ((upper[5] & 0x3f00) == 0x3f00 && size > bytes) return Imx1EimAccess::width;
        return Imx1EimAccess::ok;
    }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.DoBuffer32(upper, 6).DoBuffer32(lower, 6).DoBool(bootWidthKnown).Do32(configuration);
    }

   private:
    uint32_t upper[6] = {0x3e00, 0, 0, 0, 0, 0};
    uint32_t lower[6] = {0, 0x0802, 0x0802, 0x0802, 0x0802, 0x0802};
    bool bootWidthKnown = false;
    uint32_t configuration = 0;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Eim) == 56, "update state() and stateVersion");
#endif

#endif
