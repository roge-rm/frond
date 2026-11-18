#ifndef FROND_RX1950_BOARD_H
#define FROND_RX1950_BOARD_H

// The HP iPAQ rx1950's keys and touch panel as the S3C2442 sees them (doc R0 "Key GPIOs": keybddr, the
// OAL and Linux mach-rx1950.c agree on the pins).

#include <cstdint>
#include <cstring>

#include "s3c2442_blocks.h"

struct Rx1950Key {
    const char* name;
    S3cGpio::Port port;
    uint8_t bit;
};

// ASSUMPTION "rx1950 keys active low": a pressed key pulls its pin low, as Linux's gpio-keys table has it.
constexpr Rx1950Key rx1950Keys[] = {
    {"power", S3cGpio::f, 0},  {"record", S3cGpio::f, 7}, {"calendar", S3cGpio::g, 0},
    {"contacts", S3cGpio::g, 2}, {"inbox", S3cGpio::g, 3}, {"wireless", S3cGpio::g, 7},
    {"up", S3cGpio::g, 4},     {"down", S3cGpio::g, 6},   {"action", S3cGpio::g, 9},
    {"left", S3cGpio::g, 10},  {"right", S3cGpio::g, 11},
};

inline const Rx1950Key* rx1950Key(const char* name) {
    for (const auto& k : rx1950Keys)
        if (!std::strcmp(k.name, name)) return &k;
    return nullptr;
}

// Screen pixel to the touch panel's raw ADC reading. ASSUMPTION "rx1950 touch raw range": X grows left to
// right and Y top to bottom over 64-960 of the 10-bit range; nothing real is known, and the guest's
// calibration takes any linear mapping.
constexpr uint16_t rx1950TouchRawX(unsigned x) { return uint16_t(64 + x * 896 / 240); }
constexpr uint16_t rx1950TouchRawY(unsigned y) { return uint16_t(64 + y * 896 / 320); }

// The battery ADC inputs until R4 feeds the power state: battdrvr reads AIN0 as the battery voltage
// (AIN0 * 3.3 / 1023 * 1.3191 V; under 3.3 V it powers the device off), AIN2 as the pack thermistor
// (100 kOhm at 25 C against 127 kOhm to 3.3 V) and AIN3 as the pack ID (1650-1850 mV is one of its four
// packs). ASSUMPTION "rx1950 battery inputs": a 3.9 V battery at 25 C; AIN1 and the rest stay mid-scale.
struct Rx1950AdcInput { uint8_t channel; uint16_t value; };
constexpr Rx1950AdcInput rx1950AdcInputs[] = {{0, 917}, {2, 451}};

#endif
