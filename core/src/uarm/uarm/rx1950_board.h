#ifndef FROND_RX1950_BOARD_H
#define FROND_RX1950_BOARD_H

// The HP iPAQ rx1950's keys and touch panel as the S3C2442 sees them (doc R0 "Key GPIOs": keybddr, the
// OAL and Linux mach-rx1950.c agree on the pins).

#include <algorithm>
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

// Days from 1970-01-01 to a Gregorian date, and back (H. Hinnant's days_from_civil and civil_from_days).
constexpr int64_t rx1950DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}
struct Rx1950Date { int64_t year; unsigned month, date; };
constexpr Rx1950Date rx1950CivilFromDays(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return {int64_t(yoe) + era * 400 + (m <= 2), m, d};
}

// The RTC's time registers as seconds from 1970 (WM5's local time read as if UTC), false when they don't hold a
// time: BCD fields out of range, or a date the month doesn't have.
inline bool rx1950RtcSeconds(const S3cRtc& rtc, int64_t& seconds) {
    auto field = [](uint32_t v, unsigned low, unsigned high, unsigned& out) {
        if ((v & 0xf) > 9 || (v >> 4) > 9) return false;
        out = (v >> 4) * 10 + (v & 0xf);
        return out >= low && out <= high;
    };
    unsigned s, m, h, d, mo;
    if (!field(rtc.bcd[0], 0, 59, s) || !field(rtc.bcd[1], 0, 59, m) || !field(rtc.bcd[2], 0, 23, h) ||
        !field(rtc.bcd[3], 1, 31, d) || !field(rtc.bcd[5], 1, 12, mo))
        return false;
    const int64_t year = 1950 + int64_t(rtc.bcd[6] & 0xff);
    const int64_t days = rx1950DaysFromCivil(year, mo, d);
    if (rx1950CivilFromDays(days).date != d) return false;
    seconds = ((days * 24 + h) * 60 + m) * 60 + s;
    return true;
}

// Seconds from 1970 as the rx1950's RTC fields (rx1950RtcSeconds' other way); 1950 to 2205 only.
inline S3cRtc::Time rx1950RtcTimeAt(int64_t seconds) {
    const int64_t days = seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400;
    const int64_t rest = seconds - days * 86400;
    const auto c = rx1950CivilFromDays(days);
    // 1970-01-01 was a Thursday; WM5's day of week is Sunday 1.
    const unsigned weekday = unsigned(((days + 4) % 7 + 7) % 7);
    return {rx1950RtcYear(unsigned(c.year)), uint8_t(c.month), uint8_t(c.date), uint8_t(weekday + 1),
            uint8_t(rest / 3600), uint8_t(rest / 60 % 60), uint8_t(rest % 60)};
}

// The alarm registers as the RTC compares them, for each second of 1970 time in (from, to]: true when an enabled
// alarm (ALMEN and at least one field on) would have gone off. Days are walked one at a time and the times within a
// matching day only, so a long gap stays quick.
inline bool rx1950RtcAlarmBetween(const S3cRtc& rtc, int64_t from, int64_t to) {
    if (!(rtc.rtcalm & 0x40) || !(rtc.rtcalm & 0x3f) || to <= from) return false;
    auto bcd = [](unsigned v) { return uint32_t(((v / 10) << 4) | (v % 10)); };
    auto on = [&](unsigned field) { return (rtc.rtcalm >> field) & 1; };
    const int64_t firstDay = (from + 1) / 86400, lastDay = to / 86400;
    // Two hundred years of days at most; the year field can't count further.
    for (int64_t day = firstDay; day <= lastDay && day - firstDay < 73100; ++day) {
        const auto c = rx1950CivilFromDays(day);
        if ((on(3) && rtc.alarm[3] != bcd(c.date)) || (on(4) && rtc.alarm[4] != bcd(c.month)) ||
            (on(5) && rtc.alarm[5] != uint32_t(rx1950RtcYear(unsigned(c.year)))))
            continue;
        const int64_t dayStart = day * 86400;
        for (unsigned h = 0; h < 24; ++h) {
            if (on(2) && rtc.alarm[2] != bcd(h)) continue;
            for (unsigned m = 0; m < 60; ++m) {
                if (on(1) && rtc.alarm[1] != bcd(m)) continue;
                const int64_t minute = dayStart + (h * 60 + m) * 60;
                if (minute + 59 <= from || minute > to) continue;
                if (!on(0)) return true;
                for (unsigned s = 0; s < 60; ++s)
                    if (rtc.alarm[0] == bcd(s) && minute + s > from && minute + s <= to) return true;
            }
        }
    }
    return false;
}

// The RTC moved on by seconds, as it would have counted with the device asleep that long: a session opened again
// (SocS3C2442::ForwardClock). Only the time registers change, so a time the user set is kept and the second's phase,
// the tick and the alarm settings go on; an alarm that fell in that time is raised. False, with nothing changed,
// when the registers don't hold a time or the result would be past 2200.
inline bool rx1950RtcForward(S3cRtc& rtc, S3cIntc& intc, uint64_t seconds) {
    int64_t now;
    if (!rx1950RtcSeconds(rtc, now)) return false;
    constexpr int64_t last = 7289654399;  // 2200-12-31 23:59:59, inside the year byte
    if (seconds > uint64_t(last) || now + int64_t(seconds) > last) return false;
    const int64_t then = now + int64_t(seconds);
    const bool alarm = rx1950RtcAlarmBetween(rtc, now, then);
    rtc.setTime(rx1950RtcTimeAt(then));
    if (alarm) intc.pulse(s3cIrq::rtc);
    return true;
}

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

// The sync connector's serial port is UART0: with the cable sense high, ser2410's CRADLE2410 instance (COM1)
// sends ActiveSync's "CLIENT" probe there every second (R3). ASSUMPTION "rx1950 COM1 on UART0": the registry's
// IoBase 0x50004000 for CRADLE2410 isn't what the driver uses; the traffic decides.
constexpr unsigned rx1950SerialUart = 0;
// The infrared port is UART2 in IrDA mode (IRDA2410, COM2, IoBase 0x50008000).
// ASSUMPTION "rx1950 IR transceiver": the transceiver is powered while UART2 is in infrared mode (ULCON bit 6);
// no shutdown pin turned up in R0's GPIO list.
constexpr unsigned rx1950InfraredUart = 2;

#endif
