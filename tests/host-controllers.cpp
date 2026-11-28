// HostControllers (core/src/uarm/native/HostControllers.h): a wanted virtual game controller opens on
// a scan and closes once unplugged, an unwanted one stays closed, rumble reaches it from the SDL
// thread, and with FROND_SDL_THREAD_ABORT set a call from another thread stops the process.
#undef NDEBUG
#include "HostControllers.h"

#include <SDL.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <csignal>
#include <cstdio>
#include <thread>

static int rumbles;
static Uint16 lastLow;

static int SDLCALL rumble(void*, Uint16 low, Uint16) {
    ++rumbles;
    lastLow = low;
    return 0;
}

static int attach() {
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.name = "Frond test pad";
    desc.Rumble = rumble;
    const int index = SDL_JoystickAttachVirtualEx(&desc);
    assert(index >= 0);
    return index;
}

static SDL_JoystickID idAt(int index) { return SDL_JoystickGetDeviceInstanceID(index); }

static void pump() {
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
}

int main() {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "host-controllers: no SDL game controllers (%s), skipped\n", SDL_GetError());
        return 0;
    }
    HostControllers controllers;

    // Not wanted: plugged in, it stays closed.
    int index = attach();
    const SDL_JoystickID first = idAt(index);
    pump();
    controllers.RequestScan();
    controllers.Service(0);
    assert(!SDL_GameControllerFromInstanceID(first));
    assert(!controllers.CanRumble());

    // Wanted: the next scan opens it, and it can rumble.
    controllers.SetWanted(true);
    controllers.RequestScan();
    controllers.Service(1000);
    assert(SDL_GameControllerFromInstanceID(first));
    assert(controllers.CanRumble());

    // Rumble on and off reach the controller. SDL passes on only changes, not the repeats.
    controllers.SetRumble(true);
    controllers.Service(2000);
    assert(rumbles == 1 && lastLow == 0xc000);
    controllers.Service(2000 + 500000);
    controllers.SetRumble(false);
    controllers.Service(600000);
    assert(rumbles == 2 && lastLow == 0);

    // Unplugged: the scan closes it; plugged in again: a scan opens it again.
    SDL_JoystickDetachVirtual(index);
    pump();
    controllers.RequestScan();
    controllers.Service(800000);
    assert(!SDL_GameControllerFromInstanceID(first) && !controllers.CanRumble());
    index = attach();
    const SDL_JoystickID second = idAt(index);
    pump();
    controllers.RequestScan();
    controllers.Service(900000);
    assert(SDL_GameControllerFromInstanceID(second));

    // Scans only when asked.
    SDL_JoystickDetachVirtual(index);
    pump();
    controllers.Service(1000000);
    assert(SDL_GameControllerFromInstanceID(second));
    controllers.RequestScan();
    controllers.Service(1100000);
    assert(!SDL_GameControllerFromInstanceID(second));

    // Off the SDL thread: reported and carried on without FROND_SDL_THREAD_ABORT, stopped with it.
    fflush(stderr);
    pid_t child = fork();
    if (child == 0) {
        unsetenv("FROND_SDL_THREAD_ABORT");
        std::thread other([&] { controllers.Service(0); });
        other.join();
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    child = fork();
    if (child == 0) {
        setenv("FROND_SDL_THREAD_ABORT", "1", 1);
        std::thread other([&] { controllers.Service(0); });
        other.join();
        _exit(0);
    }
    waitpid(child, &status, 0);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    SDL_Quit();
    printf("host-controllers: ok\n");
    return 0;
}
