#ifndef FROND_PXA_BATTERY_H
#define FROND_PXA_BATTERY_H

#include <cstdint>

// The battery percent each PXA board's DAL works out from its battery ADC count, and its inverse for the battery
// setting. The E2, TX and Zire 72 read WM9712L BMON (ADC channel 6, 12 bits, 3.3 V full scale behind a divide by
// three); the TG50 reads AD7873 VBAT (12 bits, 2.5 V reference behind a divide by four). Each DAL filters the count
// and steps the shown percent towards the result, so a steady count settles on these percents.
//
// The TX, E2 and Zire 72 clamp the count to a calibration range from the 'adcc' ROM token ({centivolts, count} at
// two points; low count at +2, high at +6), else DAL defaults:
// - TX (DAL 20099db8): token in the NAND, 355 cV = 1491, 400 cV = 1682 (default 1446..1629).
// - E2 (DAL 2009eec8): token in the NAND, 340 cV = 1400, 400 cV = 1641 (default 1407..1647).
// - Zire 72 (DAL 200998a4): no token in the ROM, so its defaults 1509..1695.

enum class PxaBatteryBoard { tx, e2, zire72, tg50 };

// The counts every board read before the battery curves: 4200 mV, above each DAL's 100%. Maximum keeps them.
constexpr uint16_t pxaBatteryFullBmon = 1737;
constexpr uint16_t pxaBatteryFullVbat = 1720;

// TX (DAL 20099824, 2009eb1c) and Zire 72 (DAL 20099998, 200a59d0): a table of 11 counts for 0%, 10% ... 100%,
// the low end, low + f * (high - low) / 100 for each factor f, then the high end. The percent is
// 10 * i + (10 * (count - t[i]) + d / 2) / d between t[i] and t[i + 1], d = t[i + 1] - t[i].
struct PxaBatteryTable {
    uint16_t low, high;
    uint8_t factor[9];
};
constexpr PxaBatteryTable pxaBatteryTx{1491, 1682, {13, 22, 29, 33, 38, 47, 56, 69, 84}};
constexpr PxaBatteryTable pxaBatteryZire72{1509, 1695, {9, 16, 22, 29, 35, 47, 58, 71, 85}};

constexpr unsigned pxaBatteryTablePercent(const PxaBatteryTable& table, unsigned count) {
    if (count > table.high) count = table.high;
    if (count < table.low) count = table.low;
    uint32_t t[11]{};
    t[0] = table.low;
    for (unsigned i = 1; i < 10; ++i) t[i] = table.low + table.factor[i - 1] * uint32_t(table.high - table.low) / 100;
    t[10] = table.high;
    if (count <= t[0]) return 0;
    if (count >= t[10]) return 100;
    unsigned i = 0;
    while (count >= t[i + 1]) ++i;
    const uint32_t d = t[i + 1] - t[i];
    return 10 * i + (10 * (count - t[i]) + d / 2) / d;
}

// E2 (DAL 200a5fc8, 200a6162): the clamped count scaled to a level 0-255. On external power (GPIO9 high, state
// byte +0x9c) the DAL takes off for the charge current: level = 255 - min(255, ((255 - level) * 1150 + 500) / 1000).
// Then mV = 3400 + level * 600 / 255 on its curve (DAL 200baf8c, millivolts for 0%, 10% ... 100%).
constexpr uint16_t pxaBatteryE2Low = 1400, pxaBatteryE2High = 1641;
constexpr uint16_t pxaBatteryE2Curve[11] = {3400, 3578, 3632, 3670, 3688, 3703, 3745, 3785, 3840, 3904, 4000};

constexpr unsigned pxaBatteryE2Percent(unsigned count, bool externalPower) {
    if (count > pxaBatteryE2High) count = pxaBatteryE2High;
    if (count < pxaBatteryE2Low) count = pxaBatteryE2Low;
    unsigned level = (count - pxaBatteryE2Low) * 255 / (pxaBatteryE2High - pxaBatteryE2Low);
    if (externalPower) {
        const unsigned drop = ((255 - level) * 1150 + 500) / 1000;
        level = 255 - (drop > 255 ? 255 : drop);
    }
    const unsigned mV = 3400 + level * 600 / 255;
    for (unsigned i = 0; i < 10; ++i) {
        const unsigned low = pxaBatteryE2Curve[i], high = pxaBatteryE2Curve[i + 1];
        if (high > mV && low <= mV) return 10 * i + (10 * (mV - low) + (high - low) / 2) / (high - low);
    }
    return 100;
}

// TG50 (DAL 200233e8, 20022b98, 20022978): level = VBAT >> 3, then centivolts ((level + offset) * 100 + steps / 2)
// / steps from the DAL's Li-ion battery record (ROM 20049320: 51 steps per volt, offset 0, curve below for 0%,
// 10% ... 100%). ASSUMPTION "TG50 steps": no 'adcc' token (it would recompute steps and offset) and DAL state
// flag +0x4a bit 6 clear (it would set 85 steps).
constexpr uint16_t pxaBatteryTg50Curve[11] = {356, 361, 364, 366, 368, 370, 373, 378, 383, 389, 396};

constexpr unsigned pxaBatteryTg50Percent(unsigned count) {
    const unsigned centivolts = ((count >> 3) * 100 + 51 / 2) / 51;
    unsigned i = 10;
    while (i > 0 && pxaBatteryTg50Curve[i] > centivolts) --i;
    if (i >= 10) return 100;
    const unsigned low = pxaBatteryTg50Curve[i], high = pxaBatteryTg50Curve[i + 1];
    if (centivolts < low) return 0;
    return 10 * i + (10 * (centivolts - low) + (high - low) / 2) / (high - low);
}

constexpr unsigned pxaBatteryGuestPercent(PxaBatteryBoard board, unsigned count, bool externalPower) {
    switch (board) {
        case PxaBatteryBoard::tx: return pxaBatteryTablePercent(pxaBatteryTx, count);
        case PxaBatteryBoard::zire72: return pxaBatteryTablePercent(pxaBatteryZire72, count);
        case PxaBatteryBoard::e2: return pxaBatteryE2Percent(count, externalPower);
        case PxaBatteryBoard::tg50: return pxaBatteryTg50Percent(count);
    }
    return 100;
}

// The ADC count that reads as percent: the middle of the counts the DAL reads as exactly that percent, else the
// count of the nearest percent it can show (the lower one on a tie). 0% is the highest count that reads 0, at the
// bottom of the DAL's range; 100% is the full count above.
constexpr uint16_t pxaBatteryCount(PxaBatteryBoard board, unsigned percent, bool externalPower) {
    const bool tg50 = board == PxaBatteryBoard::tg50;
    if (percent >= 100) return tg50 ? pxaBatteryFullVbat : pxaBatteryFullBmon;
    unsigned first = 0, last = 0, bestCount = 0, bestDistance = 1000;
    bool found = false;
    for (unsigned count = 0; count < 4096; ++count) {
        const unsigned shown = pxaBatteryGuestPercent(board, count, externalPower);
        if (shown == percent) {
            if (!found) first = count;
            found = true;
            last = count;
        } else if (found) {
            break;
        }
        const unsigned distance = shown > percent ? shown - percent : percent - shown;
        if (distance < bestDistance) bestDistance = distance, bestCount = count;
    }
    if (!found) return uint16_t(bestCount);
    return uint16_t(percent == 0 ? last : (first + last) / 2);
}

#endif
