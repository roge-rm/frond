#ifndef FROND_CONTROLLER_MAP_H
#define FROND_CONTROLLER_MAP_H

// Which game controller button presses which Palm button, per device. Shared by the ARM core and the
// desktop shell, so it needs nothing but the device table: no SDL, no Qt.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "device_profile5.h"

// A controller button as SDL's game controller API numbers its buttons (SDL_GameControllerButton
// 0-20), then the sticks pushed one way and the analogue triggers, which press past half travel.
enum class PadKey : int {
    a, b, x, y, back, guide, start, leftStick, rightStick, leftShoulder, rightShoulder,
    dpadUp, dpadDown, dpadLeft, dpadRight, misc1, paddle1, paddle2, paddle3, paddle4, touchpad,
    leftLeft, leftRight, leftUp, leftDown, rightLeft, rightRight, rightUp, rightDown,
    leftTrigger, rightTrigger,
    count
};
constexpr int padButtonCount = int(PadKey::touchpad) + 1;
// Controller keys held, one bit per PadKey.
typedef uint64_t PadKeyMask;
inline PadKeyMask padKeyBit(PadKey key) { return PadKeyMask(1) << int(key); }

// Saved names: SDL's own names for the buttons and triggers, the axis name and a sign for a stick direction.
inline const char* padKeyId(PadKey key) {
    static const char* const ids[] = {
        "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder", "rightshoulder",
        "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad",
        "leftx-", "leftx+", "lefty-", "lefty+", "rightx-", "rightx+", "righty-", "righty+",
        "lefttrigger", "righttrigger"};
    static_assert(sizeof(ids) / sizeof(ids[0]) == size_t(PadKey::count), "a name per controller key");
    return int(key) >= 0 && key < PadKey::count ? ids[int(key)] : "";
}
inline const char* padKeyName(PadKey key) {
    static const char* const names[] = {
        "A", "B", "X", "Y", "Select", "Guide", "Start", "L3", "R3", "L1", "R1",
        "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Misc", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4",
        "Touchpad", "Left stick left", "Left stick right", "Left stick up", "Left stick down",
        "Right stick left", "Right stick right", "Right stick up", "Right stick down", "L2", "R2"};
    static_assert(sizeof(names) / sizeof(names[0]) == size_t(PadKey::count), "a label per controller key");
    return int(key) >= 0 && key < PadKey::count ? names[int(key)] : "";
}
inline bool padKeyFromId(const std::string& id, PadKey& key) {
    for (int i = 0; i < int(PadKey::count); ++i)
        if (id == padKeyId(PadKey(i))) { key = PadKey(i); return true; }
    return false;
}

// A Palm button a controller can press; ids as Android's PalmButton. The CLIÉ game controller's
// arrows, Home and Menu are buttons of their own here, as they only reach the pad while it's attached.
enum class PalmKey : int {
    up, down, left, right, centre, app1, app2, app3, app4, home, power, voice,
    jogUp, jogDown, jogPress, back, function, triggerLeft, triggerRight, stickPress,
    padUp, padDown, padLeft, padRight, padA, padB, padX, padY, padHome, padMenu, padHold,
    count
};
inline const char* palmKeyId(PalmKey key) {
    static const char* const ids[] = {
        "up", "down", "left", "right", "centre", "app1", "app2", "app3", "app4", "home", "power", "voice",
        "jog-up", "jog-down", "jog-press", "back", "function", "trigger-left", "trigger-right", "stick-press",
        "pad-up", "pad-down", "pad-left", "pad-right", "pad-a", "pad-b", "pad-x", "pad-y", "pad-home", "pad-menu",
        "pad-hold"};
    static_assert(sizeof(ids) / sizeof(ids[0]) == size_t(PalmKey::count), "a name per Palm button");
    return int(key) >= 0 && key < PalmKey::count ? ids[int(key)] : "";
}
inline bool palmKeyFromId(const std::string& id, PalmKey& key) {
    for (int i = 0; i < int(PalmKey::count); ++i)
        if (id == palmKeyId(PalmKey(i))) { key = PalmKey(i); return true; }
    return false;
}
inline bool clieGamepadPalmKey(PalmKey key) { return key >= PalmKey::padUp && key <= PalmKey::padHold; }
inline bool zodiacController(const DeviceProfile5& profile) { return profile.art == DeviceArt::zodiac; }

