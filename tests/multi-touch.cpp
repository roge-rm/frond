// Fingers on the ARM runner's SdlEventHandler and the 68k runner's Touch68k: several buttons held at
// once, a button with the pen, one pen finger at most, release on lift, slide-off and focus loss, and
// SDL's touch-to-mouse copies ignored. A tap on the Zaurus art's Power holds it like the toolbar.
#include <cassert>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>
#include <SDL.h>
#include "RawFingers.h"
#include "SdlEventHandler.h"
#include "Touch.h"

struct Guest : public SoC {
    explicit Guest(DeviceType5 type) : type(type) {}
    DeviceType5 type;
    // Events since the last call: keys as (key, down), pen as (-1, down) with x, y in the pen list.
    std::vector<std::pair<KeyId,bool>> keys() {
        std::vector<std::pair<KeyId,bool>> out;
        while (keyEventQueue->GetSize()) { auto e = keyEventQueue->Pop(); out.emplace_back(e.key, e.keyDown); }
        return out;
    }
    std::vector<std::pair<bool,int>> pens() {
        std::vector<std::pair<bool,int>> out;
        while (penEventQueue->GetSize()) { auto e = penEventQueue->Pop(); out.emplace_back(e.penDown, e.x); }
        return out;
    }
    bool KeyboardAttached() const override {return false;}
    uint64_t Run(uint64_t,uint64_t) override{return 0;}
    bool RunToPaceSyscall(uint16_t,uint64_t,uint64_t) override{return false;}
    bool ExecuteInjected(uint64_t,uint64_t) override{return false;}
    bool Save() override{return true;}
    bool Load(size_t,void*) override{return true;}
    void DumpMMU() override{}
    void JamKey(KeyId,uint32_t) override{}
    // Guest time in ns, moved by the tests.
    uint64_t time{0};
    uint64_t GetTime() override{return time;}
    uint32_t* GetPendingFrame(uint32_t&,uint32_t&) override{return nullptr;}
    void ResetPendingFrame() override{}
    DeviceType5 GetDeviceType() override{return type;}
    DisplayMode GetDisplayMode() override{return DisplayMode::mode_320x480;}
    void SuspendTimerInterrupts(bool) override{}
    bool LcdEnabled() override{return true;}
 protected:
    void OnSetFramebufferDirty() override{}
    bool OnSleep() override{return false;}
    void OnWakeup() override{}
    void OnSetAudioQueue(AudioQueue*) override{}
    void OnSetPcmOutputEnabled() override{}
    void OnSetPcmSuspended() override{}
    void OnSdInsert() override{}
    void OnSdRemount() override{}
    void OnSdEject() override{}
};

// Takes fingers only from the touch ids marked direct, as SDL's device type would say.
struct Handler : SdlEventHandler {
    using SdlEventHandler::SdlEventHandler;
    bool direct{true};
    bool DirectTouch(SDL_TouchID) const override { return direct; }
};

using Keys = std::vector<std::pair<KeyId,bool>>;
using Pens = std::vector<std::pair<bool,int>>;

// The first point (x, y from the top of the screen) of up to two different keys below the screen.
static std::map<KeyId,std::pair<int,int>> findKeys(const DeviceControlProfile& p, int w, int h) {
    std::map<KeyId,std::pair<int,int>> found;
    for (int y = h; y < h + deviceControlsHeight(p, w); y += 2)
        for (int x = 0; x < w; x += 2) {
            const KeyId key = deviceControlAt(p, w, h, x, y);
            if (key && !found.count(key)) found[key] = {x, y};
        }
    return found;
}

