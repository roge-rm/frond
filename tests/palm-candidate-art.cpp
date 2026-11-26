// Art for Palm devices that aren't emulated yet: key geometry, hit testing, the hidden keyboard,
// held keys and a few colours of each bar and silkscreen. With an output folder it also writes
// preview PNGs: each device whole at 1x and 3x and with a key held.
#include "DeviceControls.h"
#include <SDL_image.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace {
struct Image {
    SDL_Surface* surface;
    SDL_Renderer* renderer;
    Image(int w, int h) {
        surface=SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_RGBA32);
        renderer=SDL_CreateSoftwareRenderer(surface);
        assert(surface && renderer);
        fill({0,0,w,h},26,26,28);
    }
    ~Image() {SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);}
    void fill(SDL_Rect r, Uint8 red, Uint8 green, Uint8 blue) {
        SDL_SetRenderDrawColor(renderer,red,green,blue,255);
        SDL_RenderFillRect(renderer,&r);
    }
    SDL_Color at(int x, int y) const {
        const Uint8* p=static_cast<const Uint8*>(surface->pixels)+y*surface->pitch+x*4;
        return {p[0],p[1],p[2],255};
    }
    void save(const std::string& path) {
        SDL_RenderPresent(renderer);
        if(IMG_SavePNG(surface,path.c_str())!=0) {std::fprintf(stderr,"%s: %s\n",path.c_str(),IMG_GetError());std::exit(1);}
    }
};
int brightness(SDL_Color c) {return c.r+c.g+c.b;}

// One device's LCD size and art.
struct Device {
    const char* name;
    DeviceArt art;
    int lcdWidth, lcdHeight;
};

// Silkscreen rows in window pixels at scale 1, 0 for none.
int silkRows(const Device& d) {
    const auto& silk=deviceSilkscreen(d.art);
    return silk.count ? silk.height/silk.unit : 0;
}

// Draws the device at scale with the given keys held: a blank grey-green LCD with a grid, so it
// isn't taken for a running guest, the silkscreen under it and the strip under that.
void drawDevice(Image& image, const Device& d, float scale, DeviceKeyMask pressed, const DeviceControlView& view={}) {
    const int screenHeight=d.lcdHeight+silkRows(d);
    const SDL_Rect lcd{0,0,int(d.lcdWidth*scale),int(d.lcdHeight*scale)};
    image.fill(lcd,178,186,170);
    for(int i=1;i<8;++i) {
        image.fill({lcd.w*i/8,0,1,lcd.h},160,168,152);
        image.fill({0,lcd.h*i/8,lcd.w,1},160,168,152);
    }
    const auto& silk=deviceSilkscreen(d.art);
    if(silk.count) {
        auto* texture=createDeviceSilkscreen(image.renderer,silk,scale);
        assert(texture);
        const SDL_Rect dest{0,lcd.h,lcd.w,int(std::lround(silkRows(d)*scale))};
        SDL_RenderCopy(image.renderer,texture,nullptr,&dest);
        SDL_DestroyTexture(texture);
    }
    drawDeviceControls(image.renderer,deviceControls(d.art),d.lcdWidth,screenHeight,scale,pressed,view,{});
}

int imageHeight(const Device& d, float scale, const DeviceControlView& view={}) {
    const auto& p=deviceControls(d.art);
    return int(std::lround((d.lcdHeight+silkRows(d)+deviceControlsHeight(p,d.lcdWidth,view))*scale));
}

// Strip-unit rectangle of a key in window pixels at scale 1.
SDL_Rect windowRect(const Device& d, const DeviceButton& b) {
    return deviceButtonRect(deviceControls(d.art),b,d.lcdWidth,d.lcdHeight+silkRows(d));
}

// The key at a window pixel at scale 1.
KeyId keyAt(const Device& d, int x, int y, const DeviceControlView& view={}) {
    return deviceControlAt(deviceControls(d.art),d.lcdWidth,d.lcdHeight+silkRows(d),x,y,view);
}

