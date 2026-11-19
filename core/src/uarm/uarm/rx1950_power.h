#ifndef FROND_RX1950_POWER_H
#define FROND_RX1950_POWER_H

// The rx1950's battery and charger inputs from the machine's battery setting (BatteryPower.h). R5's SoC
// calls rx1950ApplyBattery from SetBattery and again after a reset or a state load; rx1950BatteryPins sets
// the same pins in the options before the machine is made, so a boot on the charger sees no AC edge.
//
// battdrvr.dll (REA on the IMGFS module) reads every 10 s:
// - AIN0, the battery: mV = AIN0 * 3.3 / 1023 * 1.3191 * 1000; under 3300 mV it powers the device off.
// - AIN1, the charge or load current; at 0 it takes none and leaves the voltage uncorrected.
// - AIN2, the pack thermistor (100 kOhm at 25 C against 127 kOhm to 3.3 V).
// - AIN3, the pack ID, mV = AIN3 * 3.3 / 1023 * 1000 binned as 500-700, 750-950, 1000-1200 (1100 mAh packs,
//   0 to 2) and 1650-1850 (1800 mAh, pack 3).
// - GPF2 (EINT2, both edges): low with AC in. GPF3: high when the charger says the charge is done; the driver
//   then turns the charger off (GPJ2, GPJ3 low) and shows 100%.
// - GPG1 (EINT9): high with a battery in; left high.
// Percent on battery, pack 0: 0 at 3580 mV, 5 at 3650, 25 at 3730, 50 at 3800, 100 at 4120, straight in
// between (with AIN1 0, so no current correction).

#include <cstdint>

#include "BatteryPower.h"
#include "rx1950_board.h"
#include "s3c2442_machine.h"

constexpr S3cGpio::Port rx1950AcPort = S3cGpio::f;
constexpr unsigned rx1950AcBit = 2, rx1950ChargeDoneBit = 3;

// ASSUMPTION "rx1950 pack": the standard 1100 mAh pack, pack ID 0 (600 mV on AIN3).
constexpr uint16_t rx1950PackIdCount = 186;
// ASSUMPTION "rx1950 battery current": AIN1 reads 0, no current.
constexpr uint16_t rx1950CurrentCount = 0;
// The thermistor at 25 C.
constexpr uint16_t rx1950ThermistorCount = 451;

// battdrvr's percent for pack 0 from the battery in mV, as the guest works it out.
constexpr unsigned rx1950GuestPercent(unsigned mv) {
    if (mv <= 3580) return 0;
    if (mv < 3650) return (mv - 3580) * 5 / 70;
    if (mv < 3730) return 5 + (mv - 3650) * 20 / 80;
    if (mv < 3800) return 25 + (mv - 3730) * 25 / 70;
    const unsigned p = 50 + (mv - 3800) * 50 / 320;
    return p > 100 ? 100 : p;
}

// The battery mV battdrvr reads as percent: the middle of that percent's band, 4120 at 100.
constexpr unsigned rx1950BatteryMilliVolts(unsigned percent) {
    struct Knee { unsigned percent, mv; };
    constexpr Knee knees[] = {{0, 3580}, {5, 3650}, {25, 3730}, {50, 3800}, {100, 4120}};
    if (percent >= 100) return 4120;
    unsigned i = 0;
    while (percent >= knees[i + 1].percent) ++i;
    const unsigned span = knees[i + 1].percent - knees[i].percent, rise = knees[i + 1].mv - knees[i].mv;
    // Twice the mV, so the half step stays exact.
    const unsigned twice = 2 * knees[i].mv + (2 * (percent - knees[i].percent) + 1) * rise / span;
    return (twice + 1) / 2;
}

// AIN0 for a battery voltage in mV, the nearest count.
constexpr uint16_t rx1950BatteryCount(unsigned mv) {
    // count = mv * 1023 / (3300 * 1.3191), in integers: 3300 * 1.3191 = 4353.03.
    return uint16_t((uint64_t(mv) * 1023 * 100 + 435303 / 2) / 435303);
}

// The battery mV battdrvr works out from AIN0, rounded down as its float to integer conversion does.
constexpr unsigned rx1950CountMilliVolts(uint16_t count) { return unsigned(uint64_t(count) * 435303 / 102300); }

