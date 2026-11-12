#ifndef FROND_IMX1_SYSCTRL_H
#define FROND_IMX1_SYSCTRL_H

#include <cstdint>

enum class Imx1SysctrlAccess { ok, unmapped, width, reservedBits, sideEffect, externalInput };

// System Control configuration; boot pins and attached peripheral effects stay unqualified.
class Imx1Sysctrl {
   public:
    bool dmaClockEnabled() const { return gccr & 8; }
    bool btaClockEnabled() const { return gpcr & 0x1000; }
    Imx1SysctrlAccess access(uint32_t address, uint32_t size, bool write, uint32_t& value);
    static const char* registerName(uint32_t address);
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.Do32(gccr).Do32(fmcr).Do32(gpcr);
    }

   private:
    uint32_t gccr = 0x2f;
    uint32_t fmcr = 3;
    uint32_t gpcr = 0x3fb;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Sysctrl) == 12, "update state() and stateVersion");
#endif

#endif
