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
#include "WindowScale.h"
#include <SDL_image.h>
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
        assert(e.penDown==down);if(down) { if(e.x!=x || e.y!=y)std::fprintf(stderr,"pen got %d,%d expected %d,%d\n",e.x,e.y,x,y);assert(e.x==x && e.y==y); }
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
            for(float x:{48.f,273.f})assert(brightness(at(x,60))>panel+120 && brightness(at(x+2,60))<panel+12);
            for(float y:{4.f,116.f})assert(brightness(at(silk.numbers.x,y))>panel+120);
            assert(brightness(at(silk.numbers.x,60))<panel+12);
        }
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
    for(float scale:{1.f,1.25f,1.5f,1.75f,2.f}) {
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
        // Calendar over Contacts on the left, Media over Music on the right.
        const auto& b=p.buttons;
        assert(b[0].bounds.x==b[1].bounds.x && b[0].bounds.y<b[1].bounds.y && b[2].bounds.x==b[3].bounds.x);
        assert(b[1].bounds.x+b[1].bounds.w<p.navigator.x && b[2].bounds.x>p.navigator.x+p.navigator.w);
        // Gaps between the stacked pills and between the pills and the five-way.
        assert(at(50,(b[0].bounds.y+b[0].bounds.h+b[1].bounds.y)*0.5f)==keyInvalid);
        assert(at(270,(b[2].bounds.y+b[2].bounds.h+b[3].bounds.y)*0.5f)==keyInvalid);
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

static void zireAndM515() {
    // TX and E2 keep their art through DeviceType5; the others have none yet.
    assert(deviceArt(deviceTypeTX)==DeviceArt::tx && deviceArt(deviceTypeE2)==DeviceArt::e2);
    assert(deviceArt(deviceTypePV)==DeviceArt::none && deviceArt(deviceTypeFrankenE2)==DeviceArt::none);
    assert(&deviceControls(deviceTypeTX)==&deviceControls(DeviceArt::tx) && &deviceSilkscreen(deviceTypeE2)==&deviceSilkscreen(DeviceArt::e2));
    assert(deviceControls(deviceTypeTX).unit==1 && deviceControls(deviceTypeE2).unit==1 && deviceSilkscreen(deviceTypeE2).unit==1);
    for(auto art:{DeviceArt::zire72,DeviceArt::m515}) {
        artDevice(art);
        artSilkscreen(art);
    }
    iiixArt();
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
    zireAndM515();
    silkscreen();
    lcdScaling();
    SDL_Quit();puts("PASS: TX and E2 device buttons, holds/chords, focus/drag/rotation release, touch isolation, scales, pressed rendering and E2 silkscreen; Zire 72, m515 and IIIx hit areas, pressed art and silkscreens");
}
