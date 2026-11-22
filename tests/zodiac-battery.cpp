// The Zodiac's battery setting: the level as the ASP U word on the ROM's own count-to-voltage chain, Maximum
// exactly the fixed 0x9b00, Charging on GPIO B14, the setting not saved with the state and put back after every load and reset.
// Also the headphones, the cradle button and the cradle, and the USB bridge's cable through the cradle. Made-up ROM,
// no firmware.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "UsbHostBridge.h"
#include "soc_IMX1.h"
#include "zodiac_battery.h"

namespace {
void put32le(std::vector<uint8_t>& rom, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) rom[at + i] = uint8_t(v >> (8 * i)); }
void put32be(std::vector<uint8_t>& rom, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) rom[at + i] = uint8_t(v >> (24 - 8 * i)); }

// The made-up ROM of test-zodiac-normal-runtime: the identity layout, and a branch to itself at 0.
std::vector<uint8_t> rom(uint32_t first = 0xeafffffe) {
    constexpr size_t card = 0x20000;
    std::vector<uint8_t> rom(card + 0x1000);
    put32le(rom, card + 0x78, 0x400);
    put32le(rom, card + 0xac, 0x300);
    put32be(rom, card + 0x300 + 4, 0x9a24);
    put32be(rom, card + 0x400 + 4, 0u - 0x9a24);
    size_t at = card + 0x408, value = 0;
    for (size_t length : {100, 128, 128, 128, 92}) {
        rom[at++] = uint8_t(0x80 | (length - 1));
        for (size_t i = 0; i < length; ++i, ++value) rom[at++] = uint8_t(value * 7 + 3);
    }
    put32le(rom, 0, first);
    return rom;
}

BatteryPower battery(const char* spec) {
    BatteryPower b;
    assert(BatteryPower::Parse(spec, b));
    return b;
}

std::vector<uint8_t> save(SocIMX1& soc) {
    assert(soc.Save());
    const auto state = soc.GetSavestate();
    const auto* bytes = static_cast<const uint8_t*>(state.data);
    return {bytes, bytes + state.size};
}

void load(SocIMX1& soc, std::vector<uint8_t> state) { assert(soc.Load(state.size(), state.data())); }

void curve() {
    // The ROM's voltage call with the default calibration (REA facts, 2008860e).
    const int volts[] = {373, 380, 386, 393, 400, 406, 406, 413, 420};
    for (int count = 303; count <= 311; ++count) assert(zodiacBatteryCentivolts(count) == volts[count - 303]);
    assert(zodiacBatteryCentivolts(241) == 0);
    assert(zodiacBatterySample(100) == zodiacBatteryFullSample && zodiacBatteryFullSample == 310 << 7);
    // 0% is below the record's 3.74 V shutdown and 3.75 V severe warning; 30% is the 3.80 V mild warning.
    assert(zodiacBatterySample(0) == 303 << 7);
    assert(zodiacBatterySample(30) == 304 << 7);
    assert(zodiacBatterySample(60) == 305 << 7);
    for (unsigned percent = 1; percent <= 100; ++percent) {
        const auto sample = zodiacBatterySample(percent);
        assert(!(sample & 0x7f) && sample >= zodiacBatterySample(percent - 1));
        assert(zodiacBatteryMode1(percent) >= zodiacBatteryMode1(percent - 1));
    }
    // 0% gives the ROM's "EXTREMELY low" alert, 1% an empty icon and 100% the fixed full sample.
    assert(zodiacBatteryMode1(0) == 640 && zodiacBatteryMode1(1) == 644 && zodiacBatteryMode1(100) == 712);
}

