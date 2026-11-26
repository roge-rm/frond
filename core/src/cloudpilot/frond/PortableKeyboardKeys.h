#ifndef FROND_68K_PORTABLE_KEYBOARD_KEYS_H
#define FROND_68K_PORTABLE_KEYBOARD_KEYS_H

// Host keys on the Palm Portable Keyboard, by physical US position. Left Alt is Fn and right Alt
// is Alt, as on the infrared keyboard; the Windows key is Cmd. F5 to F8 are the four application
// keys (Special Function One to Four) and F9 is Done. Page Up and Page Down are Fn with the up and
// down arrows, the keyboard's Scroll Up and Scroll Down. F1 to F4 and Escape keep their button and
// power uses.

#include <SDL.h>

#include "serial/PortableKeyboard.h"

struct PortableKeyboardKey {
    int code{-1};
    bool fn{false};
    explicit operator bool() const { return code >= 0; }
};

inline PortableKeyboardKey portableKeyboardKey(SDL_Scancode key) {
    using K = frondKeyboard::PortableKeyboard;
    switch (key) {
        case SDL_SCANCODE_1: return {0x00};
        case SDL_SCANCODE_2: return {0x01};
        case SDL_SCANCODE_3: return {0x02};
        case SDL_SCANCODE_Z: return {0x03};
        case SDL_SCANCODE_4: return {0x04};
        case SDL_SCANCODE_5: return {0x05};
        case SDL_SCANCODE_6: return {0x06};
        case SDL_SCANCODE_7: return {0x07};
        case SDL_SCANCODE_LGUI:
        case SDL_SCANCODE_RGUI: return {K::kCmd};
        case SDL_SCANCODE_Q: return {0x09};
        case SDL_SCANCODE_W: return {0x0a};
        case SDL_SCANCODE_E: return {0x0b};
        case SDL_SCANCODE_R: return {0x0c};
        case SDL_SCANCODE_T: return {0x0d};
        case SDL_SCANCODE_Y: return {0x0e};
        case SDL_SCANCODE_GRAVE: return {0x0f};
        case SDL_SCANCODE_X: return {0x10};
        case SDL_SCANCODE_A: return {0x11};
        case SDL_SCANCODE_S: return {0x12};
        case SDL_SCANCODE_D: return {0x13};
        case SDL_SCANCODE_F: return {0x14};
        case SDL_SCANCODE_G: return {0x15};
        case SDL_SCANCODE_H: return {0x16};
        case SDL_SCANCODE_SPACE: return {K::kSpace};
        case SDL_SCANCODE_CAPSLOCK: return {K::kCapsLock};
        case SDL_SCANCODE_TAB: return {K::kTab};
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL: return {K::kCtrl};
        case SDL_SCANCODE_LALT: return {K::kFn};
        case SDL_SCANCODE_RALT: return {K::kAlt};
        case SDL_SCANCODE_C: return {0x2c};
        case SDL_SCANCODE_V: return {0x2d};
        case SDL_SCANCODE_B: return {0x2e};
        case SDL_SCANCODE_N: return {0x2f};
        case SDL_SCANCODE_MINUS: return {0x30};
        case SDL_SCANCODE_EQUALS: return {0x31};
        case SDL_SCANCODE_BACKSPACE: return {K::kBackspace};
        case SDL_SCANCODE_F5: return {K::kSpecial1};
        case SDL_SCANCODE_8: return {0x34};
        case SDL_SCANCODE_9: return {0x35};
        case SDL_SCANCODE_0: return {0x36};
        case SDL_SCANCODE_LEFTBRACKET: return {0x38};
        case SDL_SCANCODE_RIGHTBRACKET: return {0x39};
        case SDL_SCANCODE_BACKSLASH: return {0x3a};
        case SDL_SCANCODE_F6: return {K::kSpecial2};
        case SDL_SCANCODE_U: return {0x3c};
        case SDL_SCANCODE_I: return {0x3d};
        case SDL_SCANCODE_O: return {0x3e};
        case SDL_SCANCODE_P: return {0x3f};
        case SDL_SCANCODE_APOSTROPHE: return {0x40};
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: return {K::kEnter};
        case SDL_SCANCODE_F7: return {K::kSpecial3};
        case SDL_SCANCODE_J: return {0x44};
        case SDL_SCANCODE_K: return {0x45};
        case SDL_SCANCODE_L: return {0x46};
        case SDL_SCANCODE_SEMICOLON: return {0x47};
        case SDL_SCANCODE_SLASH: return {0x48};
        case SDL_SCANCODE_UP: return {K::kUp};
        case SDL_SCANCODE_F8: return {K::kSpecial4};
        case SDL_SCANCODE_M: return {0x4c};
        case SDL_SCANCODE_COMMA: return {0x4d};
        case SDL_SCANCODE_PERIOD: return {0x4e};
        case SDL_SCANCODE_F9: return {K::kDone};
        case SDL_SCANCODE_DELETE: return {K::kDelete};
        case SDL_SCANCODE_LEFT: return {K::kLeft};
        case SDL_SCANCODE_DOWN: return {K::kDown};
        case SDL_SCANCODE_RIGHT: return {K::kRight};
        case SDL_SCANCODE_LSHIFT: return {K::kLeftShift};
        case SDL_SCANCODE_RSHIFT: return {K::kRightShift};
        case SDL_SCANCODE_PAGEUP: return {K::kUp, true};
        case SDL_SCANCODE_PAGEDOWN: return {K::kDown, true};
        default: return {};
    }
}

#endif
