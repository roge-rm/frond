// Exercise real SDL pointer events against the production hardware-key path.
#include <cassert>
#include <cstdio>
#include <vector>
#include <utility>
#include <SDL.h>
#include "SdlEventHandler.h"
class Guest : public SoC {
 public:
    Guest() { displayConfiguration={320,480,0,160}; }
    bool attached=false;
    void expect(std::initializer_list<std::pair<KeyId,bool>> expected) {
        assert(keyEventQueue->GetSize()==expected.size());
        for(auto [key,down]:expected) {auto e=keyEventQueue->Pop();assert(e.key==key && e.keyDown==down);}
    }
    void noPen() {assert(penEventQueue->GetSize()==0);}
    void pen(bool down,int x=0,int y=0) {
        assert(penEventQueue->GetSize()==1);auto e=penEventQueue->Pop();
        assert(e.penDown==down);if(down)assert(e.x==x && e.y==y);
    }
    std::vector<std::pair<uint8_t,bool>> events;
    bool KeyboardAttached() const override {return attached;}
    bool KeyboardKey(uint8_t code,bool down) override {events.emplace_back(code,down);return true;}
    void ReleaseKeyboard() override {events.emplace_back(0,false);}
    uint64_t Run(uint64_t,uint64_t) override{return 0;}
    bool RunToPaceSyscall(uint16_t,uint64_t,uint64_t) override{return false;}
    bool ExecuteInjected(uint64_t,uint64_t) override{return false;}
    bool Save() override{return true;}
    bool Load(size_t,void*) override{return true;}
    void DumpMMU() override{}
    void JamKey(KeyId,uint32_t) override{}
    uint64_t GetTime() override{return 0;}
    uint32_t* GetPendingFrame(uint32_t&,uint32_t&) override{return nullptr;}
    void ResetPendingFrame() override{}
    DeviceType5 GetDeviceType() override{return DeviceType5::deviceTypeTX;}
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

static void mouse(Uint32 type,int x,int y) {
    SDL_Event e{};e.type=type;
    if(type==SDL_MOUSEMOTION) {e.motion.x=x;e.motion.y=y;}
    else {e.button.button=SDL_BUTTON_LEFT;e.button.x=x;e.button.y=y;}
    assert(SDL_PushEvent(&e)==1);
}
static void key(SDL_Keycode key,bool down) {
    SDL_Event e{};e.type=down?SDL_KEYDOWN:SDL_KEYUP;e.key.keysym.sym=key;
    e.key.keysym.scancode=SDL_GetScancodeFromKey(key);assert(SDL_PushEvent(&e)==1);
}
static void window(Uint8 event) {
    SDL_Event e{};e.type=SDL_WINDOWEVENT;e.window.event=event;assert(SDL_PushEvent(&e)==1);
}
int main() {
    SDL_setenv("SDL_VIDEODRIVER","dummy",1);assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)==0);
    const auto& profile=deviceControls(deviceTypeTX);
    assert(profile.count==9 && deviceControls(deviceTypeE2).height==0);
    // A different device may use an app key for Calendar and separate up/down buttons.
    const DeviceButton otherButtons[]={{keyIdHard1,{4,4,30,30},"Calendar",DeviceControlIcon::calendar},
                                      {keyIdUp,{40,4,30,30},"Up",DeviceControlIcon::up}};
    const DeviceControlProfile other{160,40,otherButtons,2,{0,0},0};
    assert(deviceControlAt(other,160,200,50,215)==keyIdUp);
    for(auto rotation:{Rotation::portrait_0,Rotation::landscape_90,Rotation::portrait_180,Rotation::landscape_270})
    for(int scale:{1,2}) {
        Guest guest;DisplayConfiguration display{320,480,0,160};SdlEventHandler input(&guest,scale,display,rotation);
        const bool landscape=rotation==Rotation::landscape_90 || rotation==Rotation::landscape_270;
        const int width=landscape?480:320,height=landscape?320:480;
        // The curved navigator is clickable between the arrow icons as well.
        const int nx=(width-profile.width)/2+profile.navigator.x,ny=height+profile.navigator.y;
        assert(deviceControlAt(profile,width,height,nx-23,ny-10)==keyIdLeft);
        assert(deviceControlAt(profile,width,height,nx+10,ny-23)==keyIdUp);
        assert(deviceControlAt(profile,width,height,nx,ny)==keyIdSelect);
        assert(deviceControlAt(profile,width,height,nx+38,ny+38)==keyInvalid);
        for(size_t i=0;i<profile.count;++i) {
            const auto& b=profile.buttons[i];auto r=deviceButtonRect(profile,b,width,height);
            const int x=(r.x+r.w/2)*scale,y=(r.y+r.h/2)*scale;
            mouse(SDL_MOUSEBUTTONDOWN,x,y);input.HandleEvents();guest.expect({{b.key,true}});guest.noPen();
            assert(input.PressedButtons()==(1u<<b.key));
            mouse(SDL_MOUSEBUTTONUP,x,y);input.HandleEvents();guest.expect({{b.key,false}});guest.noPen();assert(!input.PressedButtons());
            // Dragging out cancels the hold, even without a mouse-up in the window.
            mouse(SDL_MOUSEBUTTONDOWN,x,y);input.HandleEvents();guest.expect({{b.key,true}});
            mouse(SDL_MOUSEMOTION,-1,y);input.HandleEvents();guest.expect({{b.key,false}});
            mouse(SDL_MOUSEBUTTONUP,-1,y);input.HandleEvents();guest.expect({});guest.noPen();
        }
        mouse(SDL_MOUSEBUTTONDOWN,0,height*scale+1);mouse(SDL_MOUSEBUTTONUP,0,height*scale+1);
        input.HandleEvents();guest.expect({});guest.noPen();
        // Rotated screen coordinates remain separate from the new strip.
        mouse(SDL_MOUSEBUTTONDOWN,0,0);input.HandleEvents();
        guest.pen(true,rotation==Rotation::landscape_90 || rotation==Rotation::portrait_180?319:0,
                       rotation==Rotation::portrait_180 || rotation==Rotation::landscape_270?479:0);
        mouse(SDL_MOUSEMOTION,0,height*scale+1);input.HandleEvents();guest.pen(false);guest.expect({});
        const auto home=deviceButtonRect(profile,profile.buttons[0],width,height);
        int x=(home.x+10)*scale,y=(home.y+10)*scale;
        // Host hardware shortcuts work without attaching the Palm keyboard.
        for (auto [host, palm] : {std::pair<SDL_Keycode,KeyId>{SDLK_LEFT,keyIdLeft},
                {SDLK_RIGHT,keyIdRight}, {SDLK_UP,keyIdUp}, {SDLK_DOWN,keyIdDown},
                {SDLK_RETURN,keyIdSelect}, {SDLK_F1,keyIdHard1}, {SDLK_F2,keyIdHard2},
                {SDLK_F3,keyIdHard3}, {SDLK_F4,keyIdHard4}, {SDLK_ESCAPE,keyIdPower}}) {
            key(host,true); input.HandleEvents(); guest.expect({{palm,true}});
            key(host,false); input.HandleEvents(); guest.expect({{palm,false}});
        }
        // The keyboard and pointer can hold the same key without releasing one another.
        key(SDLK_F1,true);mouse(SDL_MOUSEBUTTONDOWN,x,y);input.HandleEvents();guest.expect({{keyIdHard1,true}});
        mouse(SDL_MOUSEBUTTONUP,x,y);input.HandleEvents();guest.expect({});assert(input.PressedButtons()&(1u<<keyIdHard1));
        key(SDLK_F1,false);input.HandleEvents();guest.expect({{keyIdHard1,false}});
        mouse(SDL_MOUSEBUTTONDOWN,x,y);key(SDLK_F1,true);input.HandleEvents();guest.expect({{keyIdHard1,true}});
        key(SDLK_F1,false);input.HandleEvents();guest.expect({});assert(input.PressedButtons()&(1u<<keyIdHard1));
        // Independent held keys allow hardware button chords.
        key(SDLK_UP,true);input.HandleEvents();guest.expect({{keyIdUp,true}});
        window(SDL_WINDOWEVENT_FOCUS_LOST);input.HandleEvents();guest.expect({{keyIdHard1,false},{keyIdUp,false}});assert(!input.PressedButtons());
        mouse(SDL_MOUSEBUTTONUP,x,y);key(SDLK_UP,false);input.HandleEvents();guest.expect({});
        mouse(SDL_MOUSEBUTTONDOWN,x,y);input.HandleEvents();guest.expect({{keyIdHard1,true}});
        input.SetRotation(Rotation::portrait_0);guest.expect({{keyIdHard1,false}});assert(!input.PressedButtons());
        input.SetRotation(rotation);
        guest.attached=true;guest.events.clear();
        mouse(SDL_MOUSEBUTTONDOWN,x,y);input.HandleEvents();guest.expect({{keyIdHard1,true}});
        assert(guest.events.empty()); // Physical controls bypass the optional infrared accessory.
        window(SDL_WINDOWEVENT_LEAVE);input.HandleEvents();guest.expect({{keyIdHard1,false}});
        guest.attached=false;
        mouse(SDL_MOUSEBUTTONDOWN,x,y);input.HandleEvents();guest.expect({{keyIdHard1,true}});
        SDL_Event quit{};quit.type=SDL_QUIT;assert(SDL_PushEvent(&quit)==1);input.HandleEvents();
        guest.expect({{keyIdHard1,false}});assert(input.QuitRequested() && !input.PressedButtons());
        // Vector drawing uses the same profile as hit testing at each scale/orientation.
        auto* surface=SDL_CreateRGBSurfaceWithFormat(0,width*scale,(height+profile.height)*scale,32,SDL_PIXELFORMAT_RGBA32);
        assert(surface);auto* renderer=SDL_CreateSoftwareRenderer(surface);assert(renderer);
        SDL_SetRenderDrawColor(renderer,255,255,255,255);SDL_RenderClear(renderer);
        drawDeviceControls(renderer,profile,width,height,scale,0);
        auto pixel=[&](int xx,int yy){return reinterpret_cast<Uint32*>(static_cast<Uint8*>(surface->pixels)+yy*surface->pitch)[xx];};
        auto idle=pixel((home.x+5)*scale,(home.y+home.h/2)*scale);
        drawDeviceControls(renderer,profile,width,height,scale,1u<<keyIdHard1);
        assert(idle!=pixel((home.x+5)*scale,(home.y+home.h/2)*scale));
        assert(pixel(0,0)==SDL_MapRGBA(surface->format,255,255,255,255));
        SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);
    }
    SDL_Quit();puts("PASS: device buttons, holds/chords, focus/drag/rotation release, touch isolation, scales and pressed rendering");
}
