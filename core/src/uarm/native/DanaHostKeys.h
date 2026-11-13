#ifndef _DANA_HOST_KEYS_H_
#define _DANA_HOST_KEYS_H_

#include <SDL.h>

#include <map>
#include <string>
#include <vector>

#include "../../common/dana_input5.h"

// Host keys for the Dana keys a laptop keyboard lacks. Ctrl+F1 to F8 are calendar, address, todo,
// memo, find, menu, apps and send, Ctrl+Esc is power, and Ctrl+Shift+F1 to F8 are the same with
// function held. PgUp and PgDn are function+up and function+down. The Dana's ctrl press (and shift
// while ctrl is held) is held back until a key that isn't a chord comes or the key is let go alone,
// so the guest never sees ctrl or shift during a chord. Alt and GUI pass straight through and don't
// send the held back keys. Each host key releases what it pressed, even when Ctrl, Shift or Alt is
// let go first.
struct DanaHostChange {
    std::string name;
    DanaSource5 source;
    bool down;
};

// SDL on X11 can drop the release of a key typed in a burst that shares one timestamp, and then
// reports the key's next press as a repeat. A repeat sooner than a real auto-repeat delay after
// the key's own press is that next press, so Retap says it should go in as a release and a press.
class DanaRepeats {
public:
    static constexpr uint32_t kRepeatMs = 150;
    bool Retap(SDL_Scancode scancode, bool repeat, uint32_t nowMs) {
        if (!repeat) {
            downAt[scancode] = nowMs;
            return false;
        }
        const auto it = downAt.find(scancode);
        return it != downAt.end() && nowMs - it->second < kRepeatMs;
    }
    void Clear() { downAt.clear(); }

private:
    std::map<SDL_Scancode, uint32_t> downAt;
};

class DanaHostKeys {
public:
    // name is the Dana key the host key has on its own. Returns the changes to apply in order.
    std::vector<DanaHostChange> Event(SDL_Scancode scancode, bool down, const std::string& name) {
        std::vector<DanaHostChange> out;
        if (Modifier* mod = Held(scancode)) {
            const std::string key = scancode == SDL_SCANCODE_LCTRL || scancode == SDL_SCANCODE_RCTRL ? "ctrl" : name;
            const DanaSource5 source = (scancode == SDL_SCANCODE_RCTRL || scancode == SDL_SCANCODE_RSHIFT) ? DanaSource5::physicalRight : DanaSource5::physical;
            if (down) {
                *mod = {true, false, false};
                // Shift is only held back with ctrl, since a chord needs both.
                if (IsShift(scancode) && !CtrlHeld()) {
                    mod->sent = true;
                    out.push_back({key, source, true});
                }
            } else {
                if (mod->sent) out.push_back({key, source, false});
                else if (mod->held && !mod->chorded) {
                    out.push_back({key, source, true});
                    out.push_back({key, source, false});
                }
                *mod = {};
            }
            return out;
        }
        if (!down) {
            const auto it = pressed.find(scancode);
            if (it == pressed.end()) {
                out.push_back({name, Source(scancode), false});
            } else {
                out = it->second.release;
                if (it->second.function && --chordFunction == 0) out.push_back({"function", DanaSource5::physicalChord, false});
                pressed.erase(it);
            }
            return out;
        }
        if (pressed.count(scancode)) return out;
        const char* chord = CtrlHeld() ? ChordName(scancode) : nullptr;
        if (chord) {
            const bool function = ShiftHeld() && scancode != SDL_SCANCODE_ESCAPE;
            // Ctrl and shift already sent come up before the app key goes down.
            for (Modifier* mod : {&lctrl, &rctrl, &lshift, &rshift}) {
                if (mod->sent) out.push_back(Change(mod, false));
                if (mod->held) mod->chorded = true;
                mod->sent = false;
            }
            if (function && chordFunction++ == 0) out.push_back({"function", DanaSource5::physicalChord, true});
            out.push_back({chord, DanaSource5::physicalChord, true});
            pressed[scancode] = {{{chord, DanaSource5::physicalChord, false}}, function};
        } else if (scancode == SDL_SCANCODE_PAGEUP || scancode == SDL_SCANCODE_PAGEDOWN) {
            const std::string arrow = scancode == SDL_SCANCODE_PAGEUP ? "up" : "down";
            Flush(out);
            out.push_back({"function", DanaSource5::physicalPage, true});
            out.push_back({arrow, DanaSource5::physicalPage, true});
            pressed[scancode] = {{{arrow, DanaSource5::physicalPage, false}, {"function", DanaSource5::physicalPage, false}}, false};
        } else {
            if (!IsPassThrough(scancode)) Flush(out);
            out.push_back({name, Source(scancode), true});
        }
        return out;
    }

