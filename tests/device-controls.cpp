// Exercise real SDL pointer events against the production hardware-key path.
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <tuple>
#include <fstream>
#include "InputReplay.h"
#include <utility>
#include <SDL.h>
#include "SdlEventHandler.h"
#include "SdlRenderer.h"
#include "DisplayChange.h"
#include "WindowScale.h"
#include <SDL_image.h>
// Synthetic finger events come from a touch id SDL doesn't know, so count them as a touch screen.
struct TouchInput : SdlEventHandler {
    using SdlEventHandler::SdlEventHandler;
    bool DirectTouch(SDL_TouchID) const override { return true; }
};
class Guest : public SoC {
 public:
    explicit Guest(DeviceType5 device=deviceTypeTX) : device(device) {
        displayConfiguration=square() ? DisplayConfiguration{320,320,120,144} : DisplayConfiguration{320,480,0,160};
    }
    DeviceType5 device;
    // The E2 and Zire 72 have a 320x320 LCD over a 120-row silkscreen.
    bool square() const {return device==deviceTypeE2 || device==deviceTypeZire72;}
    DisplayConfiguration display() const {return displayConfiguration;}
    void setDisplay(DisplayConfiguration c) {displayConfiguration=c;}
    bool attached=false;
    bool powerSupported=true;
    bool PowerButtonSupported() const override {return powerSupported;}
    void expect(std::initializer_list<std::pair<KeyId,bool>> expected) {
        assert(keyEventQueue->GetSize()==expected.size());
        for(auto [key,down]:expected) {auto e=keyEventQueue->Pop();assert(e.key==key && e.keyDown==down);}
    }
    void noPen() {assert(penEventQueue->GetSize()==0);}
    void pen(bool down,int x=0,int y=0) {
        assert(penEventQueue->GetSize()==1);auto e=penEventQueue->Pop();
        assert(e.penDown==down);if(down) { if(e.x!=x || e.y!=y)std::fprintf(stderr,"pen got %d,%d expected %d,%d\n",e.x,e.y,x,y);assert(e.x==x && e.y==y); }
    }
    // One pen event of either position, inside the native LCD.
    void penInside(bool down) {
        assert(penEventQueue->GetSize()==1);auto e=penEventQueue->Pop();
        assert(e.penDown==down);
        if(down) assert(e.x>=0 && e.y>=0 && e.x<int(displayConfiguration.width) && e.y<int(displayConfiguration.height));
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
    std::vector<uint32_t> frame;
    uint32_t* GetPendingFrame(uint32_t& first,uint32_t& last) override {
        if(frame.empty())return nullptr;
        first=0;last=displayConfiguration.height-1;return frame.data();
    }
    void ResetPendingFrame() override{}
    DeviceType5 GetDeviceType() override{return device;}
    DisplayMode GetDisplayMode() override{return square() ? DisplayMode::mode_320x320 : DisplayMode::mode_320x480;}
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
static std::pair<int,int> windowPoint(Rotation rotation,float scale,int w,int h,int gx,int gy) {
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
// The window's silkscreen at every scale and rotation: the art drawn where a click sends the pen,
// for every button and down to the last digitizer row. The Zire 72's ROM rectangles are checked
// in artSilkscreen.
static void silkscreen(DeviceType5 device) {
    const bool e2=device==deviceTypeE2;
    const auto& silk=deviceSilkscreen(device);
    assert(silk.count==8 && silk.width==320 && silk.height==120);
    assert(!deviceSilkscreen(deviceTypeTX).count && !deviceSilkscreen(deviceTypePV).count);
    // 'aslk' 19000 in the E2 NOR, in Palm OS coordinates; the art uses them doubled from row 320.
    const std::pair<SilkscreenIcon,SDL_Rect> rom[]={
        {SilkscreenIcon::home,{0,164,23,28}},{SilkscreenIcon::menu,{0,194,23,28}},
        {SilkscreenIcon::favorites,{137,164,23,28}},{SilkscreenIcon::find,{137,194,23,28}},
        {SilkscreenIcon::clock,{25,164,11,19}},{SilkscreenIcon::brightness,{125,164,11,19}},
        {SilkscreenIcon::letters,{25,210,18,20}},{SilkscreenIcon::numbers,{118,210,18,20}}};
    for(auto [icon,r]:rom) {
        if(!e2)break;
        size_t found=0;
        for(size_t i=0;i<silk.count;++i)if(silk.buttons[i].icon==icon) {
            const auto& b=silk.buttons[i].bounds;++found;
            assert(b.x==r.x*2 && b.y==r.y*2-320 && b.w==r.w*2 && b.h==r.h*2);
        }
        assert(found==1);
    }
    auto same=[](SDL_Rect a,SDL_Rect b) {return a.x==b.x && a.y==b.y && a.w==b.w && a.h==b.h;};
    if(e2)assert(same(silk.letters,{27*2,162*2-320,62*2,56*2}) && same(silk.numbers,{89*2,162*2-320,44*2,56*2}));
    // Every button has reachable rows, and its drawn mark sits inside them.
    for(size_t i=0;i<silk.count;++i)assert(silk.buttons[i].bounds.y+16<=120);
    const auto& controls=deviceControls(device);
    for(float scale:{1.f,1.25f,1.5f,1.75f,2.f}) {
        // Unrotated reference art at this scale.
        auto* ref=SDL_CreateRGBSurfaceWithFormat(0,320*scale,120*scale,32,SDL_PIXELFORMAT_RGBA32);
        auto* refRenderer=SDL_CreateSoftwareRenderer(ref);assert(ref && refRenderer);
        auto* art=createDeviceSilkscreen(refRenderer,silk,scale);assert(art);
        SDL_RenderCopy(refRenderer,art,nullptr,nullptr);SDL_DestroyTexture(art);
        auto at=[&](float x,float y) {return readPixel(ref,int(x*scale),int(y*scale));};
        // Exact thin-line appearance samples apply at integral scales.
        if(scale==1 || scale==2) {
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
            // ABC and 123 are printed inside the reachable part of their ROM areas.
            for(size_t i=6;i<8;++i) {
                const auto& b=silk.buttons[i].bounds;int ink=0;
                for(int y=b.y;y<std::min(b.y+b.h,120);++y)for(int x=b.x;x<b.x+b.w;++x)ink+=brightness(at(x+0.5f,y+0.5f))>panel+150;
                assert(ink>20);
            }
            for(float x:{48.f,273.f})assert(brightness(at(x,60))>panel+120 && brightness(at(x+2,60))<panel+12);
            for(float y:{4.f,116.f})assert(brightness(at(silk.numbers.x,y))>panel+120);
            assert(brightness(at(silk.numbers.x,60))<panel+12);
        }
        for(auto rotation:{Rotation::portrait_0,Rotation::landscape_90,Rotation::portrait_180,Rotation::landscape_270}) {
            Guest guest(device);DisplayConfiguration display=guest.display();
            const bool landscape=rotation==Rotation::landscape_90 || rotation==Rotation::landscape_270;
            const int width=landscape?440:320,height=landscape?320:440;
            auto* window=SDL_CreateWindow("e2",0,0,width*scale,(height+controls.height)*scale,0);assert(window);
            {
                SdlRenderer renderer(window,&guest,scale,display,rotation);
                renderer.Draw(true,0);
                auto* shot=SDL_CreateRGBSurfaceWithFormat(0,width*scale,(height+controls.height)*scale,32,SDL_PIXELFORMAT_RGBA32);
                assert(SDL_RenderReadPixels(SDL_GetRenderer(window),nullptr,SDL_PIXELFORMAT_RGBA32,shot->pixels,shot->pitch)==0);
                TouchInput input(&guest,scale,display,rotation);
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
    for(float scale:{1.f,1.25f,1.5f,1.75f,2.f}) {
        Guest guest(device);DisplayConfiguration display=guest.display();
        TouchInput input(&guest,scale,display,rotation);
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
                {SDLK_F3,keyIdHard3}, {SDLK_F4,keyIdHard4}, {SDLK_F5,keyIdVoice}, {SDLK_ESCAPE,keyIdPower}}) {
            if (palm == keyIdPower && device == deviceTypeFrankenE2) {
                key(host,true); key(host,false); input.HandleEvents(); guest.expect({});
                continue;
            }
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

// Zire 72 and m515 art: no board yet, so hit areas, pressed drawing and silkscreens are checked
// through the shared helpers on their own screen sizes.
static void artDevice(DeviceArt art) {
    const auto& p=deviceControls(art);
    const bool zire=art==DeviceArt::zire72;
    // Screen width and height in LCD pixels, silkscreen included.
    const int width=zire ? 320 : 160,height=zire ? 440 : 220,u=p.unit;
    assert(p.width==320 && u==(zire ? 1 : 2) && deviceControlsHeight(p)*u==p.height);
    assert(p.count==(zire ? 9u : 6u) && p.style==(zire ? DeviceControlStyle::zire72 : DeviceControlStyle::m515));
    for(int k=0;k<4;++k)assert(p.buttons[k].key==KeyId(keyIdHard1+k));
    // Strip units to the screen point whose centre lands there.
    auto at=[&](float x,float y) {
        return deviceControlAt(p,width,height,int(std::floor(x/u)),height+int(std::floor(y/u)));
    };
    for(size_t i=0;i<p.count;++i) {
        const auto& b=p.buttons[i];
        const auto r=deviceButtonRect(p,b,width,height);
        assert(r.x>=0 && r.x+r.w<=width && r.y>=height && r.y+r.h<=height+deviceControlsHeight(p));
        // Each key hits at its centre, in strip units and through its screen rectangle.
        assert(at(b.bounds.x+b.bounds.w*0.5f,b.bounds.y+b.bounds.h*0.5f)==b.key);
        assert(deviceControlAt(p,width,height,r.x+r.w/2,r.y+r.h/2)==b.key);
        if(deviceNavigatorButton(p,b))continue;
        // Pills and round keys: their corners are outside, their ends inside.
        const auto& k=b.bounds;
        assert(at(k.x+1,k.y+1)==keyInvalid && at(k.x+k.w-2,k.y+k.h-2)==keyInvalid);
        assert(at(k.x+2,k.y+k.h*0.5f)==b.key && at(k.x+k.w-3,k.y+k.h*0.5f)==b.key);
        assert(at(k.x+k.w*0.5f,k.y-2)==keyInvalid && at(k.x+k.w*0.5f,k.y+k.h+1)==keyInvalid);
    }
    if(zire) {
        // Calendar over Contacts on the left, Camera (hard4) over Music (hard3) on the right.
        const auto& b=p.buttons;
        assert(b[0].bounds.x==b[1].bounds.x && b[0].bounds.y<b[1].bounds.y && b[2].bounds.x==b[3].bounds.x);
        assert(b[3].bounds.y<b[2].bounds.y && b[3].icon==DeviceControlIcon::media && b[2].icon==DeviceControlIcon::music);
        assert(b[1].bounds.x+b[1].bounds.w<p.navigator.x && b[2].bounds.x>p.navigator.x+p.navigator.w);
        // Gaps between the stacked pills and between the pills and the five-way.
        assert(at(50,(b[0].bounds.y+b[0].bounds.h+b[1].bounds.y)*0.5f)==keyInvalid);
        assert(at(270,(b[3].bounds.y+b[3].bounds.h+b[2].bounds.y)*0.5f)==keyInvalid);
        assert(at((b[0].bounds.x+b[0].bounds.w+p.navigator.x)*0.5f,52)==keyInvalid);
        assert(at((b[2].bounds.x+p.navigator.x+p.navigator.w)*0.5f,52)==keyInvalid);
        // Five-way: a rounded square split on its diagonals round a rounded-rectangle select key.
        const auto& n=p.navigator;const auto& s=p.select;
        const float cx=n.x+n.w*0.5f,cy=n.y+n.h*0.5f;
        assert(p.selectCorner>0 && p.selectCorner<s.h*0.5f);
        assert(at(cx,cy)==keyIdSelect && at(s.x+3.5f,s.y+3.5f)==keyIdSelect);
        // A pill select would leave this corner to the ring.
        assert(deviceRoundedDistance(s,s.h*0.5f,s.x+3.5f,s.y+3.5f)>1);
        assert(at(s.x-3,s.y-3)!=keyIdSelect && at(s.x-3,s.y-3)!=keyInvalid);
        assert(at(cx,n.y+2)==keyIdUp && at(cx,n.y+n.h-3)==keyIdDown);
        assert(at(n.x+2,cy)==keyIdLeft && at(n.x+n.w-3,cy)==keyIdRight);
        assert(at(cx,s.y-3)==keyIdUp && at(s.x-3,cy)==keyIdLeft && at(s.x+s.w+2,cy)==keyIdRight && at(cx,s.y+s.h+2)==keyIdDown);
        assert(at(n.x+1,n.y+1)==keyInvalid && at(n.x+n.w-2,n.y+n.h-2)==keyInvalid);
        assert(at(cx,n.y-2)==keyInvalid && at(n.x-2,cy)==keyInvalid);
    } else {
        // m515: no five-way; the scroll keys are round and stacked, with a gap between them.
        assert(p.navigator.w==0);
        const auto& up=p.buttons[4].bounds;const auto& dn=p.buttons[5].bounds;
        assert(p.buttons[4].key==keyIdUp && p.buttons[5].key==keyIdDown && up.x==dn.x && up.y+up.h<=dn.y);
        assert(at(up.x+up.w*0.5f,(up.y+up.h+dn.y)*0.5f)==keyInvalid);
        assert(at(up.x+up.w*0.5f,up.y+3)==keyIdUp && at(dn.x+dn.w*0.5f,dn.y+dn.h-3)==keyIdDown);
        // Gaps between the app keys and the scroll keys.
        for(int k=0;k<3;++k) {
            const auto& a=p.buttons[k].bounds;const auto& b=p.buttons[k+1].bounds;
            if(k==1)continue;
            assert(at((a.x+a.w+b.x)*0.5f,a.y+a.h*0.5f)==keyInvalid);
        }
        assert(at((p.buttons[1].bounds.x+p.buttons[1].bounds.w+up.x)*0.5f,40)==keyInvalid);
        assert(at((up.x+up.w+p.buttons[2].bounds.x)*0.5f,40)==keyInvalid);
        // The outer keys sit higher, as on the device.
        assert(p.buttons[0].bounds.y<p.buttons[1].bounds.y && p.buttons[3].bounds.y<p.buttons[2].bounds.y);
        // The scroll housing, 5 units round the keys, has the same gap above, under the
        // 4-unit bezel edge, as below.
        const int above=(up.y-5)-4,below=p.height-(dn.y+dn.h+5);
        assert(above>=2 && std::abs(above-below)<=1);
    }
    // Every key has a visible pressed state, at 1x and 2x.
    for(float scale:{1.f,2.f}) {
        const int sh=deviceControlsHeight(p);
        auto* surface=SDL_CreateRGBSurfaceWithFormat(0,int(width*scale),int(sh*scale),32,SDL_PIXELFORMAT_RGBA32);
        auto* renderer=SDL_CreateSoftwareRenderer(surface);assert(surface && renderer);
        auto sum=[&](float x,float y) {
            const auto c=readPixel(surface,int(x/u*scale),int(y/u*scale));return brightness(c);
        };
        for(size_t i=0;i<p.count;++i) {
            const auto& b=p.buttons[i].bounds;
            // A spot on the key away from its icon.
            float x=b.x+b.w*0.5f,y=b.y+b.h*0.5f;
            if(!deviceNavigatorButton(p,p.buttons[i]) && p.buttons[i].key!=keyIdUp && p.buttons[i].key!=keyIdDown)
                x=b.x+b.w*(zire ? 0.2f : 0.5f),y=b.y+b.h*(zire ? 0.5f : 0.75f);
            drawDeviceControls(renderer,p,width,0,scale,0);
            const int idle=sum(x,y);
            drawDeviceControls(renderer,p,width,0,scale,1u<<p.buttons[i].key);
            const int held=sum(x,y);
            if(held>=idle-12)std::fprintf(stderr,"key %d at %g,%g: idle %d held %d\n",p.buttons[i].key,x,y,idle,held);
            assert(held<idle-12);
        }
        SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);
    }
}

static void artSilkscreen(DeviceArt art) {
    const auto& silk=deviceSilkscreen(art);
    const bool zire=art==DeviceArt::zire72;
    assert(silk.count==8 && silk.width==320 && silk.height==120 && silk.unit==(zire ? 1 : 2));
    // ROM tables in Palm OS coordinates (Zire 72 NOR 'aslk' 19000, m515 'silk' 10002), doubled
    // from the top of the silkscreen and kept to the panel's width.
    const int top=160;
    const std::pair<SilkscreenIcon,SDL_Rect> zireRom[]={
        {SilkscreenIcon::home,{0,162,23,28}},{SilkscreenIcon::menu,{0,192,23,28}},
        {SilkscreenIcon::favorites,{138,164,27,28}},{SilkscreenIcon::find,{138,192,27,28}},
        {SilkscreenIcon::clock,{23,162,11,19}},{SilkscreenIcon::brightness,{121,162,16,19}},
        {SilkscreenIcon::letters,{23,206,18,20}},{SilkscreenIcon::numbers,{115,206,23,20}}};
    const std::pair<SilkscreenIcon,SDL_Rect> m515Rom[]={
        {SilkscreenIcon::home,{-3,163,24,27}},{SilkscreenIcon::menu,{-3,191,24,28}},
        {SilkscreenIcon::calc,{140,163,22,30}},{SilkscreenIcon::find,{140,191,22,38}},
        {SilkscreenIcon::clock,{22,163,11,19}},{SilkscreenIcon::brightness,{127,163,11,19}},
        {SilkscreenIcon::letters,{22,206,14,23}},{SilkscreenIcon::numbers,{124,204,15,25}}};
    for(auto [icon,r]:zire ? std::vector(std::begin(zireRom),std::end(zireRom)) : std::vector(std::begin(m515Rom),std::end(m515Rom))) {
        size_t found=0;
        for(size_t i=0;i<silk.count;++i)if(silk.buttons[i].icon==icon) {
            const auto& b=silk.buttons[i].bounds;++found;
            const int x0=std::max(0,r.x*2),x1=std::min(320,(r.x+r.w)*2);
            assert(b.x==x0 && b.w==x1-x0 && b.y==r.y*2-top*2 && b.h==r.h*2);
        }
        assert(found==1);
    }
    auto same=[](SDL_Rect a,SDL_Rect b) {return a.x==b.x && a.y==b.y && a.w==b.w && a.h==b.h;};
    if(zire)assert(same(silk.letters,{54,4,124,132}) && same(silk.numbers,{178,4,88,132}));
    else assert(same(silk.letters,{54,8,124,112}) && same(silk.numbers,{178,8,88,112}));
    for(float scale:{1.f,2.f}) {
        // The texture covers the LCD's silkscreen rows: 120 on the Zire 72, 60 on the m515.
        const int w=int(320*scale/silk.unit),h=int(120*scale/silk.unit);
        auto* s=SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_RGBA32);
        auto* r=SDL_CreateSoftwareRenderer(s);assert(s && r);
        auto* t=createDeviceSilkscreen(r,silk,scale);assert(t);
        int tw,th;SDL_QueryTexture(t,nullptr,nullptr,&tw,&th);assert(tw==w && th==h);
        SDL_RenderCopy(r,t,nullptr,nullptr);SDL_DestroyTexture(t);
        auto at=[&](float x,float y) {return brightness(readPixel(s,int(x*scale/silk.unit),int(y*scale/silk.unit)));};
        // Discs are lighter than the panel round their glyphs.
        const int panel=at(120,60);
        for(size_t i=0;i<4;++i) {
            const auto& b=silk.buttons[i].bounds;const int bh=std::min(b.y+b.h,120)-b.y;
            const float cx=b.x+b.w*0.5f,cy=b.y+bh*0.5f,R=std::min(b.w,bh)*0.5f-(zire ? 4 : 2);
            assert(cx-R>=0 && cx+R<=320 && cy-R>=0 && cy+R<=120);
            assert(at(cx,cy-R+2.5f)>panel+150 && at(cx-R+2.5f,cy)>panel+150);
        }
        if(zire) {
            // Full-height lines close the side columns; the split is ticks only.
            for(float x:{48.5f,273.5f})assert(at(x,60)>panel+150);
            assert(at(178.5f,4)>panel+150 && at(178.5f,60)<panel+12);
        } else {
            // One rounded box with a triangle at each end of the split.
            assert(at(160,3.5f)>panel+150 && at(160,116.5f)>panel+150 && at(49.5f,60)>panel+150 && at(271.5f,60)>panel+150);
            assert(at(178,5)>panel+150 && at(178,115)>panel+150 && at(178,60)<panel+12);
        }
        SDL_DestroyRenderer(r);SDL_FreeSurface(s);
    }
}

// Palm IIIx: no board yet either. Four round app keys, a peanut scroll rocker split at its waist
// and a teal power key at the far left, under a 160-wide screen.
static void iiixArt() {
    const auto& p=deviceControls(DeviceArt::iiix);
    const int width=160,height=220,u=2;
    assert(p.width==320 && p.height==88 && p.unit==u && deviceControlsHeight(p)==44);
    assert(p.count==7 && p.style==DeviceControlStyle::iiix && p.navigator.w==0);
    const KeyId keys[]={keyIdHard1,keyIdHard2,keyIdHard3,keyIdHard4,keyIdUp,keyIdDown,keyIdPower};
    for(size_t i=0;i<p.count;++i)assert(p.buttons[i].key==keys[i]);
    auto at=[&](float x,float y) {
        return deviceControlAt(p,width,height,int(std::floor(x/u)),height+int(std::floor(y/u)));
    };
    for(size_t i=0;i<p.count;++i) {
        const auto& b=p.buttons[i];const auto& k=b.bounds;
        const auto r=deviceButtonRect(p,b,width,height);
        assert(r.x>=0 && r.x+r.w<=width && r.y>=height && r.y+r.h<=height+deviceControlsHeight(p));
        assert(at(k.x+k.w*0.5f,k.y+k.h*0.5f)==b.key);
        assert(deviceControlAt(p,width,height,r.x+r.w/2,r.y+r.h/2)==b.key);
        // Rounded corners miss; the middles of the sides hit. Power's corners are too small to miss.
        if(b.key!=keyIdPower)assert(at(k.x+1,k.y+1)==keyInvalid && at(k.x+k.w-2,k.y+k.h-2)==keyInvalid);
        assert(at(k.x+2,k.y+k.h*0.5f)==b.key && at(k.x+k.w-3,k.y+k.h*0.5f)==b.key);
        if(b.key!=keyIdDown)assert(at(k.x+k.w*0.5f,k.y-2)==keyInvalid);
        if(b.key!=keyIdUp)assert(at(k.x+k.w*0.5f,k.y+k.h+1)==keyInvalid);
    }
    const auto* b=p.buttons;
    // App keys left to right, the outer ones lower, with gaps between them.
    for(int k=1;k<4;++k)assert(b[k].bounds.x>b[k-1].bounds.x+b[k-1].bounds.w);
    assert(b[0].bounds.y>b[1].bounds.y && b[3].bounds.y>b[2].bounds.y);
    assert(at((b[0].bounds.x+b[0].bounds.w+b[1].bounds.x)*0.5f,46)==keyInvalid);
    assert(at((b[2].bounds.x+b[2].bounds.w+b[3].bounds.x)*0.5f,46)==keyInvalid);
    // The rocker: up above its waist, down below, no gap, and gaps to the keys either side.
    const auto& up=b[4].bounds;const auto& dn=b[5].bounds;
    assert(up.x==dn.x && up.y+up.h==dn.y && b[1].bounds.x+b[1].bounds.w<up.x && up.x+up.w<b[2].bounds.x);
    assert(at(160,dn.y-2)==keyIdUp && at(160,dn.y+1)==keyIdDown && at(152,dn.y-2)==keyIdUp && at(167,dn.y+1)==keyIdDown);
    assert(at(160,up.y+3)==keyIdUp && at(160,dn.y+dn.h-3)==keyIdDown);
    // The split row hits across the drawn waist, off centre too.
    for(float x:{153.f,155.f,165.f,167.f})assert(at(x,dn.y)==keyIdUp || at(x,dn.y)==keyIdDown);
    assert(at((b[1].bounds.x+b[1].bounds.w+up.x)*0.5f,44)==keyInvalid);
    assert(at((up.x+up.w+b[2].bounds.x)*0.5f,44)==keyInvalid);
    // Power: a small rounded rectangle at the left edge, apart from Date Book.
    const auto& pw=b[6].bounds;
    assert(b[6].corner>0 && b[6].corner<pw.w*0.5f && pw.x+pw.w<b[0].bounds.x);
    assert(at(pw.x+1.5f,pw.y+3.5f)==keyIdPower && at(pw.x+pw.w-2,pw.y+pw.h-3)==keyIdPower);
    assert(at((pw.x+pw.w+b[0].bounds.x)*0.5f,pw.y+pw.h*0.5f)==keyInvalid);
    assert(at(0.5f,pw.y+pw.h*0.5f)==keyInvalid && at(pw.x+pw.w*0.5f,pw.y-2)==keyInvalid);
    // Every key, power included, has a visible pressed state at 2x and 4x.
    for(float scale:{2.f,4.f}) {
        auto* surface=SDL_CreateRGBSurfaceWithFormat(0,int(width*scale),int(44*scale),32,SDL_PIXELFORMAT_RGBA32);
        auto* renderer=SDL_CreateSoftwareRenderer(surface);assert(surface && renderer);
        auto sum=[&](float x,float y) {return brightness(readPixel(surface,int(x/u*scale),int(y/u*scale)));};
        for(size_t i=0;i<p.count;++i) {
            const auto& k=b[i].bounds;
            // A spot away from the icon: low on the app keys and power, the middle of a lobe.
            float x=k.x+k.w*0.5f,y=k.y+k.h*(i<4 ? 0.85f : i==6 ? 0.8f : 0.5f);
            drawDeviceControls(renderer,p,width,0,scale,0);
            const int idle=sum(x,y);
            drawDeviceControls(renderer,p,width,0,scale,1u<<b[i].key);
            const int held=sum(x,y);
            if(held>=idle-12)std::fprintf(stderr,"IIIx key %d at %g,%g: idle %d held %d\n",b[i].key,x,y,idle,held);
            assert(held<idle-12);
        }
        // Holding one lobe of the rocker leaves the other lobe alone.
        for(int lobe=0;lobe<2;++lobe) {
            const float x=160,y=lobe ? up.y+15.5f : dn.y+dn.h-15.5f;
            drawDeviceControls(renderer,p,width,0,scale,0);
            const auto idle=readPixel(surface,int(x/u*scale),int(y/u*scale));
            drawDeviceControls(renderer,p,width,0,scale,1u<<(lobe ? keyIdDown : keyIdUp));
            const auto held=readPixel(surface,int(x/u*scale),int(y/u*scale));
            assert(idle.r==held.r && idle.g==held.g && idle.b==held.b);
        }
        SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);
    }

    // Silkscreen: the ROM's System 'silk' 10000, doubled from row 160.
    const auto& silk=deviceSilkscreen(DeviceArt::iiix);
    assert(silk.count==6 && silk.width==320 && silk.height==120 && silk.unit==2 && silk.style==SilkscreenStyle::iiix);
    const std::pair<SilkscreenIcon,SDL_Rect> rom[]={
        {SilkscreenIcon::home,{0,164,27,28}},{SilkscreenIcon::menu,{0,192,27,28}},
        {SilkscreenIcon::calc,{133,164,27,28}},{SilkscreenIcon::find,{133,192,27,28}},
        {SilkscreenIcon::letters,{27,206,18,14}},{SilkscreenIcon::numbers,{115,206,18,14}}};
    for(auto [icon,r]:rom) {
        size_t found=0;
        for(size_t i=0;i<silk.count;++i)if(silk.buttons[i].icon==icon) {
            const auto& sb=silk.buttons[i].bounds;++found;
            assert(sb.x==r.x*2 && sb.w==r.w*2 && sb.y==(r.y-160)*2 && sb.h==r.h*2);
        }
        assert(found==1);
    }
    auto same=[](SDL_Rect a,SDL_Rect c) {return a.x==c.x && a.y==c.y && a.w==c.w && a.h==c.h;};
    assert(same(silk.letters,{54,8,124,112}) && same(silk.numbers,{178,8,88,112}));
    for(float scale:{2.f,4.f}) {
        const int w=int(160*scale),h=int(60*scale);
        auto* s=SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_RGBA32);
        auto* r=SDL_CreateSoftwareRenderer(s);assert(s && r);
        auto* t=createDeviceSilkscreen(r,silk,scale);assert(t);
        int tw,th;SDL_QueryTexture(t,nullptr,nullptr,&tw,&th);assert(tw==w && th==h);
        SDL_RenderCopy(r,t,nullptr,nullptr);SDL_DestroyTexture(t);
        auto pt=[&](float x,float y) {return brightness(readPixel(s,int(x*scale/2),int(y*scale/2)));};
        // Pale speckled panel; dark discs round light glyphs.
        const int panel=pt(120,60);
        assert(panel>400);
        for(size_t i=0;i<4;++i) {
            const auto& sb=silk.buttons[i].bounds;
            const float cx=sb.x+sb.w*0.5f,cy=sb.y+sb.h*0.5f,R=std::min(sb.w,sb.h)*0.5f-6;
            assert(cx-R>=0 && cx+R<=320 && cy-R>=0 && cy+R<=120);
            assert(pt(cx,cy-R+2.5f)<panel-200 && pt(cx-R+2.5f,cy)<panel-200);
        }
        // One rounded box inside the Graffiti areas, with a dark triangle at each end of the split.
        assert(pt(160,3.5f)<panel-200 && pt(160,116.5f)<panel-200 && pt(61.5f,60)<panel-200 && pt(258.5f,60)<panel-200);
        assert(pt(178,5)<panel-200 && pt(178,115)<panel-200 && std::abs(pt(178,60)-panel)<40);
        assert(std::abs(pt(52,60)-panel)<40 && std::abs(pt(268,60)-panel)<40);
        SDL_DestroyRenderer(r);SDL_FreeSurface(s);
    }
}

static void tg50Art() {
    const auto& p=deviceControls(DeviceArt::tg50);
    const int width=320,height=320;
    const DeviceControlView all{},noKeyboard{false,true},noTab{true,false},neither{false,false};
    assert(p.width==320 && p.height==224 && p.unit==1 && p.style==DeviceControlStyle::tg50 && p.navigator.w==0);
    assert(p.count==8+42+4 && p.keyboardHeight==176 && p.sideTabWidth==38);
    assert(deviceSilkscreen(DeviceArt::tg50).count==0);
    // The strip shrinks without the keyboard; with the side tab it scales up to the full body width.
    assert(deviceControlsHeight(p)==251 && deviceControlsHeight(p,noTab)==224);
    assert(deviceControlsHeight(p,noKeyboard)==54 && deviceControlsHeight(p,neither)==48);
    assert(deviceControlsWidth(p,width)==358 && deviceControlsWidth(p,width,noKeyboard)==358);
    assert(deviceControlsWidth(p,width,noTab)==320 && deviceControlsWidth(p,width,neither)==320);
    // Every key once: the app row, all 42 keyboard keys and the tab's four; none of the Zodiac's keys.
    std::vector<int> seen(keyIdCount,0);
    for(size_t i=0;i<p.count;++i)++seen[p.buttons[i].key];
    for(int k=keyIdHard1;k<keyIdCount;++k)assert(seen[k]==(k==keyIdLeft || k==keyIdRight || k==keyIdSelect || k==keyIdPower || k==keyIdVoice || k==keyIdHold || k==keyIdHotSync || k>=keyIdFunction ? 0 : 1));
    // Strip units to window pixels: the side tab stays 1:1, the rest scales with the window width.
    auto at=[&](float x,float y,const DeviceControlView& view) {
        const DeviceControlTransform t(p,width,view);
        if(x<0)return deviceControlAt(p,width,height,int(std::floor(t.sidebarLeft+x)),height+int(std::floor(y)),view);
        const float c=std::floor(x)+0.5f,r=std::floor(y)+0.5f;
        return deviceControlAt(p,width,height,int(std::floor(t.left+c*t.scale)),height+int(std::floor(r*t.scale)),view);
    };
    for(size_t i=0;i<p.count;++i) {
        const auto& b=p.buttons[i];const auto& k=b.bounds;
        const bool keyboard=b.key>=keyIdKeyboard,tab=b.key>=keyIdJogUp && b.key<=keyIdBack;
        assert(b.section==(keyboard ? DeviceControlSection::keyboard : tab ? DeviceControlSection::sideTab : DeviceControlSection::main));
        // Inside the strip and clear of its edges; the tab's keys inside its panel, right of the screen.
        assert(tab || (k.y>=4 && k.y+k.h<=(keyboard ? p.height-8 : 48-4)));
        if(tab) {
            const auto& t=p.sideTab;
            assert(k.x>t.x && k.x+k.w<t.x+t.w && k.y>t.y && k.y+k.h<t.y+t.h && t.x+t.w==0);
            assert(t.w==38 && k.w==30 && k.x-t.x==4 && t.x+t.w-k.x-k.w==4);
        } else assert(k.x>=4 && k.x+k.w<=width-4);
        if(keyboard)assert(k.y>=48);
        const float cx=k.x+k.w*0.5f,cy=k.y+k.h*0.5f;
        const bool rocker=b.key==keyIdUp || b.key==keyIdDown;
        assert(at(cx,cy,all)==b.key);
        if(!rocker)assert(at(k.x,cy,all)==b.key && at(k.x+k.w-1,cy,all)==b.key);
        // Rounded corners miss; half a unit past each side hits nothing, but for the rocker's split.
        // The rocker's halves meet across the split; the pill check below covers them.
        if(!rocker)assert(at(k.x,k.y,all)==keyInvalid && at(k.x+k.w-1,k.y+k.h-1,all)==keyInvalid);
        assert(at(k.x-1,cy,all)==keyInvalid && at(k.x+k.w,cy,all)==keyInvalid);
        if(b.key!=keyIdDown)assert(at(cx,k.y-1,all)==keyInvalid);
        if(b.key!=keyIdUp)assert(at(cx,k.y+k.h,all)==keyInvalid);
        // Hiding a part takes its keys' hit areas with it and leaves the rest.
        assert(at(cx,cy,noKeyboard)==(keyboard ? keyInvalid : b.key));
        assert(at(cx,cy,noTab)==(tab ? keyInvalid : b.key));
        assert(at(cx,cy,neither)==(keyboard || tab ? keyInvalid : b.key));
    }
    // Keyboard rows: 27x24 keys, gaps of 4 across and 10 down, the rows below set in.
    for(size_t i=1;i<p.count;++i) {
        const auto& a=p.buttons[i-1].bounds;const auto& b=p.buttons[i].bounds;
        if(p.buttons[i-1].key>=keyIdKeyboard && p.buttons[i].key>=keyIdKeyboard && a.y==b.y) {
            assert(b.x-(a.x+a.w)==4);
            assert(at(a.x+a.w+2,a.y+a.h*0.5f,all)==keyInvalid);
        }
    }
    // Keyboard ids follow the ROM's matrix: drive 2-7, column 0-6 of its code table.
    auto button=[&](KeyId id) -> const DeviceButton& {
        for(size_t j=0;j<p.count;++j)if(p.buttons[j].key==id)return p.buttons[j];
        abort();
    };
    auto key=[&](int drive,int column) -> const SDL_Rect& {return button(tg50Key(drive,column)).bounds;};
    assert(tg50Key(2,0)==keyIdKeyboard && tg50Key(7,6)==keyIdVoice-1);
    const std::pair<std::pair<int,int>,const char*> named[]={
        {{2,1},"Q"},{{3,5},"P"},{{2,6},"H"},{{5,6},"L"},{{3,0},"Tab"},{{6,1},"N"},{{6,3},"Comma"},
        {{6,4},"Full stop"},{{6,5},"Up"},{{6,6},"Down"},{{7,1},"Space"},{{7,2},"Minus"},{{7,3},"Left"},
        {{7,4},"Right"},{{7,5},"Enter"},{{7,6},"Backspace"},
        {{2,0},"Caps lock"},{{4,0},"Shift"},{{5,0},"Ctrl"},{{6,0},"Blue shift"},{{7,0},"Red shift"}};
    for(auto [dc,label]:named)assert(std::strcmp(button(tg50Key(dc.first,dc.second)).label,label)==0);
    assert(key(2,1).x==7 && key(3,5).x+key(3,5).w==313 && key(4,1).x==23 && key(5,1).x==key(4,2).x);
    // Rows start Q, A, Tab, Ctrl and Shift; the modifier column runs Caps, Tab, Shift, Ctrl,
    // Blue, Red down drives 2-7.
    const std::pair<int,int> first[]={{2,1},{4,1},{3,0},{5,0},{4,0}};
    for(int row=1;row<5;++row) {
        const auto& r=key(first[row].first,first[row].second);
        assert(r.y-key(first[row-1].first,first[row-1].second).y==34);
        assert(at(r.x+10,r.y-5,all)==keyInvalid);
    }
    // App row: Up over Down with no gap, gaps to the slim keys either side and round keys at the ends.
    const DeviceButton* b=p.buttons;
    assert(b[0].key==keyIdHome && b[7].key==keyIdGraffiti && b[3].key==keyIdUp && b[4].key==keyIdDown);
    const auto& up=b[3].bounds;const auto& dn=b[4].bounds;
    assert(up.x==dn.x && up.y+up.h==dn.y && at(160,dn.y-1,all)==keyIdUp && at(160,dn.y,all)==keyIdDown);
    // The halves hit the drawn pill split at its middle: every pixel centre round it hits
    // exactly where the pill is, so the curved ends miss just outside the outline. Checked
    // without the side tab, where the strip is drawn 1:1.
    const SDL_Rect pill{up.x,up.y,up.w,dn.y+dn.h-up.y};
    assert(b[3].shape.w==pill.w && b[3].shape.h==pill.h && b[4].shape.y==pill.y && b[3].corner==pill.h*0.5f);
    for(int y=pill.y-3;y<pill.y+pill.h+3;++y)for(int x=pill.x-3;x<pill.x+pill.w+3;++x) {
        const float d=deviceRoundedDistance(pill,pill.h*0.5f,x+0.5f,y+0.5f);
        assert(at(x,y,noTab)==(d<=0 ? (y<dn.y ? keyIdUp : keyIdDown) : keyInvalid));
    }
    assert(at(pill.x+1,pill.y+1,noTab)==keyInvalid && at(pill.x+1,pill.y+pill.h-2,noTab)==keyInvalid);
    assert(at(pill.x+pill.w-2,pill.y+1,noTab)==keyInvalid && at(pill.x+pill.w-2,pill.y+pill.h-2,noTab)==keyInvalid);
    for(int i:{0,1,2,4,5,6}) {
        const auto& l=b[i].bounds;const auto& r=b[i==2 ? 3 : i+1].bounds;
        assert(r.x-(l.x+l.w)>=4 && at((l.x+l.w+r.x)*0.5f,30,all)==keyInvalid);
    }
    // Side tab: jog up, press, jog down, then Back, with gaps between, level with the app row.
    const auto& t=p.buttons+50;
    assert(t[0].key==keyIdJogUp && t[1].key==keyIdJogPress && t[2].key==keyIdJogDown && t[3].key==keyIdBack);
    assert(t[0].bounds.y+height>=80 && t[3].bounds.y+height<height);
    for(int i=1;i<4;++i) {
        const auto& a=t[i-1].bounds;const auto& c=t[i].bounds;
        assert(c.y>a.y+a.h && at(a.x+a.w*0.5f,a.y+a.h,all)==keyInvalid);
    }
    assert(at(-38,-180,all)==keyInvalid && at(-1,-180,all)==keyInvalid);

    // Keys past bit 31 and on the side tab go through the event handler's pointer path:
    // pressed, shown in PressedButtons, released on leaving the key, on button up and on focus loss.
    {
        Guest guest;guest.setDisplay({320,320,0,160});DisplayConfiguration display=guest.display();
        TouchInput input(&guest,p,1,display,Rotation::portrait_0);
        auto centre=[&](KeyId id) {
            for(size_t i=0;i<p.count;++i)if(p.buttons[i].key==id) {
                const auto k=deviceButtonRect(p,p.buttons[i],width,height,all);return std::pair<int,int>{k.x+k.w/2,k.y+k.h/2};
            }
            abort();
        };
        const KeyId z=tg50Key(5,1),bs=tg50Key(7,6);
        assert(z>31 && bs==keyIdVoice-1);
        auto [zx,zy]=centre(z);
        mouse(SDL_MOUSEBUTTONDOWN,zx,zy);input.HandleEvents();guest.expect({{z,true}});
        assert(input.PressedButtons()==deviceKeyBit(z) && (input.PressedButtons()>>32)!=0);
        mouse(SDL_MOUSEMOTION,zx,zy-14);input.HandleEvents();guest.expect({{z,false}});assert(!input.PressedButtons());
        mouse(SDL_MOUSEBUTTONUP,zx,zy-14);input.HandleEvents();guest.expect({});
        auto [bx,by]=centre(bs);
        mouse(SDL_MOUSEBUTTONDOWN,bx,by);input.HandleEvents();guest.expect({{bs,true}});
        assert(input.PressedButtons()==deviceKeyBit(bs));
        window(SDL_WINDOWEVENT_FOCUS_LOST);input.HandleEvents();guest.expect({{bs,false}});assert(!input.PressedButtons());
        mouse(SDL_MOUSEBUTTONUP,bx,by);input.HandleEvents();guest.expect({});
        for(KeyId id:{keyIdBack,keyIdJogPress,keyIdHome}) {
            auto [x,y]=centre(id);
            mouse(SDL_MOUSEBUTTONDOWN,x,y);input.HandleEvents();guest.expect({{id,true}});
            assert(input.PressedButtons()==deviceKeyBit(id));
            mouse(SDL_MOUSEBUTTONUP,x,y);input.HandleEvents();guest.expect({{id,false}});assert(!input.PressedButtons());
        }
        SDL_Event home{};
        home.type = SDL_KEYDOWN; home.key.keysym.sym = SDLK_F6;
        input.HandleEvent(home); guest.expect({{keyIdHome,true}});
        home.key.repeat = 1; input.HandleEvent(home); guest.expect({});
        window(SDL_WINDOWEVENT_FOCUS_LOST); input.HandleEvents();
        guest.expect({{keyIdHome,false}});
        home.type = SDL_KEYUP; home.key.repeat = 0; input.HandleEvent(home);
        guest.expect({});
        guest.noPen();
    }

    // Pressed states, and the hidden parts drawn or left out.
    for(float scale:{1.f,2.f}) {
        const int sw=int(358*scale),sh=int((320+deviceControlsHeight(p))*scale);
        auto* surface=SDL_CreateRGBSurfaceWithFormat(0,sw,sh,32,SDL_PIXELFORMAT_RGBA32);
        auto* renderer=SDL_CreateSoftwareRenderer(surface);assert(surface && renderer);
        // Strip units to surface pixels: the side tab is drawn 1:1, the rest scaled to the full width.
        const DeviceControlTransform t(p,width,all);
        auto px=[&](float x,float y) {
            return x<0 ? std::pair<int,int>{int((t.sidebarLeft+x)*scale),int((y+320)*scale)}
                       : std::pair<int,int>{int((t.left+x*t.scale)*scale),int((320+y*t.scale)*scale)};
        };
        auto sum=[&](float x,float y) {auto [sx,sy]=px(x,y);return brightness(readPixel(surface,sx,sy));};
        for(size_t i=0;i<p.count;++i) {
            const auto& k=p.buttons[i].bounds;const KeyId id=p.buttons[i].key;
            // A spot clear of legends, icons and the light line: low on most keys, beside the
            // dimple or the jog press's dot, near the ends of the rocker's halves.
            float x=k.x+k.w*0.5f,y=k.y+k.h*0.85f;
            if(id==keyIdHome || id==keyIdGraffiti)y=k.y+k.h-4.5f;
            if(id==keyIdUp || id==keyIdDown)x=k.x+10,y=k.y+k.h*0.5f;
            if(id==keyIdJogPress)x=k.x+k.w*0.5f+6,y=k.y+k.h*0.5f;
            if(id==keyIdJogUp || id==keyIdJogDown)x=k.x+4,y=k.y+k.h*0.5f;
            if(id>=keyIdKeyboard)x=k.x+5;
            drawDeviceSidebar(renderer,p,width,height,scale,0);
            drawDeviceControls(renderer,p,width,height,scale,0);
            const int idle=sum(x,y);
            drawDeviceSidebar(renderer,p,width,height,scale,deviceKeyBit(id));
            drawDeviceControls(renderer,p,width,height,scale,deviceKeyBit(id));
            const int held=sum(x,y);
            if(held>=idle-12)std::fprintf(stderr,"TG50 key %d at %g,%g scale %g: idle %d held %d\n",id,x,y,scale,idle,held);
            assert(held<idle-12);
        }
        // Holding one half of the rocker leaves the other alone.
        for(int half=0;half<2;++half) {
            const float x=up.x+10,y=half ? up.y+3 : dn.y+dn.h-3;
            drawDeviceSidebar(renderer,p,width,height,scale,0);
            drawDeviceControls(renderer,p,width,height,scale,0);
            const auto [sx,sy]=px(x,y);
            const auto idle=readPixel(surface,sx,sy);
            drawDeviceControls(renderer,p,width,height,scale,deviceKeyBit(half ? keyIdDown : keyIdUp));
            const auto held=readPixel(surface,sx,sy);
            assert(idle.r==held.r && idle.g==held.g && idle.b==held.b);
        }
        // Hiding the keyboard leaves only the app row, hiding the sidebar restores LCD x0.
        const auto mark=[&] {SDL_SetRenderDrawColor(renderer,255,0,255,255);SDL_RenderClear(renderer);};
        auto magenta=[&](int x,int y) {const auto c=readPixel(surface,int(x*scale),int(y*scale));return c.r==255 && c.g==0 && c.b==255;};
        mark();drawDeviceControls(renderer,p,width,height,scale,0,noKeyboard);
        assert(!magenta(200,340) && magenta(200,400));
        mark();drawDeviceControls(renderer,p,width,height,scale,0,neither);
        assert(magenta(337,340) && magenta(160,400) && !magenta(38,330));
        mark();drawDeviceControls(renderer,p,width,height,scale,0,noTab);
        assert(magenta(337,340) && !magenta(5,520));
        mark();drawDeviceSidebar(renderer,p,width,height,scale,0);
        drawDeviceControls(renderer,p,width,height,scale,0,all);
        assert(!magenta(353,543) && !magenta(0,0) && !magenta(0,543));
        // The two margin pixels carry the sidebar material down to the keyboard's top edge at 320,
        // which runs the full width.
        for(int y=310;y<320;++y) {
            const auto c=readPixel(surface,0,int(y*scale));
            assert(c.r>90 && c.r<220 && c.g>90 && c.g<220);
        }
        SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);
    }
}

// Devices without a keyboard or side tab draw and hit the same whatever the view, and keys
// past bit 31 leave their art alone.
static void viewsLeaveOlderDevices() {
    const DeviceControlView views[]={{false,true},{true,false},{false,false}};
    for(auto art:{DeviceArt::tx,DeviceArt::e2,DeviceArt::zire72,DeviceArt::m515,DeviceArt::iiix}) {
        const auto& p=deviceControls(art);
        const int width=320/p.unit,height=200,sh=deviceControlsHeight(p);
        assert(p.keyboardHeight==0 && p.sideTabWidth==0);
        for(const auto& view:views) {
            assert(deviceControlsHeight(p,view)==sh && deviceControlsWidth(p,width,view)==width);
            for(int y=height;y<height+sh;++y)for(int x=0;x<width;++x)
                assert(deviceControlAt(p,width,height,x,y,view)==deviceControlAt(p,width,height,x,y));
        }
        for(float scale:{1.f,2.f}) {
            const int w=int(width*scale),h=int(sh*scale);
            auto* a=SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_RGBA32);
            auto* b=SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_RGBA32);
            auto* ra=SDL_CreateSoftwareRenderer(a);auto* rb=SDL_CreateSoftwareRenderer(b);
            const DeviceKeyMask held=deviceKeyBit(keyIdHard2)|deviceKeyBit(keyIdDown);
            drawDeviceControls(ra,p,width,0,scale,held);
            drawDeviceControls(rb,p,width,0,scale,held|deviceKeyBit(KeyId(keyIdKeyboard+20))|deviceKeyBit(KeyId(keyIdVoice-1)),
                               views[2]);
            for(int y=0;y<h;++y)assert(std::memcmp(static_cast<Uint8*>(a->pixels)+y*a->pitch,
                                                   static_cast<Uint8*>(b->pixels)+y*b->pitch,size_t(w)*4)==0);
            SDL_DestroyRenderer(ra);SDL_DestroyRenderer(rb);SDL_FreeSurface(a);SDL_FreeSurface(b);
        }
    }
}

