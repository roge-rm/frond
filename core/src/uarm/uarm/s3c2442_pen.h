#ifndef FROND_S3C2442_PEN_H
#define FROND_S3C2442_PEN_H

// Host pen events to the S3C2442's touch screen, paced in guest time. The host can queue a down and an
// up together, or the guest can run slower than real time, so each pump gives the machine at most one
// down or up. A down is held for holdNs of guest time before its up, and an up for gapNs before the next
// down. Moves while down go through as they come. WM5's touch driver misses a tap released after only a
// few ADC conversions (10 ms), so the hold is by time alone.

#include <cstdint>
#include <deque>

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

#endif
