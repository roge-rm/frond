#ifndef FROND_SERIAL_PORTABLE_KEYBOARD_H
#define FROND_SERIAL_PORTABLE_KEYBOARD_H

// The Palm Portable Keyboard (Think Outside's Stowaway for Palm handhelds) on a serial port, after
// Think Outside's "Hardware and Electronics Reference for the Palm and Stowaway Portable Keyboards"
// 1.1. It sends at 9600 8N1 and has no transmit line from the Palm, only RTS in and the HotSync line
// out.
// - Powering up, or a key pressed while RTS is low, pulses the HotSync line. Without a rising edge
//   on RTS it pulses again, three pulses in all, then sleeps until RTS rises.
// - A rising edge on RTS gets the ID FA FD, then the codes of the keys held.
// - With RTS high a key down sends its matrix code (Y << 3 | X), a key up the code with bit 7 set,
//   twice when it was the last key held. Up to 6 key downs wait to be sent.
// - RTS low is low power. RTS high for 10 minutes without a key is low power too, until RTS goes
//   low and high again.
// The Fn layer is the driver's: Fn is a key like any other. Times are nanoseconds on a clock that
// never runs backwards. Single-threaded.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace frondKeyboard {

class PortableKeyboard {
   public:
    static constexpr uint8_t kIdFirst = 0xfa, kIdSecond = 0xfd;
    static constexpr uint8_t kRelease = 0x80;
    static constexpr long kBaud = 9600;
    // Matrix codes, Y << 3 | X, from the reference's key code matrix.
    static constexpr uint8_t kCmd = 0x08, kSpace = 0x17, kCapsLock = 0x18, kTab = 0x19, kCtrl = 0x1a, kFn = 0x22,
                             kAlt = 0x23, kBackspace = 0x32, kSpecial1 = 0x33, kSpace2 = 0x37, kSpecial2 = 0x3b,
                             kEnter = 0x41, kSpecial3 = 0x42, kUp = 0x49, kSpecial4 = 0x4a, kDone = 0x4f,
                             kDelete = 0x50, kLeft = 0x51, kDown = 0x52, kRight = 0x53, kLeftShift = 0x58,
                             kRightShift = 0x59;
    static constexpr int kPulses = 3;
    static constexpr size_t kBufferedKeys = 6;
    // The reference gives no time between unanswered pulses; one second is a guess.
    static constexpr uint64_t kPulseRetryNs = 1000000000ull;
    static constexpr uint64_t kIdleSleepNs = 600ull * 1000000000ull;

    // A code the matrix has a key for.
    static bool Valid(uint8_t code) {
        static constexpr std::array<uint8_t, 12> rows = {0xff, 0xff, 0xff, 0x07, 0x0c, 0xf0,
                                                          0xff, 0xff, 0xf7, 0xf7, 0x0f, 0x03};
        return code < 0x60 && (rows[code >> 3] >> (code & 7) & 1);
    }

    // The US character on a key, with Shift if it needs it. False for characters the keyboard has
    // no key for.
    static bool CharKey(char c, uint8_t& code, bool& shift) {
        struct Legend {
            uint8_t code;
            char plain, shifted;
        };
        static constexpr Legend legends[] = {
            {0x00, '1', '!'}, {0x01, '2', '@'}, {0x02, '3', '#'}, {0x03, 'z', 'Z'}, {0x04, '4', '$'},
            {0x05, '5', '%'}, {0x06, '6', '^'}, {0x07, '7', '&'}, {0x09, 'q', 'Q'}, {0x0a, 'w', 'W'},
            {0x0b, 'e', 'E'}, {0x0c, 'r', 'R'}, {0x0d, 't', 'T'}, {0x0e, 'y', 'Y'}, {0x0f, '`', '~'},
            {0x10, 'x', 'X'}, {0x11, 'a', 'A'}, {0x12, 's', 'S'}, {0x13, 'd', 'D'}, {0x14, 'f', 'F'},
            {0x15, 'g', 'G'}, {0x16, 'h', 'H'}, {0x17, ' ', 0},   {0x19, '\t', 0},  {0x2c, 'c', 'C'},
            {0x2d, 'v', 'V'}, {0x2e, 'b', 'B'}, {0x2f, 'n', 'N'}, {0x30, '-', '_'}, {0x31, '=', '+'},
            {0x32, '\b', 0},  {0x34, '8', '*'}, {0x35, '9', '('}, {0x36, '0', ')'}, {0x38, '[', '{'},
            {0x39, ']', '}'}, {0x3a, '\\', '|'}, {0x3c, 'u', 'U'}, {0x3d, 'i', 'I'}, {0x3e, 'o', 'O'},
            {0x3f, 'p', 'P'}, {0x40, '\'', '"'}, {0x41, '\n', 0},  {0x44, 'j', 'J'}, {0x45, 'k', 'K'},
            {0x46, 'l', 'L'}, {0x47, ';', ':'}, {0x48, '/', '?'}, {0x4c, 'm', 'M'}, {0x4d, ',', '<'},
            {0x4e, '.', '>'},
        };
        if (c == 0) return false;
        if (c == '\r') c = '\n';
        for (const auto& legend : legends) {
            if (legend.plain == c || legend.shifted == c) {
                code = legend.code;
                shift = legend.shifted == c;
                return true;
            }
        }
        return false;
    }

    // Plugged in: the keyboard powers up and pulses the HotSync line. rtsHigh is the Palm's RTS now.
    void Attach(bool rtsHigh, uint64_t now) {
        *this = PortableKeyboard();
        powered = true;
        rts = rtsHigh;
        StartPulses(now);
    }
    void Detach() { *this = PortableKeyboard(); }
    bool Attached() const { return powered; }
    // Listening: RTS rose since power up and the keyboard isn't in low power.
    bool Awake() const { return powered && awake && !idleSleep; }