// The Palm buttons a device's controls list, in order. The Zodiac has its own list. Elsewhere the
// device table's buttons, without Menu and a silkscreen Home, which are taps on the screen; a CLIÉ
// that takes the game controller adds the pad's buttons. As Android lists them, the 68k devices included
// (the Dana lists none).
inline std::vector<PalmKey> controllerPalmKeys(const DeviceProfile5& profile) {
    using K = PalmKey;
    if (zodiacController(profile))
        return {K::up, K::down, K::left, K::right, K::stickPress, K::app1, K::app2, K::app3, K::app4, K::home,
                K::function, K::triggerLeft, K::triggerRight};
    std::vector<PalmKey> keys;
    const std::string text = profile.frontend.buttons ? profile.frontend.buttons : "";
    size_t start = 0;
    while (start <= text.size() && !text.empty()) {
        const size_t bar = std::min(text.find('|', start), text.size());
        PalmKey key;
        const std::string id = text.substr(start, bar - start);
        const bool silkscreenHome = id == "home" && profile.frontend.homeX >= 0;
        if (palmKeyFromId(id, key) && !silkscreenHome && std::find(keys.begin(), keys.end(), key) == keys.end())
            keys.push_back(key);
        start = bar + 1;
    }
    if (profile.frontend.clieGamepad)
        for (int i = int(K::padUp); i <= int(K::padHold); ++i) keys.push_back(PalmKey(i));
    return keys;
}

// A label for the controls list. The app buttons take the device's names; the Zodiac's Action keys go
// by place (bottom, right, left, top).
inline std::string palmKeyLabel(PalmKey key, const DeviceProfile5& profile) {
    using K = PalmKey;
    if (zodiacController(profile)) switch (key) {
        case K::up: return "Stick up";
        case K::down: return "Stick down";
        case K::left: return "Stick left";
        case K::right: return "Stick right";
        case K::app1: return "Action 1 (bottom)";
        case K::app2: return "Action 2 (right)";
        case K::app3: return "Action 3 (left)";
        case K::app4: return "Action 4 (top)";
        case K::stickPress: return "Stick press (centre)";
        default: break;
    }
    if (key >= K::app1 && key <= K::app4) {
        const std::string names = profile.frontend.appNames ? profile.frontend.appNames : "";
        size_t start = 0;
        for (int i = 0; i < int(key) - int(K::app1) && start != std::string::npos; ++i) {
            start = names.find('|', start);
            if (start != std::string::npos) ++start;
        }
        if (start != std::string::npos && start < names.size()) {
            const std::string name = names.substr(start, names.find('|', start) - start);
            if (!name.empty()) return name;
        }
        return "App " + std::to_string(int(key) - int(K::app1) + 1);
    }
    switch (key) {
        case K::up: return "Up";
        case K::down: return "Down";
        case K::left: return "Left";
        case K::right: return "Right";
        case K::centre: return "Centre";
        case K::home: return "Home";
        case K::power: return "Power";
        case K::voice: return "Voice";
        case K::jogUp: return "Jog up";
        case K::jogDown: return "Jog down";
        case K::jogPress: return "Jog press";
        case K::back: return "Back";
        case K::function: return "Function";
        case K::triggerLeft: return "Left trigger";
        case K::triggerRight: return "Right trigger";
        case K::stickPress: return "Stick press";
        case K::padUp: return "Game controller up";
        case K::padDown: return "Game controller down";
        case K::padLeft: return "Game controller left";
        case K::padRight: return "Game controller right";
        case K::padA: return "Game controller A";
        case K::padB: return "Game controller B";
        case K::padX: return "Game controller X";
        case K::padY: return "Game controller Y";
        case K::padHome: return "Game controller Home";
        case K::padMenu: return "Game controller Menu";
        case K::padHold: return "Game controller Hold";
        default: return palmKeyId(key);
    }
}

