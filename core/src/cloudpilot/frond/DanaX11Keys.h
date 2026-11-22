#pragma once

#include "../../uarm/native/DanaHostKeys.h"

class DanaX11Keys {
public:
    void Start(SDL_Window* window);
    std::optional<SDL_Scancode> NativeEvent(const SDL_SysWMmsg* message);
    std::optional<bool> Translated(SDL_Scancode scancode, bool down) {
        return keys.Translated(scancode, down);
    }
    void Clear() { keys.Clear(); }

private:
    unsigned long windowId{0};
    DanaNativeKeys keys;
};
