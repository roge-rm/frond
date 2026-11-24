#include "WindowId.h"

#include <SDL_syswm.h>

namespace {
unsigned long windowId(SDL_Window* window) {
#ifdef SDL_VIDEO_DRIVER_X11
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (window && SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_X11)
        return info.info.x11.window;
#endif
    return 0;
}
}

std::string nativeWindowId(SDL_Window* window) {
    const auto id = windowId(window);
    if (id) return "x11 " + std::to_string(id);
#ifdef SDL_VIDEO_DRIVER_WINDOWS
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (window && SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_WINDOWS && info.info.win.window)
        return "win32 " + std::to_string(uint64_t(uintptr_t(info.info.win.window)));
#endif
    return {};
}

void focusEmbeddedWindow(SDL_Window* window) {
#ifdef SDL_VIDEO_DRIVER_WINDOWS
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
    const HWND hwnd = info.info.win.window;
    if (hwnd && GetParent(hwnd) && GetFocus() != hwnd) SetFocus(hwnd);
#else
    (void)window;
#endif
}

unsigned long watchX11Pointer(SDL_Window* window) {
    const auto id = windowId(window);
    if (id) SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
    return id;
}

std::optional<uint32_t> x11PointerTime(unsigned long window, const SDL_SysWMmsg* message) {
#ifdef SDL_VIDEO_DRIVER_X11
    if (window && message && message->subsystem == SDL_SYSWM_X11) {
        const auto& event = message->msg.x11.event;
        if (event.type == ButtonPress && event.xbutton.window == window &&
            event.xbutton.button >= Button1 && event.xbutton.button <= Button3)
            return uint32_t(event.xbutton.time);
    }
#endif
    return std::nullopt;
}
