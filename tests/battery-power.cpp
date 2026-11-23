// The battery setting: parsing and the replies, the level-to-voltage curves, and what the ARM boards' battery
// inputs read back through their real ADC models (WM9712L BMON on the E2, TX and Zire 72, AD7873 VBAT on the TG50).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "BatteryPower.h"
#include "ac97dev_WM9712L.h"
#include "sspdev_AD7873.h"

static void check(bool success, const char* what) {
    if (!success) {
        std::fprintf(stderr, "FAIL: battery power: %s\n", what);
        std::abort();
    }
}

static Ac97CodecRegR readRegister;
static Ac97CodecRegW writeRegister;
static void* codec;
static SspClientProcF sspProc;
static void* sspContext;

void pxaAC97clientAdd(PxaAC97*, Ac97Codec which, Ac97CodecRegR read, Ac97CodecRegW write, void* context) {
    if (which == Ac97PrimaryAudio) readRegister = read, writeRegister = write, codec = context;
}
bool pxaAC97clientClientWantData(PxaAC97*, Ac97Codec, uint32_t*) { return false; }
void pxaAC97clientClientHaveData(PxaAC97*, Ac97Codec, uint32_t) {}
void audioQueuePush(AudioQueue*, uint32_t) {}
void pxaGpioSetState(PxaGpio*, uint_fast8_t, bool) {}
bool pxaSspAddClient(PxaSsp*, SspClientProcF proc, void* context) {
    sspProc = proc, sspContext = context;
    return true;
}
extern "C" void uarmAbort() { std::abort(); }

// One BMON conversion through the codec's digitiser registers, as the DAL polls it.
static uint16_t readBmon(WM9712L* wm) {
    check(writeRegister(codec, 0x76, 0x8000 | (6 << 12)), "poll BMON");
    wm9712Lperiodic(wm);
    uint16_t value = 0;
    check(readRegister(codec, 0x7a, &value), "read digitiser result");
    return value & 0xfff;
}

static uint16_t readVbat() {
    sspProc(sspContext, 12, 0xA6);
    return uint16_t(sspProc(sspContext, 12, 0));
}

int main() {
    BatteryPower b;
    check(b.mode == BatteryPower::Mode::maximum && b.Percent() == 100 && b.charging && b.ExternalPower() &&
              !b.ChargeInProgress(),
          "default is Maximum on the charger, charge done");
    check(b.Describe() == "max 100 charging", "default reply");
    check(BatteryPower::Parse("max", b) && b == BatteryPower{}, "no word is the default, on the charger");
    check(BatteryPower::Parse("level 20 charging", b) && b.mode == BatteryPower::Mode::level && b.Percent() == 20 &&
              b.charging && b.ExternalPower() && b.ChargeInProgress(),
          "level and charging");
    check(b.Describe() == "level 20 charging", "level reply");
    check(BatteryPower::Parse("host 57 battery", b) && b.mode == BatteryPower::Mode::host && b.Percent() == 57 &&
              !b.charging && !b.ChargeInProgress(),
          "host level");
    check(BatteryPower::Parse("max charging", b) && b.Percent() == 100 && b.charging, "max charging");
    const BatteryPower kept = b;
    for (const char* bad : {"", "level", "level 101", "level -1", "level 2x", "max 50", "host 5 maybe", "low",
                            "level 5 charging extra"})
        check(!BatteryPower::Parse(bad, b) && b == kept, bad);

    check(BatteryPower::LiIonMilliVolts(100) == 4200 && BatteryPower::LiIonMilliVolts(5) == 3700 &&
              BatteryPower::LiIonMilliVolts(0) == 3600,
          "lithium-ion curve end points");
    for (unsigned p = 1; p <= 100; ++p)
        check(BatteryPower::LiIonMilliVolts(p) > BatteryPower::LiIonMilliVolts(p - 1), "lithium-ion curve rises");

    // A Palm OS sysBattVoltageCurve (hundredths of a volt at 0%, 10% ... 100%).
    const uint16_t curve[11] = {180, 200, 210, 220, 230, 240, 250, 260, 270, 280, 300};
    check(BatteryPower::CurveCentivolts(curve, 100) == 300, "100% is entry 10");
    check(BatteryPower::CurveCentivolts(curve, 0) == 180 && BatteryPower::CurveCentivolts(curve, 50) == 240,
          "entries at the tens");
    check(BatteryPower::CurveCentivolts(curve, 95) == 290 && BatteryPower::CurveCentivolts(curve, 5) == 190,
          "between entries");

    // WM9712L BMON is behind a divide by three, 3.3 V full scale: 4200 mV reads 1737, today's full value.
    auto* wm = wm9712LInit(nullptr, nullptr, -1, 27);
    check(wm != nullptr, "codec");
    wm9712LsetAuxVoltage(wm, WM9712LauxPinBmon, BatteryPower::LiIonMilliVolts(100) / 3);
    check(readBmon(wm) == 1737, "BMON full");
    wm9712LsetAuxVoltage(wm, WM9712LauxPinBmon, BatteryPower::LiIonMilliVolts(5) / 3);
    check(readBmon(wm) == (3700 / 3 * 4095 + 1650) / 3300, "BMON nearly empty");
    wm9712LsetAuxVoltage(wm, WM9712LauxPinBmon, BatteryPower::LiIonMilliVolts(0) / 3);
    check(readBmon(wm) == (1200 * 4095 + 1650) / 3300, "BMON empty");

    // AD7873 VBAT has a divide by four into a 2.5 V reference.
    auto* ad = ad7873Init(nullptr, nullptr, -1);
    check(ad != nullptr && sspProc, "AD7873");
    ad7873setVbatt(ad, uint16_t(BatteryPower::LiIonMilliVolts(100)));
    check(readVbat() == 1050 * 4096 / 2500, "VBAT full");
    ad7873setVbatt(ad, uint16_t(BatteryPower::LiIonMilliVolts(0)));
    check(readVbat() == 900 * 4096 / 2500, "VBAT empty");

    std::puts("battery power: ok");
    return 0;
}
