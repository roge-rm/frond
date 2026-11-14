#ifndef FROND_GAMEPAD_H
#define FROND_GAMEPAD_H

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "DeviceControls.h"

// A host game controller as SDL reports it: the left stick's axes (-32768 to 32767, right and down
// positive) and the buttons a device uses.
struct GamepadState {
    int16_t leftX{0}, leftY{0};
    bool a{false}, b{false}, x{false}, y{false};
    bool leftShoulder{false}, rightShoulder{false}, leftStick{false};
    bool back{false}, start{false}, guide{false};
    bool dpadLeft{false}, dpadRight{false}, dpadUp{false}, dpadDown{false};
};

// What a controller holds on the device: keys and the analogue stick.
struct GamepadInput {
    DeviceKeyMask keys{0};
    DeviceStick stick{};
};

// POLICY, host input only: a controller's stick within this much of its centre is at rest, as worn
// sticks don't come back to zero. Past it the travel starts from nothing, so small pushes still count.
constexpr float gamepadStickRest = 0.12f;

// The Zodiac from a controller. The left stick is the analogue stick over its whole range and its
// click the stick's press. A, B, X and Y are the Action keys by place (bottom, right, left, top), the
// shoulders are the triggers, Start and Guide are Home and Back is Function. The d-pad pushes the stick
// to full travel on its axis and wins over the left stick there.
inline GamepadInput zodiacGamepadInput(const GamepadState& pad) {
    GamepadInput input;
    auto hold = [&](bool down, KeyId key) { if (down) input.keys |= deviceKeyBit(key); };
    hold(pad.a, keyIdHard1);
    hold(pad.b, keyIdHard2);
    hold(pad.x, keyIdHard3);
    hold(pad.y, keyIdHard4);
    hold(pad.leftShoulder, keyIdTriggerLeft);
    hold(pad.rightShoulder, keyIdTriggerRight);
    hold(pad.leftStick, keyIdStickPress);
    hold(pad.start || pad.guide, keyIdHome);
    hold(pad.back, keyIdFunction);
    const float x = std::max(pad.leftX / 32767.0f, -1.0f), y = std::max(pad.leftY / 32767.0f, -1.0f);
    const float length = std::hypot(x, y);
    float sx = 0, sy = 0;
    if (length > gamepadStickRest) {
        const float scale = (length - gamepadStickRest) / (1 - gamepadStickRest) / length;
        sx = std::clamp(x * scale, -1.0f, 1.0f);
        sy = std::clamp(y * scale, -1.0f, 1.0f);
    }
    const int dx = int(pad.dpadRight) - int(pad.dpadLeft), dy = int(pad.dpadDown) - int(pad.dpadUp);
    if (dx) sx = float(dx);
    if (dy) sy = float(dy);
    if (sx != 0 || sy != 0) input.stick = {sx, sy, true};
    return input;
}

#endif