// Jog controls left of the keyboard: the LCD takes the full width, the strip narrows to fit
// beside the tab and keeps its proportions; without the keyboard the tab goes back beside the LCD.
static void tg50JogBesideKeyboard() {
    const auto& p=deviceControls(DeviceArt::tg50);
    const int width=320,height=320;
    const auto beside=deviceControlView(true,JogPlacement::keyboard),screen=deviceControlView(true,JogPlacement::screen);
    const auto hidden=deviceControlView(true,JogPlacement::hidden),fallback=deviceControlView(false,JogPlacement::keyboard);
    assert(beside.keyboard && beside.sideTab && beside.sideTabBesideKeyboard && deviceSideTabBelowScreen(p,beside));
    assert(screen.sideTab && !screen.sideTabBesideKeyboard && !hidden.sideTab && !deviceSideTabBelowScreen(p,fallback));
    JogPlacement parsed;
    assert(parseJogPlacement("hidden",parsed) && parsed==JogPlacement::hidden);
    assert(parseJogPlacement("screen",parsed) && parsed==JogPlacement::screen);
    assert(parseJogPlacement("keyboard",parsed) && parsed==JogPlacement::keyboard && !parseJogPlacement("left",parsed));
    // Older devices ignore the placement.
    assert(!deviceSideTabBelowScreen(deviceControls(DeviceArt::tx),beside) && deviceScreenLeft(deviceControls(DeviceArt::tx),beside)==0);
    assert(deviceScreenLeft(p,beside)==0 && deviceControlsWidth(p,width,beside)==320 && deviceControlsHeight(p,beside)==198);
    assert(deviceScreenLeft(p,fallback)==38 && deviceControlsWidth(p,width,fallback)==358 && deviceControlsHeight(p,fallback)==54);
    assert(deviceControlsWidth(p,width,hidden)==320 && deviceControlsHeight(p,hidden)==224);
    const DeviceControlTransform t(p,width,beside);
    assert(t.left==38 && t.sidebarLeft==38 && std::fabs(t.scale-282.f/320)<1e-6f && t.sidebarTop==244);
    const DeviceControlTransform ts(p,width,fallback);
    assert(ts.sidebarTop==0 && ts.sidebarLeft==38);
    const int strip=deviceControlsHeight(p,beside);
    for(size_t i=0;i<p.count;++i) {
        const auto& b=p.buttons[i];
        const auto r=deviceButtonRect(p,b,width,height,beside);
        if(b.section==DeviceControlSection::sideTab) {
            // The tab keys stay 30 pixels wide with 4 pixel margins, inside the strip's height.
            assert(r.x==4 && r.w==30 && r.h==b.bounds.h);
            assert(r.y>height && r.y+r.h<height+strip);
        } else {
            // Everything else right of the tab, scaled the same both ways.
            assert(r.x>=38 && r.x+r.w<=width && r.y>=height && r.y+r.h<=height+strip);
            assert(std::abs(r.w-b.bounds.w*t.scale)<=1 && std::abs(r.h-b.bounds.h*t.scale)<=1);
        }
        // Hit areas line up with the drawn rectangles in every placement.
        for(const auto& view : {beside,screen,fallback}) {
            const auto k=deviceButtonRect(p,b,width,height,view);
            if(!deviceControlShown(b,view))continue;
            assert(deviceControlAt(p,width,height,k.x+k.w/2,k.y+k.h/2,view)==b.key);
        }
    }
    // Every pixel that hits a key is inside that key's drawn rectangle.
    for(const auto& view : {beside,screen,fallback,hidden}) {
        const int w=deviceControlsWidth(p,width,view),top=deviceScreenLeft(p,view) ? 0 : height;
        for(int y=top;y<height+deviceControlsHeight(p,view);++y)for(int x=0;x<w;++x) {
            const KeyId key=deviceControlAt(p,width,height,x,y,view);
            if(key==keyInvalid)continue;
            bool inside=false;
            for(size_t i=0;i<p.count;++i) {
                if(p.buttons[i].key!=key)continue;
                const auto k=deviceButtonRect(p,p.buttons[i],width,height,view);
                inside=deviceControlShown(p.buttons[i],view) && x>=k.x-1 && x<=k.x+k.w && y>=k.y-1 && y<=k.y+k.h;
            }
            assert(inside);
        }
    }
    // Drawn: no tab beside the LCD, held keys darken where they hit, the body runs the full width.
    for(float scale:{1.f,1.25f,2.f}) {
        const int sw=int(width*scale),sh=int((height+strip)*scale);
        auto* surface=SDL_CreateRGBSurfaceWithFormat(0,sw,sh,32,SDL_PIXELFORMAT_RGBA32);
        auto* renderer=SDL_CreateSoftwareRenderer(surface);assert(surface && renderer);
        SDL_SetRenderDrawColor(renderer,255,0,255,255);SDL_RenderClear(renderer);
        drawDeviceSidebar(renderer,p,width,height,scale,0,beside);
        auto c=readPixel(surface,int(17*scale),int(154*scale));assert(c.r==255 && c.g==0 && c.b==255);
        for(size_t i=0;i<p.count;++i) {
            const auto& b=p.buttons[i];
            if(b.section==DeviceControlSection::keyboard && b.key%5)continue;
            const auto r=deviceButtonRect(p,b,width,height,beside);
            // A spot clear of legends and icons, as in tg50Art.
            float x=r.x+r.w*0.5f,y=r.y+r.h*0.85f;
            if(b.key==keyIdHome || b.key==keyIdGraffiti)y=r.y+r.h-4.5f*t.scale;
            if(b.key==keyIdUp || b.key==keyIdDown)x=r.x+10*t.scale,y=r.y+r.h*0.5f;
            if(b.key==keyIdJogPress)x=r.x+r.w*0.5f+6,y=r.y+r.h*0.5f;
            if(b.key==keyIdJogUp || b.key==keyIdJogDown)x=r.x+4,y=r.y+r.h*0.5f;
            if(b.key>=keyIdKeyboard)x=r.x+5*t.scale;
            const int px=int(x*scale),py=int(y*scale);
            drawDeviceControls(renderer,p,width,height,scale,0,beside);
            const int idle=brightness(readPixel(surface,px,py));
            drawDeviceControls(renderer,p,width,height,scale,deviceKeyBit(b.key),beside);
            const int held=brightness(readPixel(surface,px,py));
            if(held>=idle-12)std::fprintf(stderr,"TG50 beside keyboard key %d at %d,%d scale %g: idle %d held %d\n",b.key,px,py,scale,idle,held);
            assert(held<idle-12);
        }
        for(int x : {0,37,38,319}) {
            const auto d=readPixel(surface,int(x*scale),int((height+strip-2)*scale));
            assert(!(d.r==255 && d.g==0 && d.b==255));
        }
        // The bezel's dark lower edge runs across the tab too.
        assert(brightness(readPixel(surface,int(10*scale),int((height+0.5f)*scale)))<
               brightness(readPixel(surface,int(10*scale),int((height+6)*scale))));
        SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);
    }
}

