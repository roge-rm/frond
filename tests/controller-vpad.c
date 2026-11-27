// Test shim for tests/controller-map-live.py, loaded with LD_PRELOAD into out/frond: attaches one
// SDL virtual game controller once the core has started SDL's game controllers, and sets its
// buttons from "b BUTTON 0|1" lines (SDL_GameControllerButton numbers) read from the FIFO in
// FROND_VPAD_FIFO. It runs on whichever thread pumps SDL's events.
#define _GNU_SOURCE
#include <SDL.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fifo = -2, attached;
static SDL_Joystick* pad;
static char line[256];
static size_t used;

static void service(void) {
    if (!attached && SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        attached = 1;
        const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
                                                    SDL_CONTROLLER_BUTTON_MAX, 0);
        if (index >= 0) pad = SDL_JoystickOpen(index);
        fprintf(stderr, "vpad: %s\n", pad ? "attached" : SDL_GetError());
    }
    if (fifo == -2) {
        const char* path = getenv("FROND_VPAD_FIFO");
        fifo = path ? open(path, O_RDONLY | O_NONBLOCK) : -1;
    }
    if (fifo < 0 || !pad) return;
    const ssize_t n = read(fifo, line + used, sizeof line - used - 1);
    if (n <= 0) return;
    used += (size_t)n;
    line[used] = 0;
    char* end;
    while ((end = strchr(line, '\n'))) {
        *end = 0;
        int button, down;
        if (sscanf(line, "b %d %d", &button, &down) == 2) {
            SDL_JoystickSetVirtualButton(pad, button, (Uint8)down);
            fprintf(stderr, "vpad: button %d %d\n", button, down);
        }
        used -= (size_t)(end + 1 - line);
        memmove(line, end + 1, used + 1);
    }
}

void SDL_PumpEvents(void) {
    static void (*real)(void);
    if (!real) real = (void (*)(void))dlsym(RTLD_NEXT, "SDL_PumpEvents");
    service();
    real();
}

int SDL_PollEvent(SDL_Event* event) {
    static int (*real)(SDL_Event*);
    if (!real) real = (int (*)(SDL_Event*))dlsym(RTLD_NEXT, "SDL_PollEvent");
    service();
    return real(event);
}

// Waits are cut short so the FIFO is read while the core sleeps on events.
int SDL_WaitEventTimeout(SDL_Event* event, int timeout) {
    static int (*real)(SDL_Event*, int);
    if (!real) real = (int (*)(SDL_Event*, int))dlsym(RTLD_NEXT, "SDL_WaitEventTimeout");
    service();
    return real(event, timeout < 0 || timeout > 20 ? 20 : timeout);
}
