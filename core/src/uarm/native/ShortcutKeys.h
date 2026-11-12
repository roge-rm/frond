#ifndef _SHORTCUT_KEYS_H_
#define _SHORTCUT_KEYS_H_

#include <SDL.h>

#include <optional>
#include <string>

// The keys of the Palm window that belong to the desktop app: F11, F12, and Ctrl with a letter or
// digit (Shift allowed, Alt not). They are sent to the app as a shortcut event ("ctrl+shift+s")
// and never reach the Palm. With the infrared keyboard attached, Ctrl keys go to its driver and
// only F11 and F12 are shortcuts.
inline std::optional<std::string> shortcutName(SDL_Keycode sym, Uint16 mod, bool keyboardAttached) {
    if (sym == SDLK_F11) return "f11";
    if (sym == SDLK_F12) return "f12";
    if (keyboardAttached || !(mod & KMOD_CTRL) || (mod & (KMOD_ALT | KMOD_GUI))) return std::nullopt;
    const bool letter = sym >= SDLK_a && sym <= SDLK_z, digit = sym >= SDLK_0 && sym <= SDLK_9;
    if (!letter && !digit) return std::nullopt;
    return std::string("ctrl+") + ((mod & KMOD_SHIFT) ? "shift+" : "") + char(sym);
}

#endif  // _SHORTCUT_KEYS_H_
