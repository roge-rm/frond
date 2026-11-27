#pragma once

#include <optional>
#include <sstream>
#include <string>

#include "DeviceControls.h"
#include "WindowLayout.h"

// A change to how the window shows the device, from the display control command. Fields
// left unset keep their current value.
struct DisplayChange {
    std::optional<WindowFit> fit;
    std::optional<bool> smooth;
    std::optional<bool> keyboard;
    std::optional<JogPlacement> jog;
    std::optional<JogLandscape> jogLandscape;
    // Draw the device's button bar art; off leaves the screen and silkscreen only.
    std::optional<bool> buttons;
    // The CLIÉ game controller attached; from the core itself, not the display command.
    std::optional<bool> gamepad;
    std::optional<ZaurusStyle> zaurus;

    // Later fields win.
    void Merge(const DisplayChange& later) {
        if (later.fit) fit = later.fit;
        if (later.smooth) smooth = later.smooth;
        if (later.keyboard) keyboard = later.keyboard;
        if (later.jog) jog = later.jog;
        if (later.jogLandscape) jogLandscape = later.jogLandscape;
        if (later.buttons) buttons = later.buttons;
        if (later.gamepad) gamepad = later.gamepad;
        if (later.zaurus) zaurus = later.zaurus;
    }
};

// What the window shows now; DisplayChange applies to it.
struct DisplaySettings {
    WindowFit fit{WindowFit::none};
    bool smooth{false};
    bool keyboard{true};
    JogPlacement jog{JogPlacement::screen};
    bool gamepad{false};
    JogLandscape jogLandscape{JogLandscape::beside};
    bool buttons{true};

    void Apply(const DisplayChange& change) {
        if (change.fit) fit = *change.fit;
        if (change.smooth) smooth = *change.smooth;
        if (change.keyboard) keyboard = *change.keyboard;
        if (change.jog) jog = *change.jog;
        if (change.jogLandscape) jogLandscape = *change.jogLandscape;
        if (change.buttons) buttons = *change.buttons;
        if (change.gamepad) gamepad = *change.gamepad;
    }
};

// Parses "fit=none|fit|whole|fill smooth=on|off keyboard=on|off jog=hidden|screen|keyboard
// jog-landscape=hidden|beside|under|over buttons=on|off", each field optional but at least one given. keyboard
// and the jog fields need a device with them
// (controls). Returns false with error set for anything else.
inline bool parseDisplayChange(const std::string& text, bool controls, DisplayChange& change, std::string& error) {
    change = {};
    std::istringstream words(text);
    std::string word;
    bool any = false;
    auto onOff = [](const std::string& value, std::optional<bool>& out) {
        if (value == "on") out = true;
        else if (value == "off") out = false;
        else return false;
        return true;
    };
    while (words >> word) {
        const auto equals = word.find('=');
        const std::string name = word.substr(0, equals);
        const std::string value = equals == std::string::npos ? std::string() : word.substr(equals + 1);
        bool ok = equals != std::string::npos;
        if (ok && name == "fit") {
            WindowFit fit;
            if (value == "none") fit = WindowFit::none;
            else ok = parseWindowFit(value, fit);
            if (ok) change.fit = fit;
        } else if (ok && name == "smooth") {
            ok = onOff(value, change.smooth);
        } else if (ok && name == "buttons") {
            ok = onOff(value, change.buttons);
        } else if (ok && (name == "keyboard" || name == "jog" || name == "jog-landscape")) {
            if (!controls) {
                error = "This device has no " + name + " controls.";
                return false;
            }
            if (name == "keyboard") ok = onOff(value, change.keyboard);
            else if (name == "jog-landscape") {
                JogLandscape jog;
                ok = parseJogLandscape(value, jog);
                if (ok) change.jogLandscape = jog;
            } else {
                JogPlacement jog;
                ok = parseJogPlacement(value, jog);
                if (ok) change.jog = jog;
            }
        } else {
            ok = false;
        }
        if (!ok) {
            error = "Invalid display setting: " + word;
            return false;
        }
        any = true;
    }
    if (!any) {
        error = "Use fit=, smooth=, keyboard=, jog=, jog-landscape= or buttons=.";
        return false;
    }
    return true;
}
