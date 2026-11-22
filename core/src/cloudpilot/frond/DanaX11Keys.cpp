#include "DanaX11Keys.h"

#if defined(__linux__) && !defined(__ANDROID__) && defined(SDL_VIDEO_DRIVER_X11)
#include <SDL_syswm.h>
#include <X11/XKBlib.h>

#include <dlfcn.h>

void DanaX11Keys::Start(SDL_Window* window) {
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(window, &wm) || wm.subsystem != SDL_SYSWM_X11) return;

    // Ask X11 for repeated presses without synthetic releases on SDL's connection.
    void* x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
    if (!x11) return;
    Bool supported = False;
    auto detectable = reinterpret_cast<decltype(&XkbSetDetectableAutoRepeat)>(
        dlsym(x11, "XkbSetDetectableAutoRepeat"));
    const bool enabled = detectable && detectable(wm.info.x11.display, True, &supported) && supported;
    dlclose(x11);
    if (!enabled) return;

    windowId = wm.info.x11.window;
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
}

std::optional<SDL_Scancode> DanaX11Keys::NativeEvent(const SDL_SysWMmsg* message) {
    keys.ClearPending();
    if (!windowId || !message || message->subsystem != SDL_SYSWM_X11) return std::nullopt;
    const auto& event = message->msg.x11.event;
    if ((event.type != KeyPress && event.type != KeyRelease) || event.xkey.window != windowId)
        return std::nullopt;
    return keys.Transition(event.xkey.keycode, event.type == KeyPress);
}
#else
void DanaX11Keys::Start(SDL_Window*) {}
std::optional<SDL_Scancode> DanaX11Keys::NativeEvent(const SDL_SysWMmsg*) { return std::nullopt; }
#endif
