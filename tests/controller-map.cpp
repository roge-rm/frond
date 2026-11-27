// The game controller map (ControllerMap.h) and how the core applies it (Gamepad.h): saved text,
// each device's buttons and defaults, and that the defaults press what the fixed mapping did.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>
#include <tuple>

#include "Gamepad.h"
#include "Controllers68k.h"

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
    int16_t x, y;
    padStickAxes(pad, map, x, y);
    return gamepadInput(map, padKeysHeld(pad), x, y, zodiac, attached);
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
    // Every listed button is a key on the device's own strip (Power and Voice can sit off it, on the side),
    // and the defaults only use listed buttons.
    for (const auto& profile : deviceProfiles5) {
        if (profile.deviceType == deviceTypeInvalid) continue;
        const auto keys = controllerPalmKeys(profile);
        const auto& strip = deviceControls(profile.art, profile.frontend.clieGamepad);
        for (auto key : keys) {
            const KeyId id = palmKeyToKeyId(key);
            // The Zaurus keys are on its own panel, not a strip.
            bool found = key == PalmKey::power || key == PalmKey::voice || profile.deviceType == deviceTypeSpitz ||
                         (zodiacController(profile) && key <= PalmKey::right);  // the Zodiac's stick
            for (size_t i = 0; i < strip.count; ++i) found |= strip.buttons[i].key == id;
            if (!found) std::fprintf(stderr, "%s: %s isn't on its strip\n", profile.name, palmKeyId(key));
            assert(found);
        }
        for (const auto& entry : controllerDefaults(profile).keys) assert(listed(profile, entry.second));
        const auto defaults = controllerDefaults(profile);
        assert(decodeControllerMap(defaults.encode(), profile) == defaults);
    }
    // Android's defaults, only the buttons the device has: the T|X has no Home key, so Start is its
    // first app button; the E2's Home is on the silkscreen and Select's Menu is a tap, so neither is mapped.
    assert(controllerDefaults(tx).encode() ==
           "a:centre,x:app2,y:app3,start:app1,leftshoulder:app1,rightshoulder:app4,dpup:up,dpdown:down,dpleft:left,dpright:right");
    assert(controllerDefaults(e2).encode() ==
           "a:centre,x:app2,y:app3,leftshoulder:app1,rightshoulder:app4,dpup:up,dpdown:down,dpleft:left,dpright:right");
    const auto rx1950 = controllerDefaults(named("hp-ipaq-rx1950"));
    assert(rx1950.keys.at(PadKey::a) == PalmKey::centre && rx1950.keys.at(PadKey::start) == PalmKey::app1);
    assert(!rx1950.keys.count(PadKey::b) && !rx1950.keys.count(PadKey::back));
    const auto zire72 = controllerDefaults(named("palm-zire-72"));
    assert(zire72.keys.size() == 9 && !zire72.keys.count(PadKey::start));
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

