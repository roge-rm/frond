#ifndef FROND_IMX1_GRAPHICS_GPIO_H
#define FROND_IMX1_GRAPHICS_GPIO_H

#include "imx1_bootstrap.h"

// Virtual two-bank GPIO. External inputs and board connections stay unknown.
class Imx1GraphicsGpio {
   public:
    struct Bank {
        uint32_t control = 0, data = 0;
        bool controlKnown = false, dataKnown = false;
        uint16_t direction() const { return data >> 16; }
        uint16_t latch() const { return data; }
        uint16_t pullDown() const { return control; }
        uint16_t padKnown() const { return controlKnown && dataKnown ? direction() : 0; }
        uint16_t padValue() const { return latch() & padKnown(); }
    };
    static bool owns(uint32_t address) {
        return address == 0x160104b8 || address == 0x160104bc || address == 0x160104c0 ||
               address == 0x16010528 || address == 0x1601052c || address == 0x16010530;
    }
    void reset() { banks[0] = banks[1] = {}; dataRead[0] = dataRead[1] = controlRead[0] = controlRead[1] = false; slotRead = false; }
    Bank bank(unsigned index) const { return index < 2 ? banks[index] : Bank{}; }
    Imx1BootstrapAccess access(uint32_t pc, uint32_t address, uint32_t size, bool write, uint32_t& value) {
        if (!owns(address)) return Imx1BootstrapAccess::unmapped;
        if (size != 4) return Imx1BootstrapAccess::width;
        const unsigned index = address >= 0x16010528;
        const auto offset = address - (index ? 0x16010528u : 0x160104b8u);
        auto& state = banks[index];
        // CNTL2/4 sample pads; a full input word has no established external levels.
        if (offset == 8) return Imx1BootstrapAccess::externalInput;
        const bool control = offset == 4;
        // The native absent-card path sets GPIO14 direction and latch together.
        if (!index && offset == 0 && ((!write && pc == 0x204d3210) ||
                                      (write && pc == 0x204d3226))) {
            if (!state.dataKnown || !state.controlKnown) return Imx1BootstrapAccess::sequence;
            if (!write) {
                value = slotData = state.data;
                slotRead = true;
            } else {
                if (!slotRead || state.data != slotData) return Imx1BootstrapAccess::sequence;
                if (value != (slotData | 0x40004000u)) return Imx1BootstrapAccess::sideEffect;
                state.data = value;
                slotRead = false;
            }
            return Imx1BootstrapAccess::ok;
        }
        // The card driver's own updates of pin 14: direction set, latch cleared (204d26f4) or set (204d2716,
        // and 204d2f6e when it comes back from sleep with the slot empty).
        if (!index && offset == 0 &&
            ((!write && (pc == 0x204d26f4 || pc == 0x204d2716 || pc == 0x204d2f6e)) ||
             (write && (pc == 0x204d2708 || pc == 0x204d272a || pc == 0x204d2f82)))) {
            if (!state.dataKnown || !state.controlKnown) return Imx1BootstrapAccess::sequence;
            if (!write) value = state.data;
            else {
                if ((value ^ state.data) & ~0x40004000u) return Imx1BootstrapAccess::sideEffect;
                state.data = value;
            }
            return Imx1BootstrapAccess::ok;
        }
        if (write) {
            if (pc == (control ? 0x200b9b8cu : 0x200b9b5cu)) {
                bool& read = control ? controlRead[index] : dataRead[index];
                if (!read || !state.controlKnown || !state.dataKnown) return Imx1BootstrapAccess::sequence;
                const uint32_t previous = control ? state.control : state.data;
                const uint32_t changed = previous ^ value;
                if ((changed && (changed & (changed - 1))) || (control && (value >> 16) != 0xffff))
                    return Imx1BootstrapAccess::sideEffect;
                if (control) state.control = value;
                else state.data = value;
                read = false;
                return Imx1BootstrapAccess::ok;
            }
            const uint32_t caller = control ? (index ? 0x200b99f0u : 0x200b99c4u) :
                                              (index ? 0x200b9a34u : 0x200b9a14u);
            if (pc != caller) return Imx1BootstrapAccess::externalInput;
            if (control) {
                if ((value >> 16) != 0xffff) return Imx1BootstrapAccess::sideEffect;
                state.control = value;
                state.controlKnown = true;
            } else {
                if (!state.controlKnown) return Imx1BootstrapAccess::sequence;
                state.data = value;
                state.dataKnown = true;
            }
        } else {
            const bool caller = control ? (pc == 0x200b9b80 || pc == 0x200b9cb4) :
                                          (pc == 0x200b9b50 || pc == 0x200b9c78);
            if (!caller) return Imx1BootstrapAccess::externalInput;
            if (!(control ? state.controlKnown : state.dataKnown)) return Imx1BootstrapAccess::sequence;
            value = control ? state.control : state.data;
            if (control) controlRead[index] = true;
            else dataRead[index] = true;
        }
        return Imx1BootstrapAccess::ok;
    }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        for (auto& b : banks) a.Do32(b.control).Do32(b.data).DoBool(b.controlKnown).DoBool(b.dataKnown);
        a.DoBool(dataRead[0]).DoBool(dataRead[1]).DoBool(controlRead[0]).DoBool(controlRead[1]);
        a.DoBool(slotRead).Do32(slotData);
    }

   private:
    // Under admitted selector ffff, direction 1 drives the latch at an ideal pad.
    Bank banks[2];
    bool dataRead[2]{}, controlRead[2]{};
    bool slotRead = false;
    uint32_t slotData = 0;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1GraphicsGpio) == 36, "update state() and stateVersion");
#endif

#endif