// A controller axis that can move the Zodiac's analogue stick: a stick axis, or a trigger (0 to full travel).
enum class PadAxis : int { none, leftX, leftY, rightX, rightY, leftTrigger, rightTrigger, count };
inline const char* padAxisId(PadAxis axis) {
    static const char* const ids[] = {"none", "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"};
    static_assert(sizeof(ids) / sizeof(ids[0]) == size_t(PadAxis::count), "a name per controller axis");
    return int(axis) >= 0 && axis < PadAxis::count ? ids[int(axis)] : "none";
}
inline const char* padAxisName(PadAxis axis) {
    static const char* const names[] = {"None", "Left stick X", "Left stick Y", "Right stick X", "Right stick Y", "L2", "R2"};
    return int(axis) >= 0 && axis < PadAxis::count ? names[int(axis)] : "None";
}
inline bool padAxisFromId(const std::string& id, PadAxis& axis) {
    for (int i = 0; i < int(PadAxis::count); ++i)
        if (id == padAxisId(PadAxis(i))) { axis = PadAxis(i); return true; }
    return false;
}
// The controller keys a stick or trigger axis presses at its ends, which come off the map when the axis moves the stick.
inline void padAxisKeys(PadAxis axis, PadKey& first, PadKey& second) {
    switch (axis) {
        case PadAxis::leftX: first = PadKey::leftLeft; second = PadKey::leftRight; break;
        case PadAxis::leftY: first = PadKey::leftUp; second = PadKey::leftDown; break;
        case PadAxis::rightX: first = PadKey::rightLeft; second = PadKey::rightRight; break;
        case PadAxis::rightY: first = PadKey::rightUp; second = PadKey::rightDown; break;
        case PadAxis::leftTrigger: first = second = PadKey::leftTrigger; break;
        case PadAxis::rightTrigger: first = second = PadKey::rightTrigger; break;
        default: first = second = PadKey::count; break;
    }
}

// One of the Zodiac stick's two axes: the controller axis that moves it, and whether it runs backwards.
struct StickAxis {
    PadAxis axis{PadAxis::none};
    bool invert{false};
    bool operator==(const StickAxis& o) const { return axis == o.axis && invert == o.invert; }
    bool operator!=(const StickAxis& o) const { return !(*this == o); }
};

// The presets for the two stick axes: the controller's left or right stick, or none. Anything else is custom.
enum class PadStick : int { left, right, none, custom };
inline const char* padStickId(PadStick stick) {
    return stick == PadStick::right ? "right" : stick == PadStick::none ? "none" : "left";
}

// One controller key presses one Palm button; a Palm button can have several keys. On the Zodiac,
// stickX and stickY say which controller axes move its analogue stick, left stick by default. A preset is
// saved as "stick:right" or "stick:none" (nothing for left, so older maps keep the left stick and older
// cores skip the entry); other choices are "stickx:AXIS" and "sticky:AXIS", the axis name with a leading
// "-" for inverted, or "none".
struct ControllerMap {
    std::map<PadKey, PalmKey> keys;
    StickAxis stickX{PadAxis::leftX, false}, stickY{PadAxis::leftY, false};