static void arm(DeviceType5 type, DeviceArt art, int w, int h) {
    Guest guest(type);
    DisplayConfiguration display{uint16_t(w), uint16_t(h), 0, 2};
    Handler input(&guest, 1, display, Rotation::portrait_0);
    const auto& p = deviceControls(art);
    const int total = h + deviceControlsHeight(p, w);
    auto found = findKeys(p, w, h);
    assert(found.size() >= 2);
    auto it = found.begin();
    const KeyId k1 = it->first; const auto p1 = it->second; ++it;
    const KeyId k2 = it->first; const auto p2 = it->second;
    auto finger = [&](Uint32 type, SDL_FingerID id, int x, int y) {
        SDL_Event e{}; e.type = type; e.tfinger.type = type; e.tfinger.touchId = 3; e.tfinger.fingerId = id;
        e.tfinger.x = (x + 0.5f) / w; e.tfinger.y = (y + 0.5f) / total;
        input.HandleEvent(e);
    };
    // Two buttons held at once.
    finger(SDL_FINGERDOWN, 1, p1.first, p1.second);
    finger(SDL_FINGERDOWN, 2, p2.first, p2.second);
    assert((guest.keys() == Keys{{k1,true},{k2,true}}));
    // SDL's mouse copy of a touch does nothing.
    SDL_Event copy{}; copy.type = SDL_MOUSEBUTTONDOWN; copy.button.which = SDL_TOUCH_MOUSEID;
    copy.button.button = SDL_BUTTON_LEFT; copy.button.x = 100; copy.button.y = 100; input.HandleEvent(copy);
    copy.type = SDL_MOUSEMOTION; copy.motion.which = SDL_TOUCH_MOUSEID; input.HandleEvent(copy);
    copy.type = SDL_MOUSEBUTTONUP; copy.button.which = SDL_TOUCH_MOUSEID; input.HandleEvent(copy);
    assert(guest.keys().empty() && guest.pens().empty());
    finger(SDL_FINGERUP, 1, p1.first, p1.second);
    assert((guest.keys() == Keys{{k1,false}}));
    // The other button held, a pen finger on the screen, and a second pen finger ignored.
    finger(SDL_FINGERDOWN, 3, 100, 100);
    assert(guest.pens().size() == 1 && guest.keys().empty());
    finger(SDL_FINGERDOWN, 4, 150, 150);
    assert(guest.pens().empty());
    finger(SDL_FINGERMOTION, 3, 110, 110);
    assert(guest.pens().size() == 1);
    finger(SDL_FINGERUP, 4, 150, 150);
    assert(guest.pens().empty() && guest.keys().empty());
    finger(SDL_FINGERUP, 3, 110, 110);
    assert((guest.pens() == Pens{{false,-1}}));
    finger(SDL_FINGERUP, 2, p2.first, p2.second);
    assert((guest.keys() == Keys{{k2,false}}));
    // A game: d-pad held, an app button pressed and released, then the thumb rolls to the next direction.
    {
        const KeyId from = found.count(keyIdUp) ? keyIdUp : k1, to = found.count(keyIdRight) ? keyIdRight : keyIdDown;
        const KeyId app = keyIdHard1;
        assert(found.count(from) && found.count(to) && found.count(app));
        const auto f = found[from], t = found[to], a = found[app];
        finger(SDL_FINGERDOWN, 8, f.first, f.second);
        finger(SDL_FINGERDOWN, 9, a.first, a.second);
        assert((guest.keys() == Keys{{from,true},{app,true}}));
        finger(SDL_FINGERUP, 9, a.first, a.second);
        assert((guest.keys() == Keys{{app,false}}));
        // The new direction goes down before the old one comes up.
        finger(SDL_FINGERMOTION, 8, t.first, t.second);
        assert((guest.keys() == Keys{{to,true},{from,false}}));
        // Rolling onto the screen lets go without a pen stroke.
        finger(SDL_FINGERMOTION, 8, 100, 100);
        assert((guest.keys() == Keys{{to,false}}) && guest.pens().empty());
        finger(SDL_FINGERUP, 8, 100, 100);
        assert(guest.keys().empty() && guest.pens().empty());
    }
    // A finger on a hard button keeps it held while the keyboard or the mouse presses and lets go of it.
    {
        const auto h = found[keyIdHard1];
        auto sym = [&](Uint32 type, SDL_Keycode key) {
            SDL_Event e{}; e.type = type; e.key.type = type; e.key.keysym.sym = key; input.HandleEvent(e);
        };
        auto mouse = [&](Uint32 type) {
            SDL_Event e{}; e.type = type; e.button.type = type; e.button.button = SDL_BUTTON_LEFT;
            e.button.x = h.first; e.button.y = h.second; input.HandleEvent(e);
        };
        finger(SDL_FINGERDOWN, 10, h.first, h.second);
        assert((guest.keys() == Keys{{keyIdHard1,true}}));
        sym(SDL_KEYDOWN, SDLK_F1); sym(SDL_KEYUP, SDLK_F1);
        mouse(SDL_MOUSEBUTTONDOWN); mouse(SDL_MOUSEBUTTONUP);
        assert(guest.keys().empty());
        finger(SDL_FINGERUP, 10, h.first, h.second);
        assert((guest.keys() == Keys{{keyIdHard1,false}}));
    }
    // Fingers from a touchpad are ignored.
    input.direct = false;
    finger(SDL_FINGERDOWN, 11, p1.first, p1.second);
    finger(SDL_FINGERDOWN, 12, 100, 100);
    assert(guest.keys().empty() && guest.pens().empty());
    input.direct = true;
    finger(SDL_FINGERUP, 11, p1.first, p1.second);
    finger(SDL_FINGERUP, 12, 100, 100);
    assert(guest.keys().empty() && guest.pens().empty());
    // A button finger that slides off releases it.
    finger(SDL_FINGERDOWN, 5, p1.first, p1.second);
    assert((guest.keys() == Keys{{k1,true}}));
    finger(SDL_FINGERMOTION, 5, 100, 100);
    assert((guest.keys() == Keys{{k1,false}}));
    finger(SDL_FINGERUP, 5, 100, 100);
    // Focus loss lets go of a button and the pen.
    finger(SDL_FINGERDOWN, 6, p2.first, p2.second);
    finger(SDL_FINGERDOWN, 7, 100, 100);
    guest.keys(); guest.pens();
    SDL_Event lost{}; lost.type = SDL_WINDOWEVENT; lost.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    input.HandleEvent(lost);
    assert((guest.keys() == Keys{{k2,false}}));
    assert(!guest.pens().empty());
    // A real mouse still works.
    SDL_Event down{}; down.type = SDL_MOUSEBUTTONDOWN; down.button.button = SDL_BUTTON_LEFT;
    down.button.x = p1.first; down.button.y = p1.second; input.HandleEvent(down);
    assert((guest.keys() == Keys{{k1,true}}));
    down.type = SDL_MOUSEBUTTONUP; input.HandleEvent(down);
    assert((guest.keys() == Keys{{k1,false}}));
}