// AIN0 for percent: the count nearest the middle of its band, moved a count at a time while battdrvr would
// read another percent. A count is 4.26 mV, more than a percent between 5 and 50%, so some percents have no
// count (18, 26, 29, 32, 35, 38, 41, 44 and 47); those get the next count up, which reads the percent above.
constexpr uint16_t rx1950BatteryAin0(unsigned percent) {
    if (percent > 100) percent = 100;
    uint16_t count = rx1950BatteryCount(rx1950BatteryMilliVolts(percent));
    for (unsigned i = 0; i < 4 && rx1950GuestPercent(rx1950CountMilliVolts(count)) < percent; ++i) ++count;
    for (unsigned i = 0; i < 4 && rx1950GuestPercent(rx1950CountMilliVolts(count)) > percent; ++i) --count;
    if (rx1950GuestPercent(rx1950CountMilliVolts(count)) < percent) ++count;
    return count;
}

struct Rx1950PowerInputs {
    uint16_t adc[4];
    bool acLow;        // GPF2 low: AC in
    bool chargeDone;   // GPF3 high
};

inline Rx1950PowerInputs rx1950PowerInputs(const BatteryPower& battery) {
    Rx1950PowerInputs in{};
    in.adc[0] = rx1950BatteryAin0(battery.Percent());
    in.adc[1] = rx1950CurrentCount;
    in.adc[2] = rx1950ThermistorCount;
    in.adc[3] = rx1950PackIdCount;
    in.acLow = battery.ExternalPower();
    // ASSUMPTION "rx1950 charge done off the charger": GPF3 reads high with no AC, as every pin did before.
    in.chargeDone = !battery.charging || battery.Percent() >= 100;
    return in;
}

// The pins in the options, before the machine is made.
inline void rx1950BatteryPins(const BatteryPower& battery, S3c2442Options& options) {
    const auto in = rx1950PowerInputs(battery);
    uint32_t& f = options.pinLevels[rx1950AcPort];
    f = in.acLow ? f & ~(1u << rx1950AcBit) : f | (1u << rx1950AcBit);
    f = in.chargeDone ? f | (1u << rx1950ChargeDoneBit) : f & ~(1u << rx1950ChargeDoneBit);
}

// The ADC inputs and pins on a running machine; a change of AC raises EINT2 for the next run.
inline void rx1950ApplyBattery(S3c2442Machine& machine, const BatteryPower& battery) {
    const auto in = rx1950PowerInputs(battery);
    for (unsigned c = 0; c < 4; ++c) machine.adc.channel[c] = in.adc[c];
    machine.setPin(rx1950AcPort, rx1950ChargeDoneBit, in.chargeDone);
    machine.setPin(rx1950AcPort, rx1950AcBit, !in.acLow);
}

// The LEDs (the OAL's LED call, nk 0x800529c8): GPA6 and GPA7 together are the solid "charged" light; GPA3
// and GPA4 with GPJ6 pick the hardware blinker, GPA3 + GPA4 while charging and GPA4 alone for a notification;
// GPA11 is the blue (wireless) LED. ASSUMPTION "rx1950 LED colours": GPA6 and GPA4 green, GPA7 and GPA3 red
// (amber with both), all active high as in Linux mach-rx1950.c. The pins keep their level asleep.
struct Rx1950Leds {
    bool green, red, blue, blinking;
    bool any() const { return green || red || blue; }
};

inline Rx1950Leds rx1950Leds(const S3c2442Machine& machine) {
    const uint32_t a = machine.gpio.dat[S3cGpio::a] & ~machine.gpio.con[S3cGpio::a];
    const bool blink = machine.gpio.dat[S3cGpio::j] & (1u << 6);
    const bool green = (a & (1u << 6)) || (blink && (a & (1u << 4)));
    const bool red = (a & (1u << 7)) || (blink && (a & (1u << 3)));
    return {green, red, bool(a & (1u << 11)), blink && (a & 0x18)};
}

// SoC::Attention's alarm LED: the notification blink (GPA4 alone with GPJ6). No vibrate motor.
inline bool rx1950NotificationLed(const S3c2442Machine& machine) {
    const uint32_t a = machine.gpio.dat[S3cGpio::a] & ~machine.gpio.con[S3cGpio::a];
    return (machine.gpio.dat[S3cGpio::j] & (1u << 6)) && (a & 0x18) == 0x10;
}

// The battery-state reply: the rx1950 takes charging, so no " no-charging".
inline std::string rx1950BatteryState(const BatteryPower& battery) { return battery.Describe(); }

#endif