static void tg50HostKeyboard();
static void zireAndM515() {
    // Art comes from the device table, by internal type for built devices and by name for all.
    assert(deviceArt(deviceTypeTX)==DeviceArt::tx && deviceArt(deviceTypeE2)==DeviceArt::e2);
    assert(deviceArt(deviceTypePV)==DeviceArt::none && deviceArt(deviceTypeFrankenE2)==DeviceArt::e2);
    assert(deviceArt("palm-tx")==DeviceArt::tx && deviceArt("palm-e2")==DeviceArt::e2);
    assert(deviceArt("palm-zire-72")==DeviceArt::zire72 && deviceArt("palm-m515")==DeviceArt::m515);
    assert(deviceArt("palm-iiix")==DeviceArt::iiix && deviceArt("sony-clie-tg50")==DeviceArt::tg50);
    assert(deviceArt("tungsten-e2")==DeviceArt::none && deviceArt("")==DeviceArt::none && deviceArt(nullptr)==DeviceArt::none);
    assert(&deviceControls(deviceTypeTX)==&deviceControls(DeviceArt::tx) && &deviceSilkscreen(deviceTypeE2)==&deviceSilkscreen(DeviceArt::e2));
    assert(deviceControls(deviceTypeTX).unit==1 && deviceControls(deviceTypeE2).unit==1 && deviceSilkscreen(deviceTypeE2).unit==1);
    for(auto art:{DeviceArt::zire72,DeviceArt::m515}) {
        artDevice(art);
        artSilkscreen(art);
    }
    iiixArt();
    tg50Art();
    tg50JogBesideKeyboard();
    tg50HostKeyboard();
    viewsLeaveOlderDevices();
}

