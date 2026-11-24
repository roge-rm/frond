#ifndef _WINDOW_ID_H_
#define _WINDOW_ID_H_

#include <SDL.h>

#include <cstdint>
#include <optional>
#include <string>

// "x11 ID" or "win32 HWND" for a window the desktop app can embed, else empty.
std::string nativeWindowId(SDL_Window* window);

// Windows: a press on the embedded window takes the keyboard there. A child window in another
// process doesn't get the focus by itself; the input queues are shared, so SetFocus works.
void focusEmbeddedWindow(SDL_Window* window);

// Native mouse presses carry the X server's time, shared with Qt's mouse events.
unsigned long watchX11Pointer(SDL_Window* window);
std::optional<uint32_t> x11PointerTime(unsigned long window, const SDL_SysWMmsg* message);

#endif  // _WINDOW_ID_H_
