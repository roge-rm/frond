// The Palm window's keys that go back to the desktop app (ShortcutKeys.h).
#include <cstdio>

#include "ShortcutKeys.h"

static int failures = 0;

static void expect(const char* what, SDL_Keycode sym, Uint16 mod, bool keyboard, const char* want, bool zaurus = false) {
    const auto got = shortcutName(sym, mod, keyboard, zaurus);
    const bool ok = want ? got && *got == want : !got;
    if (!ok) {
        std::printf("FAIL %s: got %s\n", what, got ? got->c_str() : "none");
        ++failures;
    }
}

int main() {
    expect("ctrl+s", SDLK_s, KMOD_LCTRL, false, "ctrl+s");
    expect("right ctrl+1", SDLK_1, KMOD_RCTRL, false, "ctrl+1");
    expect("ctrl+shift+s", SDLK_s, KMOD_LCTRL | KMOD_LSHIFT, false, "ctrl+shift+s");
    expect("ctrl+w", SDLK_w, KMOD_CTRL, false, "ctrl+w");
    expect("f11", SDLK_F11, 0, false, "f11");
    expect("f12", SDLK_F12, 0, false, "f12");
    expect("plain s", SDLK_s, 0, false, nullptr);
    expect("shift+s", SDLK_s, KMOD_LSHIFT, false, nullptr);
    expect("ctrl+alt+s", SDLK_s, KMOD_LCTRL | KMOD_LALT, false, nullptr);
    expect("ctrl+enter", SDLK_RETURN, KMOD_LCTRL, false, nullptr);
    expect("ctrl+f5", SDLK_F5, KMOD_LCTRL, false, nullptr);
    expect("f1", SDLK_F1, 0, false, nullptr);
    expect("escape", SDLK_ESCAPE, 0, false, nullptr);
    expect("keyboard ctrl+s", SDLK_s, KMOD_LCTRL, true, nullptr);
    expect("keyboard ctrl+1", SDLK_1, KMOD_LCTRL, true, nullptr);
    expect("keyboard f11", SDLK_F11, 0, true, "f11");
    expect("keyboard f12", SDLK_F12, KMOD_LCTRL, true, "f12");
    expect("zaurus f11", SDLK_F11, 0, false, "f11", true);
    expect("zaurus f12", SDLK_F12, 0, false, "f12", true);
    expect("zaurus ctrl+q", SDLK_q, KMOD_RCTRL, false, "ctrl+q", true);
    expect("zaurus ctrl+shift+q", SDLK_q, KMOD_LCTRL | KMOD_LSHIFT, false, nullptr, true);
    expect("zaurus ctrl+alt+q", SDLK_q, KMOD_LCTRL | KMOD_LALT, false, nullptr, true);
    expect("zaurus ctrl+c", SDLK_c, KMOD_LCTRL, false, nullptr, true);
    expect("zaurus ctrl+s", SDLK_s, KMOD_LCTRL, false, nullptr, true);
    std::printf(failures ? "shortcut-keys: FAIL\n" : "shortcut-keys: ok\n");
    return failures ? 1 : 0;
}
