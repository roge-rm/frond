// Art for Sony CLIEs that aren't emulated yet: key geometry, hit testing, the side tab in each
// place, held keys and a few colours of each bar and silkscreen. With an output folder it also
// writes preview PNGs: each device whole at 1x and 3x, with a key held, and with the side tab
// beside the buttons.
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
const DeviceControlView besideLcd{};
const DeviceControlView besideButtons=deviceControlView(true,JogPlacement::keyboard);
const DeviceControlView jogHidden=deviceControlView(true,JogPlacement::hidden);

// Silkscreen rows in window pixels at scale 1, 0 for none.
int silkRows(const Device& d) {
    const auto& silk=deviceSilkscreen(d.art);
    return silk.count ? silk.height/silk.unit : 0;
}
int screenHeight(const Device& d) {return d.lcdHeight+silkRows(d);}
const DeviceControlProfile& profileOf(const Device& d) {return deviceControls(d.art);}
int imageWidth(const Device& d, float scale, const DeviceControlView& view) {
    return int(std::lround(deviceControlsWidth(profileOf(d),d.lcdWidth,view)*scale));
}
int imageHeight(const Device& d, float scale, const DeviceControlView& view) {
    return int(std::lround((screenHeight(d)+deviceControlsHeight(profileOf(d),d.lcdWidth,view))*scale));
}

// Draws the device at scale with the given keys held: a blank grey-green LCD with a grid, so it
// isn't taken for a running guest, the silkscreen, the side tab and the strip.
void drawDevice(Image& image, const Device& d, float scale, DeviceKeyMask pressed, const DeviceControlView& view) {
    const auto& p=profileOf(d);
    const int left=deviceScreenLeft(p,view);
    const SDL_Rect lcd{int(left*scale),0,int(d.lcdWidth*scale),int(d.lcdHeight*scale)};
    image.fill(lcd,178,186,170);
    for(int i=1;i<8;++i) {
        image.fill({lcd.x+lcd.w*i/8,0,1,lcd.h},160,168,152);
        image.fill({lcd.x,lcd.h*i/8,lcd.w,1},160,168,152);
    }
    const auto& silk=deviceSilkscreen(d.art);
    if(silk.count) {
        const SDL_Rect r=deviceSilkscreenRect(p,silk,d.lcdWidth,screenHeight(d),view);
        auto* texture=createDeviceSilkscreen(image.renderer,silk,scale*r.w/d.lcdWidth);
        assert(texture);
        const SDL_Rect dest{int(std::lround(r.x*scale)),int(std::lround(r.y*scale)),
                            int(std::lround(r.w*scale)),int(std::lround(r.h*scale))};
        SDL_RenderCopy(image.renderer,texture,nullptr,&dest);
        SDL_DestroyTexture(texture);
    }
    drawDeviceSidebar(image.renderer,p,d.lcdWidth,screenHeight(d),scale,pressed,view);
    drawDeviceControls(image.renderer,p,d.lcdWidth,screenHeight(d),scale,pressed,view);
}

// A key's rectangle in window pixels at scale 1, the whole shape for half a rocker.
SDL_Rect windowRect(const Device& d, const DeviceButton& b, const DeviceControlView& view) {
    DeviceButton whole=b;
    if(b.shape.w>0)whole.bounds=b.shape;
    return deviceButtonRect(profileOf(d),whole,d.lcdWidth,screenHeight(d),view);
}

// The key at a window pixel at scale 1.
KeyId keyAt(const Device& d, int x, int y, const DeviceControlView& view) {
    return deviceControlAt(profileOf(d),d.lcdWidth,screenHeight(d),x,y,view);
}

