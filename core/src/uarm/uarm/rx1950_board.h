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

// What the steppingstone loader (block 0, not dumped) leaves in the pin setup, at power on and at a wake from
// sleep. ASSUMPTION "rx1950 loader pins": the LED pins GPA3, GPA4, GPA6, GPA7 and GPA11 and the speaker
// amplifier's GPA1 are outputs (the OAL's LED code and wavedev.dll only write GPADAT, and eboot's own pin table
// and the OAL's sleep table both have GPACON 0x5f8724) and GPB0 is TOUT0, the backlight PWM (the OAL's resume
// sets it so; nothing does at boot). GPC8 and GPC9, the Wi-Fi power switches, are outputs: tiacxwln.dll and
// eboot only write GPCDAT to power the chip, and the OAL's sleep table has GPCCON 0x55555555.
inline void rx1950LoaderPins(S3cGpio& gpio) {
    gpio.con[S3cGpio::a] &= ~((1u << 1) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 7) | (1u << 11));
    gpio.con[S3cGpio::b] = (gpio.con[S3cGpio::b] & ~3u) | 2;
    gpio.con[S3cGpio::c] = (gpio.con[S3cGpio::c] & ~0xf0000u) | 0x50000;
}

// The RTC at boot: Sunday January 1 2006, 12:00. The OAL reads BCDYEAR as a binary count of years from 1950
// (wYear = BCDYEAR + 1950, nk 0x8004abb0) and BCDDAY as day of week + 1 (Sunday 1); the other fields are BCD.
constexpr uint8_t rx1950RtcYear(unsigned year) { return uint8_t(year - 1950); }
constexpr S3cRtc::Time rx1950RtcStart{rx1950RtcYear(2006), 1, 1, 1, 12, 0, 0};

// The SD slot (SDHC_SC2410.dll's registry and code): card detect on GPF5 (EINT5), low with a card in; write
// protect on GPH8, which the driver reads as protected when high (as Linux's mach-rx1950.c does).
// ASSUMPTION "rx1950 SD write protect": GPH8 is high only with a protected card in. ASSUMPTION "rx1950 no
// SDHC": WM5 5.1.1702's sdbus.dll has no high capacity path, so cards stop at 2 GB.
constexpr S3cGpio::Port rx1950CardDetectPort = S3cGpio::f, rx1950WriteProtectPort = S3cGpio::h;
constexpr unsigned rx1950CardDetectBit = 5, rx1950WriteProtectBit = 8;
constexpr size_t rx1950MaxCardSectors = size_t(2048) << 11;

// The serial cable sense on GPF6, high with the cable attached. eboot only sets up its debug serial port then
// (GPFDAT read at 0x8c0a61cc), and with it the OS's ActiveSync looks for a PC on UART0 every second.
// ASSUMPTION "rx1950 serial cable sense": high means attached, as eboot and the OS both stay quiet on UART0
// with it low. The device boots with no cable.
constexpr S3cGpio::Port rx1950CablePort = S3cGpio::f;
constexpr unsigned rx1950CableBit = 6;

#endif
