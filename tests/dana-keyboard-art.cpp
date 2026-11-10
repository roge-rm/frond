// Dana keyboard art: geometry against the physical key inventory, held-key rendering and the
// LCD-plus-keyboard layout. With an output folder it also writes preview PNGs.
#include "DeviceControls.h"
#include "dana_keyboard5.h"
#include <SDL_image.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

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
    const Uint8* pixel(int x, int y) const {
        return static_cast<const Uint8*>(surface->pixels)+y*surface->pitch+x*4;
    }
    void save(const std::string& path) {
        SDL_RenderPresent(renderer);
        if(IMG_SavePNG(surface,path.c_str())!=0) {std::fprintf(stderr,"%s: %s\n",path.c_str(),IMG_GetError());std::exit(1);}
    }
};

// 3x5 capitals for the preview captions.
const char* captionGlyph(char c) {
    switch(c) {
        case 'A': return "010101111101101"; case 'B': return "110101110101110"; case 'C': return "011100100100011";
        case 'D': return "110101101101110"; case 'E': return "111100110100111"; case 'F': return "111100110100100";
        case 'G': return "011100101101011"; case 'H': return "101101111101101"; case 'I': return "111010010010111";
        case 'K': return "101101110101101"; case 'L': return "100100100100111"; case 'M': return "101111111101101";
        case 'N': return "110101101101101"; case 'O': return "010101101101010"; case 'P': return "110101110100100";
        case 'R': return "110101110101101"; case 'S': return "011100010001110"; case 'T': return "111010010010010";
        case 'U': return "101101101101111"; case 'W': return "101101101111010"; case 'X': return "101101010101101";
        case 'Y': return "101101010010010"; case '0': return "111101101101111"; case '1': return "010110010010111";
        case '2': return "110001010100111"; case '3': return "110001010001110"; case '5': return "111100110001110";
        case '6': return "011100111101111"; case '.': return "000000000000010"; case '/': return "001001010100100";
        default: return "000000000000000";
    }
}
void caption(Image& image, const char* text, int x, int y, int size, Uint8 grey) {
    for(const char* c=text;*c;++c,x+=4*size) {
        const char* g=captionGlyph(*c);
        for(int i=0;i<15;++i) if(g[i]=='1') image.fill({x+(i%3)*size,y+(i/3)*size,size,size},grey,grey,grey);
    }
}

// Blank LCD for the layout studies, labelled so it is not taken for a running guest.
void blankLcd(Image& image, const SDL_Rect& r) {
    image.fill(r,196,204,180);
    const int size=std::max(2,r.h/40);
    caption(image,"LCD 560X160 BLANK LAYOUT STUDY",r.x+r.h/10,r.y+r.h/10,size,120);
}

