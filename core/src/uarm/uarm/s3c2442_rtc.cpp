#include "s3c2442_blocks.h"

namespace {
    constexpr uint64_t second = 32768;
    // TICNT's count n gives a tick every (n + 1) / 128 seconds.
    constexpr uint64_t tickUnit = second / 128;

    unsigned fromBcd(uint32_t v) { return (v >> 4) * 10 + (v & 0xf); }
    uint32_t toBcd(unsigned v) { return ((v / 10) << 4) | (v % 10); }
    unsigned daysIn(unsigned month, unsigned year) {
        static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (month == 2 && year % 4 == 0) return 29;
        return month >= 1 && month <= 12 ? days[month - 1] : 31;
    }
    // BCD register order from BCDSEC at 0x70: second, minute, hour, date, day, month, year.
    enum { sec, min, hour, date, day, mon, year };
}

void S3cRtc::reset(const Time& start) {
    rtccon = 0;
    ticnt = 0;
    rtcalm = 0;
    rtcrst = 0;
    for (auto& a : alarm) a = 0;
    alarm[3] = alarm[4] = 1;  // ALMDATE and ALMMON start at 1
    bcd[sec] = toBcd(start.second);
    bcd[min] = toBcd(start.minute);
    bcd[hour] = toBcd(start.hour);
    bcd[date] = toBcd(start.date);
    bcd[day] = start.day;
    bcd[mon] = toBcd(start.month);
    bcd[year] = start.year;
    secondPhase = tickPhase = 0;
}

// Byte registers at word offsets (little endian), read and written whole.
S3cAccess S3cRtc::read(uint32_t offset, uint32_t& value) {
    switch (offset) {
        case 0x40: value = rtccon; return S3cAccess::ok;
        case 0x44: value = ticnt; return S3cAccess::ok;
        case 0x50: value = rtcalm; return S3cAccess::ok;
        case 0x6c: value = rtcrst; return S3cAccess::ok;
    }
    if (offset >= 0x54 && offset <= 0x68) { value = alarm[(offset - 0x54) / 4]; return S3cAccess::ok; }
    if (offset >= 0x70 && offset <= 0x88) { value = bcd[(offset - 0x70) / 4]; return S3cAccess::ok; }
    return S3cAccess::unknown;
}

S3cAccess S3cRtc::write(uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0x40: rtccon = value & 0xf; return S3cAccess::ok;
        case 0x44:
            // A new count starts its period over.
            if ((value ^ ticnt) & 0x7f) tickPhase = 0;
            ticnt = value & 0xff;
            return S3cAccess::ok;
        case 0x50: rtcalm = value & 0x7f; return S3cAccess::ok;
        // RTCRST, the round boundary reset: kept, but setting SRSTEN doesn't round the seconds yet.
        case 0x6c: rtcrst = value & 0xf; return S3cAccess::ok;
    }
    if (offset >= 0x54 && offset <= 0x68 && !((offset - 0x54) & 3)) {
        static const uint32_t masks[6] = {0x7f, 0x7f, 0x3f, 0x3f, 0x1f, 0xff};
        alarm[(offset - 0x54) / 4] = value & masks[(offset - 0x54) / 4];
        return S3cAccess::ok;
    }
    if (offset >= 0x70 && offset <= 0x88 && !(offset & 3)) {
        static const uint32_t masks[7] = {0x7f, 0x7f, 0x3f, 0x3f, 0x7, 0x1f, 0xff};
        // ASSUMPTION "BCD writes need RTCEN": the manual has software set RTCEN before writing the time.
        if (rtccon & 1) {
            bcd[(offset - 0x70) / 4] = value & masks[(offset - 0x70) / 4];
            if (offset == 0x70) secondPhase = 0;
        }
        return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}

void S3cRtc::secondPassed(S3cIntc& intc) {
    unsigned s = fromBcd(bcd[sec]) + 1, m = fromBcd(bcd[min]), h = fromBcd(bcd[hour]);
    unsigned d = fromBcd(bcd[date]), mo = fromBcd(bcd[mon]), y = fromBcd(bcd[year]), wd = bcd[day];
    if (s >= 60) { s = 0; ++m; }
    if (m >= 60) { m = 0; ++h; }
    if (h >= 24) {
        h = 0;
        wd = wd >= 7 ? 1 : wd + 1;
        if (++d > daysIn(mo, y)) {
            d = 1;
            // The year byte is only written when it counts on, so a board that keeps it binary keeps its value.
            if (++mo > 12) { mo = 1; bcd[year] = toBcd((y + 1) % 100); }
        }
    }
    bcd[sec] = toBcd(s); bcd[min] = toBcd(m); bcd[hour] = toBcd(h);
    bcd[date] = toBcd(d); bcd[mon] = toBcd(mo); bcd[day] = wd;
    // The alarm needs ALMEN and every field it has turned on to match.
    if (rtcalm & 0x40 && rtcalm & 0x3f) {
        static const int fields[6] = {sec, min, hour, date, mon, year};
        bool match = true;
        for (unsigned i = 0; i < 6; ++i)
            if (rtcalm & (1u << i) && alarm[i] != bcd[fields[i]]) match = false;
        if (match) intc.pulse(s3cIrq::rtc);
    }
}

void S3cRtc::advance(uint64_t ticks, S3cIntc& intc) {
    if (ticnt & 0x80) {
        const uint64_t period = ((ticnt & 0x7f) + 1) * tickUnit;
        const uint64_t total = tickPhase + ticks;
        if (total >= period) intc.pulse(s3cIrq::tick);
        tickPhase = total % period;
    }
    uint64_t total = secondPhase + ticks;
    // One second at a time, so the alarm sees each one.
    while (total >= second) {
        total -= second;
        secondPassed(intc);
    }
    secondPhase = total;
}

uint64_t S3cRtc::ticksToEvent(bool tick, bool alarmCheck) const {
    uint64_t best = UINT64_MAX;
    if (tick && ticnt & 0x80) best = ((ticnt & 0x7f) + 1) * tickUnit - tickPhase;
    if (alarmCheck && rtcalm & 0x40 && rtcalm & 0x3f && second - secondPhase < best) best = second - secondPhase;
    return best;
}