static void tg50ScreenBounds() {
    for(float scale : {1.f,1.25f,1.75f,2.f}) {
        for(auto rotation : {Rotation::portrait_0,Rotation::landscape_90,Rotation::portrait_180,Rotation::landscape_270}) {
            Guest guest(deviceTypeTG50);guest.setDisplay({320,320,0,224});auto display=guest.display();
            TouchInput input(&guest,scale,display,rotation);
            const int left=int(38*scale),extent=int(320*scale);
            auto pointer=[&](Uint32 type,int x,int y) {
                SDL_Event e{};e.type=type;e.button.button=SDL_BUTTON_LEFT;e.button.x=x;e.button.y=y;
                input.HandleEvent(e);
            };
            for(int corner : {0,1}) {
                const int p=corner ? extent-1 : 0,g=corner ? 319 : 0,opposite=319-g;
                int gx=g,gy=g;
                if(rotation==Rotation::landscape_90)gx=opposite;
                if(rotation==Rotation::portrait_180)gx=gy=opposite;
                if(rotation==Rotation::landscape_270)gy=opposite;
                pointer(SDL_MOUSEBUTTONDOWN,left+p,p);guest.pen(true,gx,gy);
                pointer(SDL_MOUSEBUTTONUP,left+p,p);guest.pen(false);
            }
            for(auto [x,y] : {std::pair<int,int>{left-1,0},{left+extent,0},{left,-1},{left,extent}}) {
                pointer(SDL_MOUSEBUTTONDOWN,x,y);guest.noPen();guest.expect({});
                pointer(SDL_MOUSEBUTTONUP,x,y);guest.noPen();guest.expect({});
            }
        }
    }
}

