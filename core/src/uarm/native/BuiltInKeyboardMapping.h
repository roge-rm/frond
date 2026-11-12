#ifndef FROND_BUILT_IN_KEYBOARD_MAPPING_H
#define FROND_BUILT_IN_KEYBOARD_MAPPING_H
#include "DeviceControls.h"

inline KeyId builtInKeyboardKey(const DeviceControlProfile& profile, SDL_Scancode scan) {
    if (profile.style != DeviceControlStyle::tg50) return keyInvalid;
    switch (scan) {
        case SDL_SCANCODE_CAPSLOCK: return tg50Key(2,0);
        case SDL_SCANCODE_TAB: return tg50Key(3,0);
        case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return tg50Key(4,0);
        case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return tg50Key(5,0);
        case SDL_SCANCODE_LALT: return tg50Key(6,0);
        case SDL_SCANCODE_RALT: return tg50Key(7,0);
        case SDL_SCANCODE_Q: return tg50Key(2,1);
        case SDL_SCANCODE_W: return tg50Key(2,2);
        case SDL_SCANCODE_E: return tg50Key(2,3);
        case SDL_SCANCODE_R: return tg50Key(2,4);
        case SDL_SCANCODE_T: return tg50Key(2,5);
        case SDL_SCANCODE_Y: return tg50Key(3,1);
        case SDL_SCANCODE_U: return tg50Key(3,2);
        case SDL_SCANCODE_I: return tg50Key(3,3);
        case SDL_SCANCODE_O: return tg50Key(3,4);
        case SDL_SCANCODE_P: return tg50Key(3,5);
        case SDL_SCANCODE_A: return tg50Key(4,1);
        case SDL_SCANCODE_S: return tg50Key(4,2);
        case SDL_SCANCODE_D: return tg50Key(4,3);
        case SDL_SCANCODE_F: return tg50Key(4,4);
        case SDL_SCANCODE_G: return tg50Key(4,5);
        case SDL_SCANCODE_H: return tg50Key(2,6);
        case SDL_SCANCODE_J: return tg50Key(3,6);
        case SDL_SCANCODE_K: return tg50Key(4,6);
        case SDL_SCANCODE_L: return tg50Key(5,6);
        case SDL_SCANCODE_Z: return tg50Key(5,1);
        case SDL_SCANCODE_X: return tg50Key(5,2);
        case SDL_SCANCODE_C: return tg50Key(5,3);
        case SDL_SCANCODE_V: return tg50Key(5,4);
        case SDL_SCANCODE_B: return tg50Key(5,5);
        case SDL_SCANCODE_N: return tg50Key(6,1);
        case SDL_SCANCODE_M: return tg50Key(6,2);
        case SDL_SCANCODE_SPACE: return tg50Key(7,1);
        case SDL_SCANCODE_MINUS: return tg50Key(7,2);
        case SDL_SCANCODE_COMMA: return tg50Key(6,3);
        case SDL_SCANCODE_PERIOD: return tg50Key(6,4);
        case SDL_SCANCODE_UP: return tg50Key(6,5);
        case SDL_SCANCODE_DOWN: return tg50Key(6,6);
        case SDL_SCANCODE_LEFT: return tg50Key(7,3);
        case SDL_SCANCODE_RIGHT: return tg50Key(7,4);
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return tg50Key(7,5);
        case SDL_SCANCODE_BACKSPACE: return tg50Key(7,6);
        default: return keyInvalid;
    }
}
#endif
