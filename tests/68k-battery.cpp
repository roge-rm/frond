// The 68k battery hooks: the ADS784x battery count on the ROM's sysBatteryData curve for the battery setting's
// level, and the m500 family's AC_PWR# line for its Charging switch.
#include <cstdio>
#include <cstdlib>

#include "EmBattery.h"
#include "EmRegsVZPalmM500.h"
#include "EmSPISlaveADS784x.h"

static void check(bool success, const char* what) {
    if (!success) {
        std::fprintf(stderr, "FAIL: 68k battery: %s\n", what);
        std::abort();
    }
}

class M500 : public EmRegsVZPalmM500 {
   public:
    uint8 PortK() { return GetPortInputValue('K'); }
};

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

    M500 board;
    gBattery = BatteryPower{};
    check(board.PortK() & 0x04, "AC_PWR# high on battery");
    check(BatteryPower::Parse("level 30 charging", gBattery), "parse");
    check(!(board.PortK() & 0x04), "AC_PWR# low on external power");
    check(BatteryPower::Parse("level 30 battery", gBattery), "parse");
    check(board.PortK() & 0x04, "AC_PWR# high again");

    std::puts("68k battery: ok");
    return 0;
}