// Every key hits at its centre and the profile's keys are unique, inside the strip and apart.
void checkKeys(const Device& d) {
    const auto& p=deviceControls(d.art);
    std::set<int> keys;
    for(size_t i=0;i<p.count;++i) {
        const auto& b=p.buttons[i];
        assert(keys.insert(b.key).second);
        assert(b.bounds.x>=0 && b.bounds.y>=0 && b.bounds.x+b.bounds.w<=p.width && b.bounds.y+b.bounds.h<=p.height);
        if(b.section==DeviceControlSection::keyboard)assert(b.bounds.y>=p.height-p.keyboardHeight);
        else assert(b.bounds.y+b.bounds.h<=p.height-p.keyboardHeight);
        const auto r=windowRect(d,b);
        assert(keyAt(d,r.x+r.w/2,r.y+r.h/2)==b.key);
        if(deviceNavigatorButton(p,b))continue;
        for(size_t j=i+1;j<p.count;++j) {
            const auto& o=p.buttons[j];
            if(deviceNavigatorButton(p,o))continue;
            SDL_Rect overlap;
            assert(!SDL_IntersectRect(&b.bounds,&o.bounds,&overlap));
        }
    }
}

// A held key changes pixels on itself and nowhere far from it.
void checkHeld(const Device& d, KeyId key, float scale) {
    const auto& p=deviceControls(d.art);
    const int w=int(d.lcdWidth*scale),h=imageHeight(d,scale);
    Image a(w,h),b(w,h);
    drawDevice(a,d,scale,0);
    drawDevice(b,d,scale,deviceKeyBit(key));
    SDL_Rect r{0,0,0,0};
    for(size_t i=0;i<p.count;++i) {
        const auto& button=p.buttons[i];
        if(button.key!=key)continue;
        // A five-way part shades across the ring's diagonals, so take the whole ring.
        r=windowRect(d,deviceNavigatorButton(p,button) ? DeviceButton{key,p.navigator,"",button.icon} : button);
    }
    assert(r.w>0);
    const int m=int(6*scale);
    int changed=0,stray=0;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        const auto pa=a.at(x,y),pb=b.at(x,y);
        if(pa.r==pb.r && pa.g==pb.g && pa.b==pb.b)continue;
        if(x>=(r.x-1)*scale-m && x<(r.x+r.w+1)*scale+m && y>=(r.y-1)*scale-m && y<(r.y+r.h+1)*scale+m)++changed;
        else ++stray;
    }
    assert(changed>20 && stray==0);
}

// Each device whole at 1x and 3x, and with a key held at 3x.
void renders(const Device& d, const std::string& out, KeyId held) {
    if(out.empty())return;
    for(float scale:{1.f,3.f}) {
        Image image(int(d.lcdWidth*scale),imageHeight(d,scale));
        drawDevice(image,d,scale,0);
        image.save(out+"/"+d.name+"-"+std::to_string(int(scale))+"x.png");
    }
    Image image(int(d.lcdWidth*3),imageHeight(d,3));
    drawDevice(image,d,3,deviceKeyBit(held));
    image.save(out+"/"+d.name+"-held-3x.png");
}

void treo650(const std::string& out) {
    const Device d{"treo-650",DeviceArt::treo650,320,320};
    const auto& p=deviceControls(d.art);
    assert(p.width==320 && p.height==252 && p.unit==1 && p.keyboardHeight==144);
    assert(p.style==DeviceControlStyle::treo650 && !deviceSilkscreen(d.art).count);
    // Six bar keys, the five-way's five parts and 35 keyboard keys, then Menu in the keyboard range.
    assert(p.count==46);
    int keyboard=0;
    for(size_t i=0;i<p.count;++i)if(p.buttons[i].section==DeviceControlSection::keyboard) {
        assert(p.buttons[i].key==treoKey(keyboard));
        ++keyboard;
    }
    assert(keyboard==35 && treoKey(35)<keyIdVoice);
    checkKeys(d);
    // The five-way's middle, its ring and the gaps round the bar keys.
    const int top=d.lcdHeight;
    assert(keyAt(d,155,top+71)==keyIdSelect && keyAt(d,155,top+42)==keyIdUp && keyAt(d,106,top+71)==keyIdLeft);
    assert(keyAt(d,50,top+30)==keyInvalid && keyAt(d,100,top+90)==keyInvalid && keyAt(d,160,top+6)==keyInvalid);
    // Between keyboard keys, across a row and between rows.
    assert(keyAt(d,33,top+126)==keyInvalid && keyAt(d,48,top+144)==keyInvalid);
    // Hidden, the keyboard's rows go and its keys don't hit.
    DeviceControlView hidden;hidden.keyboard=false;
    assert(deviceControlsHeight(p,320)==252 && deviceControlsHeight(p,320,hidden)==108);
    assert(keyAt(d,17,top+124,hidden)==keyInvalid && keyAt(d,17,top+124)==treoKey(0));
    {
        Image image(320,imageHeight(d,1,hidden));
        drawDevice(image,d,1,0,hidden);
        assert(image.surface->h==428);
    }

    // Colours: the green Phone and red End icons, a pearl letter key, a navy number key, the
    // chrome band and the grey body.
    Image image(640,imageHeight(d,2));
    drawDevice(image,d,2,0);
    auto pixel=[&](float x,float y) {return image.at(int(x*2),int((top+y)*2));};
    auto count=[&](SDL_Rect r,auto test) {
        int n=0;
        for(int y=r.y*2;y<(r.y+r.h)*2;++y)for(int x=r.x*2;x<(r.x+r.w)*2;++x)if(test(image.at(x,top*2+y)))++n;
        return n;
    };
    assert(count({13,49,32,34},[](SDL_Color c) {return c.g>c.r+50 && c.g>c.b+40;})>20);
    assert(count({275,49,32,34},[](SDL_Color c) {return c.r>c.g+80 && c.r>c.b+80;})>20);
    const auto q=pixel(10,122),e=pixel(72,126);
    assert(brightness(q)>600 && brightness(e)<300 && e.b>e.r+15);
    assert(brightness(pixel(8,30))>480 && brightness(pixel(300,100))>200);
    // Legends: dark ink on the pearl Q, light on the navy E.
    assert(count({8,114,12,18},[](SDL_Color c) {return brightness(c)<200;})>10);
    assert(count({70,118,12,18},[](SDL_Color c) {return brightness(c)>600;})>10);

    for(KeyId key:{keyIdHard1,keyIdPower,keyIdHome,treoKey(35),keyIdSelect,treoKey(0),treoKey(2),treoKey(32)})
        checkHeld(d,key,1);
    renders(d,out,treoKey(13));
}