    std::vector<PadKey> keysFor(PalmKey button) const {
        std::vector<PadKey> found;
        for (const auto& entry : keys) if (entry.second == button) found.push_back(entry.first);
        return found;
    }
    // Takes key from whatever it pressed before.
    void assign(PalmKey button, PadKey key) { keys[key] = button; }
    void remove(PadKey key) { keys.erase(key); }
    PadStick preset() const {
        if (!stickX.invert && !stickY.invert) {
            if (stickX.axis == PadAxis::leftX && stickY.axis == PadAxis::leftY) return PadStick::left;
            if (stickX.axis == PadAxis::rightX && stickY.axis == PadAxis::rightY) return PadStick::right;
        }
        if (stickX.axis == PadAxis::none && stickY.axis == PadAxis::none) return PadStick::none;
        return PadStick::custom;
    }
    // Takes the keys at an axis's ends off, so the axis only moves the stick.
    void freeAxis(PadAxis axis) {
        PadKey first, second;
        padAxisKeys(axis, first, second);
        if (first != PadKey::count) { keys.erase(first); keys.erase(second); }
    }
    // Picks a preset and takes that stick's direction keys off. Custom changes nothing.
    void setStick(PadStick chosen) {
        if (chosen == PadStick::custom) return;
        stickX = {chosen == PadStick::left ? PadAxis::leftX : chosen == PadStick::right ? PadAxis::rightX : PadAxis::none, false};
        stickY = {chosen == PadStick::left ? PadAxis::leftY : chosen == PadStick::right ? PadAxis::rightY : PadAxis::none, false};
        freeAxis(stickX.axis);
        freeAxis(stickY.axis);
    }
    // Sets the stick's X (or Y) axis and takes the keys at that axis's ends off.
    void setStickAxis(bool y, PadAxis axis, bool invert) {
        (y ? stickY : stickX) = {axis, axis != PadAxis::none && invert};
        freeAxis(axis);
    }
    bool usesAxis(PadAxis axis) const { return axis != PadAxis::none && (stickX.axis == axis || stickY.axis == axis); }
    bool operator==(const ControllerMap& o) const { return keys == o.keys && stickX == o.stickX && stickY == o.stickY; }
    bool operator!=(const ControllerMap& o) const { return !(*this == o); }

    // "key:button,..." in key order, or "none" for an empty map, so it is never an empty argument.
    std::string encode() const {
        std::string text;
        auto add = [&](const std::string& entry) { text += (text.empty() ? "" : ",") + entry; };
        for (const auto& entry : keys) add(std::string(padKeyId(entry.first)) + ':' + palmKeyId(entry.second));
        const PadStick chosen = preset();
        if (chosen == PadStick::custom) {
            for (const bool y : {false, true}) {
                const StickAxis& one = y ? stickY : stickX;
                add(std::string(y ? "sticky:" : "stickx:") + (one.invert ? "-" : "") + padAxisId(one.axis));
            }
        } else if (chosen != PadStick::left) {
            add(std::string("stick:") + padStickId(chosen));
        }
        return text.empty() ? "none" : text;
    }

    // The Palm buttons the held controller keys press, one bit per PalmKey.
    uint64_t pressed(PadKeyMask held) const {
        uint64_t buttons = 0;
        for (const auto& entry : keys)
            if (held & padKeyBit(entry.first)) buttons |= uint64_t(1) << int(entry.second);
        return buttons;
    }
};