static void tg50Visibility() {
    const auto& controls=deviceControls(deviceTypeTG50);
    for(bool keyboard : {false,true}) for(auto jog : {JogPlacement::hidden,JogPlacement::screen,JogPlacement::keyboard})
    for(auto rotation : {Rotation::portrait_0,Rotation::landscape_90,Rotation::portrait_180,Rotation::landscape_270})
    for(auto fit : {WindowFit::none,WindowFit::fit,WindowFit::whole,WindowFit::fill})
    for(float scale : {1.f,1.25f,1.75f,2.f}) {
        if(fit!=WindowFit::none && scale!=1)continue;
        const DeviceControlView view=deviceControlView(keyboard,jog);
        const bool beside=keyboard && jog==JogPlacement::keyboard;
        const int deviceWidth=jog==JogPlacement::hidden || beside ? 320 : 358;
        const int deviceHeight=keyboard ? (beside ? 518 : jog==JogPlacement::hidden ? 544 : 571)
                                        : (jog==JogPlacement::hidden ? 368 : 374);
        assert(deviceWidth==deviceControlsWidth(controls,320,view) && deviceHeight==320+deviceControlsHeight(controls,view));
        const int ww=fit==WindowFit::none ? int(deviceWidth*scale) : 521;
        const int wh=fit==WindowFit::none ? int(deviceHeight*scale) : 761;
        Guest guest(deviceTypeTG50);guest.setDisplay({320,320,0,224});auto display=guest.display();
        guest.frame.assign(320*320,0xffffffff);
        auto* win=SDL_CreateWindow("TG50 visibility",0,0,ww,wh,0);assert(win);
        {
            SdlRenderer renderer(win,&guest,scale,display,rotation,false,fit,0x1a1a1c,view);
            assert(renderer.Valid());renderer.Draw(true);
            const auto layout=renderer.Layout();
            if(fit!=WindowFit::none) {
                const auto expected=fitWindowLayout(fit,deviceWidth,deviceHeight,ww,wh);
                assert(layout.width==expected.width && layout.height==expected.height);
                assert(layout.x==expected.x && layout.y==expected.y);
                assert(layout.scaleX==expected.scaleX && layout.scaleY==expected.scaleY);
            }
            TouchInput input(&guest,scale,display,rotation,view);
            input.SetDirectCapture(false);input.SetLayout(layout,fit!=WindowFit::none);
            auto pixel=[&](float x,float y) {
                return std::pair<int,int>{int(std::floor(layout.x+(x+0.5f)*layout.scaleX)),int(std::floor(layout.y+(y+0.5f)*layout.scaleY))};
            };
            auto pointer=[&](Uint32 type,int x,int y) {
                SDL_Event e{};e.type=type;e.button.button=SDL_BUTTON_LEFT;e.button.x=x;e.button.y=y;
                input.HandleEvent(e);
            };
            // All shown controls, including the app row, keep press/release and layout-release behaviour.
            for(size_t i=0;i<controls.count;++i) {
                const auto& b=controls.buttons[i];const auto r=deviceButtonRect(controls,b,320,320,view);
                const auto [x,y]=pixel(r.x+r.w/2,r.y+r.h/2);
                pointer(SDL_MOUSEBUTTONDOWN,x,y);
                const bool shown=deviceControlShown(b,view);
                if(shown) {
                    guest.expect({{b.key,true}});assert(input.PressedButtons()==deviceKeyBit(b.key));
                    input.SetLayout(layout,fit!=WindowFit::none);guest.expect({{b.key,false}});
                } else guest.expect({});
                pointer(SDL_MOUSEBUTTONUP,x,y);guest.expect({});guest.noPen();
                assert(!input.PressedButtons() && !input.WantsCapture());
            }
            // The first and last LCD columns stay pen input after moving the sidebar out.
            for(int gx : {0,319}) {
                auto [x,y]=windowPoint(rotation,1,320,320,gx,160);
                auto at=pixel(x+deviceScreenLeft(controls,view),y);
                if(fit==WindowFit::none) {
                    at=windowPoint(rotation,scale,320,320,gx,160);
                    at.first+=int(deviceScreenLeft(controls,view)*scale);
                }
                pointer(SDL_MOUSEBUTTONDOWN,at.first,at.second);guest.pen(true,gx,160);
                input.SetRotation(rotation);guest.pen(false);
            }
            for(auto [x,y] : {std::pair<int,int>{deviceScreenLeft(controls,view)-1,0},
                             {deviceScreenLeft(controls,view)+320,0},
                             {deviceScreenLeft(controls,view),-1},{deviceScreenLeft(controls,view),320}}) {
                const auto at=pixel(x,y);
                pointer(SDL_MOUSEBUTTONDOWN,at.first,at.second);guest.noPen();guest.expect({});
                pointer(SDL_MOUSEBUTTONUP,at.first,at.second);guest.noPen();guest.expect({});
            }
            // Touches use the selected device extent and release on focus loss.
            for(size_t i=0;i<controls.count;++i) {
                const auto& b=controls.buttons[i];if(!deviceControlShown(b,view))continue;
                const auto r=deviceButtonRect(controls,b,320,320,view);
                const auto [x,y]=pixel(r.x+r.w/2,r.y+r.h/2);
                SDL_Event e{};e.type=SDL_FINGERDOWN;e.tfinger.type=e.type;e.tfinger.windowID=SDL_GetWindowID(win);
                e.tfinger.touchId=1;e.tfinger.fingerId=1;e.tfinger.x=(x+0.5f)/ww;e.tfinger.y=(y+0.5f)/wh;
                input.HandleEvent(e);guest.expect({{b.key,true}});
                SDL_Event focus{};focus.type=SDL_WINDOWEVENT;focus.window.event=SDL_WINDOWEVENT_FOCUS_LOST;
                input.HandleEvent(focus);guest.expect({{b.key,false}});assert(!input.PressedButtons());
            }
            auto* shot=SDL_CreateRGBSurfaceWithFormat(0,ww,wh,32,SDL_PIXELFORMAT_RGBA32);assert(shot);
            assert(SDL_RenderReadPixels(SDL_GetRenderer(win),nullptr,shot->format->format,shot->pixels,shot->pitch)==0);
            const auto [lx,ly]=pixel(deviceScreenLeft(controls,view)+4,40);
            const auto white=readPixel(shot,lx,ly);assert(white.r==255 && white.g==255 && white.b==255);
            if(scale==1 && fit==WindowFit::none && rotation==Rotation::portrait_0) {
                if(const char* directory=std::getenv("FROND_TG50_SCREENSHOT_DIR")) {
                    const char* placement=jog==JogPlacement::hidden ? "hidden" : jog==JogPlacement::screen ? "screen" : "keyboard";
                    const std::string path=std::string(directory)+"/tg50-visibility-keyboard"+(keyboard ? "1" : "0")+"-jog-"+placement+".png";
                    assert(IMG_SavePNG(shot,path.c_str())==0);
                }
            }
            SDL_FreeSurface(shot);
        }
        SDL_DestroyWindow(win);
    }
}

// The display command's argument, and a live renderer and input switching view and fit
// the way the SDL and guest threads do: held controls release and hit areas follow.
static void tg50LiveDisplay() {
    DisplayChange change;std::string error;
    assert(parseDisplayChange("fit=fill smooth=on keyboard=off jog=keyboard",true,change,error));
    assert(change.fit==WindowFit::fill && change.smooth==true && change.keyboard==false && change.jog==JogPlacement::keyboard);
    assert(parseDisplayChange("smooth=off",false,change,error) && !change.fit && change.smooth==false && !change.keyboard && !change.jog);
    assert(parseDisplayChange("fit=none",false,change,error) && change.fit==WindowFit::none);
    for(const char* bad : {"","fit=big","smooth=yes","keyboard","jog=left","size=fit","fit=fit extra"})
        assert(!parseDisplayChange(bad,true,change,error) && !error.empty());
    assert(!parseDisplayChange("keyboard=on",false,change,error) && !parseDisplayChange("jog=hidden",false,change,error));
    DisplaySettings settings{WindowFit::fit,false,true,JogPlacement::screen};
    DisplayChange first,second;
    assert(parseDisplayChange("fit=whole keyboard=off",true,first,error) && parseDisplayChange("fit=fill jog=hidden",true,second,error));
    first.Merge(second);settings.Apply(first);
    assert(settings.fit==WindowFit::fill && !settings.smooth && !settings.keyboard && settings.jog==JogPlacement::hidden);

    const auto& controls=deviceControls(deviceTypeTG50);
    Guest guest(deviceTypeTG50);guest.setDisplay({320,320,0,224});auto display=guest.display();
    guest.frame.assign(320*320,0xffffffff);
    const float scale=1.25f;
    struct Step {WindowFit fit;bool keyboard;JogPlacement jog;};
    const Step steps[]={{WindowFit::fit,true,JogPlacement::screen},{WindowFit::fill,false,JogPlacement::hidden},
                        {WindowFit::none,true,JogPlacement::keyboard},{WindowFit::whole,false,JogPlacement::screen},
                        {WindowFit::none,false,JogPlacement::hidden}};
    auto* win=SDL_CreateWindow("TG50 live display",0,0,521,761,SDL_WINDOW_RESIZABLE);assert(win);
    TouchInput input(&guest,scale,display,Rotation::portrait_0,deviceControlView(true,JogPlacement::screen));
    input.SetDirectCapture(false);
    KeyId held=keyInvalid;
    for(const auto& step : steps) {
        const DeviceControlView view=deviceControlView(step.keyboard,step.jog);
        const bool fitted=step.fit!=WindowFit::none;
        if(!fitted) SDL_SetWindowSize(win,int(scale*deviceControlsWidth(controls,320,view)),
                                      int(scale*(320+deviceControlsHeight(controls,320,view))));
        else SDL_SetWindowSize(win,521,761);
        SdlRenderer renderer(win,&guest,scale,display,Rotation::portrait_0,true,step.fit,0,view);
        assert(renderer.Valid());renderer.Draw(true);
        const auto layout=renderer.Layout();
        // The new view releases what the old one held.
        input.SetView(view,scale);
        if(held) guest.expect({{held,false}});
        guest.expect({});assert(!input.PressedButtons());
        input.SetLayout(layout,fitted);
        auto pixel=[&](float x,float y) {
            return std::pair<int,int>{int(std::floor(layout.x+(x+0.5f)*layout.scaleX)),int(std::floor(layout.y+(y+0.5f)*layout.scaleY))};
        };
        auto pointer=[&](Uint32 type,std::pair<int,int> at) {
            SDL_Event e{};e.type=type;e.button.button=SDL_BUTTON_LEFT;e.button.x=at.first;e.button.y=at.second;
            input.HandleEvent(e);
        };
        held=keyInvalid;
        for(size_t i=0;i<controls.count;++i) {
            const auto& b=controls.buttons[i];const auto r=deviceButtonRect(controls,b,320,320,view);
            const auto at=pixel(r.x+r.w/2,r.y+r.h/2);
            pointer(SDL_MOUSEBUTTONDOWN,at);
            if(deviceControlShown(b,view)) {
                guest.expect({{b.key,true}});
                if(!held) {held=b.key;break;}
                pointer(SDL_MOUSEBUTTONUP,at);guest.expect({{b.key,false}});
            } else {guest.expect({});pointer(SDL_MOUSEBUTTONUP,at);guest.expect({});}
            guest.noPen();
        }
        assert(held && input.PressedButtons()==deviceKeyBit(held));
        // Hidden controls take no presses, and the LCD edges stay pen input.
        for(size_t i=0;i<controls.count;++i) {
            const auto& b=controls.buttons[i];if(deviceControlShown(b,view))continue;
            const auto r=deviceButtonRect(controls,b,320,320,view);const auto at=pixel(r.x+r.w/2,r.y+r.h/2);
            SDL_Event e{};e.type=SDL_FINGERDOWN;e.tfinger.type=e.type;e.tfinger.windowID=SDL_GetWindowID(win);
            int ww=0,wh=0;SDL_GetWindowSize(win,&ww,&wh);
            e.tfinger.touchId=2;e.tfinger.fingerId=i;e.tfinger.x=(at.first+0.5f)/ww;e.tfinger.y=(at.second+0.5f)/wh;
            input.HandleEvent(e);guest.expect({});guest.noPen();
        }
        for(int gx : {0,319}) {
            auto at=pixel(gx+deviceScreenLeft(controls,view),160);
            if(!fitted) at={int(gx*scale+scale/2)+int(deviceScreenLeft(controls,view)*scale),int(160*scale+scale/2)};
            SDL_Event e{};e.type=SDL_FINGERDOWN;e.tfinger.type=e.type;e.tfinger.windowID=SDL_GetWindowID(win);
            int ww=0,wh=0;SDL_GetWindowSize(win,&ww,&wh);
            e.tfinger.touchId=3;e.tfinger.fingerId=gx;e.tfinger.x=(at.first+0.5f)/ww;e.tfinger.y=(at.second+0.5f)/wh;
            input.HandleEvent(e);guest.pen(true,gx,160);
            e.type=e.tfinger.type=SDL_FINGERUP;input.HandleEvent(e);guest.pen(false);
        }
    }
    input.SetView(deviceControlView(true,JogPlacement::screen),scale);guest.expect({{held,false}});
    SDL_DestroyWindow(win);
}

static void tg50RuntimeTab() {
    for (auto fit : {WindowFit::none, WindowFit::fit})
    for(float scale : {1.f,1.25f,1.75f,2.f}) {
        if(fit!=WindowFit::none && scale!=1)continue;
        Guest guest(deviceTypeTG50); guest.setDisplay({320,320,0,224});
        auto display = guest.display(); guest.frame.assign(320*320, 0xffffffff);
        const int tall=320+deviceControlsHeight(deviceControls(DeviceArt::tg50));
        auto* window = SDL_CreateWindow("TG50 tab",0,0,int(358*scale),int(tall*scale),0); assert(window);
        {
            SdlRenderer renderer(window,&guest,scale,display,Rotation::portrait_0,false,fit);
            assert(renderer.Valid()); renderer.Draw(true);
            auto* shot = SDL_CreateRGBSurfaceWithFormat(0,int(358*scale),int(tall*scale),32,SDL_PIXELFORMAT_RGBA32); assert(shot);
            assert(SDL_RenderReadPixels(SDL_GetRenderer(window),nullptr,shot->format->format,shot->pixels,shot->pitch)==0);
            const auto jog = readPixel(shot,int(17*scale),int(154*scale));
            assert(std::max({jog.r,jog.g,jog.b}) > 20 && std::max({jog.r,jog.g,jog.b}) < 250);
            if (fit != WindowFit::none) assert(renderer.Layout().width == 358);
            // The sidebar must stop before the first LCD column, including fractional scales.
            const int left=int(38*scale),row=int(50*scale);
            for(int x : {left,left+int(320*scale)-1}) {
                const auto c=readPixel(shot,x,row);assert(c.r==255 && c.g==255 && c.b==255);
            }
            assert(readPixel(shot,left-1,row).r<240);
            if(const char* directory=std::getenv("FROND_TG50_SCREENSHOT_DIR")) {
                const std::string suffix=scale==1 ? "" : "-"+std::to_string(int(scale*100));
                const std::string path=std::string(directory)+"/tg50-native-renderer-"+(fit==WindowFit::none ? "fixed" : "fit")+suffix+".png";
                assert(IMG_SavePNG(shot,path.c_str())==0);
            }
            SDL_FreeSurface(shot);
        }
        SDL_DestroyWindow(window);
    }
    tg50ScreenBounds();
}