// Every shown key hits at its centre, and the profile's keys are unique, inside the strip or the
// side tab and apart.
void checkKeys(const Device& d, const DeviceControlView& view) {
    const auto& p=profileOf(d);
    std::set<int> keys;
    for(size_t i=0;i<p.count;++i) {
        const auto& b=p.buttons[i];
        assert(keys.insert(b.key).second);
        if(b.section==DeviceControlSection::sideTab) {
            SDL_Rect inside;
            assert(SDL_IntersectRect(&b.bounds,&p.sideTab,&inside) && SDL_RectEquals(&inside,&b.bounds));
        } else {
            assert(b.bounds.x>=0 && b.bounds.y>=0 && b.bounds.x+b.bounds.w<=p.width && b.bounds.y+b.bounds.h<=p.height);
            if(b.section==DeviceControlSection::keyboard)assert(b.bounds.y>=p.height-p.keyboardHeight);
            else assert(b.bounds.y+b.bounds.h<=p.height-p.keyboardHeight);
        }
        if(!deviceControlShown(b,view))continue;
        const auto r=deviceButtonRect(p,b,d.lcdWidth,screenHeight(d),view);
        if(keyAt(d,r.x+r.w/2,r.y+r.h/2,view)!=b.key) {
            std::fprintf(stderr,"%s: %s misses at its centre\n",d.name,b.label);
            assert(false);
        }
        for(size_t j=i+1;j<p.count;++j) {
            SDL_Rect overlap;
            assert(!SDL_IntersectRect(&b.bounds,&p.buttons[j].bounds,&overlap));
        }
    }
}

// A held key changes pixels on itself and nowhere far from it.
void checkHeld(const Device& d, KeyId key, float scale, const DeviceControlView& view) {
    const auto& p=profileOf(d);
    const int w=imageWidth(d,scale,view),h=imageHeight(d,scale,view);
    Image a(w,h),b(w,h);
    drawDevice(a,d,scale,0,view);
    drawDevice(b,d,scale,deviceKeyBit(key),view);
    SDL_Rect r{0,0,0,0};
    for(size_t i=0;i<p.count;++i)if(p.buttons[i].key==key)r=windowRect(d,p.buttons[i],view);
    assert(r.w>0);
    const float m=6*scale;
    int changed=0,stray=0;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        const auto pa=a.at(x,y),pb=b.at(x,y);
        if(pa.r==pb.r && pa.g==pb.g && pa.b==pb.b)continue;
        if(x>=r.x*scale-m && x<(r.x+r.w)*scale+m && y>=r.y*scale-m && y<(r.y+r.h)*scale+m)++changed;
        else ++stray;
    }
    if(changed<=20 || stray) {
        std::fprintf(stderr,"%s: key %d held: %d changed, %d stray\n",d.name,int(key),changed,stray);
        assert(false);
    }
}

// The side tab in each place: keys hit, and the image is as wide and tall as the controls say.
void checkViews(const Device& d) {
    for(const auto* view:{&besideLcd,&besideButtons,&jogHidden}) {
        checkKeys(d,*view);
        Image image(imageWidth(d,1,*view),imageHeight(d,1,*view));
        drawDevice(image,d,1,0,*view);
    }
    const auto& p=profileOf(d);
    assert(deviceControlsWidth(p,d.lcdWidth,besideLcd)==d.lcdWidth+clieJogWidth);
    assert(deviceControlsWidth(p,d.lcdWidth,besideButtons)==d.lcdWidth);
    assert(deviceControlsWidth(p,d.lcdWidth,jogHidden)==d.lcdWidth);
    // Hidden, the jog's keys don't hit.
    for(size_t i=0;i<p.count;++i) {
        const auto& b=p.buttons[i];
        if(b.section!=DeviceControlSection::sideTab)continue;
        const auto r=deviceButtonRect(p,b,d.lcdWidth,screenHeight(d),besideLcd);
        assert(keyAt(d,r.x+r.w/2,r.y+r.h/2,jogHidden)!=b.key);
    }
}

