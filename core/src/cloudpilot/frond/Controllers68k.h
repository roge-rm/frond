#ifndef FROND_68K_CONTROLLERS_H
#define FROND_68K_CONTROLLERS_H

// Game controllers on the 68k runner: each controller's reading, by SDL instance id, goes through the
// device's controller map (ControllerMap.h) to key presses and releases. No SDL here, so tests can drive it.
// The runner calls it on its one thread, like its other input.

#include <map>
#include <string>

#include "../../uarm/native/Gamepad.h"

class Controllers68k {
   public:
    explicit Controllers68k(const DeviceProfile5& profile) : profile(&profile), map(controllerDefaults(profile)) {}

    // The device has buttons a map can press (Dana: none).
    bool Listed() const { return !controllerPalmKeys(*profile).empty(); }
    // Controllers are opened only while the map has keys.
    bool Wanted() const { return !map.keys.empty(); }
    const ControllerMap& Map() const { return map; }
    // The reading the runner fills from SDL's events; call Update after.
    PadReading& Pad(int id) { return pads[id]; }

    // The map from its text; keys the old map held and the new one doesn't are let go.
    template <class Apply> void SetMap(const std::string& text, Apply&& apply) {
        map = decodeControllerMap(text, *profile);
        Update(apply);
    }
    template <class Apply> void Remove(int id, Apply&& apply) {
        pads.erase(id);
        Update(apply);
    }
    // Off while the window has lost focus or is hidden; what was held is let go.
    template <class Apply> void SetFocused(bool on, Apply&& apply) {
        focused = on;
        Update(apply);
    }
    // The runner let go of every key itself: the buttons held count as released, so each has to be
    // pressed again. The axes stay as they are.
    void Forget() {
        for (auto& entry : pads) entry.second.buttons = 0, entry.second.axisHeld = 0;
        held = 0;
    }

    // Calls apply(KeyId, down) for each key whose state changed since the last call.
    template <class Apply> void Update(Apply&& apply) {
        DeviceKeyMask now = 0;
        if (focused) {
            PadKeyMask keys = 0;
            for (auto& entry : pads) keys |= padKeysHeld(entry.second);
            now = gamepadInput(map, keys, 0, 0, false, false).keys;
        }
        for (unsigned key = 1; key < keyIdCount; ++key) {
            const DeviceKeyMask bit = deviceKeyBit(KeyId(key));
            if ((now & bit) != (held & bit)) apply(KeyId(key), (now & bit) != 0);
        }
        held = now;
    }

   private:
    const DeviceProfile5* profile;
    ControllerMap map;
    std::map<int, PadReading> pads;
    DeviceKeyMask held{0};
    bool focused{true};
};

#endif