// Counts the holders of a key like the runner's Key does, so the events are what the guest sees.
struct Sink : TouchSink68k {
    const DeviceControlProfile& p; int w, h;
    KeyHolds68k holds; bool direct{true};
    bool DirectTouch(int64_t) override { return direct; }
    void Other(KeyId k, bool down) { if (down ? holds.Press(k) : holds.Release(k)) keys.emplace_back(k, down); }
    std::vector<std::pair<KeyId,bool>> keys; std::vector<std::pair<bool,int>> pens;
    Sink(const DeviceControlProfile& p, int w, int h) : p(p), w(w), h(h) {}
    KeyId KeyAt(int x, int y) override { return deviceControlAt(p, w, h, x, y); }
    void PenDown(int x, int) override { pens.emplace_back(true, x); }
    void PenUp() override { pens.emplace_back(false, -1); }
    void KeyDown(KeyId k) override { Other(k, true); }
    void KeyUp(KeyId k) override { Other(k, false); }
};

static void m68k() {
    const int w = 160, h = 220;
    const auto& p = deviceControls(DeviceArt::iiix);
    Sink sink(p, w, h);
    Touch68k touch(sink, w, h);
    auto found = findKeys(p, w, h);
    assert(found.size() >= 2);
    auto it = found.begin();
    const KeyId k1 = it->first; const auto p1 = it->second; ++it;
    const KeyId k2 = it->first; const auto p2 = it->second;
    touch.Down(1, 1, p1.first, p1.second, true, false);
    touch.Down(1, 2, p2.first, p2.second, true, false);
    assert((sink.keys == Keys{{k1,true},{k2,true}})); sink.keys.clear();
    touch.Up(1, 1);
    assert((sink.keys == Keys{{k1,false}})); sink.keys.clear();
    touch.Down(1, 3, 50, 50, true, false);
    assert((sink.pens == Pens{{true,50}}) && sink.keys.empty()); sink.pens.clear();
    touch.Down(1, 4, 60, 60, true, false);
    assert(sink.pens.empty() && touch.PenActive());
    touch.Move(1, 3, 70, 60);
    assert((sink.pens == Pens{{true,70}})); sink.pens.clear();
    touch.Up(1, 4); assert(sink.pens.empty());
    touch.Up(1, 3);
    assert((sink.pens == Pens{{false,-1}}) && !touch.PenActive()); sink.pens.clear();
    // The pen is busy while a mouse holds it.
    touch.Down(1, 5, 50, 50, true, true);
    assert(sink.pens.empty());
    touch.Move(1, 2, 50, 50);
    assert((sink.keys == Keys{{k2,false}})); sink.keys.clear();
    touch.Down(1, 6, p1.first, p1.second, true, false);
    touch.Down(1, 7, 50, 50, true, false);
    sink.keys.clear(); sink.pens.clear();
    touch.ReleaseAll();
    assert((sink.keys == Keys{{k1,false}}) && (sink.pens == Pens{{false,-1}}));
    {
        const auto f = found[keyIdUp], t = found[keyIdDown], a = found[keyIdHard1];
        sink.keys.clear(); sink.pens.clear();
        touch.Down(2, 1, f.first, f.second, true, false);
        touch.Down(2, 2, a.first, a.second, true, false);
        touch.Up(2, 2);
        touch.Move(2, 1, t.first, t.second);
        assert((sink.keys == Keys{{keyIdUp,true},{keyIdHard1,true},{keyIdHard1,false},{keyIdUp,false},{keyIdDown,true}}));
        sink.keys.clear();
        touch.Move(2, 1, 50, 50);
        assert((sink.keys == Keys{{keyIdDown,false}}) && sink.pens.empty());
        touch.Up(2, 1);
        assert(sink.keys.size() == 1);
    }
    touch.Down(1, 8, p1.first, p1.second, false, false);
    assert(sink.keys.size() == 1);
    // A finger on a button, the keyboard or the mouse pressing the same button, then letting go:
    // the button stays down until the finger lifts.
    {
        sink.keys.clear();
        touch.Down(4, 1, p1.first, p1.second, true, false);
        sink.Other(k1, true); sink.Other(k1, false);
        sink.Other(k1, true); sink.Other(k1, false);
        assert((sink.keys == Keys{{k1,true}}));
        touch.Up(4, 1);
        assert((sink.keys == Keys{{k1,true},{k1,false}}));
        // The other way round: the key is held first, the finger comes and goes.
        sink.keys.clear();
        sink.Other(k1, true);
        touch.Down(4, 2, p1.first, p1.second, true, false);
        touch.Up(4, 2);
        assert((sink.keys == Keys{{k1,true}}));
        sink.Other(k1, false);
        assert((sink.keys == Keys{{k1,true},{k1,false}}));
        // A release nobody holds is ignored.
        sink.Other(k1, false);
        assert(sink.keys.size() == 2);
    }
    // Fingers from a touchpad do nothing.
    sink.keys.clear(); sink.pens.clear(); sink.direct = false;
    touch.Down(5, 1, p1.first, p1.second, true, false);
    touch.Down(5, 2, 50, 50, true, false);
    assert(sink.keys.empty() && sink.pens.empty() && !touch.PenActive());
    sink.direct = true;
}

