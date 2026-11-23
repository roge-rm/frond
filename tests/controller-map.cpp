// The game controller map (ControllerMap.h) and how the core applies it (Gamepad.h): saved text,
// each device's buttons and defaults, and that the defaults press what the fixed mapping did.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>

#include "Gamepad.h"

static const DeviceProfile5& named(const char* name) {
    const auto* profile = deviceProfileNamed5(name);
    assert(profile);
    return *profile;
}
static bool listed(const DeviceProfile5& profile, PalmKey key) {
    const auto keys = controllerPalmKeys(profile);
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}
static PadReading buttons(std::initializer_list<PadKey> keys) {
    PadReading pad;
    for (auto key : keys) pad.buttons |= uint32_t(1) << int(key);
    return pad;
}
static GamepadInput apply(const ControllerMap& map, PadReading pad, bool zodiac, bool attached = false) {
    return gamepadInput(map, padKeysHeld(pad), pad.leftX, pad.leftY, zodiac, attached);
}

static void text() {
    const auto& tx = named("palm-tx");
    for (int i = 0; i < int(PadKey::count); ++i) {
        PadKey back;
        assert(padKeyFromId(padKeyId(PadKey(i)), back) && back == PadKey(i) && *padKeyName(PadKey(i)));
    }
    for (int i = 0; i < int(PalmKey::count); ++i) {
        PalmKey back;
        assert(palmKeyFromId(palmKeyId(PalmKey(i)), back) && back == PalmKey(i));
    }
    ControllerMap map;
    assert(map.encode() == "none");
    assert(decodeControllerMap("none", tx).keys.empty() && decodeControllerMap("", tx).keys.empty());
    map.assign(PalmKey::app1, PadKey::a);
    map.assign(PalmKey::up, PadKey::dpadUp);
    map.assign(PalmKey::up, PadKey::leftUp);
    assert(map.encode() == "a:app1,dpup:up,lefty-:up");
    assert(decodeControllerMap(map.encode(), tx) == map);
    // One key presses one button: assigning it again moves it.
    map.assign(PalmKey::app2, PadKey::a);
    assert(map.keysFor(PalmKey::app1).empty() && map.keysFor(PalmKey::app2) == std::vector<PadKey>{PadKey::a});
    assert((map.keysFor(PalmKey::up) == std::vector<PadKey>{PadKey::dpadUp, PadKey::leftUp}));
    map.remove(PadKey::dpadUp);
    assert(map.keysFor(PalmKey::up) == std::vector<PadKey>{PadKey::leftUp});
    // Malformed entries, unknown names and buttons the device doesn't list are dropped.
    const auto odd = decodeControllerMap("a:app1,,junk,b:nothing,nokey:app2,x:stick-press,y:menu,start:app4", tx);
    assert(odd.encode() == "a:app1,start:app4");
}

static void lists() {
    const auto& tx = named("palm-tx");
    const auto& e2 = named("palm-e2");
    const auto& tg50 = named("sony-clie-tg50");
    const auto& zodiac = named("tapwave-zodiac-2");
    // Menu and a silkscreen Home are taps, not keys; the TG50's Home is a key.
    assert(listed(tx, PalmKey::centre) && listed(tx, PalmKey::power) && !listed(tx, PalmKey::home));
    assert(!listed(e2, PalmKey::home) && listed(e2, PalmKey::app4));
    assert(listed(tg50, PalmKey::home) && listed(tg50, PalmKey::padHold) && listed(tg50, PalmKey::padMenu));
    assert(!listed(tx, PalmKey::padA));
    assert(controllerPalmKeys(zodiac).size() == 13 && listed(zodiac, PalmKey::stickPress) && !listed(zodiac, PalmKey::power));
    assert(palmKeyLabel(PalmKey::app1, tx) == "Home" && palmKeyLabel(PalmKey::app4, e2) == "Note Pad");
    assert(palmKeyLabel(PalmKey::app2, zodiac) == "Action 2 (right)" && palmKeyLabel(PalmKey::up, zodiac) == "Stick up");
    // frond-68k reads no game controller, so a 68k device lists nothing.
    assert(controllerPalmKeys(named("palm-m515")).empty() && controllerPalmKeys(named("sony-clie-t415")).empty());
    // Every listed button is a key on the device's own strip (Power and Voice can sit off it, on the side),
    // and the defaults only use listed buttons.
    for (const auto& profile : deviceProfiles5) {
        if (profile.deviceType == deviceTypeInvalid) continue;
        const auto keys = controllerPalmKeys(profile);
        const auto& strip = deviceControls(profile.art, profile.frontend.clieGamepad);
        for (auto key : keys) {
            const KeyId id = palmKeyToKeyId(key);
            bool found = key == PalmKey::power || key == PalmKey::voice ||
                         (zodiacController(profile) && key <= PalmKey::right);  // the Zodiac's stick
            for (size_t i = 0; i < strip.count; ++i) found |= strip.buttons[i].key == id;
            if (!found) std::fprintf(stderr, "%s: %s isn't on its strip\n", profile.name, palmKeyId(key));
            assert(found);
        }
        for (const auto& entry : controllerDefaults(profile).keys) assert(listed(profile, entry.second));
        const auto defaults = controllerDefaults(profile);
        assert(decodeControllerMap(defaults.encode(), profile) == defaults);
    }
    assert(controllerDefaults(tx).keys.empty() && controllerDefaults(e2).keys.empty());
}

