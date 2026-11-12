#ifndef FROND_IMX1_BTA_H
#define FROND_IMX1_BTA_H

#include <cstdint>

enum class Imx1BtaAccess { ok, unmapped, width, reservedBits, sideEffect };

// Bluetooth Accelerator (MC9328MX1RM chapter 16), only what the Zodiac ROM's radio probe at 200fc874 touches.
// POLICY "Zodiac Bluetooth radio absent": nothing drives the BTA's radio pins or its reference clock, so its
// SysTick never runs and BTSYS (source 55) stays low. BTTIM and BTWUI need the timer and the wake-up sequence,
// which are refused here, so they stay low too.
class Imx1Bta {
   public:
    static constexpr uint64_t interruptSources = uint64_t(7) << 55;
    static bool contains(uint32_t address) { return address >= 0x00216000 && address < 0x00217000; }
    // clocked is GPCR BTAEN (table 8-4). With it clear the module is left unmapped.
    Imx1BtaAccess access(uint32_t address, uint32_t size, bool write, uint32_t& value, bool clocked) {
        if (!clocked || (address != 0x00216110 && address != 0x00216118)) return Imx1BtaAccess::unmapped;
        if (size != 4) return Imx1BtaAccess::width;
        if (address == 0x00216110) {
            // WU_CONTROL is write only (a read gives WU_STATUS). PDE and CLR_CNT start things that have no model.
            if (!write) return Imx1BtaAccess::unmapped;
            if (value & ~0x18u) return Imx1BtaAccess::reservedBits;
            return value ? Imx1BtaAccess::sideEffect : Imx1BtaAccess::ok;
        }
        // CLK_CONTROL: RF mode, bus wait slots and the reference clock divider, reset 0 (table 16-57).
        // ASSUMPTION "BTA CLK_CONTROL reads back": a read gives the last value written. The text of 16.5.9.1
        // calls the register write-only, its bit table and the register map (table 16-10) have it as
        // read/write, and the ROM's radio probe reads it at 200fc8d4 to put it back at 200fc91c.
        if (write) {
            if (value & ~0xeff3u) return Imx1BtaAccess::reservedBits;
            // RFM takes 011 (MC13180) and 100 (SiliconWave), every other setting is reserved (table 16-57).
            // 000 is the reset value and what the ROM puts back, so it is taken as well.
            const uint32_t rfm = value >> 13;
            if (rfm != 0 && rfm != 3 && rfm != 4) return Imx1BtaAccess::reservedBits;
            clockControl = value;
        } else value = clockControl;
        return Imx1BtaAccess::ok;
    }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) { a.Do32(clockControl); }

   private:
    uint32_t clockControl = 0;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Bta) == 4, "update state() and stateVersion");
#endif

#endif
