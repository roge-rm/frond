#ifndef _WINDOW_ID_H_
#define _WINDOW_ID_H_

#include <SDL.h>

#include <cstring>
#include <string>

// X11's own headers define names (Bool, Status, None) that clash with the 68k core's, so the
// window is read without them: the x11 member of SDL's syswm union is {Display*, Window window}.
#ifdef SDL_VIDEO_DRIVER_X11
#undef SDL_VIDEO_DRIVER_X11
#define FROND_RESTORE_X11_DRIVER
#endif
#include <SDL_syswm.h>
#ifdef FROND_RESTORE_X11_DRIVER
#define SDL_VIDEO_DRIVER_X11 1
#undef FROND_RESTORE_X11_DRIVER
#endif

// "x11 ID" for a window the desktop app can embed, else empty (no X11 window, as with a Wayland
// or dummy video driver).
inline std::string x11WindowId(SDL_Window* window) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_X11) return {};
    struct X11Info {
        void* display;
        unsigned long window;
    } x11;
    static_assert(sizeof(info.info) >= sizeof(X11Info), "syswm union");
    std::memcpy(&x11, &info.info, sizeof(x11));
    return "x11 " + std::to_string(x11.window);
}

#endif  // _WINDOW_ID_H_