// Each device whole at 1x and 3x, with a key held at 3x and with the side tab beside the buttons.
void renders(const Device& d, const std::string& out, KeyId held) {
    if(out.empty())return;
    for(float scale:{1.f,3.f}) {
        Image image(imageWidth(d,scale,besideLcd),imageHeight(d,scale,besideLcd));
        drawDevice(image,d,scale,0,besideLcd);
        image.save(out+"/"+d.name+"-"+std::to_string(int(scale))+"x.png");
    }
    {
        Image image(imageWidth(d,3,besideLcd),imageHeight(d,3,besideLcd));
        drawDevice(image,d,3,deviceKeyBit(held),besideLcd);
        image.save(out+"/"+d.name+"-held-3x.png");
    }
    Image image(imageWidth(d,2,besideButtons),imageHeight(d,2,besideButtons));
    drawDevice(image,d,2,0,besideButtons);
    image.save(out+"/"+d.name+"-jog-below-2x.png");
}

// Count of pixels in a window rectangle at scale that pass a test.
template <class Test>
int count(const Image& image, SDL_Rect r, float scale, Test test) {
    int n=0;
    for(int y=int(r.y*scale);y<int((r.y+r.h)*scale);++y)
        for(int x=int(r.x*scale);x<int((r.x+r.w)*scale);++x)if(test(image.at(x,y)))++n;
    return n;
}

