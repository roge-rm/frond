#ifndef FROND_BATTERY_POWER_H
#define FROND_BATTERY_POWER_H

#include <cstdint>
#include <sstream>
#include <string>

// The machine's battery as the user sets it: Maximum, a set level, or the host's own battery (the frontend
// passes the host's level and charging state in). charging means on external power (the charger); the default is a
// full battery on the charger, also for a spec with no charging or battery word. A full battery on the charger has
// finished charging: ChargeInProgress() is false there, as a real charger stops at full.
// It's a machine setting: kept by the frontend, never in a session file.
struct BatteryPower {
    enum class Mode { maximum, level, host };
    Mode mode = Mode::maximum;
    unsigned percent = 100;  // 0-100, ignored at Maximum
    bool charging = true;

    unsigned Percent() const { return mode == Mode::maximum ? 100 : (percent > 100 ? 100 : percent); }
    bool ExternalPower() const { return charging; }
    // On the charger and still charging, for boards with a separate charge status pin.
    bool ChargeInProgress() const { return charging && Percent() < 100; }

    bool operator==(const BatteryPower& o) const {
        return mode == o.mode && Percent() == o.Percent() && charging == o.charging;
    }
    bool operator!=(const BatteryPower& o) const { return !(*this == o); }

    // "max [charging|battery]", "level N [charging|battery]" or "host N [charging|battery]", N 0-100.
    static bool Parse(const std::string& text, BatteryPower& out) {
        std::istringstream words(text);
        std::string mode, word;
        if (!(words >> mode)) return false;
        BatteryPower parsed;
        if (mode == "max") {
            parsed.mode = Mode::maximum;
        } else if (mode == "level" || mode == "host") {
            parsed.mode = mode == "level" ? Mode::level : Mode::host;
            if (!(words >> word) || word.empty() || word.size() > 3 ||
                word.find_first_not_of("0123456789") != std::string::npos)
                return false;
            const unsigned value = unsigned(std::stoul(word));
            if (value > 100) return false;
            parsed.percent = value;
        } else {
            return false;
        }
        if (words >> word) {
            if (word != "charging" && word != "battery") return false;
            parsed.charging = word == "charging";
        }
        if (words >> word) return false;
        out = parsed;
        return true;
    }

    std::string Describe() const {
        const char* name = mode == Mode::maximum ? "max" : mode == Mode::level ? "level" : "host";
        return std::string(name) + " " + std::to_string(Percent()) + (charging ? " charging" : " battery");
    }

    // A lithium-ion cell: 4200 mV full, 3700 nearly empty (5%), 3600 where the Palm shuts down (0%).
    static uint32_t LiIonMilliVolts(unsigned percent) {
        if (percent >= 100) return 4200;
        if (percent <= 5) return 3600 + percent * 20;
        return 3700 + (percent - 5) * 500 / 95;
    }

    // Palm OS sysBattVoltageCurve: 11 voltages in hundredths of a volt for 0%, 10%, ... 100%.
    // The voltage between two entries for percent, so 100% gives entry 10 exactly.
    static uint32_t CurveCentivolts(const uint16_t (&curve)[11], unsigned percent) {
        if (percent >= 100) return curve[10];
        const unsigned index = percent / 10, rest = percent % 10;
        const int low = curve[index], high = curve[index + 1];
        return uint32_t(low + (high - low) * int(rest) / 10);
    }
};

#endif
