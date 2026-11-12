#ifndef FROND_IMX1_GRAPHICS_SD_H
#define FROND_IMX1_GRAPHICS_SD_H

#include <algorithm>
#include "imx1_bootstrap.h"

// Explicit virtual empty SD slot. Time is in the shared 200 MHz guest domain.
class Imx1GraphicsSd {
   public:
    static bool owns(uint32_t address) {
        return address == 0x1601090c || address == 0x16010910 ||
               (address >= 0x16202000 && address < 0x16202200);
    }
    void reset() { *this = {}; }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.Do32(control).Do32(interface).Do32(clock).Do32(status1).Do32(status2);
        a.Do32(command).Do32(argumentLow).Do32(argumentHigh).Do32(interruptMask).Do32(option).Do32(port);
        a.Do64(clockRemaining).Do64(commandRemaining).Do64(resetElapsed);
        a.DoBool(commandPending).DoBool(resetHeld).DoBool(released);
    }
    void advance(uint64_t ticks) {
        if (resetHeld) resetElapsed = std::min<uint64_t>(32, resetElapsed + std::min<uint64_t>(ticks,32));
        if (clockRemaining) {
            const auto step = std::min(ticks, clockRemaining);
            clockRemaining -= step;
            ticks -= step;
        }
        if (!clockRemaining && (clock & 0x100) && commandPending && !resetHeld) {
            const auto step = std::min(ticks, commandRemaining);
            commandRemaining -= step;
            if (!commandRemaining) {
                commandPending = false;
                status1 |= 1;
                status2 = 0x80 | ((command & 0x700) == 0x300 ? 0 : 0x40);
            }
        }
    }
    // Ticks until the clock settles or the pending command ends.
    uint64_t idleQuietTicks() const {
        if (clockRemaining) return clockRemaining;
        return (clock & 0x100) && commandPending && !resetHeld ? commandRemaining : UINT64_MAX;
    }
    uint32_t controlValue() const { return control; }
    uint32_t clockValue() const { return clockRemaining ? clock & ~0x100u : clock; }
    uint32_t statusValue() const { return status2; }
    bool pending() const { return commandPending; }
    bool cardEventsKnown() const { return released && (interruptMask == 0x31d || interruptMask == 0x305); }
    Imx1BootstrapAccess access(uint32_t pc, uint32_t address, uint32_t size, bool write, uint32_t& value) {
        using A = Imx1BootstrapAccess;
        if (!owns(address)) return A::unmapped;
        const bool config = address < 0x16200000;
        if (size != 4 && !(size == 2 && !write && !config)) return A::width;
        if (config) {
            if (address == 0x16010910) {
                if (!write) {
                    if (pc != 0x204d26a2 && pc != 0x204d26b4 && pc != 0x204d3618 && pc != 0x204d3728) return A::externalInput;
                    value = control; // Card-present and write-protect inputs are zero for the empty slot.
                } else {
                    if (pc != 0x204d26b2 && pc != 0x204d26ba && pc != 0x204d273e) return A::externalInput;
                    if (value & ~0x438000u) return A::sideEffect;
                    control = value;
                    if (value & 0x8000) { commandPending = false; commandRemaining = 0; clock = 0; clockRemaining = 0; }
                }
            } else {
                if (pc != (write ? 0x204d26c2u : 0x204d26bcu)) return A::externalInput;
                if (write) {
                    if (value & ~0x08000000u) return A::sideEffect;
                    interface = value;
                } else value = interface;
            }
            return A::ok;
        }
        const uint32_t offset = address - 0x16202000;
        if (offset & 3) return A::unmapped;
        if (offset == 0x1c0) {
            if (!write || (pc != 0x204d26cc && pc != 0x204d26d8)) return A::externalInput;
            if (value != 0 && value != 7) return A::sideEffect;
            if (value == 7 && (!resetHeld || resetElapsed < 32)) return A::sequence;
            if (!value) {
                resetHeld = true; resetElapsed = 0; released = false;
                status1 = 0; status2 = 0x80; clock = 0; clockRemaining = 0;
                commandPending = false; commandRemaining = 0; argumentLow = argumentHigh = command = 0;
                interruptMask = option = port = 0;
            } else { resetHeld = false; released = true; }
            return A::ok;
        }
        if (!released || resetHeld || (control & 0x438000) != 0x430000 || interface != 0x08000000) return A::sequence;
        const bool native = (pc >= 0x204d2174 && pc <= 0x204d2190) ||
                            (pc >= 0x204d226e && pc <= 0x204d22b2) ||
                            (pc >= 0x204d22b4 && pc <= 0x204d2326) ||
                            (pc >= 0x204d2456 && pc <= 0x204d269c) ||
                            (pc >= 0x204d2748 && pc <= 0x204d294a) ||
                            (pc >= 0x204d363e && pc <= 0x204d3656) || pc == 0x204d2736;
        if (!native) return A::externalInput;
        if (!write) {
            switch (offset) {
                case 0x38: value = status1; break;
                case 0x3c: value = status2; break;
                case 0x40: value = interruptMask; break;
                case 0x48: value = clockValue(); break;
                case 0x50: value = option; break;
                default: return A::externalInput;
            }
            return A::ok;
        }
        if (value >> 16) return A::sideEffect;
        switch (offset) {
            case 0x00:
                // No card supplies data or responses. Other command formats stay guarded.
                if (value != 0x300 && value != 0x437 && value != 0x701) return A::externalInput;
                if (commandPending) return A::sequence;
                command = value; commandPending = true;
                commandRemaining = uint64_t((value == 0x300 ? 48 : 64)) * 2 * ((clock & 0xff) + 1);
                status1 &= ~1u; status2 = 0x4000;
                break;
            case 0x04: if (value != 0x200) return A::sideEffect; port = value; break;
            case 0x08: if (commandPending) return A::sequence; argumentLow = value; break;
            case 0x0c: if (commandPending) return A::sequence; argumentHigh = value; break;
            case 0x38: status1 &= value; break;
            case 0x3c: if (value & ~0x7fu) return A::sideEffect; status2 = (status2 & ~0x7fu) | (status2 & value & 0x7f); break;
            case 0x40:
                if (pc == 0x204d22a0) {
                    if (value != ((interruptMask & ~0x18u) | 0x305u)) return A::sideEffect;
                } else if (pc != 0x204d22f0 || value != (interruptMask | 0x31du)) return A::sideEffect;
                if (value != 0x31d && value != 0x305) return A::externalInput;
                interruptMask = value;
                break;
            case 0x48:
                if (value & ~0x1ffu) return A::sideEffect;
                if ((value & 0xff) != 0 && (value & 0xff) != 1 && (value & 0xff) != 0x40) return A::sideEffect;
                if ((value & 0x100) && (!(clock & 0x100) || (value & 0xff) != (clock & 0xff))) clockRemaining = 20000;
                if (!(value & 0x100)) clockRemaining = 0;
                clock = value;
                break;
            case 0x50: if (value & ~0x8000u) return A::sideEffect; option = value; break;
            case 0x6c: case 0x1f0: if (value) return A::sideEffect; break;
            case 0x1e8: if (value != 0x700) return A::sideEffect; break;
            default: return A::externalInput;
        }
        return A::ok;
    }
   private:
    uint32_t control = 0, interface = 0, clock = 0, status1 = 0, status2 = 0x80;
    uint32_t command = 0, argumentLow = 0, argumentHigh = 0, interruptMask = 0, option = 0, port = 0;
    uint64_t clockRemaining = 0, commandRemaining = 0, resetElapsed = 0;
    bool commandPending = false, resetHeld = false, released = false;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1GraphicsSd) == 80, "update state() and stateVersion");
#endif
#endif
