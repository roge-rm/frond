#ifndef FROND_IMX1_TIMER_H
#define FROND_IMX1_TIMER_H

#include <cstdint>

enum class Imx1TimerAccess { ok, unmapped, width, reservedBits, sideEffect, externalInput };

// MC9328MX1RM chapter 26. The caller supplies elapsed PERCLK1 edges.
class Imx1Timer {
   public:
    Imx1TimerAccess access(uint32_t offset, uint32_t size, bool write, uint32_t& value,
                           bool outputIsolated = false);
    void advance(uint64_t perclk1Cycles);
    // PERCLK1 cycles until the next compare match, 0 when not counting.
    uint64_t idleQuietCycles() const;
    bool canInterrupt() const { return (control & 0x11) == 0x11 && (control & 0xe); }
    bool enabled() const { return control & 1; }
    bool outputIsolationRequired() const { return enabled() && (control & 0x20); }
    bool interruptOutput() const { return (control & 0x10) && (status & 1); }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.Do32(control).Do32(prescaler).Do32(compare).Do32(counter).Do32(status).Do32(statusRead).Do32(phase);
    }
    static const char* registerName(uint32_t address);

   private:
    uint32_t control = 0, prescaler = 0, compare = 0xffffffff;
    uint32_t counter = 0, status = 0, statusRead = 0;
    uint32_t phase = 0;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Timer) == 28, "update state() and stateVersion");
#endif

#endif
