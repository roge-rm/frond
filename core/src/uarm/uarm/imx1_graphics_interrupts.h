#ifndef FROND_IMX1_GRAPHICS_INTERRUPTS_H
#define FROND_IMX1_GRAPHICS_INTERRUPTS_H

#include "imx1_bootstrap.h"

// Native groups are 0x50, 0x02000000 and 1. The virtual profile assigns bit 0 to scan blank entry.
class Imx1GraphicsInterrupts {
   public:
    static constexpr uint32_t nativeMask = 0x02000051, displayMask = 1;
    static bool owns(uint32_t address) { return address == 0x16010200 || address == 0x16010204; }
    void reset() { *this = {}; }
    uint32_t enableValue() const { return enabled; }
    uint32_t pendingValue() const { return pending; }
    uint32_t knownMask() const { return known; }
    Imx1GpioLine interruptOutput() const {
        if (enabled & pending & known) return Imx1GpioLine::asserted;
        return enabled & ~known ? Imx1GpioLine::unknown : Imx1GpioLine::clear;
    }
    // The SD block's card insert or remove event (Imx1GraphicsSd::attachCard).
    static constexpr uint32_t sdMask = 0x02000000;
    void raiseSd() { pending |= sdMask; known |= sdMask; }
    void observeDisplay(bool sourceKnown, bool event) {
        if (sourceKnown && event) { pending |= displayMask; known |= displayMask; }
        else if (!sourceKnown && !(pending & known & displayMask)) known &= ~displayMask;
    }
    // The USB function's group (0x50; the driver uses bit 6) as the USB driver drives it: it sets bit 6 and
    // clears 0x10, 0x20 and 0x80 (20177eae-20177ec2), clears all four and acknowledges what is pending
    // among them (20177f20-20177f4a).
    static constexpr uint32_t usbMask = 0x40;
    void raiseUsb() { pending |= usbMask; known |= usbMask; }
    Imx1BootstrapAccess usbAccess(uint32_t address, bool write, uint32_t& value) {
        using A = Imx1BootstrapAccess;
        if (!write) {
            if (address == 0x16010204 && known != 0xffffffff) return A::externalInput;
            value = address == 0x16010200 ? enabled : pending;
            return A::ok;
        }
        if (address == 0x16010200) {
            if (((value ^ enabled) & ~0xf0u) || (value & 0xb0)) return A::sideEffect;
            enabled = value;
        } else {
            if (value & ~0xf0u) return A::sideEffect;
            pending &= ~value;
        }
        return A::ok;
    }
    Imx1BootstrapAccess access(uint32_t pc, uint32_t address, uint32_t size, bool write, uint32_t& value,
                              bool emptySdCardEventsKnown = false) {
        using A = Imx1BootstrapAccess;
        if (!owns(address)) return A::unmapped;
        if (size != 4) return A::width;
        if (address == 0x16010200) {
            if (pc == (write ? 0x204d22aau : 0x204d22a4u)) {
                if (!emptySdCardEventsKnown) return A::externalInput;
                if (!write) value = enabled;
                else {
                    if (value != (enabled | 0x02000000u)) return A::sideEffect;
                    enabled = value;
                }
                return A::ok;
            }
            if (pc == (write ? 0x204d22beu : 0x204d22b8u)) {
                if (!write) value = enabled;
                else {
                    if (value != (enabled & ~0x02000000u)) return A::sideEffect;
                    enabled = value;
                }
                return A::ok;
            }
            if (!write) {
                if (pc != 0x20086dee && pc != 0x200876f2) return A::externalInput;
                value = enabled;
            } else {
                // 20086c06 is the driver's shutdown (20086c00, called when the display goes to sleep): it
                // stores 0, every source off, as the start-up write at 20086970 does.
                if (pc != 0x20086970 && pc != 0x20086c06 && pc != 0x20086e08 && pc != 0x20087702) return A::externalInput;
                if ((pc == 0x20086970 || pc == 0x20086c06) && value != 0) return A::sideEffect;
                if (value & ~nativeMask) return A::reservedBits;
                if (value & ~(displayMask | (emptySdCardEventsKnown ? 0x02000000u : 0u))) return A::externalInput;
                if (value & ~known) return A::externalInput;
                enabled = value;
            }
        } else if (write && pc == 0x204d21b6) {
            // The card interrupt handler's own acknowledge.
            if (value != sdMask) return A::sideEffect;
            pending &= ~value;
        } else if (write) {
            if (pc != 0x20087824) return A::externalInput;
            if (value & ~nativeMask) return A::reservedBits;
            pending &= ~value;
            known |= value;
        } else {
            if ((pc != 0x200868cc && pc != 0x200b8cb4) || known != 0xffffffff) return A::externalInput;
            value = pending;
        }
        return A::ok;
    }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) { a.Do32(enabled).Do32(pending).Do32(known); }
   private:
    // Virtual cold reset clears latches. Unadmitted engine producers cannot submit work.
    uint32_t enabled = 0, pending = 0, known = 0xffffffff;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1GraphicsInterrupts) == 12, "update state() and stateVersion");
#endif

#endif