// Which controller stick moves the Zodiac's stick: left by default, right or none from the map.
static void zodiacStick() {
    const auto& zodiac = named("tapwave-zodiac-2");
    const auto defaults = controllerDefaults(zodiac);
    assert(defaults.preset() == PadStick::left && defaults.encode().find(",stick:") == std::string::npos);
    assert(palmKeyLabel(PalmKey::stickPress, zodiac) == "Stick press (centre)");
    // Saved as an entry of its own, which a core without it skips as an unknown key.
    ControllerMap right = defaults;
    right.setStick(PadStick::right);
    assert(right.preset() == PadStick::right && right != defaults);
    for (auto key : {PadKey::rightLeft, PadKey::rightRight, PadKey::rightUp, PadKey::rightDown})
        assert(!right.keys.count(key));
    assert(right.encode().find("stick:right") != std::string::npos);
    assert(decodeControllerMap(right.encode(), zodiac) == right);
    assert(decodeControllerMap("stick:none", zodiac).preset() == PadStick::none);
    assert(decodeControllerMap("stick:none", zodiac).keys.empty());
    assert(decodeControllerMap("a:app1,stick:sideways", zodiac).preset() == PadStick::left);
    ControllerMap none;
    none.setStick(PadStick::none);
    assert(none.encode() == "stick:none");
    // Right: the right stick moves it over its range and the left stick does nothing.
    PadReading pad;
    pad.rightX = 32767; pad.rightY = -32768;
    auto input = apply(right, pad, true);
    assert(input.stick.held && input.stick.x == 1 && input.stick.y == -1 && input.keys == 0);
    pad = {};
    pad.leftX = 32767;
    assert(!apply(right, pad, true).stick.held);
    // Left keeps the old behaviour; none leaves the stick to the d-pad.
    assert(apply(defaults, pad, true).stick.held && apply(defaults, pad, true).stick.x == 1);
    ControllerMap off = defaults;
    off.setStick(PadStick::none);
    assert(!apply(off, pad, true).stick.held);
    pad.rightX = 32767;
    assert(apply(off, pad, true).keys == deviceKeyBit(keyIdHard2));
    pad.buttons = uint32_t(1) << int(PadKey::dpadLeft);
    assert(apply(off, pad, true).stick.held && apply(off, pad, true).stick.x == -1);
}

// Each stick axis from any controller axis, with invert; presets and old saved values load as before.
static void zodiacStickAxes() {
    const auto& zodiac = named("tapwave-zodiac-2");
    // Old saved values: nothing, "stick:right" and "stick:none" are the presets they were.
    assert(decodeControllerMap("none", zodiac).preset() == PadStick::left);
    assert(decodeControllerMap("a:app1", zodiac).stickX == StickAxis({PadAxis::leftX, false}));
    const auto right = decodeControllerMap("a:app1,stick:right", zodiac);
    assert(right.stickX == StickAxis({PadAxis::rightX, false}) && right.stickY == StickAxis({PadAxis::rightY, false}));
    assert(decodeControllerMap("stick:none", zodiac).stickX.axis == PadAxis::none);
    // A custom pair: right stick X, left stick Y upside down. The keys at those axes' ends come off.
    ControllerMap map = controllerDefaults(zodiac);
    map.assign(PalmKey::app1, PadKey::rightLeft);
    map.assign(PalmKey::app2, PadKey::leftUp);
    map.setStickAxis(false, PadAxis::rightX, false);
    map.setStickAxis(true, PadAxis::leftY, true);
    assert(map.preset() == PadStick::custom && !map.keys.count(PadKey::rightLeft) && !map.keys.count(PadKey::leftUp));
    const std::string text = map.encode();
    assert(text.find("stickx:rightx") != std::string::npos && text.find("sticky:-lefty") != std::string::npos);
    assert(text.find(",stick:") == std::string::npos);
    assert(decodeControllerMap(text, zodiac) == map);
    PadReading pad;
    pad.rightX = -32768; pad.leftY = -20000; pad.leftX = 32767;
    auto input = apply(map, pad, true);
    assert(input.stick.held && input.stick.x == -1 && input.stick.y > 0.5f && input.stick.y < 0.7f);
    // Invert at the end of travel stays in range.
    pad = {};
    pad.leftY = -32768;
    input = apply(map, pad, true);
    assert(input.stick.held && input.stick.y == 1);
    // A trigger can be an axis; the other axis can be none.
    map.setStickAxis(false, PadAxis::none, true);
    map.setStickAxis(true, PadAxis::rightTrigger, false);
    assert(map.stickX == StickAxis({PadAxis::none, false}) && map.usesAxis(PadAxis::rightTrigger) && !map.usesAxis(PadAxis::none));
    assert(!map.keys.count(PadKey::rightTrigger));
    assert(decodeControllerMap(map.encode(), zodiac) == map);
    pad = {};
    pad.rightTrigger = 32767;
    input = apply(map, pad, true);
    assert(input.stick.held && input.stick.x == 0 && input.stick.y == 1);
    // Choosing a preset afterwards resets both axes; bad axis names are skipped.
    map.setStick(PadStick::left);
    assert(map.preset() == PadStick::left);
    assert(decodeControllerMap("stickx:sideways,sticky:-", zodiac).preset() == PadStick::left);
    // Keys at an axis the text binds come off, whichever comes first in the text.
    for (const char* text : {"lefty-:app2,lefty+:app3,leftx-:app1,stickx:rightx,sticky:lefty",
                             "stickx:rightx,sticky:lefty,lefty-:app2,lefty+:app3,leftx-:app1"}) {
        const auto decoded = decodeControllerMap(text, zodiac);
        assert(!decoded.keys.count(PadKey::leftUp) && !decoded.keys.count(PadKey::leftDown));
        assert(decoded.keys.count(PadKey::leftLeft));
    }
}

