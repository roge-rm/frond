#include "DanaView.h"
#include <cassert>
#include <iostream>
#include <cmath>

static DanaScan5 scan(const char* name) {return *DanaScan5::Named(name);}

static void parsing() {
    DanaViewState state;std::string error;
    assert(state.Parse("keyboard=compact zoom=1.5 pan-x=420 pan-y=120 pointer=pan density=3 physical=off",error));
    const auto before=state.Describe("compact");
    for(const char* bad:{"zoom=nan","pan-x=inf","pan-y=-1","pan-y=161","pan-x=561","zoom=1e309",
        "density=0","density=100","zoom=1 zoom=2","physical=maybe","page=fn","zoom=2 keyboard=bad","keyboard=full mystery=2",""}) {
        assert(!state.Parse(bad,error));assert(state.Describe("compact")==before);
    }
    assert(state.Parse("zoom=fit physical=on",error));assert(state.zoom==0&&state.physical);
    std::cout<<"atomic parser and finite bounds pass\n";
}
static void layouts() {
    DanaViewState s;auto phone=DanaViewLayout::Make(1170,2280,s);
    s.density=3;phone=DanaViewLayout::Make(1170,2280,s);
    assert(phone.resolved=="compact"&&phone.keyboard.h==756&&phone.keyboard.y==1524);
    assert(phone.screen.w==3360); // Keyboard width never determines LCD zoom.
    const auto original=phone.screen.w;
    s.keyboard="hidden";auto hidden=DanaViewLayout::Make(1170,2280,s);
    assert(hidden.keyboard.h==0&&hidden.screen.w==original);
    s.keyboard="auto";s.physical=true;
    assert(DanaViewLayout::Make(2560,1600,s).resolved=="hidden");
    s.physical=false;s.density=2;
    assert(DanaViewLayout::Make(1600,1200,s).resolved=="full");
    assert(DanaViewLayout::Make(1000,1600,s).resolved=="compact");
    s.keyboard="compact";s.panX=500;s.panY=130;
    const auto saved=s.Describe("compact");
    auto portrait=DanaViewLayout::Make(780,1520,s);
    auto landscape=DanaViewLayout::Make(1520,780,s);
    hidden=DanaViewLayout::Make(2560,1600,s);
    assert(s.Describe("compact")==saved);
    assert(portrait.panX<500&&landscape.panX<500&&hidden.panX<500);
    assert(portrait.screen.w==2240&&landscape.screen.w==2240&&hidden.screen.w==2240);
    s.keyboard="hidden";s.zoom=0;
    auto fit=DanaViewLayout::Make(390,700,s);
    assert(std::abs(fit.screen.w-390)<.01&&std::abs(fit.screen.h-390*160./560)<.01);
    int x=-1,y=-1;assert(!fit.GuestPoint(20,0,x,y));assert(!fit.GuestPoint(390,400,x,y));
    for(auto size:{std::pair<int,int>{390,700},{844,346},{1000,1600},{2400,1600}}) {
        for(double zoom:{1.,1.5,2.,3.}) {
            s.zoom=zoom;s.panX=157;s.panY=74;s.keyboard="compact";
            auto l=DanaViewLayout::Make(size.first,size.second,s);
            for(int py=0;py<l.viewport.h;py+=7)for(int px=0;px<l.viewport.w;px+=7) {
                int gx=0,gy=0;
                if(!l.GuestPoint(px+.5,py+.5,gx,gy))continue;
                const double wx=l.screen.x+(gx+.5)*l.scale,wy=l.screen.y+(gy+.5)*l.scale;
                assert(std::abs(wx-(px+.5))<=l.scale*.5+.001);
                assert(std::abs(wy-(py+.5))<=l.scale*.5+.001);
                assert(gx>=0&&gx<560&&gy>=0&&gy<160);
            }
            assert(!l.GuestPoint(0,l.viewport.h,x,y));
        }
    }
    std::cout<<"density, independent LCD, automatic layouts, crop inverse and desired view preservation pass\n";
}
static void ownership() {
    DanaInput5 input;DanaScreenKeys keys;
    std::array<bool,128> hardware{};bool accept=true;int transitions=0;
    auto send=[&](const std::vector<DanaTransition5>& batch) {
        if(!accept)return false;
        for(const auto& change:batch){hardware[change.scan.Code()]=change.down;++transitions;}return true;
    };
    auto apply=[&](const auto& batch){return input.Keys(DanaSource5::screen,batch,send);};
    assert(keys.Down(1,100,{scan("a")},apply));
    assert(keys.Down(1,101,{scan("a")},apply));assert(transitions==1);
    keys.Up(1,100,apply);assert(hardware[scan("a").Code()]&&transitions==1);
    keys.Up(1,101,apply);assert(!hardware[scan("a").Code()]&&transitions==2);
    assert(input.Key(DanaSource5::physical,scan("lshift"),true,send));
    assert(keys.Down(1,1,{scan("lshift"),scan("2")},apply));keys.Cancel(apply);
    assert(hardware[scan("lshift").Code()]&&!hardware[scan("2").Code()]);
    assert(input.Key(DanaSource5::physical,scan("lshift"),false,send));
    accept=false;assert(!keys.Down(1,4,{scan("a"),scan("b")},apply));
    assert(!keys.Owns(1,4)&&!hardware[scan("a").Code()]&&!hardware[scan("b").Code()]);
    accept=true;assert(keys.Down(1,5,{scan("a")},apply));
    accept=false;keys.Up(1,5,apply);assert(hardware[scan("a").Code()]);
    assert(!keys.Down(1,6,{scan("b")},apply));assert(!keys.Owns(1,6));
    accept=true;assert(keys.Flush(apply));assert(!hardware[scan("a").Code()]);
    assert(keys.Modifier(scan("lshift"),apply));
    assert(keys.Down(1,7,{scan("a")},apply));keys.Up(1,7,apply);
    assert(!hardware[scan("lshift").Code()]);
    assert(keys.Modifier(scan("ctrl"),apply));assert(keys.Modifier(scan("ctrl"),apply));
    assert(keys.Down(1,8,{scan("a")},apply));keys.Up(1,8,apply);
    assert(hardware[scan("ctrl").Code()]);keys.Cancel(apply);assert(!hardware[scan("ctrl").Code()]);
    assert(keys.Down(1,9,{scan("function"),scan("up")},apply));
    accept=false;keys.Cancel(apply);assert(hardware[scan("function").Code()]);
    accept=true;assert(keys.Flush(apply));
    for(bool key:hardware)assert(!key);
    // The ST accepts eight scans. A ninth pointer must not partly own its chord.
    auto bounded=[&](const std::vector<DanaTransition5>& batch) {
        auto next=hardware;
        for(const auto& change:batch)next[change.scan.Code()]=change.down;
        if(std::count(next.begin(),next.end(),true)>8)return false;
        return send(batch);
    };
    auto boundedApply=[&](const auto& batch){return input.Keys(DanaSource5::screen,batch,bounded);};
    const char* letters[]={"a","b","c","d","e","f","g","h"};
    for(int i=0;i<8;++i)assert(keys.Down(2,i,{scan(letters[i])},boundedApply));
    assert(!keys.Down(2,8,{scan("lshift"),scan("i")},boundedApply));
    assert(!keys.Owns(2,8)&&!input.Held(scan("lshift"))&&!input.Held(scan("i")));
    keys.Up(2,0,boundedApply);keys.Up(2,1,boundedApply);
    assert(keys.Down(2,8,{scan("lshift"),scan("i")},boundedApply));keys.Cancel(boundedApply);
    for(bool key:hardware)assert(!key);
    std::cout<<"multi-pointer ownership, physical modifiers, atomic refusal, retained release, one-shot/sticky and cancellation pass\n";
}
static void compact() {
    for(bool portrait:{false,true})for(const char* page:{"abc","symbols","more","fn"}) {
        auto keys=danaCompactKeys(page,portrait);assert(!keys.empty());
        bool hasPage=false,hasFn=false;
        for(const auto& key:keys) {
            assert(key.art.bounds.x>=0&&key.art.bounds.y>=0&&key.art.bounds.w>0&&key.art.bounds.h>0);
            assert(key.art.bounds.x+key.art.bounds.w<=(portrait?390:510));
            assert(key.art.bounds.y+key.art.bounds.h<=(portrait?252:180));
            assert(!key.action.empty()||!key.chord.empty());
            hasPage|=key.action=="abc"||key.action=="symbols";
            hasFn|=key.action=="fn";
            if(key.chord.size()==2) {
                assert(key.chord.front().Code()==scan(page==std::string("fn")?"function":"lshift").Code());
            }
        }
        assert(hasPage&&hasFn);
    }
    auto fn=danaCompactKeys("fn",true);
    for(const char* name:{"calendar","address","todo","memo","find","menu","apps","send","f1","f8","power","caps","delete"}) {
        bool found=false;for(const auto& key:fn)found|=key.art.name==std::string(name);assert(found);
    }
    assert(SDL_Init(SDL_INIT_VIDEO)==0);
    auto* surface=SDL_CreateRGBSurfaceWithFormat(0,780,504,32,SDL_PIXELFORMAT_RGBA32);assert(surface);
    auto* renderer=SDL_CreateSoftwareRenderer(surface);assert(renderer);
    auto keys=danaCompactKeys("abc",true);std::vector<DanaKeyArt> caps;std::vector<bool> held;
    for(auto& key:keys){caps.push_back(key.art);held.push_back(false);}
    SDL_Rect dest{0,0,780,504};drawDanaKeyCaps(renderer,dest,390,252,caps.data(),caps.size(),held);
    SDL_RenderPresent(renderer);const auto before=std::vector<uint8_t>(static_cast<uint8_t*>(surface->pixels),static_cast<uint8_t*>(surface->pixels)+surface->h*surface->pitch);
    held[10]=true;drawDanaKeyCaps(renderer,dest,390,252,caps.data(),caps.size(),held);SDL_RenderPresent(renderer);
    assert(!std::equal(before.begin(),before.end(),static_cast<uint8_t*>(surface->pixels)));
    SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);SDL_Quit();
    std::cout<<"all compact pages, typed chords, Dana extras and headless SDL pressed rendering pass\n";
}
int main() {parsing();layouts();ownership();compact();}
