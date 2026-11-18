// The battery setting's Charging switch on the PXA boards' power inputs, firmware-free: the TX's GPIO90 and
// GPIO13 (USB detect), the E2's GPIO9 and GPIO35 (USB detect) and the TG50's uc A13, A14 and A2, at the default,
// on the charger, and again after a reset.
#include <cassert>
#include <cstdio>
#include <vector>

#include "CPU.h"
#include "mem.h"
#include "soc_PXA.h"

namespace {
uint32_t read(SocPXA& soc, uint32_t address, uint32_t size = 4) {
    uint32_t value = 0;
    assert(memAccess(cpuGetMem(soc.GetCpu()), address, size, false, &value));
    return value;
}
bool gpio(SocPXA& soc, unsigned pin) { return read(soc, 0x40e00000 + 4 * (pin / 32)) & (1u << (pin % 32)); }
BatteryPower battery(const char* spec) {
    BatteryPower b;
    assert(BatteryPower::Parse(spec, b));
    return b;
}

// Today's levels off the charger: the TX reads external power (GPIO90 high).
constexpr bool txPowerOffCharger = true;

void tx() {
    std::vector<uint8_t> rom(16 * 1024 * 1024);
    std::vector<uint8_t> nand(132 * 1024 * 1024, 0xff);
    SocPXA soc(DeviceType5::deviceTypeTX, 32 * 1024 * 1024, rom.data(), rom.size(), nand.data(), nand.size(), -1, 2);
    assert(gpio(soc, 90) == txPowerOffCharger);
    assert(soc.SetBattery(battery("level 50 charging")) && gpio(soc, 90));
    soc.Reset();
    assert(gpio(soc, 90));
    assert(soc.SetBattery(battery("level 50")) && gpio(soc, 90) == txPowerOffCharger);
    // The USB test cable still drives GPIO13 (low = cable in).
    assert(soc.SetUsbTestCable(false) && gpio(soc, 13));
    assert(soc.SetUsbTestCable(true) && !gpio(soc, 13));
    assert(soc.SetUsbTestCable(false) && gpio(soc, 13));
}

void e2() {
    std::vector<uint8_t> rom(8 * 1024 * 1024);
    std::vector<uint8_t> nand(33 * 1024 * 1024, 0xff);
    SocPXA soc(DeviceType5::deviceTypeE2, 32 * 1024 * 1024, rom.data(), rom.size(), nand.data(), nand.size(), -1, 0);
    assert(!gpio(soc, 9) && gpio(soc, 35));
    assert(soc.SetBattery(battery("max charging")) && gpio(soc, 9) && gpio(soc, 35));
    soc.Reset();
    assert(gpio(soc, 9));
    assert(soc.SetBattery(battery("max")) && !gpio(soc, 9));
}

void tg50() {
    std::vector<uint8_t> rom(4096);
    SocPXA soc(deviceTypeTG50, 16u << 20, rom.data(), rom.size(), nullptr, 0, -1, 0);
    const auto portA = [&] { return read(soc, 0x08000000) & 0xffff; };
    constexpr uint32_t a2 = 1u << 2, a13 = 1u << 13, a14 = 1u << 14;
    assert((portA() & (a2 | a13 | a14)) == (a13 | a14));
    assert(soc.SetBattery(battery("level 80 charging")) && !(portA() & (a2 | a13 | a14)));
    soc.Reset();
    assert(!(portA() & (a2 | a13 | a14)));
    assert(soc.SetBattery(battery("level 80")) && (portA() & (a2 | a13 | a14)) == (a13 | a14));
}
}  // namespace

int main() {
    tx();
    e2();
    tg50();
    std::puts("pxa battery pins: ok");
    return 0;
}