void t615c(const std::string& out) {
    const Device d{"t615c",DeviceArt::t615c,320,320};
    const auto& p=profileOf(d);
    const auto& silk=deviceSilkscreen(d.art);
    assert(p.width==320 && p.height==104 && p.unit==1 && p.style==DeviceControlStyle::t415 && p.count==10);
    assert(p.silkscreenHeight==120 && p.sideTabWidth==clieJogWidth);
    assert(silk.count==7 && silk.width==320 && silk.height==120 && silk.unit==1 && silk.style==SilkscreenStyle::t415);
    for(size_t i=0;i<silk.count;++i) {
        const auto& r=silk.buttons[i].bounds;
        assert(r.x>=0 && r.y>=0 && r.x+r.w<=silk.width && r.y+r.h<=silk.height);
    }
    checkViews(d);
    renders(d,out,keyIdHard2);
    // The jog sits beside the same LCD rows as on the T415: jog press from LCD row 62.
    for(size_t i=0;i<p.count;++i)if(p.buttons[i].key==keyIdJogPress)
        assert(deviceButtonRect(p,p.buttons[i],320,440,besideLcd).y==62);
    // Beside the LCD the strip starts 38 pixels in, under the 320 LCD and 120 silkscreen rows.
    const int top=440,l=clieJogWidth;
    assert(keyAt(d,l+37,top+59,besideLcd)==keyIdHard1 && keyAt(d,l+160,top+45,besideLcd)==keyIdUp);
    assert(keyAt(d,l+160,top+72,besideLcd)==keyIdDown && keyAt(d,l+283,top+59,besideLcd)==keyIdHard4);
    assert(keyAt(d,l+140,top+59,besideLcd)==keyInvalid && keyAt(d,l+160,top+20,besideLcd)==keyInvalid);

    // Colours at 2x: the black silkscreen with pale tiles, the light brightness mark, the
    // chrome slats and the silver body.
    Image image(imageWidth(d,2,besideLcd),imageHeight(d,2,besideLcd));
    drawDevice(image,d,2,0,besideLcd);
    auto pixel=[&](float x,float y) {return image.at(int((x+l)*2),int(y*2));};
    assert(brightness(pixel(110,380))<120 && brightness(pixel(5,338))>400 && brightness(pixel(285,339))>400);
    assert(count(image,{l+2,320+98,8,10},2,[](SDL_Color c) {return brightness(c)>450;})>4);
    assert(brightness(pixel(37,top+50))>400 && brightness(pixel(160,top+98))>400);
    assert(brightness(image.at(10,40))>400);

    for(KeyId key:{keyIdHard1,keyIdHard4,keyIdUp,keyIdDown,keyIdJogUp,keyIdJogPress,keyIdBack})
        checkHeld(d,key,1,besideLcd);
    checkHeld(d,keyIdJogDown,1,besideButtons);
}
void n710c(const std::string& out) {
    const Device d{"n710c",DeviceArt::n710c,320,320};
    const auto& p=profileOf(d);
    const auto& silk=deviceSilkscreen(d.art);
    assert(p.width==320 && p.height==100 && p.unit==1 && p.style==DeviceControlStyle::n710c && p.count==10);
    assert(p.silkscreenHeight==144 && p.sideTabWidth==clieJogWidth);
    assert(silk.count==7 && silk.width==320 && silk.height==144 && silk.unit==1 && silk.style==SilkscreenStyle::t415);
    for(size_t i=0;i<silk.count;++i) {
        const auto& r=silk.buttons[i].bounds;
        assert(r.x>=0 && r.y>=0 && r.x+r.w<=silk.width && r.y+r.h<=silk.height);
    }
    checkViews(d);
    renders(d,out,keyIdHard3);
    // The jog sits in the upper half of the LCD's side: jog press from LCD row 78.
    for(size_t i=0;i<p.count;++i)if(p.buttons[i].key==keyIdJogPress)
        assert(deviceButtonRect(p,p.buttons[i],320,464,besideLcd).y==78);
    const int top=464,l=clieJogWidth;
    assert(keyAt(d,l+57,top+59,besideLcd)==keyIdHard1 && keyAt(d,l+263,top+59,besideLcd)==keyIdHard4);
    assert(keyAt(d,l+160,top+45,besideLcd)==keyIdUp && keyAt(d,l+160,top+72,besideLcd)==keyIdDown);
    assert(keyAt(d,l+84,top+59,besideLcd)==keyInvalid && keyAt(d,l+160,top+15,besideLcd)==keyInvalid);
    assert(keyAt(d,l+20,top+59,besideLcd)==keyInvalid && keyAt(d,l+160,top+96,besideLcd)==keyInvalid);

    // Colours at 2x: the black silkscreen with pale tiles and the light brightness mark, the
    // chrome band, the dark well, blue icons on the keys and the silver body and side.
    Image image(imageWidth(d,2,besideLcd),imageHeight(d,2,besideLcd));
    drawDevice(image,d,2,0,besideLcd);
    auto pixel=[&](float x,float y) {return image.at(int((x+l)*2),int(y*2));};
    assert(brightness(pixel(110,380))<120 && brightness(pixel(12,333))>400 && brightness(pixel(276,333))>400);
    assert(count(image,{l+11,431,10,10},2,[](SDL_Color c) {return brightness(c)>450;})>4);
    assert(brightness(pixel(160,top+12))>450 && brightness(pixel(84,top+80))<150);
    assert(count(image,{l+34,top+44,46,30},2,[](SDL_Color c) {return c.b>c.r+40;})>10);
    assert(brightness(pixel(160,top+97))>400 && brightness(image.at(8,600))>400);
    // The jack's dark hole by the top of the side.
    assert(brightness(image.at(38,32))<100);

    for(KeyId key:{keyIdHard1,keyIdHard2,keyIdHard3,keyIdHard4,keyIdUp,keyIdDown,keyIdJogUp,keyIdJogPress,keyIdBack})
        checkHeld(d,key,1,besideLcd);
    checkHeld(d,keyIdBack,1,besideButtons);
}
void nx70v(const std::string& out) {
    const Device d{"nx70v",DeviceArt::nx70v,320,480};
    const auto& p=profileOf(d);
    assert(p.width==320 && p.height==372 && p.unit==1 && p.keyboardHeight==220 && p.style==DeviceControlStyle::nx70v);
    assert(!deviceSilkscreen(d.art).count && p.sideTabWidth==clieJogWidth && p.silkscreenHeight==0);
    // Six app keys and rocker halves, 42 keyboard keys and the jog's four.
    assert(p.count==52);
    int keyboard=0;
    for(size_t i=0;i<p.count;++i)if(p.buttons[i].section==DeviceControlSection::keyboard) {
        assert(p.buttons[i].key==nx70vKey(keyboard));
        ++keyboard;
    }
    assert(keyboard==nx70vKeyCount && nx70vKey(nx70vKeyCount-1)<keyIdVoice);
    checkViews(d);
    renders(d,out,nx70vKey(13));
    DeviceControlView noKeyboard;noKeyboard.keyboard=false;
    const auto noKeyboardBelow=deviceControlView(false,JogPlacement::keyboard);
    checkKeys(d,noKeyboard);
    checkKeys(d,noKeyboardBelow);
    if(!out.empty()) {
        Image image(imageWidth(d,1,noKeyboard),imageHeight(d,1,noKeyboard));
        drawDevice(image,d,1,0,noKeyboard);
        image.save(out+"/nx70v-no-keyboard-1x.png");
    }
    // Hidden, the keyboard's rows go and its keys don't hit; with the jog set beside the keyboard it
    // goes beside the LCD.
    assert(deviceControlsHeight(p,320)==372 && deviceControlsHeight(p,320,noKeyboard)==152);
    assert(deviceScreenLeft(p,noKeyboardBelow)==clieJogWidth && deviceScreenLeft(p,besideButtons)==0);
    // Beside the keyboard the strip shrinks to the width right of the side tab.
    assert(deviceControlsHeight(p,320,besideButtons)==int(std::ceil(372*float(320-clieJogWidth)/320)));

    const int top=480,l=clieJogWidth;
    assert(keyAt(d,l+81,top+79,besideLcd)==keyIdHard1 && keyAt(d,l+239,top+79,besideLcd)==keyIdHard4);
    assert(keyAt(d,l+118,top+121,besideLcd)==keyIdHard2 && keyAt(d,l+202,top+121,besideLcd)==keyIdHard3);
    assert(keyAt(d,l+160,top+66,besideLcd)==keyIdUp && keyAt(d,l+160,top+92,besideLcd)==keyIdDown);
    assert(keyAt(d,l+120,top+79,besideLcd)==keyInvalid && keyAt(d,l+160,top+26,besideLcd)==keyInvalid);
    assert(keyAt(d,l+160,top+121,besideLcd)==keyInvalid);
    assert(keyAt(d,l+41,top+176,besideLcd)==nx70vKey(0) && keyAt(d,l+56,top+176,besideLcd)==keyInvalid);
    assert(keyAt(d,l+41,top+196,besideLcd)==keyInvalid && keyAt(d,l+250,top+290,besideLcd)==nx70vKey(35));
    assert(keyAt(d,l+41,top+176,noKeyboard)==keyInvalid);

    // Colours at 2x: the bright barrel and its dark camera, pale app keys and keycaps, dark ink on
    // Q and the blue 1 printed under it, the grey app panel.
    Image image(imageWidth(d,2,besideLcd),imageHeight(d,2,besideLcd));
    drawDevice(image,d,2,0,besideLcd);
    auto pixel=[&](float x,float y) {return image.at(int((x+l)*2),int((top+y)*2));};
    assert(brightness(pixel(160,26))>500 && brightness(pixel(36,30))<150);
    assert(brightness(pixel(70,70))>550 && brightness(pixel(33,170))>600);
    assert(count(image,{l+33,top+168,16,16},2,[](SDL_Color c) {return brightness(c)<300;})>8);
    assert(count(image,{l+42,top+189,14,12},2,[](SDL_Color c) {return c.b>c.r+50;})>4);
    const auto panel=pixel(160,140);
    assert(brightness(panel)>420 && brightness(panel)<620);
    assert(brightness(image.at(8,400))>400);

    for(KeyId key:{keyIdHard1,keyIdHard3,keyIdUp,keyIdDown,nx70vKey(0),nx70vKey(35),nx70vKey(37),keyIdJogPress,keyIdBack})
        checkHeld(d,key,1,besideLcd);
    for(KeyId key:{nx70vKey(5),keyIdJogUp,keyIdHard4})checkHeld(d,key,1,besideButtons);
    checkHeld(d,keyIdDown,1,noKeyboard);
}
}  // namespace

int main(int argc, char** argv) {
    const std::string out=argc>1 ? argv[1] : "";
    t615c(out);
    n710c(out);
    nx70v(out);
    std::puts("sony candidate art ok");
    return 0;
}
