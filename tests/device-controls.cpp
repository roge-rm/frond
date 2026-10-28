// Exercise real SDL pointer events against the production hardware-key path.
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <tuple>
#include <utility>
#include <SDL.h>
#include "SdlEventHandler.h"
#include "SdlRenderer.h"
class Guest : public SoC {
 public:
    explicit Guest(DeviceType5 device=deviceTypeTX) : device(device) {
        displayConfiguration=device==deviceTypeE2 ? DisplayConfiguration{320,320,120,144} : DisplayConfiguration{320,480,0,160};
    }
    DeviceType5 device;
    DisplayConfiguration display() const {return displayConfiguration;}
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
    DeviceType5 GetDeviceType() override{return device;}
    DisplayMode GetDisplayMode() override{return device==deviceTypeE2 ? DisplayMode::mode_320x320 : DisplayMode::mode_320x480;}
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
// Window pixel for a guest point: the inverse of SdlEventHandler's rotation, at the pixel's centre.
static std::pair<int,int> windowPoint(Rotation rotation,int scale,int w,int h,int gx,int gy) {
    const int x=gx*scale+scale/2,y=gy*scale+scale/2;
    switch(rotation) {
        case Rotation::landscape_90: return {y,scale*w-1-x};
        case Rotation::portrait_180: return {scale*w-1-x,scale*h-1-y};
        case Rotation::landscape_270: return {scale*h-1-y,x};
        default: return {x,y};
    }
}
static SDL_Color readPixel(SDL_Surface* s,int x,int y) {
    SDL_Color c;const auto p=reinterpret_cast<Uint32*>(static_cast<Uint8*>(s->pixels)+y*s->pitch)[x];
    SDL_GetRGB(p,s->format,&c.r,&c.g,&c.b);return c;
}
static int brightness(SDL_Color c) {return c.r+c.g+c.b;}
// The E2 silkscreen is drawn on the ROM's own rectangles, and taps on it reach the guest there.
static void silkscreen() {
    const auto& silk=deviceSilkscreen(deviceTypeE2);
    assert(silk.count==8 && silk.width==320 && silk.height==120);
    assert(!deviceSilkscreen(deviceTypeTX).count && !deviceSilkscreen(deviceTypePV).count);
    // 'aslk' 19000 in the E2 NOR, in Palm OS coordinates; the art uses them doubled from row 320.
    const std::pair<SilkscreenIcon,SDL_Rect> rom[]={
        {SilkscreenIcon::home,{0,164,23,28}},{SilkscreenIcon::menu,{0,194,23,28}},
        {SilkscreenIcon::favorites,{137,164,23,28}},{SilkscreenIcon::find,{137,194,23,28}},
        {SilkscreenIcon::clock,{25,164,11,19}},{SilkscreenIcon::brightness,{125,164,11,19}},
        {SilkscreenIcon::letters,{25,210,18,20}},{SilkscreenIcon::numbers,{118,210,18,20}}};
    for(auto [icon,r]:rom) {
        size_t found=0;
        for(size_t i=0;i<silk.count;++i)if(silk.buttons[i].icon==icon) {
            const auto& b=silk.buttons[i].bounds;++found;
            assert(b.x==r.x*2 && b.y==r.y*2-320 && b.w==r.w*2 && b.h==r.h*2);
        }
        assert(found==1);
    }
    auto same=[](SDL_Rect a,SDL_Rect b) {return a.x==b.x && a.y==b.y && a.w==b.w && a.h==b.h;};
    assert(same(silk.letters,{27*2,162*2-320,62*2,56*2}) && same(silk.numbers,{89*2,162*2-320,44*2,56*2}));
    const auto& controls=deviceControls(deviceTypeE2);
    for(int scale:{1,2}) {
        // Unrotated reference art at this scale.
        auto* ref=SDL_CreateRGBSurfaceWithFormat(0,320*scale,120*scale,32,SDL_PIXELFORMAT_RGBA32);
        auto* refRenderer=SDL_CreateSoftwareRenderer(ref);assert(ref && refRenderer);
        auto* art=createDeviceSilkscreen(refRenderer,silk,scale);assert(art);
        SDL_RenderCopy(refRenderer,art,nullptr,nullptr);SDL_DestroyTexture(art);
        auto at=[&](float x,float y) {return readPixel(ref,int(x*scale),int(y*scale));};
        // Home, Menu, Favorites and Find are light discs inside their reachable rectangle on the dark panel.
        for(size_t i=0;i<4;++i) {
            const auto& b=silk.buttons[i];const int h=std::min(b.bounds.y+b.bounds.h,120)-b.bounds.y;
            const float cx=b.bounds.x+b.bounds.w*0.5f,cy=b.bounds.y+h*0.5f,R=std::min(b.bounds.w,h)*0.5f-4;
            assert(cy-R-1.5f>b.bounds.y && cy+R+1.5f<b.bounds.y+h);
            const int panel=brightness(at(b.bounds.x+1.5f,b.bounds.y+1.5f));
            assert(panel<300 && brightness(at(cx,cy-R-1.5f))<panel+12 && brightness(at(cx,cy+R+1.5f))<panel+12);
            assert(brightness(at(cx,cy-R+1.5f))>panel+180 && brightness(at(cx-R+1.5f,cy))>panel+180);
        }
        // Lines between the columns and Graffiti areas; the letter/number split shows as ticks only.
        const int panel=brightness(at(120,60));
        for(float x:{48.f,273.f})assert(brightness(at(x,60))>panel+120 && brightness(at(x+2,60))<panel+12);
        for(float y:{4.f,116.f})assert(brightness(at(silk.numbers.x,y))>panel+120);
        assert(brightness(at(silk.numbers.x,60))<panel+12);
        for(auto rotation:{Rotation::portrait_0,Rotation::landscape_90,Rotation::portrait_180,Rotation::landscape_270}) {
            Guest guest(deviceTypeE2);DisplayConfiguration display=guest.display();
            const bool landscape=rotation==Rotation::landscape_90 || rotation==Rotation::landscape_270;
            const int width=landscape?440:320,height=landscape?320:440;
            auto* window=SDL_CreateWindow("e2",0,0,width*scale,(height+controls.height)*scale,0);assert(window);
            {
                SdlRenderer renderer(window,&guest,scale,display,rotation);
                renderer.Draw(true,0);
                auto* shot=SDL_CreateRGBSurfaceWithFormat(0,width*scale,(height+controls.height)*scale,32,SDL_PIXELFORMAT_RGBA32);
                assert(SDL_RenderReadPixels(SDL_GetRenderer(window),nullptr,SDL_PIXELFORMAT_RGBA32,shot->pixels,shot->pitch)==0);
                SdlEventHandler input(&guest,scale,display,rotation);
                for(size_t i=0;i<silk.count;++i) {
                    const auto& b=silk.buttons[i].bounds;const int h=std::min(b.y+b.h,120)-b.y;
                    for(auto [px,py]:{std::pair<int,int>{b.x+b.w/2,b.y+h/2},{b.x+2,b.y+2},{b.x+b.w-3,b.y+2},
                                      {b.x+2,b.y+h-3},{b.x+b.w-3,b.y+h-3}}) {
                        // The window shows the art for the guest point a click there sends.
                        const auto [wx,wy]=windowPoint(rotation,scale,320,440,px,320+py);
                        const auto drawn=readPixel(shot,wx,wy),expected=readPixel(ref,px*scale+scale/2,py*scale+scale/2);
                        assert(std::abs(drawn.r-expected.r)<=2 && std::abs(drawn.g-expected.g)<=2 && std::abs(drawn.b-expected.b)<=2);
                    }
                    const auto [wx,wy]=windowPoint(rotation,scale,320,440,b.x+b.w/2,320+b.y+h/2);
                    mouse(SDL_MOUSEBUTTONDOWN,wx,wy);input.HandleEvents();guest.pen(true,b.x+b.w/2,320+b.y+h/2);guest.expect({});
                    mouse(SDL_MOUSEBUTTONUP,wx,wy);input.HandleEvents();guest.pen(false);guest.expect({});
                }
                // Graffiti strokes pass through too, down to the last digitizer row.
                for(auto [px,py]:{std::pair<int,int>{silk.letters.x+20,silk.letters.y+50},{silk.numbers.x+40,silk.numbers.y+60},{160,119}}) {
                    const auto [wx,wy]=windowPoint(rotation,scale,320,440,px,320+py);
                    mouse(SDL_MOUSEBUTTONDOWN,wx,wy);input.HandleEvents();guest.pen(true,px,320+py);guest.expect({});
                    mouse(SDL_MOUSEBUTTONUP,wx,wy);input.HandleEvents();guest.pen(false);
                }
                SDL_FreeSurface(shot);
            }
            SDL_DestroyWindow(window);
        }
        SDL_DestroyRenderer(refRenderer);SDL_FreeSurface(ref);
    }
}

static void exercise(DeviceType5 device) {
    const auto& profile=deviceControls(device);
    for(auto rotation:{Rotation::portrait_0,Rotation::landscape_90,Rotation::portrait_180,Rotation::landscape_270})
    for(int scale:{1,2}) {
        Guest guest(device);DisplayConfiguration display=guest.display();
        SdlEventHandler input(&guest,scale,display,rotation);
        const int screenWidth=display.width,screenHeight=display.height+display.graffitiHeight;
        const bool landscape=rotation==Rotation::landscape_90 || rotation==Rotation::landscape_270;
        const int width=landscape?screenHeight:screenWidth,height=landscape?screenWidth:screenHeight;
        const auto& nav=profile.navigator;
        const int left=(width-profile.width)/2+nav.x,top=height+nav.y;
        const int nx=left+nav.w/2,ny=top+nav.h/2;
        // The whole rounded five-way ring is clickable, split along its diagonals around the select pill.
        assert(deviceControlAt(profile,width,height,left+2,ny)==keyIdLeft);
        assert(deviceControlAt(profile,width,height,left+nav.w-3,ny+4)==keyIdRight);
        assert(deviceControlAt(profile,width,height,nx+10,top+2)==keyIdUp);
        assert(deviceControlAt(profile,width,height,nx-10,top+nav.h-3)==keyIdDown);
        assert(deviceControlAt(profile,width,height,left+14,top+7)==keyIdUp);
        assert(deviceControlAt(profile,width,height,left+8,top+12)==keyIdLeft);
        assert(deviceControlAt(profile,width,height,nx,ny)==keyIdSelect);
        assert(deviceControlAt(profile,width,height,nx+profile.select.w/2-2,ny)==keyIdSelect);
        assert(deviceControlAt(profile,width,height,nx,ny-profile.select.h/2-3)==keyIdUp);
        assert(deviceControlAt(profile,width,height,nx-profile.select.w/2-3,ny)==keyIdLeft);
        // Rounded corners and the gaps between controls are not buttons.
        assert(deviceControlAt(profile,width,height,left+1,top+1)==keyInvalid);
        assert(deviceControlAt(profile,width,height,left+nav.w-2,top+nav.h-2)==keyInvalid);
        assert(deviceControlAt(profile,width,height,left-3,ny)==keyInvalid);
        assert(deviceControlAt(profile,width,height,nx,top+nav.h+1)==keyInvalid);
        for(int k=0;k<4;++k) {
            const auto& r=profile.buttons[k].bounds;const int x0=(width-profile.width)/2+r.x;
            // Rounded app keys: the corners and the gap above are outside.
            assert(deviceControlAt(profile,width,height,x0+1,height+r.y+1)==keyInvalid);
            assert(deviceControlAt(profile,width,height,x0+1,height+r.y+r.h/2)==profile.buttons[k].key);
            assert(deviceControlAt(profile,width,height,x0+r.w/2,height+r.y-2)==keyInvalid);
            // E2 app keys sit on one straight line through the five-way's centre.
            if(device==deviceTypeE2)assert(r.y==profile.buttons[0].bounds.y && r.h==profile.buttons[0].bounds.h &&
                                           r.y*2+r.h==nav.y*2+nav.h);
        }
        const auto pocket=deviceButtonRect(profile,profile.buttons[0],width,height);
        assert(deviceControlAt(profile,width,height,pocket.x,pocket.y)==keyInvalid);
        assert(deviceControlAt(profile,width,height,pocket.x+pocket.w,pocket.y+pocket.h/2)==keyInvalid);
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
        guest.pen(true,rotation==Rotation::landscape_90 || rotation==Rotation::portrait_180?screenWidth-1:0,
                       rotation==Rotation::portrait_180 || rotation==Rotation::landscape_270?screenHeight-1:0);
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
        // Each five-way part darkens where it is hit.
        for(auto [part,px,py]:{std::tuple<KeyId,int,int>{keyIdUp,nx,top+4},{keyIdDown,nx,top+nav.h-4},
                {keyIdLeft,left+4,ny},{keyIdRight,left+nav.w-4,ny},{keyIdSelect,nx,ny}}) {
            assert(deviceControlAt(profile,width,height,px,py)==part);
            drawDeviceControls(renderer,profile,width,height,scale,0);
            const auto before=pixel(px*scale,py*scale);
            drawDeviceControls(renderer,profile,width,height,scale,1u<<part);
            const auto after=pixel(px*scale,py*scale);
            Uint8 r0,g0,b0,r1,g1,b1;
            SDL_GetRGB(before,surface->format,&r0,&g0,&b0);SDL_GetRGB(after,surface->format,&r1,&g1,&b1);
            assert(r1+g1+b1<r0+g0+b0);
        }
        assert(pixel(0,0)==SDL_MapRGBA(surface->format,255,255,255,255));
        SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);
    }
}

