#ifndef FROND_ZODIAC_LED_H
#define FROND_ZODIAC_LED_H

#include <cstdint>

// The Zodiac's power LED as its ROM drives it (LED driver 200977e8-20097b56). The PWM sets the brightness:
// level 3 on the charger (20097b56), full on with a fade or blink on a low battery (200977e8 and the 50 ms
// timer 20097932), reset and off with a good battery (20097b3a). 2009780e picks the colour on GPIO D18 and
// D19: mode 0 D18 alone (charging with A14 high), mode 2 both (charging with A14 low, or a low battery),
// mode 1 D19 alone (a lower battery), mode 3 neither. ASSUMPTION "D18 is the green die and D19 the red one of
// one two-colour LED, so both make amber, and A14 high means charge done".
constexpr uint8_t zodiacLedGreen = 1, zodiacLedRed = 2;

// Colour bits from port D's direction, in-use, data and OCR2 registers and whether the PWM drives a non-zero
// sample. A colour line counts only while it is a GPIO output from the data register, driven high.
inline uint8_t zodiacPowerLed(uint32_t direction, uint32_t inUse, uint32_t data, uint32_t ocr2, bool pwmDriving) {
    if (!pwmDriving) return 0;
    auto high = [&](unsigned pin) {
        const uint32_t bit = 1u << pin;
        return (direction & bit) && (inUse & bit) && ((ocr2 >> ((pin - 16) * 2)) & 3) == 3 && (data & bit);
    };
    return (high(18) ? zodiacLedGreen : 0) | (high(19) ? zodiacLedRed : 0);
}

#endif