static void lcdScaling() {
    // A high-frequency guest frame distinguishes nearest from linear filtering.
    for(auto device:{deviceTypeTX,deviceTypeE2})
    for(float scale:{1.f,1.25f,1.5f,1.75f,2.f})
    for(auto rotation:{Rotation::portrait_0,Rotation::landscape_90,Rotation::portrait_180,Rotation::landscape_270}) {
        Guest guest(device);auto display=guest.display();
        guest.frame.resize(display.width*display.height);
        for(int y=0;y<display.height;++y)for(int x=0;x<display.width;++x)
            guest.frame[y*display.width+x]=(x%2) ? 0xffffffff : 0xff000000;
        const bool landscape=rotation==Rotation::landscape_90 || rotation==Rotation::landscape_270;
        const int width=landscape?display.height+display.graffitiHeight:display.width;
        const int height=landscape?display.width:display.height+display.graffitiHeight;
        auto* win=SDL_CreateWindow("LCD scale",0,0,width*scale,(height+deviceControls(device).height)*scale,0);assert(win);
        int blended[2]{};
        for(int smooth=0;smooth<2;++smooth) {
            SdlRenderer renderer(win,&guest,scale,display,rotation,smooth);
            renderer.Draw(true);
            auto* shot=SDL_CreateRGBSurfaceWithFormat(0,width*scale,(height+deviceControls(device).height)*scale,32,SDL_PIXELFORMAT_RGBA32);assert(shot);
            assert(SDL_RenderReadPixels(SDL_GetRenderer(win),nullptr,shot->format->format,shot->pixels,shot->pitch)==0);
            // Inspect the interior LCD, away from borders and the printed panel.
            for(int y=5;y<display.height-5;++y)for(int x=5;x<display.width-5;++x) {
                auto [wx,wy]=windowPoint(rotation,scale,display.width,display.height+display.graffitiHeight,x,y);
                const auto c=readPixel(shot,wx,wy);
                blended[smooth]+=c.r>0 && c.r<255;
            }
            SDL_FreeSurface(shot);
            bool blank=true;
            const char* path="build/step7-scale-frame.png";
            assert(renderer.SaveFramebuffer(path,blank) && !blank);
            auto* frame=IMG_Load(path);assert(frame);
            assert(frame->w==display.width && frame->h==display.height);
            for(int x=0;x<display.width;++x)assert(readPixel(frame,x,4).r==(x%2?255:0));
            SDL_FreeSurface(frame);std::remove(path);
        }
        assert(blended[0]==0);
        if(scale>1)assert(blended[1]>0);
        SDL_DestroyWindow(win);
    }
    assert(fittingWindowScale(320,564,1920,1080)==1.75f);
    assert(fittingWindowScale(320,520,1920,1080)==1.75f);
    assert(fittingWindowScale(320,564,1920,1200)==2.f);
    assert(fittingWindowScale(320,564,1280,800)==1.25f);
    assert(fittingWindowScale(320,564,500,2000)==1.5f);
    assert(fittingWindowScale(320,564,300,500)==1.f);
}

// Add beside tg50Art() in tests/device-controls.cpp, call from main.
static void tg50HostKeyboard() {
    Guest guest(deviceTypeTG50); guest.setDisplay({320,320,0,224});
    auto display = guest.display();
    TouchInput input(&guest,1,display,Rotation::portrait_0);
    auto physical = [&](SDL_Scancode scan, bool down, bool repeat=false) {
        SDL_Event e{}; e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        e.key.keysym.scancode = scan; e.key.keysym.sym = SDL_GetKeyFromScancode(scan);
        e.key.repeat = repeat; input.HandleEvent(e);
    };
    physical(SDL_SCANCODE_LSHIFT,true); guest.expect({{tg50Key(4,0),true}});
    physical(SDL_SCANCODE_RSHIFT,true); guest.expect({});
    physical(SDL_SCANCODE_Q,true); guest.expect({{tg50Key(2,1),true}});
    physical(SDL_SCANCODE_Q,true,true); guest.expect({});
    physical(SDL_SCANCODE_LSHIFT,false); guest.expect({});
    physical(SDL_SCANCODE_RSHIFT,false); guest.expect({{tg50Key(4,0),false}});
    physical(SDL_SCANCODE_Q,false); guest.expect({{tg50Key(2,1),false}});
    physical(SDL_SCANCODE_LALT,true); physical(SDL_SCANCODE_RALT,true);
    guest.expect({{tg50Key(6,0),true},{tg50Key(7,0),true}});
    physical(SDL_SCANCODE_LCTRL,true); physical(SDL_SCANCODE_RCTRL,true);
    guest.expect({{tg50Key(5,0),true}});
    physical(SDL_SCANCODE_LCTRL,false); guest.expect({});
    SDL_Event lost{}; lost.type=SDL_WINDOWEVENT; lost.window.event=SDL_WINDOWEVENT_FOCUS_LOST;
    input.HandleEvent(lost);
    guest.expect({{tg50Key(5,0),false},{tg50Key(6,0),false},{tg50Key(7,0),false}});
    physical(SDL_SCANCODE_RCTRL,false); physical(SDL_SCANCODE_LALT,false); physical(SDL_SCANCODE_RALT,false);
    guest.expect({}); assert(!input.PressedButtons());
    // Physical arrows move the keyboard cursor, controller scancode0 keeps the rocker.
    physical(SDL_SCANCODE_DOWN,true); physical(SDL_SCANCODE_DOWN,false);
    guest.expect({{tg50Key(6,6),true},{tg50Key(6,6),false}});
    SDL_Event controller{}; controller.type=SDL_KEYDOWN; controller.key.keysym.sym=SDLK_DOWN;
    input.HandleEvent(controller); controller.type=SDL_KEYUP; input.HandleEvent(controller);
    guest.expect({{keyIdDown,true},{keyIdDown,false}});
    // A pointer and physical keyboard can own the same switch independently.
    // Q's centre in window pixels.
    const auto& art=deviceControls(DeviceArt::tg50);
    SDL_Rect q{};
    for(size_t i=0;i<art.count;++i)if(art.buttons[i].key==tg50Key(2,1))q=deviceButtonRect(art,art.buttons[i],320,320);
    auto pointer = [&](Uint32 type) {
        SDL_Event e{}; e.type=type; e.button.button=SDL_BUTTON_LEFT;
        e.button.x=q.x+q.w/2; e.button.y=q.y+q.h/2; input.HandleEvent(e);
    };
    physical(SDL_SCANCODE_Q,true); guest.expect({{tg50Key(2,1),true}});
    pointer(SDL_MOUSEBUTTONDOWN); guest.expect({});
    physical(SDL_SCANCODE_Q,false); guest.expect({});
    pointer(SDL_MOUSEBUTTONUP); guest.expect({{tg50Key(2,1),false}});
    pointer(SDL_MOUSEBUTTONDOWN); guest.expect({{tg50Key(2,1),true}});
    physical(SDL_SCANCODE_Q,true); guest.expect({});
    pointer(SDL_MOUSEBUTTONUP); guest.expect({});
    physical(SDL_SCANCODE_Q,false); guest.expect({{tg50Key(2,1),false}});
    auto finger = [&](Uint32 type, SDL_FingerID id, int x, int y) {
        SDL_Event e{}; e.type=type; e.tfinger.type=type; e.tfinger.touchId=7;
        // Below the LCD, x and y-320 are strip units, scaled to the window like the drawing.
        const DeviceControlTransform t(art,320,{});
        const float wx=y<320 ? x+38 : t.left+x*t.scale,wy=y<320 ? y : 320+(y-320)*t.scale;
        e.tfinger.fingerId=id; e.tfinger.x=(wx+0.5f)/358; e.tfinger.y=(wy+0.5f)/(320+deviceControlsHeight(art));
        input.HandleEvent(e);
    };
    finger(SDL_FINGERDOWN,1,85,485); guest.expect({{tg50Key(6,0),true}});
    finger(SDL_FINGERDOWN,2,20,384); guest.expect({{tg50Key(2,1),true}});
    // SDL's duplicate mouse copy must not replace the first finger's modifier.
    SDL_Event duplicate{}; duplicate.type=SDL_MOUSEBUTTONDOWN;
    duplicate.button.which=SDL_TOUCH_MOUSEID; duplicate.button.button=SDL_BUTTON_LEFT;
    duplicate.button.x=q.x+q.w/2; duplicate.button.y=q.y+q.h/2; input.HandleEvent(duplicate);
    guest.expect({});
    finger(SDL_FINGERUP,2,20,384); guest.expect({{tg50Key(2,1),false}});
    finger(SDL_FINGERUP,1,85,485); guest.expect({{tg50Key(6,0),false}});
    finger(SDL_FINGERDOWN,3,40,520); guest.expect({{tg50Key(4,0),true}});
    physical(SDL_SCANCODE_LSHIFT,true); guest.expect({});
    finger(SDL_FINGERUP,3,40,520); guest.expect({});
    physical(SDL_SCANCODE_LSHIFT,false); guest.expect({{tg50Key(4,0),false}});
    finger(SDL_FINGERDOWN,4,40,520); finger(SDL_FINGERDOWN,5,20,384);
    guest.expect({{tg50Key(4,0),true},{tg50Key(2,1),true}});
    input.HandleEvent(lost);
    guest.expect({{tg50Key(4,0),false},{tg50Key(2,1),false}});
    finger(SDL_FINGERUP,4,40,520); finger(SDL_FINGERUP,5,20,384); guest.expect({});
    finger(SDL_FINGERDOWN,6,20,384); guest.expect({{tg50Key(2,1),true}});
    SDL_Event resize{}; resize.type=SDL_WINDOWEVENT; resize.window.event=SDL_WINDOWEVENT_SIZE_CHANGED;
    input.HandleEvent(resize); guest.expect({{tg50Key(2,1),false}});
    finger(SDL_FINGERDOWN,7,20,384); guest.expect({{tg50Key(2,1),true}});
    input.SetRotation(Rotation::portrait_180); guest.expect({{tg50Key(2,1),false}});
    input.SetRotation(Rotation::portrait_0);
    finger(SDL_FINGERDOWN,8,100,100); guest.pen(true,100,100);
    finger(SDL_FINGERDOWN,9,120,120); guest.noPen();
    finger(SDL_FINGERUP,9,120,120); guest.noPen();
    finger(SDL_FINGERUP,8,100,100); guest.pen(false);
    // An attached infrared keyboard owns host typing; screen keys keep their matrix route.
    guest.attached=true; guest.events.clear();
    physical(SDL_SCANCODE_A,true); physical(SDL_SCANCODE_A,false);
    guest.expect({});
    assert((guest.events==std::vector<std::pair<uint8_t,bool>>{{0x1c,true},{0x1c,false}}));
}

static void firmwarePowerInput() {
    Guest guest(deviceTypeE2);
    guest.powerSupported=false;
    auto display=guest.display();
    TouchInput input(&guest,1,display,Rotation::portrait_0);
    SDL_Event event{};
    event.type=SDL_KEYDOWN; event.key.keysym.sym=SDLK_ESCAPE;
    input.HandleEvent(event); guest.expect({});
    event.type=SDL_KEYUP; input.HandleEvent(event); guest.expect({});
    guest.powerSupported=true;
    event.type=SDL_KEYDOWN; input.HandleEvent(event); guest.expect({{keyIdPower,true}});
    event.type=SDL_KEYUP; input.HandleEvent(event); guest.expect({{keyIdPower,false}});
    const char* path="build/firmware-power.replay";
    { std::ofstream replay(path); replay << "0 key-down power\n1 key-up power\n2 key-down hard1\n3 key-up hard1\n"; }
    InputReplay replay;
    std::string error;
    assert(replay.Load(path,320,440,error));
    guest.powerSupported=false;
    replay.Dispatch(10,guest,[](const char*,bool&) {return false;});
    guest.expect({{keyIdHard1,true},{keyIdHard1,false}});
    std::remove(path);
}