// The Zodiac: the d-pad pushes the stick, A, B, X and Y and the right stick are the Action keys by
// place, the shoulders the triggers, Start and Guide Home, Select Function and L3 the stick's press.
// The CLIÉ game controller: the same face buttons by name, the d-pad and left stick its arrows, Start
// and Guide Home, Select Menu, R1 Hold, and L1 the first app button. Other devices as Android's
// ControllerMap.defaults: the d-pad and A for the 5-way, L1, X, Y and R1 the app buttons left to right,
// Start Home (or the first app button on a device with no Home at all), L2, R2, R3 and L3 the Jog Dial and Back, B
// nothing; only the buttons the device lists.
inline ControllerMap controllerDefaults(const DeviceProfile5& profile) {
    using P = PadKey;
    using K = PalmKey;
    ControllerMap map;
    const auto allowed = controllerPalmKeys(profile);
    auto has = [&](PalmKey key) { return std::find(allowed.begin(), allowed.end(), key) != allowed.end(); };
    if (zodiacController(profile)) {
        map.keys = {{P::dpadUp, K::up}, {P::dpadDown, K::down}, {P::dpadLeft, K::left}, {P::dpadRight, K::right},
                    {P::leftStick, K::stickPress}, {P::a, K::app1}, {P::b, K::app2}, {P::x, K::app3}, {P::y, K::app4},
                    {P::rightDown, K::app1}, {P::rightRight, K::app2}, {P::rightLeft, K::app3}, {P::rightUp, K::app4},
                    {P::start, K::home}, {P::guide, K::home}, {P::back, K::function},
                    {P::leftShoulder, K::triggerLeft}, {P::rightShoulder, K::triggerRight}};
    } else if (profile.frontend.clieGamepad && !deviceProfileRuns68k5(int(profile.deviceType))) {
        map.keys = {{P::a, K::padA}, {P::b, K::padB}, {P::x, K::padX}, {P::y, K::padY},
                    {P::start, K::padHome}, {P::guide, K::padHome}, {P::back, K::padMenu}, {P::rightShoulder, K::padHold},
                    {P::dpadUp, K::padUp}, {P::dpadDown, K::padDown}, {P::dpadLeft, K::padLeft}, {P::dpadRight, K::padRight},
                    {P::leftUp, K::padUp}, {P::leftDown, K::padDown}, {P::leftLeft, K::padLeft}, {P::leftRight, K::padRight},
                    {P::leftShoulder, K::app1}};
    } else {
        // Start is Home on a device with one, a silkscreen Home too (a tap the desktop doesn't offer yet).
        const std::string buttons = std::string("|") + (profile.frontend.buttons ? profile.frontend.buttons : "") + "|";
        const bool homeButton = buttons.find("|home|") != std::string::npos;
        const std::pair<PadKey, PalmKey> keys[] = {
            {P::dpadUp, K::up}, {P::dpadDown, K::down}, {P::dpadLeft, K::left}, {P::dpadRight, K::right},
            {P::a, K::centre}, {P::leftShoulder, K::app1}, {P::x, K::app2}, {P::y, K::app3}, {P::rightShoulder, K::app4},
            {P::start, homeButton ? K::home : K::app1},
            {P::leftTrigger, K::jogUp}, {P::rightTrigger, K::jogDown}, {P::rightStick, K::jogPress}, {P::leftStick, K::back}};
        for (const auto& [key, button] : keys)
            if (has(button)) map.keys[key] = button;
    }
    for (auto it = map.keys.begin(); it != map.keys.end();) it = has(it->second) ? std::next(it) : map.keys.erase(it);
    return map;
}

// Unknown or malformed entries and buttons the device doesn't list are skipped; "none" or empty is no keys.
inline ControllerMap decodeControllerMap(const std::string& text, const DeviceProfile5& profile) {
    ControllerMap map;
    const auto allowed = controllerPalmKeys(profile);
    std::vector<PadAxis> bound;  // axes the text binds to the stick, wherever their keys come in the text
    size_t start = 0;
    while (start < text.size()) {
        const size_t comma = std::min(text.find(',', start), text.size());
        const std::string entry = text.substr(start, comma - start);
        start = comma + 1;
        const size_t colon = entry.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = entry.substr(0, colon);
        if (name == "stick") {
            const std::string id = entry.substr(colon + 1);
            if (id == "right") map.setStick(PadStick::right);
            else if (id == "none") map.setStick(PadStick::none);
            else if (id == "left") map.setStick(PadStick::left);
            else continue;
            bound.push_back(map.stickX.axis);
            bound.push_back(map.stickY.axis);
            continue;
        }
        if (name == "stickx" || name == "sticky") {
            std::string id = entry.substr(colon + 1);
            const bool invert = !id.empty() && id[0] == '-';
            PadAxis axis;
            if (invert) id.erase(0, 1);
            if (padAxisFromId(id, axis)) {
                map.setStickAxis(name == "sticky", axis, invert);
                bound.push_back(axis);
            }
            continue;
        }
        PadKey key;
        PalmKey button;
        if (!padKeyFromId(entry.substr(0, colon), key) || !palmKeyFromId(entry.substr(colon + 1), button)) continue;
        if (std::find(allowed.begin(), allowed.end(), button) == allowed.end()) continue;
        map.keys[key] = button;
    }
    for (const auto axis : bound) map.freeAxis(axis);
    return map;
}

