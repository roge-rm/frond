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

    // Later fields win.
    void Merge(const DisplayChange& later) {
        if (later.fit) fit = later.fit;
        if (later.smooth) smooth = later.smooth;
        if (later.keyboard) keyboard = later.keyboard;
        if (later.jog) jog = later.jog;
    }
};

// What the window shows now; DisplayChange applies to it.
struct DisplaySettings {
    WindowFit fit{WindowFit::none};
    bool smooth{false};
    bool keyboard{true};
    JogPlacement jog{JogPlacement::screen};

    void Apply(const DisplayChange& change) {
        if (change.fit) fit = *change.fit;
        if (change.smooth) smooth = *change.smooth;
        if (change.keyboard) keyboard = *change.keyboard;
        if (change.jog) jog = *change.jog;
    }
};

// Parses "fit=none|fit|whole|fill smooth=on|off keyboard=on|off jog=hidden|screen|keyboard",
// each field optional but at least one given. keyboard and jog need a device with them
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
        } else if (ok && (name == "keyboard" || name == "jog")) {
            if (!controls) {
                error = "This device has no " + name + " controls.";
                return false;
            }
            if (name == "keyboard") ok = onOff(value, change.keyboard);
            else {
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
        error = "Use fit=, smooth=, keyboard= or jog=.";
        return false;
    }
    return true;
}
