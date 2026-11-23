// The battery setting: parsing and the replies, the level-to-voltage curves, each PXA DAL's percent as modelled in
// pxa_battery.h, and what the ARM boards' battery inputs read back through their real ADC models (WM9712L BMON on
// the E2, TX and Zire 72, AD7873 VBAT on the TG50).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "BatteryPower.h"
#include "ac97dev_WM9712L.h"
#include "pxa_battery.h"
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

    // A Palm OS sysBattVoltageCurve (hundredths of a volt at 0%, 10% ... 100%).
    const uint16_t curve[11] = {180, 200, 210, 220, 230, 240, 250, 260, 270, 280, 300};
    check(BatteryPower::CurveCentivolts(curve, 100) == 300, "100% is entry 10");
    check(BatteryPower::CurveCentivolts(curve, 0) == 180 && BatteryPower::CurveCentivolts(curve, 50) == 240,
          "entries at the tens");
    check(BatteryPower::CurveCentivolts(curve, 95) == 290 && BatteryPower::CurveCentivolts(curve, 5) == 190,
          "between entries");

    // Each DAL's table at its own points, from the live tables in a TX and a Zire 72 session's RAM.
    const unsigned txTable[11] = {1491, 1515, 1533, 1546, 1554, 1563, 1580, 1597, 1622, 1651, 1682};
    const unsigned zire72Table[11] = {1509, 1525, 1538, 1549, 1562, 1574, 1596, 1616, 1641, 1667, 1695};
    for (unsigned i = 0; i < 11; ++i) {
        check(pxaBatteryGuestPercent(PxaBatteryBoard::tx, txTable[i], false) == 10 * i, "TX table");
        check(pxaBatteryGuestPercent(PxaBatteryBoard::zire72, zire72Table[i], false) == 10 * i, "Zire 72 table");
    }
    // The tester's TX at 25%: the old lithium-ion guess (3805 mV, BMON 1574) read 56%.
    check(pxaBatteryGuestPercent(PxaBatteryBoard::tx, 1574, true) == 56, "old TX 25% read 56%");
    // E2 at the ends of its calibration, and its charger correction.
    check(pxaBatteryGuestPercent(PxaBatteryBoard::e2, 1400, false) == 0, "E2 empty");
    check(pxaBatteryGuestPercent(PxaBatteryBoard::e2, 1641, true) == 100, "E2 full on the charger");
    check(pxaBatteryGuestPercent(PxaBatteryBoard::e2, 1522, true) < pxaBatteryGuestPercent(PxaBatteryBoard::e2, 1522, false),
          "E2 reads lower on the charger");
    // TG50: level = VBAT >> 3, 51 steps per volt, so level 182 is 3.57 V.
    check(pxaBatteryGuestPercent(PxaBatteryBoard::tg50, 1455, false) == 0, "TG50 0%");
    check(pxaBatteryGuestPercent(PxaBatteryBoard::tg50, 202 << 3, false) == 100, "TG50 3.96 V is 100%");

    // Set percents read back as the same percent through each DAL's curve, on and off the charger. The TG50 has
    // about one count per 2% near the middle of its curve, so it gets the nearest percent it can show.
    for (auto board : {PxaBatteryBoard::tx, PxaBatteryBoard::e2, PxaBatteryBoard::zire72, PxaBatteryBoard::tg50})
        for (bool charger : {false, true})
            for (unsigned percent : {0u, 5u, 25u, 50u, 75u, 100u}) {
                const unsigned shown = pxaBatteryGuestPercent(board, pxaBatteryCount(board, percent, charger), charger);
                const unsigned off = shown > percent ? shown - percent : percent - shown;
                check(board == PxaBatteryBoard::tg50 ? off <= 3 : off == 0, "set percent reads back");
            }
    for (unsigned percent = 0; percent <= 100; ++percent)
        for (bool charger : {false, true})
            for (auto board : {PxaBatteryBoard::tx, PxaBatteryBoard::zire72}) {
                const unsigned shown = pxaBatteryGuestPercent(board, pxaBatteryCount(board, percent, charger), charger);
                check((shown > percent ? shown - percent : percent - shown) <= 1, "every percent within 1");
            }
    // A percent the DAL can't show gets the count of the nearest one it can, never a count below the empty one.
    // The TG50 shows only 25, 35 and 45 between 20% and 50% (one centivolt per 8 counts).
    for (auto board : {PxaBatteryBoard::tx, PxaBatteryBoard::e2, PxaBatteryBoard::zire72, PxaBatteryBoard::tg50})
        for (bool charger : {false, true})
            for (unsigned percent = 1; percent <= 100; ++percent) {
                const unsigned count = pxaBatteryCount(board, percent, charger);
                const unsigned shown = pxaBatteryGuestPercent(board, count, charger);
                check(count >= pxaBatteryCount(board, 0, charger), "no count below empty");
                check((shown > percent ? shown - percent : percent - shown) <= (board == PxaBatteryBoard::tg50 ? 5u : 2u),
                      "every percent within 2, TG50 5");
            }
    check(pxaBatteryCount(PxaBatteryBoard::tx, 100, true) == pxaBatteryFullBmon &&
              pxaBatteryCount(PxaBatteryBoard::tg50, 100, false) == pxaBatteryFullVbat,
          "Maximum keeps the old full counts");

    // WM9712L BMON is behind a divide by three, 3.3 V full scale: 4200 mV reads 1737, the full count.
    auto* wm = wm9712LInit(nullptr, nullptr, -1, 27);
    check(wm != nullptr, "codec");
    wm9712LsetAuxVoltage(wm, WM9712LauxPinBmon, 4200 / 3);
    check(readBmon(wm) == pxaBatteryFullBmon, "BMON full");
    wm9712LsetAuxCount(wm, WM9712LauxPinBmon, pxaBatteryCount(PxaBatteryBoard::tx, 25, false));
    check(pxaBatteryGuestPercent(PxaBatteryBoard::tx, readBmon(wm), false) == 25, "BMON count reads back");

    // AD7873 VBAT has a divide by four into a 2.5 V reference.
    auto* ad = ad7873Init(nullptr, nullptr, -1);
    check(ad != nullptr && sspProc, "AD7873");
    ad7873setVbatt(ad, 4200);
    check(readVbat() == pxaBatteryFullVbat, "VBAT full");
    ad7873setVbattCount(ad, pxaBatteryCount(PxaBatteryBoard::tg50, 25, false));
    check(pxaBatteryGuestPercent(PxaBatteryBoard::tg50, readVbat(), false) == 25, "VBAT count reads back");

    std::puts("battery power: ok");
    return 0;
}
