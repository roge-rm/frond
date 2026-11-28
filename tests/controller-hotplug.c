// Test shim for tests/controller-hotplug.py, loaded with LD_PRELOAD into out/frond. Reads lines from the
// FIFO in FROND_VPAD_FIFO on the thread that pumps SDL's events: "attach" plugs in an SDL virtual game
// controller (SDL_JoystickAttachVirtualEx), "detach" unplugs it and "b BUTTON 0|1" sets a button
// (SDL_GameControllerButton numbers). It logs the core's opens, closes and rumbles, and any SDL
// joystick or game controller call the core makes off the pumping thread.
#define _GNU_SOURCE
#include <SDL.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fifo = -2, pumped, index_ = -1;
static pthread_t pumpThread;
static SDL_Joystick* pad;
static char line[256];
static size_t used;

#define REAL(name) static __typeof__(name)* real; if (!real) real = (__typeof__(name)*)dlsym(RTLD_NEXT, #name)

static void check(const char* what) {
    if (pumped && !pthread_equal(pthread_self(), pumpThread)) fprintf(stderr, "vpad: off-thread %s\n", what);
}

static int SDLCALL rumble(void* userdata, Uint16 low, Uint16 high) {
    (void)userdata;
    check("rumble");
    fprintf(stderr, "vpad: rumble %u %u\n", low, high);
    return 0;
}

static void attach(void) {
    if (pad) return;
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.name = "Frond test pad";
    desc.Rumble = rumble;
    index_ = SDL_JoystickAttachVirtualEx(&desc);
    if (index_ >= 0) pad = SDL_JoystickOpen(index_);
    fprintf(stderr, "vpad: %s\n", pad ? "attached" : SDL_GetError());
}

static void detach(void) {
    if (!pad) return;
    const int at = index_;
    // The device index may have moved; find ours by instance id.
    const SDL_JoystickID id = SDL_JoystickInstanceID(pad);
    SDL_JoystickClose(pad);
    pad = NULL;
    int found = at;
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_JoystickGetDeviceInstanceID(i) == id) found = i;
    fprintf(stderr, "vpad: %s\n", SDL_JoystickDetachVirtual(found) == 0 ? "detached" : SDL_GetError());
}

static void service(void) {
    if (!pumped) {
        pumped = 1;
        pumpThread = pthread_self();
    }
    if (fifo == -2) {
        const char* path = getenv("FROND_VPAD_FIFO");
        fifo = path ? open(path, O_RDONLY | O_NONBLOCK) : -1;
    }
    if (fifo < 0 || !SDL_WasInit(SDL_INIT_GAMECONTROLLER)) return;
    const ssize_t n = read(fifo, line + used, sizeof line - used - 1);
    if (n <= 0) return;
    used += (size_t)n;
    line[used] = 0;
    char* end;
    while ((end = strchr(line, '\n'))) {
        *end = 0;
        int button, down;
        if (!strcmp(line, "attach")) attach();
        else if (!strcmp(line, "detach")) detach();
        else if (pad && sscanf(line, "b %d %d", &button, &down) == 2) {
            SDL_JoystickSetVirtualButton(pad, button, (Uint8)down);
            fprintf(stderr, "vpad: button %d %d\n", button, down);
        }
        used -= (size_t)(end + 1 - line);
        memmove(line, end + 1, used + 1);
    }
}

void SDL_PumpEvents(void) {
    REAL(SDL_PumpEvents);
    service();
    real();
}

// The core's own calls: SDL's internal ones don't come through here.
SDL_GameController* SDL_GameControllerOpen(int joystick_index) {
    REAL(SDL_GameControllerOpen);
    check("SDL_GameControllerOpen");
    SDL_GameController* controller = real(joystick_index);
    fprintf(stderr, "vpad: opened %s\n", controller ? SDL_GameControllerName(controller) : SDL_GetError());
    return controller;
}

void SDL_GameControllerClose(SDL_GameController* controller) {
    REAL(SDL_GameControllerClose);
    check("SDL_GameControllerClose");
    fprintf(stderr, "vpad: closed\n");
    real(controller);
}

int SDL_GameControllerRumble(SDL_GameController* controller, Uint16 low, Uint16 high, Uint32 ms) {
    REAL(SDL_GameControllerRumble);
    check("SDL_GameControllerRumble");
    return real(controller, low, high, ms);
}

SDL_bool SDL_GameControllerHasRumble(SDL_GameController* controller) {
    REAL(SDL_GameControllerHasRumble);
    check("SDL_GameControllerHasRumble");
    return real(controller);
}

SDL_GameController* SDL_GameControllerFromInstanceID(SDL_JoystickID id) {
    REAL(SDL_GameControllerFromInstanceID);
    check("SDL_GameControllerFromInstanceID");
    return real(id);
}

SDL_bool SDL_GameControllerGetAttached(SDL_GameController* controller) {
    REAL(SDL_GameControllerGetAttached);
    check("SDL_GameControllerGetAttached");
    return real(controller);
}

int SDL_NumJoysticks(void) {
    REAL(SDL_NumJoysticks);
    check("SDL_NumJoysticks");
    return real();
}

SDL_bool SDL_IsGameController(int joystick_index) {
    REAL(SDL_IsGameController);
    check("SDL_IsGameController");
    return real(joystick_index);
}

SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int device_index) {
    REAL(SDL_JoystickGetDeviceInstanceID);
    check("SDL_JoystickGetDeviceInstanceID");
    return real(device_index);
}
