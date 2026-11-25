// RenderLink's event handover: queued events and ones pushed by another thread while the
// filter goes in reach the guest queue once each, in order, and render resets ask for a
// full redraw and a new renderer, an event ends the guest thread's wait, and a press or
// release marks input pending until it's taken. A game controller's presses and stick reach the
// guest queue as controller events, and the key Set controls saves for a button matches them.
#include <SDL.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "ControllerMap.h"
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

    // The guest thread's wait ends when an event comes in, and runs its whole time without one.
    {
        RenderLink waiting(4, 4);
        waiting.InstallFilter();
        std::vector<RenderLink::Input> taken;
        waiting.TakeInputs(taken);
        auto started = std::chrono::steady_clock::now();
        waiting.WaitForInput(20000);
        const auto idle = std::chrono::steady_clock::now() - started;
        check(idle >= std::chrono::milliseconds(19), "a wait with no input runs its time");
        std::thread pusher([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            pushUser(1);
        });
        started = std::chrono::steady_clock::now();
        waiting.WaitForInput(2000000);
        const auto woken = std::chrono::steady_clock::now() - started;
        pusher.join();
        check(woken < std::chrono::milliseconds(500), "input ends the wait");
        waiting.TakeInputs(taken);
        check(taken.size() == 1 && taken[0].event.user.code == 1, "the waking event is queued");
        // An event taken before the wait doesn't end the next one.
        pushUser(2);
        waiting.TakeInputs(taken);
        started = std::chrono::steady_clock::now();
        waiting.WaitForInput(20000);
        check(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(19), "taken input doesn't wake");
        SDL_SetEventFilter(nullptr, nullptr);
    }

    // A queued press or release marks input pending for the running SoC until it's taken; other events don't.
    {
        RenderLink pending(4, 4);
        pending.InstallFilter();
        std::vector<RenderLink::Input> taken;
        pushUser(3);
        SDL_Event event{};
        event.type = SDL_MOUSEMOTION;
        SDL_PushEvent(&event);
        check(!pending.inputPending.load(), "motion and other events don't end a slice");
        event = {};
        event.type = SDL_MOUSEBUTTONDOWN;
        SDL_PushEvent(&event);
        check(pending.inputPending.load(), "a press ends a slice");
        pending.TakeInputs(taken);
        check(!pending.inputPending.load() && taken.size() == 3, "taking the input clears it");
        event = {};
        event.type = SDL_KEYUP;
        SDL_PushEvent(&event);
        check(pending.inputPending.load(), "a release ends a slice");
        pending.TakeInputs(taken);
        // What Android pushes: touches, and controller buttons from ZodiacPad (its axes don't end a slice).
        for (const uint32_t type : {uint32_t(SDL_FINGERDOWN), uint32_t(SDL_FINGERUP), uint32_t(SDL_CONTROLLERBUTTONDOWN),
                                    uint32_t(SDL_CONTROLLERBUTTONUP)}) {
            event = {};
            event.type = type;
            SDL_PushEvent(&event);
            check(pending.inputPending.load(), "a touch or controller button ends a slice");
            pending.TakeInputs(taken);
        }
        event = {};
        event.type = SDL_CONTROLLERAXISMOTION;
        SDL_PushEvent(&event);
        event = {};
        event.type = SDL_FINGERMOTION;
        SDL_PushEvent(&event);
        check(!pending.inputPending.load(), "stick and finger motion don't end a slice");
        pending.TakeInputs(taken);
        SDL_SetEventFilter(nullptr, nullptr);
    }

    // A virtual game controller with the filter in: SDL makes its controller events from the
    // joystick ones in an event watch, which runs only for events the filter lets through.
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_InitSubSystem: %s\n", SDL_GetError());
        return 1;
    }
    {
        SDL_VirtualJoystickDesc desc{};
        desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        desc.naxes = SDL_CONTROLLER_AXIS_MAX;
        desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        desc.name = "Frond test controller";
        const int index = SDL_JoystickAttachVirtualEx(&desc);
        check(index >= 0 && SDL_IsGameController(index), "the virtual joystick is a game controller");
        SDL_GameController* controller = index >= 0 ? SDL_GameControllerOpen(index) : nullptr;
        check(controller != nullptr, "the virtual controller opens");
        if (controller) {
            SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
            const SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
            RenderLink pads(4, 4);
            SDL_PumpEvents();
            SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
            pads.InstallFilter();
            std::vector<RenderLink::Input> taken;
            pads.TakeInputs(taken);
            // What Set controls saves when the button goes down in --controller-listen.
            PadKey key = PadKey::count;
            check(padKeyFromId("a", key) && key == PadKey::a, "Set controls names A");
            ControllerMap map;
            map.assign(PalmKey::app1, key);
            SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, SDL_PRESSED);
            SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTX, 32767);
            // The SDL thread's pump.
            SDL_PumpEvents();
            pads.DrainQueue();
            pads.TakeInputs(taken);
            PadReading reading;
            bool joystickEvents = false;
            for (const auto& input : taken) {
                const auto& event = input.event;
                if (event.type >= SDL_JOYAXISMOTION && event.type < SDL_CONTROLLERAXISMOTION) joystickEvents = true;
                if (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.which == id)
                    reading.buttons |= uint32_t(1) << event.cbutton.button;
                if (event.type == SDL_CONTROLLERAXISMOTION && event.caxis.which == id &&
                    event.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX)
                    reading.rightX = event.caxis.value;
            }
            const PadKeyMask held = padKeysHeld(reading);
            check(map.pressed(held) == uint64_t(1) << int(PalmKey::app1), "a bound controller button presses its Palm button");
            check((held & padKeyBit(PadKey::rightRight)) != 0, "the controller's stick reaches the guest");
            check(!joystickEvents, "the joystick events stay with SDL");
            SDL_SetEventFilter(nullptr, nullptr);
            SDL_GameControllerClose(controller);
        }
        if (index >= 0) SDL_JoystickDetachVirtual(index);
    }

    SDL_Quit();
    if (failures) return 1;
    std::puts("render link: ok");
    return 0;
}