int differing(const Image& a, const Image& b, const SDL_Rect& inside, const SDL_Rect& outside, int& stray) {
    int changed=0;stray=0;
    for(int y=0;y<a.surface->h;++y)for(int x=0;x<a.surface->w;++x) {
        if(std::memcmp(a.pixel(x,y),b.pixel(x,y),3)==0)continue;
        const SDL_Point p{x,y};
        if(SDL_PointInRect(&p,&inside))++changed;
        else if(!SDL_PointInRect(&p,&outside))++stray;
    }
    return changed;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string out=argc>1 ? argv[1] : "";
    const auto& art=danaKeyboardArt();

    // Every physical key has exactly one cap, and nothing else does.
    assert(art.count==std::size(danaKeys5));
    std::set<std::string> names;
    for(size_t i=0;i<art.count;++i) {
        const auto& key=art.keys[i];
        assert(names.insert(key.name).second);
        assert(danaScanNamed5(key.name)!=0);
        assert(danaKeyArtIndex(key.name)==int(i));
    }
    assert(danaKeyArtIndex("iso")<0);
    // Caps sit in the well, inside the art, without overlapping.
    for(size_t i=0;i<art.count;++i) {
        const auto& r=art.keys[i].bounds;
        assert(r.x>=0 && r.y>=0 && r.x+r.w<=art.width && r.y+r.h<=art.height && r.w>=34 && r.h>=34);
        auto inWell=[&](int x,int y) {
            const SDL_Point p{x,y};
            return SDL_PointInRect(&p,&art.well) || SDL_PointInRect(&p,&art.arrowWell);
        };
        assert(inWell(r.x,r.y) && inWell(r.x+r.w-1,r.y) && inWell(r.x,r.y+r.h-1) && inWell(r.x+r.w-1,r.y+r.h-1));
        for(size_t j=i+1;j<art.count;++j)assert(!SDL_HasIntersection(&r,&art.keys[j].bounds));
        assert(danaKeyAt(r.x+r.w*0.5f,r.y+r.h*0.5f)==int(i));
        // Every key but Space has something printed on it.
        assert(std::strcmp(art.keys[i].name,"space")==0 || *art.keys[i].legend || *art.keys[i].second ||
               art.keys[i].mark!=DanaKeyMark::none);
    }
    // Wide keys are wider than letters; the top row is a single row of 17.
    auto width=[&](const char* name) {return art.keys[danaKeyArtIndex(name)].bounds.w;};
    assert(width("space")>4*width("a") && width("enter")>width("a") && width("backspace")>width("a"));
    assert(width("lshift")==width("rshift"));
    int top=0;
    for(size_t i=0;i<art.count;++i)top+=art.keys[i].topRow;
    assert(top==17);
    // Body and the space under the board's left side hit nothing.
    assert(danaKeyAt(4,4)<0 && danaKeyAt(200,270)<0 && danaKeyAt(art.width-2,art.height-2)<0);

    // Any number of held keys.
    DanaKeysHeld held;
    for(size_t i=0;i<art.count;++i)assert(held.Set(art.keys[i].name,true));
    assert(held.Count()==art.count && art.count>64);
    assert(!held.Set("iso",true) && held.Count()==art.count);
    held.Clear();
    assert(held.Set("lshift",true) && held.Held(danaKeyArtIndex("lshift")) && held.Count()==1);
    assert(held.Set("lshift",false) && held.Count()==0);

    // Holding a key changes its own cap only, and every cap has bright legend ink.
    const int W=1120,H=danaKeyboardHeight(W);
    const float k=float(W)/art.width;
    Image released(W,H);
    drawDanaKeyboard(released.renderer,{0,0,W,H},DanaKeysHeld{});
    for(size_t i=0;i<art.count;++i) {
        const auto& r=art.keys[i].bounds;
        const SDL_Rect cell{int(r.x*k),int(r.y*k),int(std::ceil(r.w*k))+1,int(std::ceil(r.h*k))+1};
        const SDL_Rect near{cell.x-2,cell.y-2,cell.w+4,cell.h+4};
        Image pressed(W,H);
        DanaKeysHeld one;one.Set(art.keys[i].name,true);
        drawDanaKeyboard(pressed.renderer,{0,0,W,H},one);
        int stray=0;
        const int changed=differing(released,pressed,cell,near,stray);
        assert(stray==0 && changed>cell.w*cell.h/3);
        int ink=0;
        for(int y=cell.y;y<cell.y+cell.h;++y)for(int x=cell.x;x<cell.x+cell.w;++x) {
            const Uint8* p=released.pixel(x,y);
            ink+=p[0]>200 && p[1]>170;
        }
        if(std::strcmp(art.keys[i].name,"space")!=0 && ink<12) {
            std::fprintf(stderr,"%s: no legend ink\n",art.keys[i].name);return 1;
        }
    }

    // One column, LCD over keyboard, centred in portrait and landscape; hidden keyboard too.
    for(auto view : {SDL_Point{1080,2400},SDL_Point{2400,1080},SDL_Point{1920,1080},SDL_Point{720,1280}}) {
        for(bool keyboard : {true,false}) {
            const auto l=danaLayout(view.x,view.y,keyboard);
            assert(l.screen.w>0 && l.screen.x>=0 && l.screen.y>=0);
            assert(std::abs(l.screen.h*560-l.screen.w*160)<=560);
            assert(l.keyboard.y==l.screen.y+l.screen.h);
            assert(l.keyboard.y+l.keyboard.h<=view.y && l.screen.x+l.screen.w<=view.x);
            assert(std::abs(2*l.screen.x+l.screen.w-view.x)<=1);
            assert(std::abs(2*l.screen.y+l.screen.h+l.keyboard.h-view.y)<=1);
            assert(keyboard ? l.keyboard.w==l.screen.w && l.keyboard.h==danaKeyboardHeight(l.screen.w) : l.keyboard.h==0);
            assert(l.screen.w==view.x || l.keyboard.y+l.keyboard.h>=view.y-1);
        }
    }
    std::printf("dana keyboard art: %zu keys, geometry, held caps, legends and layouts pass\n",art.count);
    if(out.empty())return 0;

    // Previews.
    for(int scale : {1,2,3}) {
        Image image(560*scale,danaKeyboardHeight(560*scale));
        drawDanaKeyboard(image.renderer,{0,0,560*scale,danaKeyboardHeight(560*scale)},DanaKeysHeld{});
        image.save(out+"/keyboard-released-"+std::to_string(scale)+"x.png");
    }
    DanaKeysHeld chord;
    for(const char* name : {"lshift","command","q","enter","space","up","find","apps","backspace","f3","function","right"})
        chord.Set(name,true);
    {
        Image image(1120,danaKeyboardHeight(1120));
        drawDanaKeyboard(image.renderer,{0,0,1120,danaKeyboardHeight(1120)},chord);
        image.save(out+"/keyboard-pressed-2x.png");
    }
    {
        // Released over pressed, at 3x, for the cap and legend changes side by side.
        const int w=1680,h=danaKeyboardHeight(w);
        Image image(w,h*2+12);
        drawDanaKeyboard(image.renderer,{0,0,w,h},DanaKeysHeld{});
        drawDanaKeyboard(image.renderer,{0,h+12,w,h},chord);
        image.save(out+"/keyboard-released-over-pressed-3x.png");
    }
    struct Study { const char* name; int w, h; bool keyboard; };
    for(const auto& s : {Study{"portrait",1080,2400,true},Study{"portrait-hidden",1080,2400,false},
                         Study{"landscape",2400,1080,true},Study{"landscape-hidden",2400,1080,false}}) {
        Image image(s.w,s.h);
        const auto l=danaLayout(s.w,s.h,s.keyboard);
        blankLcd(image,l.screen);
        if(s.keyboard)drawDanaKeyboard(image.renderer,l.keyboard,DanaKeysHeld{});
        caption(image,s.keyboard ? "KEYBOARD SHOWN" : "KEYBOARD HIDDEN",24,24,6,150);
        image.save(out+"/layout-"+s.name+".png");
    }
    {
        // Contact sheet at 2x: the TG50 and IIIx strips, drawn at the left edge, beside the
        // Dana keyboard with the chord held.
        const auto& tg50=deviceControls(DeviceArt::tg50);
        const auto& iiix=deviceControls(DeviceArt::iiix);
        const int tgH=deviceControlsHeight(tg50,320)*2,ixH=deviceControlsHeight(iiix,160)*2;
        const int danaW=1120,danaH=danaKeyboardHeight(danaW);
        Image image(640+80+danaW+40,std::max(tgH+ixH+120,danaH+80));
        caption(image,"TG50",8,12,4,170);
        drawDeviceControls(image.renderer,tg50,320,20,2,deviceKeyBit(tg50Key(4,1)));
        caption(image,"IIIX",8,tgH+72,4,170);
        drawDeviceControls(image.renderer,iiix,160,(tgH+100)/2,2,deviceKeyBit(keyIdHard2));
        caption(image,"DANA",720,20,4,170);
        drawDanaKeyboard(image.renderer,{720,60,danaW,danaH},chord);
        image.save(out+"/contact-sheet.png");
    }
    std::printf("previews written to %s\n",out.c_str());
    return 0;
}
