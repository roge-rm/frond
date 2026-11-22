// Host inputs read through the PXA GPIO registers and the TG50's UC, without firmware.
#include <cassert>
#include <cstdio>
#include <vector>

#include "CPU.h"
#include "mem.h"
#include "sdcard.h"
#include "soc_PXA.h"

namespace {
uint32_t read(SocPXA& soc, uint32_t address, uint32_t size = 4) {
    uint32_t value = 0;
    assert(memAccess(cpuGetMem(soc.GetCpu()), address, size, false, &value));
    return value;
}
void write(SocPXA& soc, uint32_t address, uint32_t value) {
    assert(memAccess(cpuGetMem(soc.GetCpu()), address, 4, true, &value));
}
bool gpio(SocPXA& soc, unsigned pin) {
    return read(soc, 0x40e00000 + (pin >= 96 ? 0x100 : 4 * (pin / 32))) & (1u << (pin % 32));
}
BatteryPower battery(const char* spec) {
    BatteryPower b;
    assert(BatteryPower::Parse(spec, b));
    return b;
}

// Off the charger the TX's GPIO90 is low.
constexpr bool txPowerOffCharger = false;

void headphones(SocPXA& soc, unsigned pin) {
    assert(soc.HeadphonesSupported() && !soc.HeadphonesIn() && !gpio(soc, pin));
    assert(soc.SetHeadphones(true) && soc.HeadphonesIn() && gpio(soc, pin));
    soc.Reset();
    assert(gpio(soc, pin));
    assert(soc.Save());
    const auto state = soc.GetSavestate();
    assert(soc.SetHeadphones(false));
    assert(soc.Load(state.size, state.data));
    assert(!soc.HeadphonesIn() && !gpio(soc, pin));
}

void card(SocPXA& soc, unsigned pin) {
    assert(!gpio(soc, pin));
    sdCardInitialize(65536, "pxa-card-protect");
    soc.SetSdWriteProtect(true);
    assert(!gpio(soc, pin));
    soc.SdInsert();
    assert(soc.SdInserted() && gpio(soc, pin));
    soc.Reset();
    assert(soc.SdInserted() && gpio(soc, pin));
    assert(soc.Save());
    const auto state = soc.GetSavestate();
    soc.SetSdWriteProtect(false);
    assert(soc.Load(state.size, state.data) && soc.SdRemount());
    assert(!gpio(soc, pin));
    soc.SetSdWriteProtect(true);
    assert(gpio(soc, pin));
    soc.SdEject();
    assert(!gpio(soc, pin));
    sdCardReset();
}

void tx() {
    std::vector<uint8_t> rom(16 * 1024 * 1024);
    std::vector<uint8_t> nand(132 * 1024 * 1024, 0xff);
    SocPXA soc(DeviceType5::deviceTypeTX, 32 * 1024 * 1024, rom.data(), rom.size(), nand.data(), nand.size(), -1, 2);
    assert(gpio(soc, 90) == txPowerOffCharger && gpio(soc, 13));
    assert(soc.SetBattery(battery("level 50 charging")) && gpio(soc, 90));
    soc.Reset();
    assert(gpio(soc, 90));
    assert(soc.SetBattery(battery("level 50")) && gpio(soc, 90) == txPowerOffCharger);
    // The USB test cable still drives GPIO13 (low = cable in).
    assert(soc.SetUsbTestCable(false) && gpio(soc, 13));
    assert(soc.SetUsbTestCable(true) && !gpio(soc, 13));
    assert(soc.SetUsbTestCable(false) && gpio(soc, 13));
    headphones(soc, 107);
    card(soc, 115);
}

void e2(DeviceType5 type) {
    std::vector<uint8_t> rom(8 * 1024 * 1024);
    std::vector<uint8_t> nand(33 * 1024 * 1024, 0xff);
    SocPXA soc(type, 32 * 1024 * 1024, rom.data(), rom.size(), nand.data(), nand.size(), -1, 0);
    assert(!gpio(soc, 9) && gpio(soc, 35));
    assert(soc.SetBattery(battery("max charging")) && gpio(soc, 9) && gpio(soc, 35));
    soc.Reset();
    assert(gpio(soc, 9));
    assert(soc.SetBattery(battery("max")) && !gpio(soc, 9));
    headphones(soc, 12);
    card(soc, 51);
}

void zire72() {
    std::vector<uint8_t> rom(0xe40000);
    SocPXA soc(deviceTypeZire72, 32u << 20, rom.data(), rom.size(), nullptr, 0, -1, 2);
    assert(soc.HeadphonesSupported() && !soc.HeadphonesIn());
    assert(gpio(soc, 99));
    assert(soc.SetBattery(battery("level 50 charging")) && !gpio(soc, 99));
    soc.Reset();
    assert(!gpio(soc, 99));
    assert(soc.SetBattery(battery("level 50")) && gpio(soc, 99));
    card(soc, 115);

    constexpr uint32_t codec = 0x40500200;
    constexpr uint32_t jack = 2;
    const auto jackStatus = [&] {
        read(soc, codec + 2 * 0x54);  // The controller returns the previous codec read.
        return read(soc, codec + 2 * 0x54) & jack;
    };
    const auto armJack = [&] {
        write(soc, codec + 2 * 0x56, 0);     // codec GPIO IRQ output
        write(soc, codec + 2 * 0x4c, jack);  // input
        write(soc, codec + 2 * 0x4e, jack);  // rising
        write(soc, codec + 2 * 0x50, jack);  // sticky
        write(soc, codec + 2 * 0x52, jack);  // interrupt
    };
    armJack();
    assert(!gpio(soc, 27));
    assert(soc.SetHeadphones(true) && gpio(soc, 27));
    assert(jackStatus());
    write(soc, codec + 2 * 0x4e, 0);  // falling
    write(soc, codec + 2 * 0x54, 0);
    assert(!gpio(soc, 27));
    assert(soc.Save());
    const auto state = soc.GetSavestate();
    assert(soc.SetHeadphones(false) && gpio(soc, 27));
    assert(soc.Load(state.size, state.data) && gpio(soc, 27));
    assert(!soc.HeadphonesIn() && jackStatus());
    assert(soc.SetHeadphones(true));
    soc.Reset();
    assert(soc.HeadphonesIn());
    armJack();
    assert(gpio(soc, 27));
}

void tg50() {
    std::vector<uint8_t> rom(4096);
    SocPXA soc(deviceTypeTG50, 16u << 20, rom.data(), rom.size(), nullptr, 0, -1, 0);
    assert(!soc.HeadphonesSupported() && !soc.SetHeadphones(true));
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
    e2(deviceTypeE2);
    e2(deviceTypeFrankenE2);
    zire72();
    tg50();
    std::puts("pxa battery pins: ok");
    return 0;
}
