#ifndef FROND_ATTENTION_STATE_H
#define FROND_ATTENTION_STATE_H

#include <cstdint>
#include <string>

// The LED and vibrate motor as a guest drives them, the same for every device and runner. The guest steps any
// blink or buzz pattern itself, so this holds levels only; front ends debounce. Host side only: never saved
// in sessions and never part of a frame hash, as the ROM writes it again.
struct AttentionState {
    static constexpr uint8_t green = 1, red = 2, amber = green | red;
    uint8_t ledColour = 0;  // 0 off, green, red or amber
    bool vibrate = false;
    // A second on/off lamp output whose place on the device isn't known (the Zodiac's YMF768 LED pin). Reported,
    // not drawn.
    bool led = false;
    bool operator==(const AttentionState& o) const {
        return ledColour == o.ledColour && vibrate == o.vibrate && led == o.led;
    }
    bool operator!=(const AttentionState& o) const { return !(*this == o); }
    static const char* ColourName(uint8_t colour) {
        switch (colour & amber) {
            case green: return "green";
            case red: return "red";
            case amber: return "amber";
            default: return "off";
        }
    }
    // The desktop event's text: "led=green vibrate=1", with " lamp=1" while the second lamp is on.
    std::string Describe() const {
        std::string text = std::string("led=") + ColourName(ledColour) + " vibrate=" + (vibrate ? "1" : "0");
        if (led) text += " lamp=1";
        return text;
    }
};

// Hands a guest's attention on at most once per interval, so a low-battery fade or blink doesn't post an event
// and a repaint every few slices. A change held back goes out once the interval is over, so the last state always
// arrives. A vibrate change passes at once, so rumble keeps its timing.
class AttentionThrottle {
   public:
    static constexpr uint64_t intervalUsec = 100000;
    // The state to report now; changed when it differs from the one returned before.
    const AttentionState& Update(const AttentionState& latest, uint64_t nowUsec, bool& changed) {
        changed = false;
        if (latest == shown) return shown;
        if (latest.vibrate != shown.vibrate || !started || nowUsec - shownUsec >= intervalUsec) {
            shown = latest;
            shownUsec = nowUsec;
            started = changed = true;
        }
        return shown;
    }
    const AttentionState& Shown() const { return shown; }

   private:
    AttentionState shown{};
    uint64_t shownUsec = 0;
    bool started = false;
};

#endif