void machine() {
    auto image = rom();
    std::string why;
    SocIMX1* soc = SocIMX1::Create(image.data(), image.size(), why);
    assert(soc && soc->BatterySupported());
    assert(soc->Battery() == BatteryPower{} && soc->BatterySample() == zodiacBatteryFullSample);
    assert(soc->BatteryGaugeSample() == zodiacBatteryFullMode1);
    const auto atMaximum = save(*soc);

    // Live: both inputs follow the setting at once.
    assert(soc->SetBattery(battery("level 0")));
    assert(soc->BatterySample() == 303 << 7 && soc->BatteryGaugeSample() == 640);

    // A load and a reset put the setting back over the state's fixed inputs.
    load(*soc, atMaximum);
    assert(soc->BatterySample() == 303 << 7 && soc->BatteryGaugeSample() == 640);
    soc->Reset();
    assert(soc->BatterySample() == 303 << 7 && soc->BatteryGaugeSample() == 640);
    assert(soc->SetBattery(battery("level 30")) && soc->BatterySample() == 304 << 7);
    assert(soc->BatteryGaugeSample() == zodiacBatteryMode1(30));
    assert(soc->SetBattery(battery("max")) && soc->BatterySample() == zodiacBatteryFullSample);
    assert(soc->BatteryGaugeSample() == zodiacBatteryFullMode1);

    // Charging pulls B14 low, through a load and a reset as well. Off the charger it's high.
    assert(!soc->ChargerInputLow());
    assert(soc->SetBattery(battery("level 40 charging")) && soc->ChargerInputLow());
    load(*soc, atMaximum);
    assert(soc->ChargerInputLow());
    soc->Reset();
    assert(soc->ChargerInputLow() && soc->BatterySample() == zodiacBatterySample(40));
    assert(soc->SetBattery(battery("level 40")) && !soc->ChargerInputLow());

    // A state made at another level loads at the setting the machine has.
    assert(soc->SetBattery(battery("level 5")));
    const auto atLevel = save(*soc);
    assert(soc->SetBattery(battery("max")));
    load(*soc, atLevel);
    assert(soc->BatterySample() == zodiacBatteryFullSample && soc->BatteryGaugeSample() == zodiacBatteryFullMode1);

    // Headphones: B16 high while in, put back after a load and a reset. B14 stays as set.
    assert(soc->HeadphonesSupported() && !soc->HeadphonesIn() && !soc->HeadphoneInputHigh());
    assert(soc->SetHeadphones(true) && soc->HeadphonesIn() && soc->HeadphoneInputHigh() && !soc->ChargerInputLow());
    load(*soc, atMaximum);
    assert(soc->HeadphoneInputHigh());
    soc->Reset();
    assert(soc->HeadphoneInputHigh());
    assert(soc->SetBattery(battery("max charging")) && soc->HeadphoneInputHigh() && soc->ChargerInputLow());
    assert(soc->SetHeadphones(false) && !soc->HeadphoneInputHigh() && soc->ChargerInputLow());
    assert(soc->SetBattery(battery("max")));

    // The cradle button: D22 low while held, let go by a load.
    assert(soc->CradleButtonSupported() && !soc->CradleButtonDown() && !soc->CradleInputLow());
    assert(soc->SetCradleButton(true) && soc->CradleButtonDown() && soc->CradleInputLow());
    assert(soc->SetCradleButton(false) && !soc->CradleButtonDown() && !soc->CradleInputLow());
    assert(soc->SetCradleButton(true));
    load(*soc, atMaximum);
    assert(!soc->CradleButtonDown() && !soc->CradleInputLow());
    // Pressed again before the release after a load is pumped: the release leaves D22 low.
    assert(soc->SetCradleButton(true));
    soc->Run(1000, 32000000);
    assert(soc->CradleButtonDown() && soc->CradleInputLow());
    assert(soc->SetCradleButton(false) && !soc->CradleInputLow());
    // A HotSync press holds D22 for half a guest second, then lets go by itself; a load lets go at once.
    assert(soc->PressHotSync() && soc->CradleButtonDown() && soc->CradleInputLow() && !soc->PressHotSync());
    soc->Run(100000, 1000000);
    assert(soc->CradleButtonDown() && soc->CradleInputLow());
    const uint64_t pressed = soc->GetTime();
    for (int i = 0; i < 2000 && soc->CradleButtonDown(); ++i) soc->Run(100000, 1000000);
    assert(soc->GetTime() - pressed >= 300000000 && soc->GetTime() - pressed < 600000000);
    assert(!soc->CradleButtonDown() && !soc->CradleInputLow());
    assert(soc->PressHotSync());
    load(*soc, atMaximum);
    assert(!soc->CradleButtonDown() && !soc->CradleInputLow() && soc->PressHotSync());
    load(*soc, atMaximum);
    assert(!soc->PowerSleeping());

    // The cradle: A11 and A1 high and the ID sample 0 while attached, put back after a load and a reset.
    // Detached is the 900 every state was made with, so a machine with no cradle saves as before.
    assert(soc->CradleSupported() && !soc->CradleAttached() && !soc->AccessoryPinsHigh() && soc->AccessorySample() == 900);
    const auto detached = save(*soc);
    assert(soc->SetCradle(false) && save(*soc) == detached);
    assert(soc->SetCradle(true) && soc->CradleAttached() && soc->AccessoryPinsHigh() && soc->AccessorySample() == 0);
    const auto attached = save(*soc);
    assert(attached != detached && soc->CradleSetting());
    // A short host hold stays down until the guest has time to see it.
    assert(soc->SetCradleButton(true) && soc->SetCradleButton(false));
    assert(soc->CradleButtonDown() && soc->CradleInputLow());
    const uint64_t shortPress = soc->GetTime();
    for (int i = 0; i < 2000 && soc->CradleButtonDown(); ++i) soc->Run(100000, 1000000);
    assert(soc->GetTime() - shortPress >= 499000000);
    assert(!soc->CradleButtonDown() && !soc->CradleInputLow());
    load(*soc, atMaximum);
    assert(soc->AccessoryPinsHigh() && soc->AccessorySample() == 0);
    soc->Reset();
    assert(soc->AccessoryPinsHigh() && soc->AccessorySample() == 0 && !soc->HeadphoneInputHigh());
    assert(soc->SetCradle(false) && !soc->AccessoryPinsHigh() && soc->AccessorySample() == 900);
    load(*soc, attached);
    assert(!soc->CradleAttached() && !soc->AccessoryPinsHigh() && soc->AccessorySample() == 900);

    // The USB bridge takes out only the cradle it put in. Off, a load, a reset, usb-mode off and Stop keep the
    // user's cradle.
    const auto cradleIn = [&] { return soc->CradleAttached() && soc->AccessoryPinsHigh() && soc->AccessorySample() == 0; };
    // A second of guest time, past the bridge's half second replug.
    const auto run = [&](UsbHostBridge& usb) {
        for (const uint64_t start = soc->GetTime(); soc->GetTime() - start < 1000000000;) {
            soc->Run(100000, 1000000);
            usb.Service();
        }
    };
    std::string error;
    {
        UsbHostBridge usb;
        assert(soc->SetCradle(true));
        assert(usb.Start(soc, {HostSerial::Kind::off, 0, {}}, error) && cradleIn());
        load(*soc, atMaximum);
        run(usb);
        assert(cradleIn() && !soc->UsbCableAttached());
        soc->Reset();
        run(usb);
        assert(cradleIn());
        // On: the cable comes in with the user's cradle, goes out and back after a load, and usb-mode off
        // leaves the cradle in.
        assert(usb.Start(soc, {HostSerial::Kind::pty, 0, {}}, error));
        run(usb);
        assert(soc->UsbCableAttached() && cradleIn());
        load(*soc, atMaximum);
        soc->Run(100000, 1000000);
        assert(!soc->UsbCableAttached() && cradleIn());
        run(usb);
        assert(soc->UsbCableAttached() && cradleIn());
        assert(usb.Start(soc, {HostSerial::Kind::off, 0, {}}, error));
        run(usb);
        assert(!soc->UsbCableAttached() && cradleIn());
        // With no cradle of the user's, the cable alone puts it in and Stop takes it out.
        assert(soc->SetCradle(false) && !soc->CradleAttached() && !soc->AccessoryPinsHigh());
        assert(usb.Start(soc, {HostSerial::Kind::pty, 0, {}}, error));
        run(usb);
        assert(soc->UsbCableAttached() && cradleIn() && !soc->CradleSetting());
        assert(soc->SetCradle(true) && soc->CradleSetting() && cradleIn());
        assert(soc->SetCradle(false) && !soc->CradleSetting() && cradleIn());
        usb.Stop();
        assert(!soc->CradleAttached() && !soc->AccessoryPinsHigh() && soc->AccessorySample() == 900);
    }
}
}  // namespace