void zire31(const std::string& out) {
    const Device d{"zire-31",DeviceArt::zire31,160,160};
    const auto& p=deviceControls(d.art);
    const auto& silk=deviceSilkscreen(d.art);
    assert(p.width==320 && p.height==112 && p.unit==2 && p.count==8 && p.style==DeviceControlStyle::zire31);
    assert(silk.count==8 && silk.unit==2 && silk.height==128 && silkRows(d)==64 && silk.style==SilkscreenStyle::zire31);
    for(size_t i=0;i<silk.count;++i) {
        const auto& r=silk.buttons[i].bounds;
        assert(r.x>=0 && r.y>=0 && r.x+r.w<=silk.width && r.y<silk.height);
    }
    checkKeys(d);
    // Window pixels: the strip starts under the 160 LCD rows and the 64 silkscreen rows.
    const int top=224;
    assert(deviceControlsHeight(p,160)==56);
    assert(keyAt(d,80,top+28)==keyIdSelect && keyAt(d,80,top+9)==keyIdUp && keyAt(d,62,top+28)==keyIdLeft);
    assert(keyAt(d,27,top+27)==keyIdHard1 && keyAt(d,133,top+27)==keyIdHard2 && keyAt(d,4,top+27)==keyIdPower);
    assert(keyAt(d,48,top+27)==keyInvalid && keyAt(d,110,top+50)==keyInvalid && keyAt(d,27,top+50)==keyInvalid);

    // Colours at 2x: the black silkscreen and its light discs, the blue body, dark app keys,
    // the black five-way and the green power light.
    Image image(320,imageHeight(d,2));
    drawDevice(image,d,2,0);
    auto pixel=[&](float x,float y) {return image.at(int(x*2),int(y*2));};
    assert(brightness(pixel(80,163))<150 && brightness(pixel(12,176))>450 && brightness(pixel(149,176))>450);
    const auto blue=pixel(5,top+6);
    assert(blue.b>blue.r+30);
    const auto app=pixel(27,top+31);
    assert(brightness(app)<330 && app.b>app.r);
    assert(brightness(pixel(64,top+10))<200);
    int green=0;
    for(int y=(top+19)*2;y<(top+27)*2;++y)for(int x=0;x<20;++x) {
        const auto c=image.at(x,y);
        if(c.g>c.r+60 && c.g>c.b+40)++green;
    }
    assert(green>4);

    for(KeyId key:{keyIdPower,keyIdHard1,keyIdHard2,keyIdSelect,keyIdUp,keyIdRight})checkHeld(d,key,2);
    renders(d,out,keyIdHard1);
}
}  // namespace

int main(int argc, char** argv) {
    const std::string out=argc>1 ? argv[1] : "";
    treo650(out);
    zire31(out);
    std::puts("palm candidate art ok");
    return 0;
}
