#ifndef FROND_HOST_CONTROLLERS_H
#define FROND_HOST_CONTROLLERS_H

#include <SDL.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

// The host's game controllers, opened, closed and rumbled on the SDL thread only: SDL's Windows
// joystick backends keep their state on the thread that pumps events, and a call from another
// thread races the pump when a controller is plugged in or out. Wanted, scan and rumble requests
// may come from any thread; Service, on the SDL thread, carries them out.
class HostControllers {
   public:
    // Made on the SDL thread.
    HostControllers() : sdlThread(std::this_thread::get_id()) {}

    // Any thread: controllers plugged in from now on are opened (or not). Already opened ones stay open.
    void SetWanted(bool on) { wanted.store(on, std::memory_order_relaxed); }
    // Any thread: the next Service opens the wanted controllers not yet open and closes the unplugged ones.
    void RequestScan() { scan.store(true, std::memory_order_release); }
    // Any thread, from SDL's event filter: a controller came or went.
    void Notice(const SDL_Event& event) {
        if (event.type == SDL_CONTROLLERDEVICEADDED || event.type == SDL_CONTROLLERDEVICEREMOVED) RequestScan();
    }
    // Any thread: the rumble the guest's motor wants.
    void SetRumble(bool on) { rumbleWanted.store(on, std::memory_order_relaxed); }
    // Any thread: an open controller can rumble, as of the last Service.
    bool CanRumble() const { return canRumble.load(std::memory_order_relaxed); }

    // SDL thread, after each event pump: applies the requests. nowUsec is host time.
    void Service(uint64_t nowUsec) {
        ServiceScan();
        if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)) return;
        CheckThread("HostControllers::Service");
        // Sent for 1 s and again every 0.5 s, so a missed off or a controller added meanwhile is never far behind.
        constexpr uint32_t rumbleMs = 1000;
        const bool on = rumbleWanted.load(std::memory_order_relaxed);
        const bool due = !(on == rumbling && (!on || nowUsec - rumbleSentUsec < rumbleMs * 500));
        if (due) {
            rumbling = on;
            rumbleSentUsec = nowUsec;
        }
        bool any = false;
        for (auto* controller : open) {
#if SDL_VERSION_ATLEAST(2, 0, 18)
            if (!SDL_GameControllerHasRumble(controller)) continue;
#endif
            any = true;
            if (due) SDL_GameControllerRumble(controller, on ? 0xc000 : 0, on ? 0xffff : 0, on ? rumbleMs : 0);
        }
        canRumble.store(any, std::memory_order_relaxed);
    }

    // SDL thread: only the scan, if one was asked for.
    void ServiceScan() {
        if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)) return;
        CheckThread("HostControllers::ServiceScan");
        if (scan.exchange(false, std::memory_order_acquire)) Scan();
    }

    // An SDL controller call off the SDL thread is reported once. With FROND_SDL_THREAD_ABORT set in the
    // environment (the tests set it) it stops the process instead.
    void CheckThread(const char* what) const {
        if (std::this_thread::get_id() == sdlThread) return;
        static const bool stop = getenv("FROND_SDL_THREAD_ABORT") != nullptr;
        if (stop) {
            fprintf(stderr, "frond: %s off the SDL thread\n", what);
            abort();
        }
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) fprintf(stderr, "frond: %s off the SDL thread\n", what);
    }

   private:
    void Scan() {
        for (size_t i = 0; i < open.size();) {
            if (SDL_GameControllerGetAttached(open[i])) {
                ++i;
                continue;
            }
            SDL_GameControllerClose(open[i]);
            open.erase(open.begin() + long(i));
        }
        if (!wanted.load(std::memory_order_relaxed)) return;
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (!SDL_IsGameController(i) || SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i))) continue;
            if (auto* controller = SDL_GameControllerOpen(i)) open.push_back(controller);
        }
    }

    const std::thread::id sdlThread;
    std::atomic<bool> wanted{false}, scan{false}, rumbleWanted{false}, canRumble{false};
    // SDL thread only.
    std::vector<SDL_GameController*> open;
    bool rumbling{false};
    uint64_t rumbleSentUsec{0};
};

#endif
