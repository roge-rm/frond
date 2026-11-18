#ifndef FROND_ZODIAC_BATTERY_H
#define FROND_ZODIAC_BATTERY_H

#include <cstdint>
#include <cstdlib>

#include "BatteryPower.h"

// The Zodiac's battery as its ROM reads it. The DAL's battery read (ROM 20088588) takes bits 15..7 of an ASP U
// sample, a 9 bit count, and its voltage call (2008860e) turns that into hundredths of a volt with the default
// calibration (div 109, offset -242; Frond has no factory 'TTcf' record). One count is about 66 mV.
// The OS reads that through the DAL's Li-ion battery record (ROM d2260): warnings at 3.80 and 3.75 V,
// shutdown at 3.74 V, and the curve below for 0%, 10%, ... 100%.
constexpr uint16_t zodiacBatteryFullSample = 0x9b00;  // count 310, 4.13 V, the curve's 100%
constexpr uint16_t zodiacBatteryCurve[11] = {374, 376, 378, 380, 382, 384, 386, 390, 395, 405, 413};

constexpr int zodiacBatteryCentivolts(int count) {
    const int div = 109, offset = -242;
    if (count + offset < 0) return 0;
    const int scaled = ((count + offset) * 100 + div / 2) / div;
    return scaled * 10000 / 1500;
}

// The count whose voltage is nearest the curve's voltage for percent (the lower one on a tie), as the U
// sample. 100% gives zodiacBatteryFullSample exactly.
inline uint16_t zodiacBatterySample(unsigned percent) {
    const int wanted = int(BatteryPower::CurveCentivolts(zodiacBatteryCurve, percent));
    int best = 0;
    for (int count = 1; count < 512; ++count)
        if (std::abs(zodiacBatteryCentivolts(count) - wanted) < std::abs(zodiacBatteryCentivolts(best) - wanted))
            best = count;
    return uint16_t(best << 7);
}

#endif