// POLICY, host input only: a stick or trigger presses its key past half travel and lets go at about a
// third, so it doesn't flicker near the press point.
constexpr int padAxisPress = 16384, padAxisRelease = 11469;

// One controller's axes and buttons as SDL reports them (axes -32768 to 32767, right and down positive,
// triggers 0 to 32767), and which axis keys it holds now, kept between calls for the release point.
struct PadReading {
    int16_t leftX{0}, leftY{0}, rightX{0}, rightY{0}, leftTrigger{0}, rightTrigger{0};
    uint32_t buttons{0};  // one bit per SDL button, 0-20
    PadKeyMask axisHeld{0};
};

inline int16_t padAxisValue(const PadReading& pad, PadAxis axis) {
    switch (axis) {
        case PadAxis::leftX: return pad.leftX;
        case PadAxis::leftY: return pad.leftY;
        case PadAxis::rightX: return pad.rightX;
        case PadAxis::rightY: return pad.rightY;
        case PadAxis::leftTrigger: return pad.leftTrigger;
        case PadAxis::rightTrigger: return pad.rightTrigger;
        default: return 0;
    }
}

// The values of the axes that move the Zodiac's analogue stick, inverted where the map says; 0, 0 for none.
inline void padStickAxes(const PadReading& pad, const ControllerMap& map, int16_t& x, int16_t& y) {
    auto one = [&](const StickAxis& bound) {
        const int value = padAxisValue(pad, bound.axis);
        return int16_t(bound.invert ? std::min(-value, 32767) : value);
    };
    x = one(map.stickX);
    y = one(map.stickY);
}

// The controller keys a reading holds: its buttons and the axis keys past their press point.
inline PadKeyMask padKeysHeld(PadReading& pad) {
    PadKeyMask held = pad.buttons & ((PadKeyMask(1) << padButtonCount) - 1);
    auto axis = [&](PadKey key, int travel) {
        const bool was = (pad.axisHeld & padKeyBit(key)) != 0;
        const bool now = travel > padAxisPress || (was && travel > padAxisRelease);
        if (now) pad.axisHeld |= padKeyBit(key); else pad.axisHeld &= ~padKeyBit(key);
        if (now) held |= padKeyBit(key);
    };
    axis(PadKey::leftLeft, -int(pad.leftX));
    axis(PadKey::leftRight, pad.leftX);
    axis(PadKey::leftUp, -int(pad.leftY));
    axis(PadKey::leftDown, pad.leftY);
    // The d-pad wins over the left stick on its axis.
    const auto bits = [](PadKey a, PadKey b) { return padKeyBit(a) | padKeyBit(b); };
    if (held & bits(PadKey::dpadLeft, PadKey::dpadRight)) held &= ~bits(PadKey::leftLeft, PadKey::leftRight);
    if (held & bits(PadKey::dpadUp, PadKey::dpadDown)) held &= ~bits(PadKey::leftUp, PadKey::leftDown);
    axis(PadKey::rightLeft, -int(pad.rightX));
    axis(PadKey::rightRight, pad.rightX);
    axis(PadKey::rightUp, -int(pad.rightY));
    axis(PadKey::rightDown, pad.rightY);
    axis(PadKey::leftTrigger, pad.leftTrigger);
    axis(PadKey::rightTrigger, pad.rightTrigger);
    return held;
}

#endif