int main() {
    SDL_setenv("SDL_VIDEODRIVER","dummy",1);assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)==0);
    const auto& profile=deviceControls(deviceTypeTX);
    assert(profile.count==9 && profile.style==DeviceControlStyle::tx && profile.height==84);
    const auto& e2=deviceControls(deviceTypeE2);
    assert(e2.count==9 && e2.height==80 && e2.style==DeviceControlStyle::e2);
    // E2 app keys follow the ROM: hard1-4 are Calendar, Contacts, Tasks and Note Pad, left to right.
    for(int k=0;k<4;++k)assert(e2.buttons[k].key==KeyId(keyIdHard1+k) && (k==0 || e2.buttons[k].bounds.x>e2.buttons[k-1].bounds.x));
    assert(e2.buttons[1].bounds.x+e2.buttons[1].bounds.w<e2.navigator.x && e2.buttons[2].bounds.x>e2.navigator.x+e2.navigator.w);
    assert(deviceControls(deviceTypePV).height==0 && deviceControls(deviceTypeFrankenE2).height==0);
    // A different device may use an app key for Calendar and separate up/down buttons.
    const DeviceButton otherButtons[]={{keyIdHard1,{4,4,30,30},"Calendar",DeviceControlIcon::calendar},
                                      {keyIdUp,{40,4,30,30},"Up",DeviceControlIcon::up}};
    const DeviceControlProfile other{160,40,otherButtons,2,{0,0,0,0},0,{0,0,0,0}};
    assert(deviceControlAt(other,160,200,50,215)==keyIdUp);
    exercise(deviceTypeTX);
    exercise(deviceTypeE2);
    silkscreen();
    SDL_Quit();puts("PASS: TX and E2 device buttons, holds/chords, focus/drag/rotation release, touch isolation, scales, pressed rendering and E2 silkscreen");
}