    // Sends the ctrl and shift presses held back, for input that didn't come from this keyboard.
    std::vector<DanaHostChange> Flush() {
        std::vector<DanaHostChange> out;
        Flush(out);
        return out;
    }

    // The window lost focus, or anything else released all keys.
    void Clear() {
        lctrl = rctrl = lshift = rshift = {};
        pressed.clear();
        chordFunction = 0;
    }

    static const char* ChordName(SDL_Scancode scancode) {
        switch (scancode) {
            case SDL_SCANCODE_F1: return "calendar";
            case SDL_SCANCODE_F2: return "address";
            case SDL_SCANCODE_F3: return "todo";
            case SDL_SCANCODE_F4: return "memo";
            case SDL_SCANCODE_F5: return "find";
            case SDL_SCANCODE_F6: return "menu";
            case SDL_SCANCODE_F7: return "apps";
            case SDL_SCANCODE_F8: return "send";
            case SDL_SCANCODE_ESCAPE: return "power";
            default: return nullptr;
        }
    }

private:
    struct Modifier {
        bool held{false}, sent{false}, chorded{false};
    };
    struct Pressed {
        std::vector<DanaHostChange> release;
        bool function;
    };

    Modifier* Held(SDL_Scancode scancode) {
        switch (scancode) {
            case SDL_SCANCODE_LCTRL: return &lctrl;
            case SDL_SCANCODE_RCTRL: return &rctrl;
            case SDL_SCANCODE_LSHIFT: return &lshift;
            case SDL_SCANCODE_RSHIFT: return &rshift;
            default: return nullptr;
        }
    }
    static bool IsShift(SDL_Scancode scancode) { return scancode == SDL_SCANCODE_LSHIFT || scancode == SDL_SCANCODE_RSHIFT; }
    static bool IsPassThrough(SDL_Scancode scancode) {
        return scancode == SDL_SCANCODE_LALT || scancode == SDL_SCANCODE_RALT || scancode == SDL_SCANCODE_LGUI || scancode == SDL_SCANCODE_RGUI;
    }
    bool CtrlHeld() const { return lctrl.held || rctrl.held; }
    bool ShiftHeld() const { return lshift.held || rshift.held; }

    static DanaSource5 Source(SDL_Scancode scancode) {
        return (scancode == SDL_SCANCODE_RALT || scancode == SDL_SCANCODE_RGUI) ? DanaSource5::physicalRight : DanaSource5::physical;
    }

    DanaHostChange Change(const Modifier* mod, bool down) const {
        if (mod == &lctrl) return {"ctrl", DanaSource5::physical, down};
        if (mod == &rctrl) return {"ctrl", DanaSource5::physicalRight, down};
        if (mod == &lshift) return {"lshift", DanaSource5::physical, down};
        return {"rshift", DanaSource5::physicalRight, down};
    }

    // A key that isn't a chord sends the ctrl and shift presses held back.
    void Flush(std::vector<DanaHostChange>& out) {
        for (Modifier* mod : {&lctrl, &rctrl, &lshift, &rshift})
            if (mod->held && !mod->sent) {
                out.push_back(Change(mod, true));
                mod->sent = true;
            }
    }

    Modifier lctrl, rctrl, lshift, rshift;
    int chordFunction{0};
    std::map<SDL_Scancode, Pressed> pressed;
};

#endif  // _DANA_HOST_KEYS_H_
