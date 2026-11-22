#ifndef _WINDOW_ID_H_
#define _WINDOW_ID_H_

#include <SDL.h>

#include <cstdint>
#include <optional>
#include <string>

// "x11 ID" for a window the desktop app can embed, else empty.
std::string x11WindowId(SDL_Window* window);

// Native mouse presses carry the X server's time, shared with Qt's mouse events.
unsigned long watchX11Pointer(SDL_Window* window);
std::optional<uint32_t> x11PointerTime(unsigned long window, const SDL_SysWMmsg* message);

#endif  // _WINDOW_ID_H_