// Zodiac art: no board yet, so its two arrangements, hit areas, stick, held and released input and
// pressed drawing are checked on a stand-in guest.
struct ZodiacGuest : Guest {
    ZodiacGuest() : Guest(deviceTypeZodiac2) {setDisplay({480,320,0,160});}
    std::vector<DeviceStick> sticks;
    void AnalogStick(float x,float y) override {sticks.push_back({x,y,x!=0 || y!=0});}
    DeviceStick lastStick() {assert(!sticks.empty());auto s=sticks.back();sticks.clear();return s;}
    void noStick() {assert(sticks.empty());}
};
struct ZodiacInput : SdlEventHandler {
    using SdlEventHandler::SdlEventHandler;
    bool DirectTouch(SDL_TouchID) const override {return true;}
};
static void zodiacControls() {
    const auto* profile=deviceProfile5(deviceTypeZodiac2);
    assert(profile && profile->frontend.available && profile->board==DeviceBoard5::imx1Zodiac);
    assert(deviceArt("tapwave-zodiac-2")==DeviceArt::zodiac);
    const auto& p=deviceControls(deviceTypeZodiac2);
    assert(p.style==DeviceControlStyle::zodiac && p.panelWidth==150 && p.width==480);
    DeviceControlView below,around;around.arrangement=DeviceControlArrangement::around;
    assert(deviceControlsWidth(p,480,below)==480 && deviceControlsHeight(p,480,below)==200 && !deviceScreenLeft(p,below));
    assert(deviceControlsWidth(p,480,around)==780 && deviceControlsHeight(p,480,around)==0 && deviceScreenLeft(p,around)==150);
    // Older devices keep their strip whatever the arrangement says.
    for(auto art:{DeviceArt::tx,DeviceArt::e2,DeviceArt::tg50}) {
        const auto& o=deviceControls(art);
        assert(deviceControlsWidth(o,320,around)==deviceControlsWidth(o,320,below));
        assert(deviceControlsHeight(o,320,around)==deviceControlsHeight(o,320,below));
        assert(deviceControlViewForWindow(o,below,2400,1080).arrangement==DeviceControlArrangement::below);
    }
    // The arrangement follows the window's shape alone: landscape or square around, portrait below,
    // and around with no window to fit, as for a fixed desktop window.
    auto chosen=[&](DeviceControlView from,int w,int h) {return deviceControlViewForWindow(p,from,w,h).arrangement;};
    for(auto from:{below,around}) {
        assert(chosen(from,2400,1080)==DeviceControlArrangement::around);
        assert(chosen(from,1200,1000)==DeviceControlArrangement::around);
        assert(chosen(from,1280,800)==DeviceControlArrangement::around);
        assert(chosen(from,1000,1000)==DeviceControlArrangement::around);
        assert(chosen(from,1080,2400)==DeviceControlArrangement::below);
        assert(chosen(from,800,1280)==DeviceControlArrangement::below);
        assert(chosen(from,1000,1001)==DeviceControlArrangement::below);
        assert(chosen(from,0,0)==DeviceControlArrangement::around);
    }
    // Every real control in both arrangements; no five-way.
    for(auto section:{DeviceControlSection::below,DeviceControlSection::around}) {
        std::vector<KeyId> keys;int sticks=0;
        for(size_t i=0;i<p.count;++i) if(p.buttons[i].section==section) {
            if(p.buttons[i].icon==DeviceControlIcon::stick) ++sticks; else keys.push_back(p.buttons[i].key);
        }
        std::sort(keys.begin(),keys.end());
        const std::vector<KeyId> expected{keyIdHard1,keyIdHard2,keyIdHard3,keyIdHard4,keyIdHome,
                                          keyIdFunction,keyIdTriggerLeft,keyIdTriggerRight};
        assert(keys==expected && sticks==1);
    }
    for(size_t i=0;i<p.count;++i) {
        assert(p.buttons[i].key<keyIdUp || p.buttons[i].key>keyIdSelect);
        // The stick's entry carries its press; Action keys are named by place, not A to D.
        assert((p.buttons[i].icon==DeviceControlIcon::stick)==(p.buttons[i].key==keyIdStickPress));
        if(p.buttons[i].icon==DeviceControlIcon::action) assert(std::strstr(p.buttons[i].label,"action button"));
    }
    static_assert(keyIdStickPress>=64,"a key past the old 64-bit mask");
    auto rectOf=[&](KeyId key,const DeviceControlView& view,int sw,int sh) {
        for(size_t i=0;i<p.count;++i) if(p.buttons[i].key==key && deviceControlShown(p.buttons[i],view))
            return deviceButtonRect(p,p.buttons[i],sw,sh,view);
        assert(false);return SDL_Rect{};
    };
    // Both arrangements at the shown LCD's size for 0/180 (480x320) and 90/270 (320x480).
    for(auto [sw,sh]:{std::pair{480,320},std::pair{320,480}})
    for(auto view:{below,around}) {
        const bool side=view.arrangement==DeviceControlArrangement::around;
        const int width=deviceControlsWidth(p,sw,view),height=sh+deviceControlsHeight(p,sw,view);
        assert(width==(side ? sw+300 : sw));
        if(!side) assert(deviceControlsHeight(p,sw,view)==(sw==480 ? 200 : 134));
        std::vector<SDL_Rect> shown;
        for(size_t i=0;i<p.count;++i) {
            const auto& b=p.buttons[i];if(!deviceControlShown(b,view))continue;
            const auto r=deviceButtonRect(p,b,sw,sh,view);
            // Inside the window and off the LCD: in a side panel, or under the screen.
            assert(r.x>=0 && r.y>=0 && r.x+r.w<=width && r.y+r.h<=height);
            if(side) assert(r.x+r.w<=150 || r.x>=150+sw); else assert(r.y>=sh);
            // Scaled to 320 wide the smallest keys stay a usable size.
            assert(std::min(r.w,r.h)>=(sw==480 ? 24 : 16));
            for(const auto& o:shown) assert(!SDL_HasIntersection(&r,&o));
            shown.push_back(r);
            const int cx=r.x+r.w/2,cy=r.y+r.h/2;
            if(b.icon==DeviceControlIcon::stick) {
                assert(deviceControlAt(p,sw,sh,cx,cy,view)==keyInvalid && deviceStickGrab(r,cx+0.5f,cy+0.5f));
                assert(deviceStickKnobAt(r,cx+0.5f,cy+0.5f) && !deviceStickKnobAt(r,r.x+r.w*0.5f,r.y+2.f));
                const auto full=deviceStickAt(r,cx+r.w*2.f,cy+0.5f);
                assert(full.held && std::fabs(full.x-1)<1e-4f && std::fabs(full.y)<0.01f);
                const auto half=deviceStickAt(r,r.x+r.w*0.5f,r.y+r.h*0.5f-r.w*deviceStickTravel*0.5f);
                assert(std::fabs(half.x)<1e-4f && std::fabs(half.y+0.5f)<1e-4f);
            } else {
                assert(deviceControlAt(p,sw,sh,cx,cy,view)==b.key);
                // Round Action keys miss at their corners.
                if(b.icon==DeviceControlIcon::action) assert(deviceControlAt(p,sw,sh,r.x,r.y,view)==keyInvalid);
            }
        }
        const SDL_Rect stick=deviceStickRect(p,sw,sh,view);
        assert(shown.size()==9 && stick.w>0);
        // The middle of the Action diamond is a gap; Bottom is hard1 and Top hard4.
        const auto a=rectOf(keyIdHard1,view,sw,sh),d=rectOf(keyIdHard4,view,sw,sh);
        assert(a.y>d.y && deviceControlAt(p,sw,sh,a.x+a.w/2,(a.y+d.y+d.h)/2,view)==keyInvalid);
        const auto fn=rectOf(keyIdFunction,view,sw,sh),home=rectOf(keyIdHome,view,sw,sh);
        const auto tl=rectOf(keyIdTriggerLeft,view,sw,sh),tr=rectOf(keyIdTriggerRight,view,sw,sh);
        // Function sits above Home in both arrangements.
        assert(fn.y+fn.h<=home.y);
        if(side) {
            // Left panel top to bottom: trigger, stick, Function, Home; the right panel has the
            // trigger and the diamond only. The triggers stay at the top of a taller panel and the
            // rest are centred on it.
            const int top=(sh-320)/2;
            assert(tl.y==8 && tr.y==8 && tr.x>=150+sw);
            assert(stick.x+stick.w<=150 && fn.x+fn.w<=150 && home.x+home.w<=150);
            assert(tl.y+tl.h<=stick.y && stick.y+stick.h<=fn.y && stick.y==52+top && home.y==272+top);
            for(KeyId k:{keyIdHard1,keyIdHard2,keyIdHard3,keyIdHard4}) assert(rectOf(k,view,sw,sh).x>=150+sw);
        } else {
            // Under the screen: Function and Home between the stick and the diamond.
            const auto left=rectOf(keyIdHard3,view,sw,sh);
            assert(fn.x>=stick.x+stick.w && home.x>=stick.x+stick.w);
            assert(fn.x+fn.w<=left.x && home.x+home.w<=left.x);
        }
    }

    // Input on a stand-in guest, around the screen at scale 1.
    ZodiacGuest guest;auto display=guest.display();
    ZodiacInput input(&guest,1.f,display,Rotation::portrait_0,around);
    input.SetDirectCapture(false);
    auto centre=[&](KeyId key,const DeviceControlView& view) {
        for(size_t i=0;i<p.count;++i) if(p.buttons[i].key==key && deviceControlShown(p.buttons[i],view)) {
            const auto r=deviceButtonRect(p,p.buttons[i],480,320,view);return std::pair<int,int>{r.x+r.w/2,r.y+r.h/2};
        }
        assert(false);return std::pair<int,int>{};
    };
    auto pointer=[&](Uint32 type,int x,int y) {
        SDL_Event e{};e.type=type;
        if(type==SDL_MOUSEMOTION) {e.motion.x=x;e.motion.y=y;} else {e.button.button=SDL_BUTTON_LEFT;e.button.x=x;e.button.y=y;}
        input.HandleEvent(e);
    };
    auto finger=[&](Uint32 type,SDL_FingerID id,int x,int y,int w,int h) {
        SDL_Event e{};e.type=type;e.tfinger.type=type;e.tfinger.touchId=9;e.tfinger.fingerId=id;
        e.tfinger.x=(x+0.5f)/w;e.tfinger.y=(y+0.5f)/h;input.HandleEvent(e);
    };
    auto keyEvent=[&](SDL_Keycode k,bool down) {
        SDL_Event e{};e.type=down ? SDL_KEYDOWN : SDL_KEYUP;e.key.keysym.sym=k;input.HandleEvent(e);
    };
    for(KeyId k:{keyIdHard1,keyIdHard2,keyIdHard3,keyIdHard4,keyIdHome,keyIdFunction,keyIdTriggerLeft,keyIdTriggerRight}) {
        const auto [x,y]=centre(k,around);
        pointer(SDL_MOUSEBUTTONDOWN,x,y);guest.expect({{k,true}});assert(input.PressedButtons()==deviceKeyBit(k));
        pointer(SDL_MOUSEBUTTONUP,x,y);guest.expect({{k,false}});guest.noPen();
    }
    // The pointer holds the stick and keeps full deflection off the well; letting go centres it.
    const SDL_Rect well=deviceStickRect(p,480,320,around);
    const int sx=well.x+well.w/2,sy=well.y+well.h/2;
    pointer(SDL_MOUSEBUTTONDOWN,sx,sy);auto st=guest.lastStick();
    assert(input.Stick().held && std::fabs(st.x)<0.05f && std::fabs(st.y)<0.05f);
    pointer(SDL_MOUSEMOTION,sx+200,sy);st=guest.lastStick();assert(std::fabs(st.x-1)<0.01f && std::fabs(st.y)<0.01f);
    pointer(SDL_MOUSEMOTION,sx,sy-int(well.w*deviceStickTravel/2));st=guest.lastStick();
    assert(std::fabs(st.x)<0.05f && std::fabs(st.y+0.5f)<0.05f);
    pointer(SDL_MOUSEBUTTONUP,sx,sy);st=guest.lastStick();
    assert(st.x==0 && st.y==0 && !input.Stick().held);guest.expect({});guest.noPen();
    // A tap on the knob pushes the stick in; a drag from it or a press off the knob doesn't.
    pointer(SDL_MOUSEBUTTONDOWN,sx,sy);st=guest.lastStick();assert(st.x==0 && st.y==0 && input.Stick().held);
    pointer(SDL_MOUSEMOTION,sx+1,sy);guest.noStick();
    pointer(SDL_MOUSEBUTTONUP,sx+1,sy);guest.expect({{keyIdStickPress,true},{keyIdStickPress,false}});
    st=guest.lastStick();assert(st.x==0 && st.y==0 && !input.Stick().held);
    pointer(SDL_MOUSEBUTTONDOWN,sx,sy);guest.lastStick();
    pointer(SDL_MOUSEMOTION,sx+30,sy);st=guest.lastStick();assert(st.x>0.5f);
    pointer(SDL_MOUSEMOTION,sx,sy);st=guest.lastStick();assert(std::fabs(st.x)<0.05f);
    pointer(SDL_MOUSEBUTTONUP,sx,sy);guest.lastStick();guest.expect({});
    pointer(SDL_MOUSEBUTTONDOWN,sx,well.y+3);st=guest.lastStick();assert(st.y<-0.9f);
    pointer(SDL_MOUSEBUTTONUP,sx,well.y+3);guest.lastStick();guest.expect({});
    finger(SDL_FINGERDOWN,7,sx,sy,780,320);guest.lastStick();
    finger(SDL_FINGERUP,7,sx,sy,780,320);guest.expect({{keyIdStickPress,true},{keyIdStickPress,false}});guest.lastStick();
    finger(SDL_FINGERDOWN,8,sx,sy,780,320);guest.lastStick();
    finger(SDL_FINGERMOTION,8,sx,sy+40,780,320);st=guest.lastStick();assert(st.y>0.9f);
    finger(SDL_FINGERUP,8,sx,sy,780,320);guest.lastStick();guest.expect({});
    // F10 holds it in until let go, drawn pressed; a tap meanwhile doesn't press it again.
    keyEvent(SDLK_F10,true);guest.expect({{keyIdStickPress,true}});assert(input.PressedButtons()==deviceKeyBit(keyIdStickPress));
    pointer(SDL_MOUSEBUTTONDOWN,sx,sy);pointer(SDL_MOUSEBUTTONUP,sx,sy);guest.sticks.clear();guest.expect({});
    keyEvent(SDLK_F10,false);guest.expect({{keyIdStickPress,false}});
    // The LCD sits between the panels.
    pointer(SDL_MOUSEBUTTONDOWN,150,0);guest.pen(true,0,0);pointer(SDL_MOUSEBUTTONUP,150,0);guest.pen(false);
    pointer(SDL_MOUSEBUTTONDOWN,629,319);guest.pen(true,479,319);pointer(SDL_MOUSEBUTTONUP,629,319);guest.pen(false);
    pointer(SDL_MOUSEBUTTONDOWN,149,160);guest.noPen();guest.expect({});pointer(SDL_MOUSEBUTTONUP,149,160);
    // Several fingers: the stick, a trigger and an Action key at once; focus loss lets all go.
    const auto [rx,ry]=centre(keyIdTriggerRight,around);const auto [ax,ay]=centre(keyIdHard2,around);
    finger(SDL_FINGERDOWN,1,sx,sy,780,320);guest.lastStick();
    finger(SDL_FINGERDOWN,2,rx,ry,780,320);finger(SDL_FINGERDOWN,3,ax,ay,780,320);
    guest.expect({{keyIdTriggerRight,true},{keyIdHard2,true}});
    finger(SDL_FINGERMOTION,1,sx-100,sy+100,780,320);st=guest.lastStick();
    assert(st.x<-0.6f && st.y>0.6f && std::fabs(std::hypot(st.x,st.y)-1)<0.01f);
    assert(input.PressedButtons()==(deviceKeyBit(keyIdTriggerRight)|deviceKeyBit(keyIdHard2)));
    // Releasing one contact leaves the other key and the deflected stick held.
    finger(SDL_FINGERUP,3,ax,ay,780,320);guest.expect({{keyIdHard2,false}});guest.noStick();
    assert(input.PressedButtons()==deviceKeyBit(keyIdTriggerRight));
    assert(input.Stick().held && input.Stick().x<-0.6f && input.Stick().y>0.6f);
    finger(SDL_FINGERDOWN,3,ax,ay,780,320);guest.expect({{keyIdHard2,true}});guest.noStick();
    // A second finger on the well doesn't take the stick from the first.
    finger(SDL_FINGERDOWN,4,sx,sy,780,320);guest.noStick();guest.expect({});
    SDL_Event lost{};lost.type=SDL_WINDOWEVENT;lost.window.event=SDL_WINDOWEVENT_FOCUS_LOST;input.HandleEvent(lost);
    guest.expect({{keyIdTriggerRight,false},{keyIdHard2,false}});st=guest.lastStick();
    assert(st.x==0 && st.y==0 && !input.Stick().held && !input.PressedButtons());
    for(SDL_FingerID id:{1,2,3,4}) finger(SDL_FINGERUP,id,0,0,780,320);
    guest.expect({});guest.noStick();
    // Arrow keys stand in for the stick, not a five-way; F7 to F9 are Function and the triggers.
    keyEvent(SDLK_RIGHT,true);st=guest.lastStick();assert(st.x==1 && st.y==0);
    keyEvent(SDLK_UP,true);st=guest.lastStick();assert(std::fabs(st.x-0.7071f)<1e-3f && std::fabs(st.y+0.7071f)<1e-3f);
    keyEvent(SDLK_RIGHT,false);st=guest.lastStick();assert(st.x==0 && st.y==-1);
    keyEvent(SDLK_UP,false);st=guest.lastStick();assert(st.x==0 && st.y==0);guest.expect({});
    keyEvent(SDLK_F7,true);keyEvent(SDLK_F8,true);keyEvent(SDLK_F9,true);keyEvent(SDLK_F6,true);keyEvent(SDLK_F10,true);
    guest.expect({{keyIdFunction,true},{keyIdTriggerLeft,true},{keyIdTriggerRight,true},{keyIdHome,true},{keyIdStickPress,true}});
    // Changing arrangement lets go of the pointer and fingers on the old one; keyboard keys stay
    // held until their key-up or focus loss.
    keyEvent(SDLK_LEFT,true);guest.lastStick();
    pointer(SDL_MOUSEBUTTONDOWN,sx,sy);guest.lastStick();
    input.SetView(below,1.f);
    // The pointer lets go and the held arrow key takes the stick.
    st=guest.lastStick();assert(st.x==-1 && st.y==0);guest.expect({});
    assert(input.PressedButtons()==(deviceKeyBit(keyIdHome)|deviceKeyBit(keyIdFunction)|deviceKeyBit(keyIdTriggerLeft)|
                                    deviceKeyBit(keyIdTriggerRight)|deviceKeyBit(keyIdStickPress)));
    input.HandleEvent(lost);
    guest.expect({{keyIdHome,false},{keyIdFunction,false},{keyIdTriggerLeft,false},{keyIdTriggerRight,false},
                  {keyIdStickPress,false}});
    st=guest.lastStick();assert(st.x==0 && st.y==0 && !input.PressedButtons());
    // Below the screen at scale 2: the stick, keys and the LCD's corners.
    input.SetView(below,2.f);guest.noStick();
    const SDL_Rect low=deviceStickRect(p,480,320,below);
    pointer(SDL_MOUSEBUTTONDOWN,2*(low.x+low.w/2),2*(low.y+low.h/2));guest.lastStick();
    pointer(SDL_MOUSEMOTION,2*(low.x+low.w/2),2*(low.y+low.h+40));st=guest.lastStick();assert(std::fabs(st.y-1)<0.01f);
    pointer(SDL_MOUSEBUTTONUP,0,0);st=guest.lastStick();assert(st.x==0 && st.y==0);
    const auto [bx,by]=centre(keyIdHard3,below);
    pointer(SDL_MOUSEBUTTONDOWN,2*bx,2*by);guest.expect({{keyIdHard3,true}});pointer(SDL_MOUSEBUTTONUP,2*bx,2*by);guest.expect({{keyIdHard3,false}});
    pointer(SDL_MOUSEBUTTONDOWN,959,639);guest.pen(true,479,319);pointer(SDL_MOUSEBUTTONUP,959,639);guest.pen(false);
    pointer(SDL_MOUSEBUTTONDOWN,959,641);guest.noPen();pointer(SDL_MOUSEBUTTONUP,959,641);
    // The TX has no Function or triggers, and its arrows stay the five-way.
    Guest tx(deviceTypeTX);auto txDisplay=tx.display();
    SdlEventHandler txInput(&tx,1.f,txDisplay,Rotation::portrait_0);
    for(SDL_Keycode k:{SDLK_F7,SDLK_F8,SDLK_F9,SDLK_F10}) {SDL_Event e{};e.type=SDL_KEYDOWN;e.key.keysym.sym=k;txInput.HandleEvent(e);}
    tx.expect({});
    {SDL_Event e{};e.type=SDL_KEYDOWN;e.key.keysym.sym=SDLK_RIGHT;txInput.HandleEvent(e);tx.expect({{keyIdRight,true}});}
    // At 90 and 270 the shown LCD is 320x480: keys, the stick and its press follow it in both arrangements.
    for(auto rotation:{Rotation::landscape_90,Rotation::landscape_270})
    for(auto view:{around,below}) {
        ZodiacGuest turned;auto turnedDisplay=turned.display();
        ZodiacInput sideways(&turned,1.f,turnedDisplay,rotation,view);sideways.SetDirectCapture(false);
        auto press=[&](Uint32 type,int x,int y) {
            SDL_Event e{};e.type=type;
            if(type==SDL_MOUSEMOTION) {e.motion.x=x;e.motion.y=y;} else {e.button.button=SDL_BUTTON_LEFT;e.button.x=x;e.button.y=y;}
            sideways.HandleEvent(e);
        };
        for(KeyId k:{keyIdHard1,keyIdHard2,keyIdHard3,keyIdHard4,keyIdHome,keyIdFunction,keyIdTriggerLeft,keyIdTriggerRight}) {
            const auto r=rectOf(k,view,320,480);
            press(SDL_MOUSEBUTTONDOWN,r.x+r.w/2,r.y+r.h/2);turned.expect({{k,true}});
            press(SDL_MOUSEBUTTONUP,r.x+r.w/2,r.y+r.h/2);turned.expect({{k,false}});turned.noPen();
        }
        const SDL_Rect w=deviceStickRect(p,320,480,view);
        const int wx=w.x+w.w/2,wy=w.y+w.h/2;
        press(SDL_MOUSEBUTTONDOWN,wx,wy);turned.lastStick();press(SDL_MOUSEBUTTONUP,wx,wy);
        turned.expect({{keyIdStickPress,true},{keyIdStickPress,false}});turned.lastStick();
        press(SDL_MOUSEBUTTONDOWN,wx,wy);turned.lastStick();
        press(SDL_MOUSEMOTION,wx-w.w,wy);auto t=turned.lastStick();assert(std::fabs(t.x+1)<0.01f);
        press(SDL_MOUSEBUTTONUP,wx-w.w,wy);turned.lastStick();turned.expect({});
        // The LCD's corners still reach the pen, and the pixel past it doesn't.
        const int left=deviceScreenLeft(p,view);
        press(SDL_MOUSEBUTTONDOWN,left,0);turned.penInside(true);press(SDL_MOUSEBUTTONUP,left,0);turned.pen(false);
        press(SDL_MOUSEBUTTONDOWN,left+319,479);turned.penInside(true);press(SDL_MOUSEBUTTONUP,left+319,479);turned.pen(false);
        press(SDL_MOUSEBUTTONDOWN,left+320,479);turned.noPen();press(SDL_MOUSEBUTTONUP,left+320,479);
    }

    // Drawing at both LCD shapes: panels beside a magenta LCD, the strip below; each key darkens
    // where it hits, the knob moves and darkens when pushed in.
    for(auto [sw,sh]:{std::pair{480,320},std::pair{320,480}})
    for(auto view:{around,below}) {
        const bool side=view.arrangement==DeviceControlArrangement::around;
        const float scale=1.5f;
        const int w=int(deviceControlsWidth(p,sw,view)*scale),h=int((sh+deviceControlsHeight(p,sw,view))*scale);
        const std::string name=std::string("zodiac-")+(side ? "around" : "below")+(sw==480 ? "" : "-90");
        auto* surface=SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_RGBA32);
        auto* renderer=SDL_CreateSoftwareRenderer(surface);assert(surface && renderer);
        auto draw=[&](DeviceKeyMask pressed,DeviceStick stick) {
            SDL_SetRenderDrawColor(renderer,255,0,255,255);SDL_RenderClear(renderer);
            drawDeviceSidebar(renderer,p,sw,sh,scale,pressed,view,stick);
            drawDeviceControls(renderer,p,sw,sh,scale,pressed,view,stick);
            SDL_RenderPresent(renderer);
        };
        draw(0,{});
        const auto magenta=[](SDL_Color c) {return c.r==255 && c.g==0 && c.b==255;};
        assert(magenta(readPixel(surface,int((deviceScreenLeft(p,view)+sw/2)*scale),int(sh/2*scale))));
        assert(!magenta(readPixel(surface,2,h-3)) && !magenta(readPixel(surface,w-3,side ? 2 : h-3)));
        if(side) assert(!magenta(readPixel(surface,w-3,h-3)) && magenta(readPixel(surface,int((150+sw-1)*scale),2)));
        if(const char* directory=std::getenv("FROND_ZODIAC_SCREENSHOT_DIR")) {
            const std::string path=std::string(directory)+"/"+name+".png";
            assert(IMG_SavePNG(surface,path.c_str())==0);
        }
        for(size_t i=0;i<p.count;++i) {
            const auto& b=p.buttons[i];if(!deviceControlShown(b,view) || b.icon==DeviceControlIcon::stick)continue;
            const auto r=deviceButtonRect(p,b,sw,sh,view);
            const int cx=int((r.x+r.w*0.5f)*scale),cy=int((r.y+r.h*0.7f)*scale);
            draw(0,{});const int up=brightness(readPixel(surface,cx,cy));
            draw(deviceKeyBit(b.key),{});const int held=brightness(readPixel(surface,cx,cy));
            assert(held<up);
        }
        const auto r=deviceStickRect(p,sw,sh,view);
        const int kx=int((r.x+r.w*0.5f)*scale),ky=int((r.y+r.h*0.5f)*scale),off=int(r.w*0.42f*scale);
        draw(0,{});const int rest=brightness(readPixel(surface,kx+off,ky)),centre=brightness(readPixel(surface,kx,ky+2));
        draw(0,{1,0,true});const int pushed=brightness(readPixel(surface,kx+off,ky));
        assert(pushed>rest+20);
        draw(deviceKeyBit(keyIdStickPress),{0,0,true});assert(brightness(readPixel(surface,kx,ky+2))<centre);
        if(const char* directory=std::getenv("FROND_ZODIAC_SCREENSHOT_DIR")) {
            draw(deviceKeyBit(keyIdHard1)|deviceKeyBit(keyIdTriggerRight),{0.7f,-0.7f,true});
            const std::string path=std::string(directory)+"/"+name+"-held.png";
            assert(IMG_SavePNG(surface,path.c_str())==0);
        }
        SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);
    }
    // A fitted renderer lays out each arrangement at its own shape.
    for(auto [view,ww,wh,rotation]:{std::tuple{around,1560,800,Rotation::portrait_0},std::tuple{below,800,1560,Rotation::portrait_0},
                                    std::tuple{around,1200,1000,Rotation::landscape_90},std::tuple{below,800,1280,Rotation::landscape_270}}) {
        assert(deviceControlViewForWindow(p,{},ww,wh).arrangement==view.arrangement);
        auto* win=SDL_CreateWindow("Zodiac",0,0,ww,wh,SDL_WINDOW_RESIZABLE);assert(win);
        guest.frame.assign(480*320,0xffffffff);
        SdlRenderer renderer(win,&guest,1,display,rotation,false,WindowFit::fit,0x0b0c11,view);
        assert(renderer.Valid());renderer.Draw(true,deviceKeyBit(keyIdHard1),{0.5f,0,true});
        const auto layout=renderer.Layout();
        const int sw=rotation==Rotation::landscape_90 || rotation==Rotation::landscape_270 ? 320 : 480,sh=800-sw;
        const float aspect=float(deviceControlsWidth(p,sw,view))/(sh+deviceControlsHeight(p,sw,view));
        assert(std::fabs(float(layout.width)/layout.height-aspect)<0.01f && (layout.width==ww || layout.height==wh));
        SDL_DestroyWindow(win);
    }
}