static void clieDefaults() {
    const auto& tg50 = named("sony-clie-tg50");
    const auto map = controllerDefaults(tg50);
    // Attached, then detached: the arrows, Home, X, Y and Hold go to the TG50's own buttons.
    const std::tuple<PadKey, KeyId, KeyId> fixed[] = {
        {PadKey::a, keyIdPadA, keyInvalid}, {PadKey::b, keyIdPadB, keyInvalid}, {PadKey::x, keyIdPadX, keyIdHard2},
        {PadKey::y, keyIdPadY, keyIdHard3}, {PadKey::start, keyIdPadHome, keyIdHome}, {PadKey::guide, keyIdPadHome, keyIdHome},
        {PadKey::back, keyIdPadMenu, keyInvalid}, {PadKey::rightShoulder, keyIdPadHold, keyIdHard4},
        {PadKey::dpadUp, keyIdPadUp, keyIdUp}, {PadKey::dpadDown, keyIdPadDown, keyIdDown},
        {PadKey::dpadLeft, keyIdPadLeft, keyInvalid}, {PadKey::dpadRight, keyIdPadRight, keyInvalid}};
    for (const auto& [button, key, detached] : fixed) {
        assert(apply(map, buttons({button}), false, true).keys == deviceKeyBit(key));
        assert(apply(map, buttons({button}), false, false).keys == (detached ? deviceKeyBit(detached) : 0));
    }
    PadReading stick;
    stick.leftX = -20000;
    assert(apply(map, stick, false, true).keys == deviceKeyBit(keyIdPadLeft));
    stick.leftX = 0; stick.leftY = 20000;
    assert(apply(map, stick, false, true).keys == deviceKeyBit(keyIdPadDown));
    // The d-pad wins over the stick on its axis.
    PadReading both = buttons({PadKey::dpadLeft});
    both.leftX = 20000; both.leftY = 20000;
    assert(apply(map, both, false, true).keys == (deviceKeyBit(keyIdPadLeft) | deviceKeyBit(keyIdPadDown)));
    // L1 is the first app button either way.
    assert(apply(map, buttons({PadKey::leftShoulder}), false, true).keys == deviceKeyBit(keyIdHard1));
    assert(apply(map, buttons({PadKey::leftShoulder}), false, false).keys == deviceKeyBit(keyIdHard1));
    PadReading trigger;
    trigger.leftTrigger = 30000;
    assert(apply(map, trigger, false, true).keys == 0);
    // The TG50's own buttons can be added.
    ControllerMap own = map;
    own.assign(PalmKey::app4, PadKey::leftTrigger);
    assert(apply(own, trigger, false, false).keys == deviceKeyBit(keyIdHard4));
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

// The 68k devices list their own buttons as Android does, with Android's defaults.
static void devices68k() {
    using K = PalmKey;
    const auto& iiix = named("palm-iiix");
    const auto& m515 = named("palm-m515");
    const auto& t415 = named("sony-clie-t415");
    auto keys = [](const DeviceProfile5& profile) {
        std::string text;
        for (auto key : controllerPalmKeys(profile)) text += std::string(text.empty() ? "" : "|") + palmKeyId(key);
        return text;
    };
    assert(keys(iiix) == "up|down|app1|app2|app3|app4|power");
    assert(keys(m515) == "up|down|app1|app2|app3|app4");
    assert(keys(t415) == "up|down|app1|app2|app3|app4|jog-up|jog-down|jog-press|back");
    assert(controllerPalmKeys(named("alphasmart-dana")).empty() && controllerDefaults(named("alphasmart-dana")).keys.empty());
    assert(palmKeyLabel(K::app1, iiix) == "Date Book" && palmKeyLabel(K::app4, m515) == "Note Pad");
    assert(palmKeyLabel(K::jogPress, t415) == "Jog press");
    // No Home key, so Start is the first app button; Power has no default.
    assert(controllerDefaults(iiix).encode() ==
           "x:app2,y:app3,start:app1,leftshoulder:app1,rightshoulder:app4,dpup:up,dpdown:down");
    assert(controllerDefaults(m515) == controllerDefaults(iiix));
    assert(controllerDefaults(t415).encode() ==
           "x:app2,y:app3,start:app1,leftstick:back,rightstick:jog-press,leftshoulder:app1,rightshoulder:app4,"
           "dpup:up,dpdown:down,lefttrigger:jog-up,righttrigger:jog-down");
    // Buttons the device doesn't have are dropped from a saved map.
    assert(decodeControllerMap("a:centre,b:power,x:jog-up", iiix).encode() == "b:power");
}

// The 68k runner's controllers: presses and releases through the map, counted per key across controllers.
static void runner68k() {
    Controllers68k pads(named("sony-clie-t415"));
    std::string log;
    auto apply = [&](KeyId key, bool down) { log += std::to_string(int(key)) + (down ? "+" : "-"); };
    auto said = [&](const std::string& want) { const bool same = log == want; log.clear(); return same; };
    auto press = [&](int pad, PadKey key, bool down) {
        auto& reading = pads.Pad(pad);
        if (down) reading.buttons |= uint32_t(1) << int(key);
        else reading.buttons &= ~(uint32_t(1) << int(key));
        pads.Update(apply);
    };
    const std::string hard1 = std::to_string(int(keyIdHard1)), jogUp = std::to_string(int(keyIdJogUp));
    press(0, PadKey::leftShoulder, true);
    assert(said(hard1 + "+"));
    // Start is the first app button too, and a second controller holding it changes nothing.
    press(0, PadKey::start, true);
    press(1, PadKey::leftShoulder, true);
    assert(said(""));
    press(0, PadKey::leftShoulder, false);
    press(0, PadKey::start, false);
    assert(said(""));
    pads.Remove(1, apply);
    assert(said(hard1 + "-"));
    // L2 past half travel turns the Jog Dial up.
    pads.Pad(0).leftTrigger = 30000;
    pads.Update(apply);
    assert(said(jogUp + "+"));
    pads.SetFocused(false, apply);
    assert(said(jogUp + "-"));
    pads.SetFocused(true, apply);
    assert(said(jogUp + "+"));
    // A new map lets go of what it no longer holds.
    pads.SetMap("none", apply);
    assert(said(jogUp + "-") && !pads.Wanted());
    pads.SetMap("lefttrigger:back", apply);
    assert(said(std::to_string(int(keyIdBack)) + "+"));
    // After the runner lets go of everything itself, a held button presses again only once pressed again.
    pads.Pad(0).leftTrigger = 0;
    pads.Update(apply);
    log.clear();
    press(0, PadKey::a, true);
    pads.SetMap("a:app2", apply);
    assert(said(std::to_string(int(keyIdHard2)) + "+"));
    pads.Forget();
    pads.Update(apply);
    assert(said(""));
    press(0, PadKey::a, true);
    assert(said(std::to_string(int(keyIdHard2)) + "+"));
}

int main() {
    text();
    lists();
    zodiacDefaults();
    zodiacStick();
    zodiacStickAxes();
    clieDefaults();
    triggers();
    devices68k();
    runner68k();
    std::puts("controller-map: ok");
    return 0;
}
