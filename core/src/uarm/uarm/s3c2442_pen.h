#ifndef FROND_S3C2442_PEN_H
#define FROND_S3C2442_PEN_H

// Host pen events to the S3C2442's touch screen, paced in guest time. The host can queue a down and an
// up together, or the guest can run slower than real time, so each pump gives the machine at most one
// down or up. A down is held for holdNs of guest time before its up, and an up for gapNs before the next
// down. Moves while down go through as they come. WM5's touch driver misses a tap released after only a
// few ADC conversions (10 ms), so the hold is by time alone. Host keys go through S3cKeyPacer the same way.

#include <cstdint>
#include <deque>
#include <vector>

#include "s3c2442_machine.h"

class S3cPenPacer {
   public:
    static constexpr uint64_t holdNs = 40000000, gapNs = 20000000;

    // Raw ADC units, as S3c2442Machine::setStylus takes them.
    void push(bool down, uint16_t x, uint16_t y) { queue.push_back({down, x, y}); }
    bool empty() const { return queue.empty(); }

    // The pen up on the machine with nothing queued, as after a reset or a load.
    void clear(S3c2442Machine& m) {
        queue.clear();
        down = false;
        changedNs = m.nanoseconds();
        m.setStylus(false, 0, 0);
    }

    // Applies what's due at the machine's time now: moves while down, then at most one down or up.
    void pump(S3c2442Machine& m) {
        const uint64_t now = m.nanoseconds();
        while (!queue.empty()) {
            const Event e = queue.front();
            if (e.down == down) {
                queue.pop_front();
                if (down) m.setStylus(true, e.x, e.y);
                continue;
            }
            const uint64_t held = now - changedNs;
            if (held < (down ? holdNs : gapNs)) return;
            queue.pop_front();
            down = e.down;
            changedNs = now;
            m.setStylus(e.down, e.x, e.y);
            return;
        }
    }

   private:
    struct Event { bool down; uint16_t x, y; };
    std::deque<Event> queue;
    bool down = false;
    uint64_t changedNs = 0;
};

// Host key events to the machine's key pins (active low), paced in guest time like the pen: a key's down
// is held holdNs before its up, and its up gapNs before its next down. Each key keeps its own time;
// events are applied in the order they came, so a key that isn't due holds back the ones after it.
class S3cKeyPacer {
   public:
    static constexpr uint64_t holdNs = S3cPenPacer::holdNs, gapNs = S3cPenPacer::gapNs;

    void push(S3cGpio::Port port, uint8_t bit, bool down) { queue.push_back({port, bit, down}); }
    bool empty() const { return queue.empty(); }

    // Every key up with nothing queued, as after a reset or a load. The caller sets the pins.
    void clear() {
        queue.clear();
        keys.clear();
    }

    // Applies every event that's due at the machine's time now, in order.
    void pump(S3c2442Machine& m) {
        const uint64_t now = m.nanoseconds();
        while (!queue.empty()) {
            const Event e = queue.front();
            Key& k = keyFor(e.port, e.bit, now);
            if (e.down != k.down) {
                if (now - k.changedNs < (k.down ? holdNs : gapNs)) return;
                k.down = e.down;
                k.changedNs = now;
                m.setPin(e.port, e.bit, !e.down);
            }
            queue.pop_front();
        }
    }

   private:
    struct Event { S3cGpio::Port port; uint8_t bit; bool down; };
    struct Key { S3cGpio::Port port; uint8_t bit; bool down; uint64_t changedNs; };
    std::deque<Event> queue;
    std::vector<Key> keys;

    // A key not seen yet counts as up long enough for a down now.
    Key& keyFor(S3cGpio::Port port, uint8_t bit, uint64_t now) {
        for (auto& k : keys)
            if (k.port == port && k.bit == bit) return k;
        keys.push_back({port, bit, false, now >= gapNs ? now - gapNs : 0});
        return keys.back();
    }
};

#endif