// SDL's renderer moves finger points into its viewport; the 68k runner takes the window points back.
static void rawFingers() {
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    assert(SDL_InitSubSystem(SDL_INIT_VIDEO) == 0);
    // Installed before the window, as the runner does: installing flushes the queue, so the
    // window's own first events must come after it.
    RawFingers::Install();
    SDL_Window* window = SDL_CreateWindow("raw", 0, 0, 100, 200, 0);
    assert(window);
    assert(SDL_HasEvent(SDL_WINDOWEVENT));
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    assert(renderer);
    const SDL_Rect viewport{0, 50, 100, 100};
    SDL_RenderSetViewport(renderer, &viewport);
    auto push = [&](uint32_t type, SDL_FingerID finger, float y) {
        SDL_Event event{};
        event.tfinger.type = type;
        event.tfinger.touchId = 7;
        event.tfinger.fingerId = finger;
        event.tfinger.x = 0.5f;
        event.tfinger.y = y;
        assert(SDL_PushEvent(&event) == 1);
    };
    auto next = [&](float& x, float& y) {
        // SDL_PollEvent stops once at the sentinel its pump leaves behind.
        SDL_Event event;
        for (int pass = 0; pass < 2; ++pass)
            while (SDL_PollEvent(&event))
                if (event.type == SDL_FINGERDOWN || event.type == SDL_FINGERMOTION) {
                    RawFingers::Take(event.tfinger, x, y);
                    return event.tfinger.y;
                }
        assert(false);
        return 0.0f;
    };
    float x = 0, y = 0;
    push(SDL_FINGERDOWN, 1, 0.3f);
    assert(next(x, y) != 0.3f);
    assert(x == 0.5f && y == 0.3f);
    push(SDL_FINGERDOWN, 2, 0.6f);
    push(SDL_FINGERMOTION, 1, 0.35f);
    push(SDL_FINGERMOTION, 1, 0.4f);
    // The first motion is dropped before it's read; the later one still gets its own point.
    SDL_FlushEvent(SDL_FINGERMOTION);
    SDL_Delay(2);
    push(SDL_FINGERMOTION, 1, 0.45f);
    next(x, y);
    assert(y == 0.6f);
    next(x, y);
    assert(y == 0.45f);
    SDL_SetEventFilter(nullptr, nullptr);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

// The side panel's Power (and the keyboard's) is a tap: the guest gets the key held 2.75 s of guest
// time, as the toolbar's Power gives it, however short the click or finger. The toolbar's replayed
// Power, F10 and the tap share one hold.
static void zaurus() {
    Guest guest(deviceTypeSpitz);
    guest.time = 5000000000ull;
    DisplayConfiguration display{480, 640, 0, 2};
    DeviceControlView view;
    view.zaurus = ZaurusStyle::view;
    Handler input(&guest, 1, display, Rotation::portrait_0, view);
    const auto layout = zaurusDeviceLayout(480, 640, ZaurusStyle::view);
    const auto& art = zaurusArt();
    const auto power = zaurusKeyRect(layout, art.keys[zaurusKeyIndex("expower")]);
    const auto ok = zaurusKeyRect(layout, art.keys[zaurusKeyIndex("exok")]);
    const KeyId okKey = zaurusArtKey(art.keys[zaurusKeyIndex("exok")]);
    const int px = power.x + power.w / 2, py = power.y + power.h / 2;
    const int ox = ok.x + ok.w / 2, oy = ok.y + ok.h / 2;
    const SDL_Point size = deviceControlSize(deviceControls(deviceTypeSpitz, view), 480, 640, view);
    auto finger = [&](Uint32 type, int x, int y) {
        SDL_Event e{}; e.type = type; e.tfinger.type = type; e.tfinger.touchId = 3; e.tfinger.fingerId = 1;
        e.tfinger.x = (x + 0.5f) / size.x; e.tfinger.y = (y + 0.5f) / size.y;
        input.HandleEvent(e);
    };
    auto mouse = [&](Uint32 type, int x, int y) {
        SDL_Event e{}; e.type = type; e.button.type = type; e.button.button = SDL_BUTTON_LEFT;
        e.button.x = x; e.button.y = y; input.HandleEvent(e);
    };
    auto f10 = [&](Uint32 type) {
        SDL_Event e{}; e.type = type; e.key.type = type;
        e.key.keysym.scancode = SDL_SCANCODE_F10; e.key.keysym.sym = SDLK_F10; input.HandleEvent(e);
    };
    auto pass = [&](uint64_t ms) { guest.time += ms * 1000000; input.ServiceHolds(); };
    const Keys down{{keyIdPower,true}}, up{{keyIdPower,false}};
    // A finger tap: down at once, up after 2.75 s of guest time.
    finger(SDL_FINGERDOWN, px, py);
    finger(SDL_FINGERUP, px, py);
    assert(guest.keys() == down);
    assert(input.PressedButtons() & deviceKeyBit(keyIdPower));
    pass(2749);
    assert(guest.keys().empty());
    pass(1);
    assert(guest.keys() == up);
    assert(!(input.PressedButtons() & deviceKeyBit(keyIdPower)));
    // A mouse click, then a second click during the hold: one press, held 2.75 s from the second.
    mouse(SDL_MOUSEBUTTONDOWN, px, py); mouse(SDL_MOUSEBUTTONUP, px, py);
    pass(1000);
    mouse(SDL_MOUSEBUTTONDOWN, px, py); mouse(SDL_MOUSEBUTTONUP, px, py);
    pass(2000);
    assert(guest.keys() == down);
    pass(750);
    assert(guest.keys() == up);
    // Host time alone doesn't end it: the guest paused, the hold waits.
    finger(SDL_FINGERDOWN, px, py); finger(SDL_FINGERUP, px, py);
    for (int i = 0; i < 5; ++i) input.ServiceHolds();
    assert(guest.keys() == down);
    pass(2750);
    assert(guest.keys() == up);
    // The side OK still follows the finger.
    finger(SDL_FINGERDOWN, ox, oy);
    assert((guest.keys() == Keys{{okKey,true}}));
    finger(SDL_FINGERUP, ox, oy);
    assert((guest.keys() == Keys{{okKey,false}}));
    // A finger sliding from OK onto Power taps Power and lets go of OK; lifting it adds nothing.
    finger(SDL_FINGERDOWN, ox, oy);
    finger(SDL_FINGERMOTION, px, py);
    assert((guest.keys() == Keys{{okKey,true},{keyIdPower,true},{okKey,false}}));
    finger(SDL_FINGERUP, px, py);
    pass(2750);
    assert(guest.keys() == up);
    // The toolbar's replayed Power overlapping a tap: one press, let go when both are done.
    assert(input.ReplayKey(keyIdPower, true));
    assert(guest.keys() == down);
    finger(SDL_FINGERDOWN, px, py); finger(SDL_FINGERUP, px, py);
    pass(1000);
    assert(input.ReplayKey(keyIdPower, false));
    assert(guest.keys().empty() && (input.PressedButtons() & deviceKeyBit(keyIdPower)));
    pass(1750);
    assert(guest.keys() == up);
    // The other way round, and other keys stay with the SoC.
    finger(SDL_FINGERDOWN, px, py); finger(SDL_FINGERUP, px, py);
    input.ReplayKey(keyIdPower, true);
    pass(2750);
    assert(guest.keys() == down);
    input.ReplayKey(keyIdPower, false);
    assert(guest.keys() == up);
    assert(!input.ReplayKey(okKey, true));
    // F10 held across a tap: one press, let go after both.
    f10(SDL_KEYDOWN);
    assert(guest.keys() == down);
    finger(SDL_FINGERDOWN, px, py); finger(SDL_FINGERUP, px, py);
    f10(SDL_KEYUP);
    assert(guest.keys().empty());
    pass(2750);
    assert(guest.keys() == up);
    // Focus loss keeps a tap and a replayed Power: they let go by themselves.
    finger(SDL_FINGERDOWN, px, py); finger(SDL_FINGERUP, px, py);
    input.ReplayKey(keyIdPower, true);
    guest.keys();
    SDL_Event lost{}; lost.type = SDL_WINDOWEVENT; lost.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    input.HandleEvent(lost);
    assert(guest.keys().empty());
    pass(2750);
    assert(guest.keys().empty());
    input.ReplayKey(keyIdPower, false);
    assert(guest.keys() == up);
    // A new replay replacing one that holds Power lets go of it; F10 works after.
    input.ReplayKey(keyIdPower, true);
    input.ReleaseReplayKeys();
    assert((guest.keys() == Keys{{keyIdPower,true},{keyIdPower,false}}));
    input.ReleaseReplayKeys();
    f10(SDL_KEYDOWN); f10(SDL_KEYUP);
    assert((guest.keys() == Keys{{keyIdPower,true},{keyIdPower,false}}));
    // Going to the background or quitting lets go of a tap and a replayed Power at once.
    for (const Uint32 type : {Uint32(SDL_APP_DIDENTERBACKGROUND), Uint32(SDL_QUIT)}) {
        finger(SDL_FINGERDOWN, px, py); finger(SDL_FINGERUP, px, py);
        input.ReplayKey(keyIdPower, true);
        assert(guest.keys() == down);
        SDL_Event e{}; e.type = type; input.HandleEvent(e);
        assert(guest.keys() == up);
        assert(!(input.PressedButtons() & deviceKeyBit(keyIdPower)));
        pass(2750);
        input.ReplayKey(keyIdPower, false);
        assert(guest.keys() == up);
        SDL_Event back{}; back.type = SDL_APP_DIDENTERFOREGROUND; input.HandleEvent(back);
    }
    // A reset (guest time going back) lets go of the tap instead of holding Power while it boots.
    finger(SDL_FINGERDOWN, px, py); finger(SDL_FINGERUP, px, py);
    pass(500);
    guest.time = 1000000;
    input.ServiceHolds();
    assert((guest.keys() == Keys{{keyIdPower,true},{keyIdPower,false}}));
    pass(3000);
    assert(guest.keys().empty());
}

int main() {
    assert(SDL_Init(SDL_INIT_EVENTS) == 0);
    arm(deviceTypeTX, DeviceArt::tx, 320, 480);
    arm(deviceTypeE2, DeviceArt::e2, 320, 320);
    arm(deviceTypeFrankenE2, DeviceArt::e2, 320, 480);
    arm(deviceTypeZire72, DeviceArt::zire72, 320, 320);
    zaurus();
    m68k();
    rawFingers();
    SDL_Quit();
    puts("PASS: multi-touch buttons and pen on the ARM and 68k runners, Zaurus Power tap");
}
