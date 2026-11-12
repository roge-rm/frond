#include "imx1_rtc.h"

#include <cassert>
#include <limits>
#include <numeric>

namespace {
constexpr uint32_t base = 0x00204000, cycle = 512 * 24 * 60 * 60;
constexpr uint32_t masks[] = {0x1f3f, 0x3f, 0x1f3f, 0x3f, 0xe1, 0xffbf, 0xffbf, 0x3f, 0x1ff, 0x1ff};
uint32_t seconds(Imx1RtcTime t) { return ((t.day * 24 + t.hour) * 60 + t.minute) * 60 + t.second; }
uint32_t hourmin(uint32_t time) { return ((time / 3600) % 24) << 8 | ((time / 60) % 60); }
}

bool Imx1Rtc::validTime(Imx1RtcTime t) {
    return t.day < 512 && t.hour < 24 && t.minute < 60 && t.second < 60;
}
bool Imx1Rtc::initialise(Imx1RtcTime time) {
    if (used || known || !validTime(time)) return false;
    known = true;
    tod = seconds(time);
    return true;
}
const char* Imx1Rtc::registerName(uint32_t address) {
    if (address < base || address >= base + 40) return nullptr;
    constexpr const char* names[] = {"RTC.HOURMIN", "RTC.SECONDS", "RTC.ALRM_HM", "RTC.ALRM_SEC",
        "RTC.RCCTL", "RTC.RTCISR", "RTC.RTCIENR", "RTC.STPWCH", "RTC.DAYR", "RTC.DAYALARM"};
    return names[(address - base) / 4];
}
bool Imx1Rtc::referenceRatioSupported(uint64_t denominator) const {
    if (!denominator) return false;
    if (!known) return true;
    const auto factor = denominator / imx1Gcd(denominator, referenceDenominator);
    // Two proper fractions can then be added without overflowing uint64_t.
    return factor <= (std::numeric_limits<uint64_t>::max() / 2) / referenceDenominator;
}
void Imx1Rtc::advanceReference(uint64_t numerator, uint64_t denominator) {
    assert(referenceRatioSupported(denominator));
    if (!numerator) return;
    used = true;
    elapsed = true;
    if (!known) return;
    const auto common = referenceDenominator * (denominator / imx1Gcd(denominator, referenceDenominator));
    const auto fraction = referencePhase * (common / referenceDenominator) +
                          (numerator % denominator) * (common / denominator);
    referencePhase = fraction % common;
    referenceDenominator = common;
    advanceTicks(numerator / denominator + fraction / common);
}
uint64_t Imx1Rtc::idleQuietSteps(uint64_t numerator, uint64_t denominator) const {
    if (!known) return UINT64_MAX;
    if (prescaler >= divisor()) return 1;
    // The next second, or an enabled sampling period if that comes first.
    uint64_t ticks = divisor() - prescaler;
    for (unsigned bit = 8; bit < 16; ++bit) {
        const uint32_t period = 1u << (21 - bit);
        if ((enable & (1u << bit)) && period - samplingPhase % period < ticks) ticks = period - samplingPhase % period;
    }
    return imx1StepsUntil(referencePhase, referenceDenominator, numerator, denominator, ticks);
}
void Imx1Rtc::advanceTicks(uint64_t ticks) {
    if (!ticks) return;
    for (unsigned bit = 8; bit < 16; ++bit) {
        const uint32_t period = 1u << (21 - bit);
        if (ticks >= period || samplingPhase % period + ticks >= period) status |= enable & (1u << bit);
    }
    samplingPhase = (samplingPhase + ticks % 8192) % 8192;
    const auto count = uint64_t(prescaler) + ticks % divisor();
    const auto whole = ticks / divisor() + count / divisor();
    prescaler = count % divisor();
    if (!whole) return;
    status |= enable & 0x10;
    if (whole >= 60 || tod % 60 + whole >= 60) status |= enable & 2;
    if (whole >= 3600 || tod % 3600 + whole >= 3600) status |= enable & 0x20;
    if (whole >= 86400 || tod % 86400 + whole >= 86400) status |= enable & 8;
    const uint32_t untilAlarm = (alarm + cycle - tod) % cycle;
    if (whole >= (untilAlarm ? untilAlarm : cycle)) status |= enable & 4;
    tod = (tod + whole % cycle) % cycle;
}
uint64_t Imx1Rtc::assertedInterrupts() const {
    // Figure 23-1 and table 10-1 separate sampling requests from the other RTC events.
    return ((status & enable & 0x003f) ? uint64_t(1) << 17 : 0) |
           ((status & enable & 0xff00) ? uint64_t(1) << 18 : 0);
}
bool Imx1Rtc::preservesWatchdogClock(const Imx1Rtc& next, uint32_t address) const {
    // Alarm, status and interrupt masks are separate from the prescaler (figure 23-1).
    // Counter writes, changed control and the exceptional 2HZ gate retain their guards.
    if (address != base + 8 && address != base + 12 && address != base + 16 && address != base + 20 &&
        address != base + 24 && address != base + 36) return false;
    return next.watchdogClockBoundSupported() && control == next.control &&
        ((enable ^ next.enable) & 0x80) == 0 && prescaler == next.prescaler &&
        samplingPhase == next.samplingPhase && referencePhase == next.referencePhase &&
        referenceDenominator == next.referenceDenominator;
}
Imx1RtcAccess Imx1Rtc::access(uint32_t address, uint32_t size, bool write, uint32_t& value) {
    if (!registerName(address)) return Imx1RtcAccess::unmapped;
    if (size != 4 || (address & 3)) return Imx1RtcAccess::width;
    used = true;
    const unsigned index = (address - base) / 4;
    if (!write) {
        switch (index) {
            case 0: case 1: case 8:
                if (!known) return Imx1RtcAccess::externalInput;
                value = index == 0 ? hourmin(tod) : index == 1 ? tod % 60 : tod / 86400;
                break;
            case 2: value = hourmin(alarm); break;
            case 3: value = alarm % 60; break;
            case 4: value = control; break;
            case 5: value = status; break;
            case 6: value = enable; break;
            case 7: value = 0x3f; break;
            case 9: value = alarm / 86400; break;
        }
        return Imx1RtcAccess::ok;
    }
    if (value & ~masks[index]) return Imx1RtcAccess::reservedBits;
    if ((index == 0 || index == 2) && ((value >> 8) >= 24 || (value & 63) >= 60))
        return Imx1RtcAccess::reservedBits;
    if ((index == 1 || index == 3) && value >= 60) return Imx1RtcAccess::reservedBits;
    if (index == 4) {
        if (((value >> 5) & 3) == 2) return Imx1RtcAccess::reservedBits;
        // SWR, enable transitions and live prescaler retiming need separate qualification.
        if ((value & 1) || ((value ^ control) & 0x80)) return Imx1RtcAccess::sideEffect;
        const uint32_t nextDivisor = ((value >> 5) & 3) == 1 ? 32000 : 32768;
        if (elapsed && nextDivisor != divisor()) return Imx1RtcAccess::sideEffect;
        control = value;
    } else if (index == 5) status &= ~value;
    else if (index == 6) {
        // The 2HZ clock/status exception and minute stopwatch are outside this subset.
        if (value & 0x81) return Imx1RtcAccess::sideEffect;
        if (value && !known) return Imx1RtcAccess::externalInput;
        if ((value & 4) && !(enable & 4) && !(status & 4) && tod == alarm) return Imx1RtcAccess::sideEffect;
        enable = value;
    } else if (index == 7) return Imx1RtcAccess::sideEffect;
    else {
        const bool counter = index == 0 || index == 1 || index == 8;
        if (counter && !known) return Imx1RtcAccess::externalInput;
        uint32_t next = counter ? tod : alarm;
        if (index == 0 || index == 2) next = (next / 86400) * 86400 + (value >> 8) * 3600 + (value & 63) * 60 + next % 60;
        else if (index == 1 || index == 3) next = (next / 60) * 60 + value;
        else next = value * 86400 + next % 86400;
        // Software-induced equality has no qualified alarm-comparator timing here.
        if ((enable & 4) && (counter ? next == alarm : next == tod)) return Imx1RtcAccess::sideEffect;
        if (counter) tod = next;
        else alarm = next;
    }
    return Imx1RtcAccess::ok;
}
