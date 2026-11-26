#ifndef FROND_GAMEPAD_H
#define FROND_GAMEPAD_H

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ControllerMap.h"
#include "DeviceControls.h"

// What a controller holds on the device: keys and the analogue stick.
struct GamepadInput {
    DeviceKeyMask keys{0};
    DeviceStick stick{};
};

// POLICY, host input only: a controller's stick within this much of its centre is at rest, as worn
// sticks don't come back to zero. Past it the travel starts from nothing, so small pushes still count.
constexpr float gamepadStickRest = 0.12f;

// The core's key for a Palm button in a controller map; keyInvalid for none.
inline KeyId palmKeyToKeyId(PalmKey key) {
    switch (key) {
        case PalmKey::up: return keyIdUp;
        case PalmKey::down: return keyIdDown;
        case PalmKey::left: return keyIdLeft;
        case PalmKey::right: return keyIdRight;
        case PalmKey::centre: return keyIdSelect;
        case PalmKey::app1: return keyIdHard1;
        case PalmKey::app2: return keyIdHard2;
        case PalmKey::app3: return keyIdHard3;
        case PalmKey::app4: return keyIdHard4;
        case PalmKey::home: return keyIdHome;
        case PalmKey::power: return keyIdPower;
        case PalmKey::voice: return keyIdVoice;
        case PalmKey::jogUp: return keyIdJogUp;
        case PalmKey::jogDown: return keyIdJogDown;
        case PalmKey::jogPress: return keyIdJogPress;
        case PalmKey::back: return keyIdBack;
        case PalmKey::function: return keyIdFunction;
        case PalmKey::triggerLeft: return keyIdTriggerLeft;
        case PalmKey::triggerRight: return keyIdTriggerRight;
        case PalmKey::stickPress: return keyIdStickPress;
        case PalmKey::padUp: return keyIdPadUp;
        case PalmKey::padDown: return keyIdPadDown;
        case PalmKey::padLeft: return keyIdPadLeft;
        case PalmKey::padRight: return keyIdPadRight;
        case PalmKey::padA: return keyIdPadA;
        case PalmKey::padB: return keyIdPadB;
        case PalmKey::padX: return keyIdPadX;
        case PalmKey::padY: return keyIdPadY;
        case PalmKey::padHome: return keyIdPadHome;
        case PalmKey::padMenu: return keyIdPadMenu;
        case PalmKey::padHold: return keyIdPadHold;
        default: return keyInvalid;
    }
}

// The Zodiac's Up, Down, Left and Right as last applied, so the one pressed last on an axis wins.
// The caller keeps it between calls.
struct GamepadDirections {
    bool left{false}, right{false}, up{false}, down{false};
    int x{0}, y{0};
};

// The CLIÉ's own button a game controller button presses while the controller is detached; count for none.
inline PalmKey clieDetachedKey(PalmKey key) {
    switch (key) {
        case PalmKey::padUp: return PalmKey::up;
        case PalmKey::padDown: return PalmKey::down;
        case PalmKey::padHome: return PalmKey::home;
        case PalmKey::padX: return PalmKey::app2;
        case PalmKey::padY: return PalmKey::app3;
        case PalmKey::padHold: return PalmKey::app4;
        default: return PalmKey::count;
    }
}

// What the controllers hold on the device, from the controller keys held (padKeysHeld over every
// controller) through the map. On the Zodiac (zodiac) the map's stick (its axes as stickX, stickY,
// padStickAxes) is the analogue stick over its whole range, and Up, Down, Left and Right push it to full travel on their axis,
// winning over the left stick there, the one pressed last if both are held. The CLIÉ game controller's
// buttons reach it only while it is attached (padAttached); detached, its arrows, Home, X, Y and Hold
// press the CLIÉ's own Up, Down, Home and second to fourth app buttons, as on Android.
inline GamepadInput gamepadInput(const ControllerMap& map, PadKeyMask held, int16_t stickX, int16_t stickY, bool zodiac,
                                 bool padAttached, GamepadDirections* directions = nullptr) {
    GamepadInput input;
    const uint64_t pressed = map.pressed(held);
    auto on = [&](PalmKey key) { return ((pressed >> int(key)) & 1) != 0; };
    for (int i = 0; i < int(PalmKey::count); ++i) {
        const auto key = PalmKey(i);
        if (!on(key)) continue;
        if (zodiac && key >= PalmKey::up && key <= PalmKey::right) continue;
        if (clieGamepadPalmKey(key) && !padAttached) {
            if (const KeyId id = palmKeyToKeyId(clieDetachedKey(key))) input.keys |= deviceKeyBit(id);
            continue;
        }
        if (const KeyId id = palmKeyToKeyId(key)) input.keys |= deviceKeyBit(id);
    }
    if (!zodiac) return input;
    const float x = std::max(stickX / 32767.0f, -1.0f), y = std::max(stickY / 32767.0f, -1.0f);
    const float length = std::hypot(x, y);
    float sx = 0, sy = 0;
    if (length > gamepadStickRest) {
        const float scale = (length - gamepadStickRest) / (1 - gamepadStickRest) / length;
        sx = std::clamp(x * scale, -1.0f, 1.0f);
        sy = std::clamp(y * scale, -1.0f, 1.0f);
    }
    GamepadDirections none, &last = directions ? *directions : none;
    auto axis = [](bool negative, bool positive, bool& wasNegative, bool& wasPositive, int& last) {
        if (!(negative && positive)) last = positive ? 1 : negative ? -1 : 0;
        else if (!wasPositive) last = 1;
        else if (!wasNegative) last = -1;
        wasNegative = negative;
        wasPositive = positive;
        return last;
    };
    if (const int dx = axis(on(PalmKey::left), on(PalmKey::right), last.left, last.right, last.x)) sx = float(dx);
    if (const int dy = axis(on(PalmKey::up), on(PalmKey::down), last.up, last.down, last.y)) sy = float(dy);
    if (sx != 0 || sy != 0) input.stick = {sx, sy, true};
    return input;
}

#endif
