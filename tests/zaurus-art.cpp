// Sharp Zaurus SL-C3000 art: keys against the Spitz keyboard matrix, geometry, hit testing,
// held-key drawing and both layouts. With an output folder it also writes preview PNGs.
#include "DeviceControls.h"
#include <SDL_image.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>

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

// Blank LCD for the previews: a pale panel with a grid, so it isn't taken for a running guest.
void blankLcd(Image& image, const SDL_Rect& r) {
    image.fill(r,206,214,222);
    for(int i=1;i<8;++i) {
        image.fill({r.x+r.w*i/8,r.y,1,r.h},180,190,200);
        image.fill({r.x,r.y+r.h*i/8,r.w,1},180,190,200);
    }
}

// The Spitz matrix (row, column) of every key the Linux spitz keymap lists.
const std::pair<int,int> spitzMatrix[]={
    {0,0},{0,1},{0,2},{0,3},{0,4},{0,5},{0,6},{0,7},{0,8},{0,9},{0,10},
    {1,1},{1,2},{1,3},{1,4},{1,5},{1,6},{1,7},{1,8},{1,9},{1,10},
    {2,0},{2,1},{2,2},{2,3},{2,4},{2,5},{2,6},{2,7},
    {3,0},{3,1},{3,2},{3,3},{3,4},{3,5},{3,6},{3,7},{3,9},
    {4,0},{4,1},{4,2},{4,3},{4,4},{4,5},{4,6},{4,8},{4,9},
    {5,0},{5,1},{5,2},{5,3},{5,4},{5,5},{5,7},{5,10},
    {6,0},{6,1},{6,2},{6,3},{6,4},{6,5},{6,6},{6,7},{6,8},
};

SDL_Rect partRect(const ZaurusLayout& l, ZaurusPart part) {
    return part==ZaurusPart::keyboard ? l.keyboard : part==ZaurusPart::icons ? l.icons : l.side;
}

void draw(Image& image, const ZaurusLayout& l, const ZaurusKeysHeld& held) {
    blankLcd(image,l.screen);
    drawZaurusControls(image.renderer,l,held);
}
}  // namespace