// The defaults press what the fixed mapping did before maps.
static void zodiacDefaults() {
    const auto map = controllerDefaults(named("tapwave-zodiac-2"));
    const std::pair<PadKey, KeyId> fixed[] = {
        {PadKey::a, keyIdHard1}, {PadKey::b, keyIdHard2}, {PadKey::x, keyIdHard3}, {PadKey::y, keyIdHard4},
        {PadKey::leftShoulder, keyIdTriggerLeft}, {PadKey::rightShoulder, keyIdTriggerRight},
        {PadKey::leftStick, keyIdStickPress}, {PadKey::start, keyIdHome}, {PadKey::guide, keyIdHome},
        {PadKey::back, keyIdFunction}};
    for (const auto& [button, key] : fixed) {
        const auto input = apply(map, buttons({button}), true);
        assert(input.keys == deviceKeyBit(key) && !input.stick.held);
    }
    for (auto unused : {PadKey::rightStick, PadKey::misc1, PadKey::paddle1, PadKey::touchpad})
        assert(apply(map, buttons({unused}), true).keys == 0);
    PadReading right;
    right.rightX = 32767;
    assert(apply(map, right, true).keys == deviceKeyBit(keyIdHard2));
    right.rightX = 0; right.rightY = -32768;
    assert(apply(map, right, true).keys == deviceKeyBit(keyIdHard4));
    // Press past half travel, let go at about a third.
    PadReading held;
    auto down = [&](int y) {
        held.rightY = int16_t(y);
        return (gamepadInput(map, padKeysHeld(held), 0, 0, true, false).keys & deviceKeyBit(keyIdHard1)) != 0;
    };
    assert(!down(16384) && down(16385) && down(12000) && down(11500) && !down(11000) && !down(12000));
    // The d-pad pushes the stick to full travel on its axis, over the left stick.
    auto dpad = buttons({PadKey::dpadLeft});
    dpad.leftX = 32767; dpad.leftY = 32767;
    const auto stick = apply(map, dpad, true).stick;
    assert(stick.held && stick.x == -1 && stick.y == 1);
    assert(apply(map, dpad, true).keys == 0);
    PadReading rest;
    rest.leftX = 3000; rest.leftY = -2000;
    assert(!apply(map, rest, true).stick.held);
    // Moved elsewhere, a d-pad button presses that instead, and the stick stays analogue.
    ControllerMap moved = map;
    moved.assign(PalmKey::app1, PadKey::dpadUp);
    const auto pressed = apply(moved, buttons({PadKey::dpadUp}), true);
    assert(pressed.keys == deviceKeyBit(keyIdHard1) && !pressed.stick.held);
}

static void clieDefaults() {
    const auto& tg50 = named("sony-clie-tg50");
    const auto map = controllerDefaults(tg50);
    const std::pair<PadKey, KeyId> fixed[] = {
        {PadKey::a, keyIdPadA}, {PadKey::b, keyIdPadB}, {PadKey::x, keyIdPadX}, {PadKey::y, keyIdPadY},
        {PadKey::start, keyIdPadHome}, {PadKey::guide, keyIdPadHome}, {PadKey::back, keyIdPadMenu},
        {PadKey::rightShoulder, keyIdPadHold}, {PadKey::dpadUp, keyIdPadUp}, {PadKey::dpadDown, keyIdPadDown},
        {PadKey::dpadLeft, keyIdPadLeft}, {PadKey::dpadRight, keyIdPadRight}};
    for (const auto& [button, key] : fixed) {
        assert(apply(map, buttons({button}), false, true).keys == deviceKeyBit(key));
        // Nothing while the game controller isn't attached.
        assert(apply(map, buttons({button}), false, false).keys == 0);
    }
    PadReading stick;
    stick.leftX = -20000;
    assert(apply(map, stick, false, true).keys == deviceKeyBit(keyIdPadLeft));
    stick.leftX = 0; stick.leftY = 20000;
    assert(apply(map, stick, false, true).keys == deviceKeyBit(keyIdPadDown));
    assert(apply(map, buttons({PadKey::leftShoulder}), false, true).keys == 0);
    // The TG50's own buttons can be added and work with the controller detached.
    ControllerMap own = map;
    own.assign(PalmKey::app1, PadKey::leftShoulder);
    assert(apply(own, buttons({PadKey::leftShoulder}), false, false).keys == deviceKeyBit(keyIdHard1));
}

static void triggers() {
    ControllerMap map;
    map.assign(PalmKey::up, PadKey::leftTrigger);
    PadReading pad;
    pad.leftTrigger = 30000;
    assert(apply(map, pad, false).keys == deviceKeyBit(keyIdUp));
    pad.leftTrigger = 10000;
    assert(apply(map, pad, false).keys == 0);
}

int main() {
    text();
    lists();
    zodiacDefaults();
    clieDefaults();
    triggers();
    std::puts("controller-map: ok");
    return 0;
}
