// The 68k battery hooks: the ADS784x battery count on the ROM's sysBatteryData curve for the battery setting's
// level, the m500 family's AC_PWR# line and the CLIE DC_IN lines for its Charging switch, and which devices
// can take Charging at all.
#include <cstdio>
#include <cstdlib>

#include "EmBattery.h"
#include "EmDevice.h"
#include "EmRegsEZPalmIIIc.h"
#include "EmRegsEZPalmV.h"
#include "EmRegsMB86189.h"
#include "EmRegsSZNaples.h"
#include "EmRegsSZRedwood.h"
#include "EmRegsSonyDSP.h"
#include "EmRegsVZPegVenice.h"
#include "EmRegsVZPalmM130.h"
#include "EmRegsVZPalmM500.h"
#include "EmRegsVZPalmM505.h"
#include "EmSPISlaveADS784x.h"

static void check(bool success, const char* what) {
    if (!success) {
        std::fprintf(stderr, "FAIL: 68k battery: %s\n", what);
        std::abort();
    }
}

class PalmV : public EmRegsEZPalmV {
   public:
    uint8 DockId() {
        auto* adc = GetSPISlave();
        check(adc != nullptr, "Palm V ADC");
        adc->DoExchange(7, 0xec);
        return (adc->DoExchange(15, 0) >> 7) & 0xff;
    }
};

template <class Board, class Chip>
class Exposed : public Board {
   public:
    explicit Exposed(Chip& chip) : Board(chip) {}
    uint8 Port(int port) { return this->GetPortInputValue(port); }
};

// DC_IN is active low: high on battery, low on the charger (ROM HwrDockStatus on Venice and Redwood).
template <class Board, class Chip>
static void dcIn(Chip& chip, int port, uint8 bit, const char* what) {
    Exposed<Board, Chip> board(chip);
    gBattery = BatteryPower{};
    check(board.Port(port) & bit, what);
    check(BatteryPower::Parse("max charging", gBattery), "parse");
    check(!(board.Port(port) & bit), what);
    gBattery = BatteryPower{};
    check(board.Port(port) & bit, what);
}

int main() {
    // A lithium-ion style table, 2.9 V at 0% to 4.2 V at 100%, with 4096 steps per 5 V.
    const uint16_t curve[11] = {290, 360, 368, 372, 375, 378, 382, 387, 393, 401, 420};
    const UInt16 steps = 4096 * 100 / 500;
    const Int16 offset = 0;
    // 100% is the count from entry [10], the value every 68k EZ/VZ board read before the setting.
    const uint16 full7843 = uint16(((420 * steps + 50) / 100 - offset) << 4);
    check(EmSPISlaveADS784x::BatteryCount(curve, steps, offset, 100, false) == full7843, "ADS7843 full");
    uint16 full7846 = full7843;  // the ADS7846 halves it in 16 bits, as the board always did
    full7846 *= 2;
    full7846 /= 4;
    check(EmSPISlaveADS784x::BatteryCount(curve, steps, offset, 100, true) == full7846, "ADS7846 full");
    check(EmSPISlaveADS784x::BatteryCount(curve, steps, offset, 0, false) ==
              uint16(((290 * steps + 50) / 100 - offset) << 4),
          "empty is entry [0]");
    uint16 last = 0;
    for (unsigned p = 0; p <= 100; p += 5) {
        const uint16 count = EmSPISlaveADS784x::BatteryCount(curve, steps, offset, p, false);
        check(count >= last, "count rises with the level");
        last = count;
    }

    EmRegsVZPalmM500 m500;
    EmRegsVZPalmM505 m520(true);
    EmRegsVZPalmM130 m130;
    EmRegsEZPalmIIIc iiic;
    PalmV palmV;
    for (bool charging : {false, true, false}) {
        check(BatteryPower::Parse(charging ? "level 30 charging" : "level 30 battery", gBattery), "parse");
        check(bool(m500.GetPortInputValue('K') & 0x04) != charging, "m500 PK2 AC_PWR#");
        check(bool(m500.GetPortInputValue('E') & 0x40) != charging, "m500 PE6 CHARGING#");
        check(bool(m520.GetPortInputValue('K') & 0x04) != charging, "m520 PK2 AC_PWR#");
        check(bool(m520.GetPortInputValue('K') & 0x02) != charging, "m520 PK1 CHARGING#");
        check(bool(m130.GetPortInputValue('C') & 0x40) != charging, "m130 PC6 power");
        check(bool(iiic.GetPortInputValue('D') & 0x40) != charging, "IIIc PD6 adapter detect");
        check(palmV.DockId() == (charging ? 96 : 0), "Palm V/Vx charger ID");
    }

    EmRegsMB86189 mb86189(0x10800000);
    EmRegsSonyDSP dsp(0x11000000);
    dcIn<EmRegsVzPegVenice>(mb86189, 'J', 0x04, "Venice PJ2 DC_IN");
    dcIn<EmRegsSzRedwood>(dsp, 'P', 0x80, "Redwood PP7 DC_IN");
    dcIn<EmRegsSzNaples>(dsp, 'P', 0x80, "Naples PP7 DC_IN");

    // Charging reaches the guest through the dock status patch or a modelled input, and nowhere else.
    for (const char* id : {"PalmIIIx", "PalmM125", "PEG-N700C/N710C"})
        check(!EmDevice(id).BatteryChargingSupported(), id);
    for (const char* id : {"PalmIIIc", "PalmV", "PalmVx", "PalmM500", "PalmM130", "PalmM520", "AlphaSmartDana",
                           "DanaWireless", "PEG-T400", "NR70", "YSX1230"})
        check(EmDevice(id).BatteryChargingSupported(), id);

    std::puts("68k battery: ok");
    return 0;
}