// A machine that stops: the first instruction is undefined and FROND_ZODIAC_STOP_ON_EXCEPTION stops on it.
// A reset and a load start it again, with the setting in place.
void halted() {
    setenv("FROND_ZODIAC_STOP_ON_EXCEPTION", "1", 1);
    auto image = rom(0xe7f000f0);
    std::string why;
    SocIMX1* soc = SocIMX1::Create(image.data(), image.size(), why);
    assert(soc);
    const auto fresh = save(*soc);
    assert(soc->SetBattery(battery("level 20")));
    const auto stop = [&] {
        soc->Run(1000000, 32000000);
        assert(soc->Halted());
    };
    stop();
    load(*soc, fresh);
    assert(!soc->Halted() && soc->BatterySample() == zodiacBatterySample(20) && !soc->ChargerInputLow());
    stop();
    soc->Reset();
    assert(!soc->Halted() && soc->BatterySample() == zodiacBatterySample(20) && !soc->ChargerInputLow());
    stop();
    // Halted, the headphone and cradle commands report the failure and change nothing.
    assert(!soc->SetHeadphones(true) && !soc->HeadphonesIn());
    assert(!soc->SetCradleButton(true) && !soc->CradleButtonDown());
    assert(!soc->PressHotSync() && !soc->CradleButtonDown());
    assert(!soc->SetCradle(true) && !soc->CradleAttached());
    assert(soc->SetBattery(battery("level 20 charging")));
    soc->Reset();
    assert(soc->ChargerInputLow());
}

