#ifndef FROND_S3C2442_PEN_H
#define FROND_S3C2442_PEN_H

// Host pen events to the S3C2442's touch screen, paced in guest time. The host can queue a down and an
// up together, or the guest can run slower than real time, so each pump gives the machine at most one
// down or up. A down is held at least holdNs of guest time, and as long as the host held it (up to
// hostHoldLimitNs), so a tap or a tap-and-hold reaches the guest as long as it was on the host however
// slowly the guest runs. An up is held gapNs before the next down. Moves while down go through as they
// come. WM5's screen alignment misses taps held less than about 100 ms (80 ms was missed, 100 ms taken),
// so holdNs leaves some room over that. Host keys go through S3cKeyPacer the same way.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

#include "s3c2442_machine.h"

// How long the host held a down before its up, in ns of the clock the events were stamped with, kept to
// limitNs. 0 where the up was stamped first.
inline uint64_t s3cHostHeld(uint64_t downAtNs, uint64_t upAtNs, uint64_t limitNs) {
    return upAtNs > downAtNs ? std::min(upAtNs - downAtNs, limitNs) : 0;
}

class S3cPenPacer {
   public:
    static constexpr uint64_t holdNs = 120000000, gapNs = 20000000, hostHoldLimitNs = 1000000000;

    // Raw ADC units, as S3c2442Machine::setStylus takes them. atNs is when the host made the event, on any
    // clock that runs at real time (or the guest's own, for replays in guest time).
    void push(bool down, uint16_t x, uint16_t y, uint64_t atNs = 0) { queue.push_back({down, x, y, atNs}); }
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
            const uint64_t wanted = down ? std::max(holdNs, s3cHostHeld(downAtNs, e.atNs, hostHoldLimitNs)) : gapNs;
            if (held < wanted) return;
            queue.pop_front();
            down = e.down;
            changedNs = now;
            if (e.down) downAtNs = e.atNs;
            m.setStylus(e.down, e.x, e.y);
            return;
        }
    }

   private:
    struct Event { bool down; uint16_t x, y; uint64_t atNs; };
    std::deque<Event> queue;
    bool down = false;
    uint64_t changedNs = 0, downAtNs = 0;
};

// Host key events to the machine's key pins (active low), paced in guest time like the pen: a key's down
// is held holdNs (and as long as the host held it, up to hostHoldLimitNs) before its up, and its up gapNs
// before its next down. Each key keeps its own time; events are applied in the order they came, so a key
// that isn't due holds back the ones after it. A jam (SoC::JamKey) holds a key down on its own; the pin is
// low while either holds it.
class S3cKeyPacer {
   public:
    static constexpr uint64_t holdNs = 40000000, gapNs = 20000000, hostHoldLimitNs = S3cPenPacer::hostHoldLimitNs;

    void push(S3cGpio::Port port, uint8_t bit, bool down, uint64_t atNs = 0) { queue.push_back({port, bit, down, atNs}); }
    bool empty() const { return queue.empty(); }

    // Every key up and no jams with nothing queued, as after a reset or a load. The caller sets the pins.
    void clear() {
        queue.clear();
        keys.clear();
    }

    // Holds a key down for a jam, or lets go of the jam; the pin then follows the host's state of the key.
    void jam(S3c2442Machine& m, S3cGpio::Port port, uint8_t bit, bool on) {
        Key& k = keyFor(port, bit, m.nanoseconds());
        k.jammed = on;
        m.setPin(port, bit, !(k.down || k.jammed));
    }

    // Applies every event that's due at the machine's time now, in order.
    void pump(S3c2442Machine& m) {
        const uint64_t now = m.nanoseconds();
        while (!queue.empty()) {
            const Event e = queue.front();
            Key& k = keyFor(e.port, e.bit, now);
            if (e.down != k.down) {
                const uint64_t wanted = k.down ? std::max(holdNs, s3cHostHeld(k.downAtNs, e.atNs, hostHoldLimitNs)) : gapNs;
                if (now - k.changedNs < wanted) return;
                k.down = e.down;
                k.changedNs = now;
                if (e.down) k.downAtNs = e.atNs;
                m.setPin(e.port, e.bit, !(k.down || k.jammed));
            }
            queue.pop_front();
        }
    }

   private:
    struct Event { S3cGpio::Port port; uint8_t bit; bool down; uint64_t atNs; };
    struct Key { S3cGpio::Port port; uint8_t bit; bool down; uint64_t changedNs; uint64_t downAtNs; bool jammed; };
    std::deque<Event> queue;
    std::vector<Key> keys;

    // A key not seen yet counts as up long enough for a down now.
    Key& keyFor(S3cGpio::Port port, uint8_t bit, uint64_t now) {
        for (auto& k : keys)
            if (k.port == port && k.bit == bit) return k;
        keys.push_back({port, bit, false, now >= gapNs ? now - gapNs : 0, 0, false});
        return keys.back();
    }
};

#endif
