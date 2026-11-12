#ifndef FROND_IMX1_RESET_H
#define FROND_IMX1_RESET_H

#include <cstdint>

// MC9328MX1RM chapter 6, the reset module. RSR at 0021b800 is its only register and says what the
// last hardware reset was (tables 6-2 and 6-3): 0 after power-on (POR), EXR (bit 0) after a qualified
// RESET_IN, WDR (bit 1) after a watchdog time-out. Bits 31 to 2 read 0. POR resets the module's own
// flip-flops, RESET_IN does not reset it (table 6-1), so the value lasts until the next reset.
enum class Imx1ResetSource : uint32_t { powerOn = 0, external = 1, watchdog = 2 };
enum class Imx1ResetAccess { ok, width, readOnly };

class Imx1Reset {
   public:
    static const char* registerName(uint32_t address) { return (address & ~3u) == 0x0021b800 ? "RST.RSR" : nullptr; }
    void set(Imx1ResetSource last) { source = uint32_t(last); }
    Imx1ResetSource last() const { return Imx1ResetSource(source); }
    // The manual draws all 32 bits as read-only and gives no result for a write or for a narrower
    // read, so only the 32-bit read is answered.
    Imx1ResetAccess access(uint32_t address, uint32_t size, bool write, uint32_t& value) const {
        if (size != 4 || (address & 3)) return Imx1ResetAccess::width;
        if (write) return Imx1ResetAccess::readOnly;
        value = source;
        return Imx1ResetAccess::ok;
    }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.Do32(source);
        a.Require(source <= 2, "reset source");
    }

   private:
    uint32_t source = 0;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Reset) == 4, "update state() and stateVersion");
#endif

#endif
