#ifndef _SPEED_SETTING5_H_
#define _SPEED_SETTING5_H_

#include <cstdint>
#include <string>

// A machine's speed: Original (the device's own clock), Faster (the device's clock times a multiplier) or
// Uncapped. Faster keeps guest time, timers and sound as at Original and runs the CPU num / den times as
// fast, so the guest gets that many times the instructions per guest second.
struct SpeedSetting5 {
    enum class Kind { original, faster, uncapped };
    Kind kind{Kind::original};
    uint32_t num{1}, den{1};  // the multiplier, 1 / 1 unless Faster

    bool operator==(const SpeedSetting5& other) const {
        return kind == other.kind && num == other.num && den == other.den;
    }
    bool operator!=(const SpeedSetting5& other) const { return !(*this == other); }
    double Multiplier() const { return double(num) / den; }

    // "original", "uncapped" or the multiplier as "1.5x", "2x" and so on.
    std::string Name() const {
        if (kind == Kind::original) return "original";
        if (kind == Kind::uncapped) return "uncapped";
        if (den == 1) return std::to_string(num) + "x";
        return std::to_string(num / den) + "." + std::to_string((num % den) * 10 / den) + "x";
    }

    static SpeedSetting5 Original() { return {}; }
    static SpeedSetting5 Uncapped() { return {Kind::uncapped, 1, 1}; }
    // The multipliers Faster offers, 2x by default.
    static SpeedSetting5 Faster(uint32_t num = 2, uint32_t den = 1) { return {Kind::faster, num, den}; }

    // Parses original, uncapped, or one of the Faster multipliers: 1.5x, 2x, 3x, 4x, 8x.
    static bool Parse(const std::string& text, SpeedSetting5& out) {
        if (text == "original") { out = Original(); return true; }
        if (text == "uncapped") { out = Uncapped(); return true; }
        static const struct { const char* name; uint32_t num, den; } multipliers[] = {
            {"1.5x", 3, 2}, {"2x", 2, 1}, {"3x", 3, 1}, {"4x", 4, 1}, {"8x", 8, 1}};
        for (const auto& m : multipliers)
            if (text == m.name) { out = Faster(m.num, m.den); return true; }
        return false;
    }
    static const char* Choices() { return "original|1.5x|2x|3x|4x|8x|uncapped"; }
};

#endif  // _SPEED_SETTING5_H_