int main(int argc,char** argv) {
    SDL_setenv("SDL_VIDEODRIVER","dummy",1);assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)==0);
    if(argc==2 && std::strcmp(argv[1],"--tg50-visibility")==0) {
        tg50Visibility();SDL_Quit();puts("PASS: TG50 independent keyboard/jog visibility, app row, mouse/touch releases, rotations and fitted/fractional layouts");return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--tg50-live-display")==0) {
        tg50LiveDisplay();SDL_Quit();puts("PASS: TG50 display command parsing and live view/fit changes with releases and hit areas");return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--zodiac")==0) {
        zodiacControls();SDL_Quit();puts("PASS: Zodiac below and around arrangements, keys, stick, multi-touch, releases and pressed art");return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--tg50-layout")==0) {
        tg50RuntimeTab();SDL_Quit();puts("PASS: TG50 sidebar/LCD pixel boundaries and pen bounds at fractional scales and four rotations");return 0;
    }
    firmwarePowerInput();
    const auto& profile=deviceControls(deviceTypeTX);
    assert(profile.count==9 && profile.style==DeviceControlStyle::tx && profile.height==84);
    const auto& e2=deviceControls(deviceTypeE2);
    assert(e2.count==9 && e2.height==80 && e2.style==DeviceControlStyle::e2);
    // E2 app keys follow the ROM: hard1-4 are Calendar, Contacts, Tasks and Note Pad, left to right.
    for(int k=0;k<4;++k)assert(e2.buttons[k].key==KeyId(keyIdHard1+k) && (k==0 || e2.buttons[k].bounds.x>e2.buttons[k-1].bounds.x));
    assert(e2.buttons[1].bounds.x+e2.buttons[1].bounds.w<e2.navigator.x && e2.buttons[2].bounds.x>e2.navigator.x+e2.navigator.w);
    assert(deviceControls(deviceTypePV).height==0);
    assert(&deviceControls(deviceTypeFrankenE2)==&e2);
    for (size_t i=0;i<deviceControls(deviceTypeFrankenE2).count;++i)
        assert(deviceControls(deviceTypeFrankenE2).buttons[i].key!=keyIdPower);
    assert(!deviceSilkscreen(deviceTypeFrankenE2).count && !deviceSilkscreen(deviceTypeFrankenE2).height);
    // A different device may use an app key for Calendar and separate up/down buttons.
    const DeviceButton otherButtons[]={{keyIdHard1,{4,4,30,30},"Calendar",DeviceControlIcon::calendar},
                                      {keyIdUp,{40,4,30,30},"Up",DeviceControlIcon::up}};
    const DeviceControlProfile other{160,40,otherButtons,2,{0,0,0,0},0,{0,0,0,0}};
    assert(deviceControlAt(other,160,200,50,215)==keyIdUp);
    exercise(deviceTypeTX);
    exercise(deviceTypeE2);
    exercise(deviceTypeFrankenE2);
    exercise(deviceTypeZire72);
    zireAndM515();
    silkscreen(deviceTypeE2);
    silkscreen(deviceTypeZire72);
    lcdScaling();
    tg50RuntimeTab();
    tg50LiveDisplay();
    zodiacControls();
    SDL_Quit();puts("PASS: TX, E2 and Zire 72 device buttons, holds/chords, focus/drag/rotation release, touch isolation, scales, pressed rendering and E2 and Zire 72 silkscreens; Zire 72, m515, IIIx and TG50 hit areas, pressed art and silkscreens; TG50 keyboard and side tab hiding; Zodiac arrangements, stick and keys");
}
