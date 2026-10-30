// RenderLink's event handover: queued events and ones pushed by another thread while the
// filter goes in reach the guest queue once each, in order, and render resets ask for a
// full redraw and a new renderer.
#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "RenderThread.h"

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static void pushUser(int code) {
    SDL_Event event{};
    event.type = SDL_USEREVENT;
    event.user.code = code;
    SDL_PushEvent(&event);
}

int main() {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    for (int round = 0; round < 200; ++round) {
        RenderLink link(4, 4);
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        constexpr int queued = 20, total = 2000;
        for (int i = 0; i < queued; ++i) pushUser(i);
        // Another thread keeps pushing while the filter goes in, as Android's UI thread can.
        std::atomic<bool> started{false};
        std::thread pusher([&] {
            started = true;
            for (int i = queued; i < total; ++i) pushUser(i);
        });
        while (!started) std::this_thread::yield();
        link.InstallFilter();
        pusher.join();
        link.DrainQueue();

        std::vector<RenderLink::Input> inputs;
        link.TakeInputs(inputs);
        bool inOrder = int(inputs.size()) == total;
        for (int i = 0; inOrder && i < total; ++i)
            inOrder = !inputs[i].layout && inputs[i].event.type == SDL_USEREVENT && inputs[i].event.user.code == i;
        check(inOrder, "every event once, in order");
        if (!inOrder) {
            std::fprintf(stderr, "round %d: %zu events\n", round, inputs.size());
            break;
        }
        SDL_SetEventFilter(nullptr, nullptr);
    }

    RenderLink link(4, 4);
    link.InstallFilter();
    SDL_Event event{};
    event.type = SDL_RENDER_TARGETS_RESET;
    SDL_PushEvent(&event);
    bool background = false;
    RenderNotices notices = link.TakeNotices(background);
    check(notices.redrawAll && !notices.lost && notices.pump, "targets reset redraws everything");
    event.type = SDL_RENDER_DEVICE_RESET;
    SDL_PushEvent(&event);
    notices = link.TakeNotices(background);
    check(notices.redrawAll && notices.lost, "device reset makes a new renderer");
    SDL_SetEventFilter(nullptr, nullptr);

    SDL_Quit();
    if (failures) return 1;
    std::puts("render link: ok");
    return 0;
}