int main(int argc, char** argv) {
    const std::string out=argc>1 ? argv[1] : "";
    const auto& art=zaurusArt();

    // One key per matrix position, every matrix key drawn; names unique; the five hard icons
    // are off the matrix.
    std::set<std::string> names;
    std::set<std::pair<int,int>> matrix;
    int icons=0,side=0;
    for(size_t i=0;i<art.count;++i) {
        const auto& k=art.keys[i];
        assert(names.insert(k.name).second);
        assert(zaurusKeyIndex(k.name)==int(i));
        if(k.part==ZaurusPart::icons) {++icons;assert(k.row<0 && k.column<0);continue;}
        if(k.part==ZaurusPart::side)++side;
        assert(matrix.insert({k.row,k.column}).second);
    }
    assert(icons==5 && side==4);
    assert(matrix.size()==std::size(spitzMatrix));
    for(const auto& m : spitzMatrix)assert(matrix.count(m));
    assert(art.count<=128);

    // Keys sit inside their part without overlapping another key of it.
    for(size_t i=0;i<art.count;++i) {
        const auto& a=art.keys[i];
        const auto& r=a.bounds;
        const int w=a.part==ZaurusPart::keyboard ? zaurusKeyboardWidth : a.part==ZaurusPart::icons ? zaurusIconsLength : zaurusSideWidth;
        const int h=a.part==ZaurusPart::keyboard ? zaurusKeyboardHeight : a.part==ZaurusPart::icons ? zaurusIconsDepth : 704;
        assert(r.x>=0 && r.y>=0 && r.x+r.w<=w && r.y+r.h<=h && r.w>=20 && r.h>=20);
        for(size_t j=i+1;j<art.count;++j)
            if(art.keys[j].part==a.part)assert(!SDL_HasIntersection(&r,&art.keys[j].bounds));
    }

    // Layouts: centred, nothing outside the view, the right parts shown, every shown key hit at its centre.
    for(auto view : {SDL_Point{1080,2400},SDL_Point{2400,1080},SDL_Point{1920,1080},SDL_Point{800,600}}) {
        for(auto style : {ZaurusStyle::clamshell,ZaurusStyle::view}) {
            const auto l=zaurusLayout(view.x,view.y,style);
            const bool clam=style==ZaurusStyle::clamshell;
            assert(l.screen.w>0 && l.icons.w>0);
            assert(std::abs(l.screen.w*(clam ? 480 : 640)-l.screen.h*(clam ? 640 : 480))<=640);
            assert(clam ? l.keyboard.w>0 && l.side.w==0 : l.keyboard.w==0 && l.side.w>0);
            SDL_Rect all=l.screen;
            for(const auto& r : {l.icons,l.keyboard,l.side})if(r.w>0)SDL_UnionRect(&all,&r,&all);
            assert(all.x>=0 && all.y>=0 && all.x+all.w<=view.x && all.y+all.h<=view.y);
            assert(std::abs(2*all.x+all.w-view.x)<=1 && std::abs(2*all.y+all.h-view.y)<=1);
            assert(all.w>=view.x-1 || all.h>=view.y-1);
            if(clam) {
                assert(l.icons.x==l.screen.x+l.screen.w && l.icons.h==l.screen.h);
                assert(l.keyboard.y==l.screen.y+l.screen.h && l.keyboard.w==l.screen.w+l.icons.w);
            } else {
                assert(l.side.x+l.side.w==l.screen.x && l.side.h==l.screen.h+l.icons.h);
                assert(l.icons.y==l.screen.y+l.screen.h && l.icons.w==l.screen.w);
            }
            for(size_t i=0;i<art.count;++i) {
                const auto r=zaurusKeyRect(l,art.keys[i]);
                const bool shown=partRect(l,art.keys[i].part).w>0;
                assert(shown==(r.w>0));
                if(!shown)continue;
                SDL_Rect part=partRect(l,art.keys[i].part);
                assert(SDL_HasIntersection(&r,&part));
                assert(zaurusKeyAt(l,r.x+r.w/2,r.y+r.h/2)==int(i));
            }
            // The LCD hits no key.
            assert(zaurusKeyAt(l,l.screen.x+l.screen.w/2,l.screen.y+l.screen.h/2)<0);
        }
    }
    // Clamshell turns the icon strip: the dictionary at the bottom, Home at the top.
    {
        const auto l=zaurusLayout(1920,1080,ZaurusStyle::clamshell);
        const auto dict=zaurusKeyRect(l,art.keys[zaurusKeyIndex("icon-dictionary")]);
        const auto home=zaurusKeyRect(l,art.keys[zaurusKeyIndex("icon-home")]);
        assert(dict.y>home.y && dict.h>dict.w);
        const auto v=zaurusLayout(1080,1920,ZaurusStyle::view);
        assert(zaurusKeyRect(v,art.keys[zaurusKeyIndex("icon-dictionary")]).x<
               zaurusKeyRect(v,art.keys[zaurusKeyIndex("icon-home")]).x);
    }

    // Holding a key changes the picture near that key only.
    for(auto style : {ZaurusStyle::clamshell,ZaurusStyle::view}) {
        const auto l=zaurusLayout(1400,1400,style);
        Image released(1400,1400);
        draw(released,l,ZaurusKeysHeld{});
        for(size_t i=0;i<art.count;++i) {
            const auto r=zaurusKeyRect(l,art.keys[i]);
            if(r.w<=0)continue;
            ZaurusKeysHeld one;assert(one.Set(art.keys[i].name,true));
            Image pressed(1400,1400);
            draw(pressed,l,one);
            // Legends and chevrons printed beside a key may change with it.
            const int m=2*std::max(r.w,r.h);
            const SDL_Rect near{r.x-m,r.y-m,r.w+2*m,r.h+2*m};
            int changed=0,stray=0;
            for(int y=0;y<1400;++y)for(int x=0;x<1400;++x) {
                if(std::memcmp(released.pixel(x,y),pressed.pixel(x,y),3)==0)continue;
                const SDL_Point p{x,y};
                if(SDL_PointInRect(&p,&r))++changed;
                else if(!SDL_PointInRect(&p,&near))++stray;
            }
            if(stray || changed<8) {
                std::fprintf(stderr,"%s: %d changed, %d stray\n",art.keys[i].name,changed,stray);
                return 1;
            }
        }
    }
    std::printf("zaurus art: %zu keys, matrix, geometry, layouts and held keys pass\n",art.count);
    if(out.empty())return 0;

    // Previews: both styles at a phone's and a desktop's size, released and with keys held.
    ZaurusKeysHeld chord;
    for(const char* name : {"lshift","q","enter","space","up","ok","calendar","fn","icon-mail","exjogdown","exok"})
        chord.Set(name,true);
    struct Preview { const char* name; int w, h; ZaurusStyle style; bool held; };
    for(const auto& p : {Preview{"landscape-keyboard",1600,1300,ZaurusStyle::clamshell,false},
                         Preview{"landscape-keyboard-held",1600,1300,ZaurusStyle::clamshell,true},
                         Preview{"landscape-keyboard-phone",2400,1080,ZaurusStyle::clamshell,false},
                         Preview{"portrait-viewer",1000,1400,ZaurusStyle::view,false},
                         Preview{"portrait-viewer-held",1000,1400,ZaurusStyle::view,true},
                         Preview{"portrait-viewer-phone",1080,2400,ZaurusStyle::view,false}}) {
        Image image(p.w,p.h);
        draw(image,zaurusLayout(p.w,p.h,p.style),p.held ? chord : ZaurusKeysHeld{});
        image.save(out+"/"+p.name+".png");
    }
    {
        // The keyboard alone at 3x its units.
        const int w=1800,h=w*zaurusKeyboardHeight/zaurusKeyboardWidth;
        Image image(w,h);
        ZaurusLayout l{ZaurusStyle::clamshell,{0,0,0,0},{0,0,0,0},{0,0,w,h},{0,0,0,0}};
        drawZaurusControls(image.renderer,l,ZaurusKeysHeld{});
        image.save(out+"/keyboard-3x.png");
    }
    std::printf("previews written to %s\n",out.c_str());
    return 0;
}