    void SetRts(bool high, uint64_t now) {
        const bool rose = high && !rts;
        const bool fell = !high && rts;
        rts = high;
        if (!powered) return;
        if (rose) {
            pulses = 0;
            ignored = false;
            idleSleep = false;
            awake = true;
            lastKey = now;
            queue.clear();
            queue.push_back(kIdFirst);
            queue.push_back(kIdSecond);
            for (uint8_t code = 0; code < held.size(); ++code)
                if (held[code]) queue.push_back(code);
            ++ids;
        } else if (fell) {
            awake = false;
            queue.clear();
        }
    }

    // A key going down or up. False when it can't be taken: not plugged in, no such key, or 6 key
    // downs already waiting.
    bool Key(uint8_t code, bool down, uint64_t now) {
        if (!powered || !Valid(code)) return false;
        if (held[code] == down) return true;
        if (down && Awake() && QueuedDowns() >= kBufferedKeys) return false;
        held[code] = down;
        if (Awake()) {
            lastKey = now;
            if (down) {
                queue.push_back(code);
            } else {
                queue.push_back(uint8_t(code | kRelease));
                if (HeldCount() == 0) queue.push_back(uint8_t(code | kRelease));
            }
        } else if (!rts && down && !ignored && pulses == 0) {
            // The code goes out too, but nothing listens while RTS is low; it is sent again with
            // the ID if the key is still down when RTS rises.
            StartPulses(now);
        }
        return true;
    }

    void Service(uint64_t now) {
        if (!powered) return;
        if (!rts && pulses > 0 && now >= pulseDue) {
            if (pulses < kPulses) {
                ++pulses;
                ++pendingPulses;
                pulseDue = now + kPulseRetryNs;
            } else {
                pulses = 0;
                ignored = true;
            }
        }
        if (Awake() && now - lastKey >= kIdleSleepNs) {
            idleSleep = true;
            queue.clear();
        }
    }

    // HotSync line pulses since the last call.
    int TakePulses() {
        const int n = pendingPulses;
        pendingPulses = 0;
        return n;
    }

    size_t Available() const { return queue.size(); }
    size_t Read(uint8_t* data, size_t max) {
        size_t n = 0;
        while (n < max && !queue.empty()) {
            data[n++] = queue.front();
            queue.pop_front();
        }
        return n;
    }
    // Bytes nobody could receive, such as while the Palm's line driver is off.
    void Drop() { queue.clear(); }

    // ready, low-power (RTS low), pulsing, asleep (pulses ignored) or idle (10 minutes without a key).
    const char* Describe() const {
        if (!powered) return "off";
        if (Awake()) return "ready";
        if (rts && idleSleep) return "idle";
        if (ignored) return "asleep";
        if (pulses > 0) return "pulsing";
        return "low-power";
    }
    unsigned IdsSent() const { return ids; }

   private:
    void StartPulses(uint64_t now) {
        pulses = 1;
        ++pendingPulses;
        pulseDue = now + kPulseRetryNs;
    }
    size_t QueuedDowns() const {
        size_t n = 0;
        for (const auto byte : queue)
            if (!(byte & kRelease)) ++n;
        return n;
    }
    size_t HeldCount() const {
        size_t n = 0;
        for (const bool down : held) n += down;
        return n;
    }

    bool powered{false}, rts{false}, awake{false}, idleSleep{false}, ignored{false};
    int pulses{0}, pendingPulses{0};
    uint64_t pulseDue{0}, lastKey{0};
    unsigned ids{0};
    std::array<bool, 0x60> held{};
    std::deque<uint8_t> queue;
};

// Types text on the keyboard as a person would: each key held and let go 50 ms apart, so 10 keys a
// second, Shift around shifted characters. A key pressed while the keyboard is in low power is
// held, and the rest wait, until it is listening (or 3 s), as the driver only hears keys still down
// when it raises RTS.
class PortableKeyboardTypist {
   public:
    static constexpr uint64_t kStepNs = 50000000ull;
    static constexpr uint64_t kWakeWaitNs = 3000000000ull;

    // False if a character has no key; nothing is queued then.
    bool Type(const std::string& text) {
        std::deque<Step> more;
        for (const char c : text) {
            uint8_t code;
            bool shift;
            if (!PortableKeyboard::CharKey(c, code, shift)) return false;
            if (shift) more.push_back({PortableKeyboard::kLeftShift, true});
            more.push_back({code, true});
            more.push_back({code, false});
            if (shift) more.push_back({PortableKeyboard::kLeftShift, false});
        }
        steps.insert(steps.end(), more.begin(), more.end());
        return true;
    }
    bool Busy() const { return !steps.empty(); }
    void Clear() {
        steps.clear();
        waiting = false;
    }

    void Service(PortableKeyboard& keyboard, uint64_t now) {
        while (!steps.empty() && now >= next) {
            const auto step = steps.front();
            // A key that woke the keyboard stays down, and the next key waits, until it listens.
            if (waiting && !keyboard.Awake() && now - waitingSince < kWakeWaitNs) return;
            if (step.down) {
                keyboard.Key(step.code, true, now);
                waiting = !keyboard.Awake();
                waitingSince = now;
            } else {
                keyboard.Key(step.code, false, now);
                waiting = false;
            }
            steps.pop_front();
            next = now + kStepNs;
        }
    }

   private:
    struct Step {
        uint8_t code;
        bool down;
    };
    std::deque<Step> steps;
    uint64_t next{0}, waitingSince{0};
    bool waiting{false};
};

}  // namespace frondKeyboard

#endif
