#ifndef FROND_68K_TOUCH_H
#define FROND_68K_TOUCH_H

// Touch input for the 68k runner: each finger is the pen (one at most) or one held button, as on a
// real Palm that reads several keys and the pen at once. Points are device pixels, y from the top
// of the pen area. KeyDown and KeyUp come once per finger; the sink counts the holders of each key. A button is released when its finger lifts or slides off it, and a finger sliding onto another button switches keys. A button finger never becomes the pen.

#include <algorithm>
#include <array>
#include <cstdint>
#include "keys.h"

// How many sources (mouse, keyboard, fingers) hold each key. A key goes down with its first holder
// and up with its last.
class KeyHolds68k {
   public:
    // True when the key should now go down.
    bool Press(KeyId key) { return ++holds[key] == 1; }
    // True when the key should now go up; a release nobody holds is ignored.
    bool Release(KeyId key) { return holds[key] > 0 && --holds[key] == 0; }
    void Clear() { holds.fill(0); }

   private:
    std::array<int, keyIdCount> holds{};
};

class TouchSink68k {
   public:
    virtual ~TouchSink68k() = default;
    // Only touch screens count; a touchpad's fingers are not taps on the device.
    virtual bool DirectTouch(int64_t touch) = 0;
    virtual KeyId KeyAt(int x, int y) = 0;
    virtual void PenDown(int x, int y) = 0;
    virtual void PenUp() = 0;
    virtual void KeyDown(KeyId key) = 0;
    virtual void KeyUp(KeyId key) = 0;
};

class Touch68k {
   public:
    Touch68k(TouchSink68k& sink, int width, int penHeight) : sink(sink), width(width), penHeight(penHeight) {}

    bool PenActive() const {
        return std::any_of(fingers.begin(), fingers.end(), [](const Finger& f) { return f.active && f.pen; });
    }

    // inside is false for a point outside the device. penBusy is true while a mouse holds the pen.
    void Down(int64_t touch, int64_t id, int x, int y, bool inside, bool penBusy) {
        if (Find(touch, id) || !inside || !sink.DirectTouch(touch)) return;
        Finger* finger = std::find_if(fingers.begin(), fingers.end(), [](const Finger& f) { return !f.active; });
        if (finger == fingers.end()) return;
        if (y < penHeight) {
            if (penBusy || PenActive()) return;
            *finger = {true, true, touch, id, keyInvalid};
            sink.PenDown(std::clamp(x, 0, width - 1), y);
        } else {
            const KeyId key = sink.KeyAt(x, y);
            if (key == keyInvalid) return;
            *finger = {true, false, touch, id, key};
            sink.KeyDown(key);
        }
    }

    void Move(int64_t touch, int64_t id, int x, int y) {
        Finger* finger = Find(touch, id);
        if (!finger) return;
        if (finger->pen) sink.PenDown(std::clamp(x, 0, width - 1), std::clamp(y, 0, penHeight - 1));
        else {
            // Rolling onto another button lets go of the old key and presses the new one.
            const KeyId key = sink.KeyAt(x, y);
            if (key == finger->key) return;
            if (key == keyInvalid) return Release(*finger);
            sink.KeyUp(finger->key);
            finger->key = key;
            sink.KeyDown(key);
        }
    }

    void Up(int64_t touch, int64_t id) {
        if (Finger* finger = Find(touch, id)) Release(*finger);
    }

    void ReleaseAll() {
        for (auto& finger : fingers) if (finger.active) Release(finger);
    }

   private:
    struct Finger {
        bool active{false}, pen{false};
        int64_t touch{0}, id{0};
        KeyId key{keyInvalid};
    };

    Finger* Find(int64_t touch, int64_t id) {
        for (auto& finger : fingers)
            if (finger.active && finger.touch == touch && finger.id == id) return &finger;
        return nullptr;
    }

    void Release(Finger& finger) {
        if (finger.pen) sink.PenUp();
        else sink.KeyUp(finger.key);
        finger = {};
    }

    TouchSink68k& sink;
    int width, penHeight;
    std::array<Finger, 16> fingers{};
};

#endif
