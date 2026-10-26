#include <cassert>
#include <cstdio>
#include <vector>
#include <utility>
#include <SDL.h>
#include "SdlEventHandler.h"
class Guest : public SoC {
 public:
    bool attached=true;
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
static void key(SDL_Scancode scan,bool down,bool repeat=false){
    SDL_Event e{};e.type=down?SDL_KEYDOWN:SDL_KEYUP;e.key.keysym.scancode=scan;e.key.keysym.sym=SDL_GetKeyFromScancode(scan);e.key.repeat=repeat;assert(SDL_PushEvent(&e)==1);
}
int main(){
    assert(SDL_Init(SDL_INIT_EVENTS)==0);
    Guest guest;DisplayConfiguration display{320,480,0,2};SdlEventHandler input(&guest,1,display,Rotation::portrait_0);
    key(SDL_SCANCODE_Z,true);key(SDL_SCANCODE_Z,true,true);key(SDL_SCANCODE_Z,false);input.HandleEvents();
    assert((guest.events==std::vector<std::pair<uint8_t,bool>>{{0x1a,true},{0x1a,false}}));guest.events.clear();
    key(SDL_SCANCODE_LCTRL,true);key(SDL_SCANCODE_RCTRL,true);key(SDL_SCANCODE_LCTRL,false);input.HandleEvents();
    assert((guest.events==std::vector<std::pair<uint8_t,bool>>{{0x14,true}}));
    key(SDL_SCANCODE_RCTRL,false);input.HandleEvents();assert(guest.events.back()==std::make_pair(uint8_t(0x14),false));guest.events.clear();
    key(SDL_SCANCODE_Z,true);SDL_Event focus{};focus.type=SDL_WINDOWEVENT;focus.window.event=SDL_WINDOWEVENT_FOCUS_LOST;assert(SDL_PushEvent(&focus)==1);key(SDL_SCANCODE_Z,false);input.HandleEvents();
    assert((guest.events==std::vector<std::pair<uint8_t,bool>>{{0x1a,true},{0,false}}));guest.events.clear();
    guest.attached=false;key(SDL_SCANCODE_Z,true);key(SDL_SCANCODE_Z,false);input.HandleEvents();assert(guest.events.empty());
    SDL_Quit();puts("PASS: host keyboard scancodes, repeat suppression, shared modifiers, focus-loss release and detached input");
}