// A guest waiting with nothing to wake it: its clock stands still, but a USB host tick that's due (the plug-in)
// runs it on to the slice end, where the host plugs in.
void waiting() {
    auto image = rom(0xee070f90);        // wait for interrupt
    put32le(image, 4, 0xeafffffd);       // b 0
    std::string why;
    SocIMX1* soc = SocIMX1::Create(image.data(), image.size(), why);
    assert(soc);
    soc->Run(100000, 1000000);
    const uint64_t idle = soc->GetTime();
    soc->Run(100000, 1000000);
    assert(soc->GetTime() == idle);
    UsbHostBridge usb;
    std::string error;
    assert(usb.Start(soc, {HostSerial::Kind::pty, 0, {}}, error) && !soc->UsbCableAttached());
    for (int i = 0; i < 4 && !soc->UsbCableAttached(); ++i) { soc->Run(100000, 1000000); usb.Service(); }
    assert(soc->UsbCableAttached() && soc->GetTime() > idle);
    usb.Stop();
    assert(!soc->CradleAttached());
}

int main(int argc, char** argv) {
    // One SocIMX1 per process: the halted and waiting cases run as their own processes.
    if (argc > 1 && std::string(argv[1]) == "halted") {
        halted();
        std::puts("zodiac-battery halted: ok");
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "waiting") {
        waiting();
        std::puts("zodiac-battery waiting: ok");
        return 0;
    }
    curve();
    machine();
    std::puts("zodiac-battery: ok");
    return 0;
}
