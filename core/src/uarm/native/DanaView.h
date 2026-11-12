#ifndef FROND_DANA_VIEW_H
#define FROND_DANA_VIEW_H
#include "DeviceControls.h"
#include "dana_input5.h"
#include <cerrno>
#include <map>
#include <sstream>
#include <set>
#include <limits>

// Desired offsets stay in guest pixels; the layout clamps only its rendered copy.
struct DanaViewState {
    std::string keyboard{"auto"}, pointer{"touch"}, page{"abc"};
    double zoom{2}, panX{0}, panY{0}, density{1};
    bool physical{false};
    bool Parse(const std::string& argument, std::string& error) {
        DanaViewState next=*this;
        std::istringstream words(argument);
        std::string word;
        std::set<std::string> seen;
        while(words>>word) {
            const auto eq=word.find('=');
            const auto key=word.substr(0,eq),value=eq==std::string::npos?"":word.substr(eq+1);
            bool ok=!value.empty() && seen.insert(key).second;
            if(key=="keyboard") { ok=ok&&(value=="auto"||value=="compact"||value=="full"||value=="hidden"); next.keyboard=value; }
            else if(key=="pointer") { ok=ok&&(value=="touch"||value=="pan"); next.pointer=value; }
            else if(key=="page") { ok=ok&&(value=="abc"||value=="symbols"||value=="more"||value=="fn"); next.page=value; }
            else if(key=="physical") { ok=ok&&(value=="on"||value=="off"); next.physical=value=="on"; }
            else if(key=="zoom" && value=="fit") next.zoom=0;
            else if(key=="zoom"||key=="pan-x"||key=="pan-y"||key=="density") {
                char* end=nullptr; errno=0;
                const double number=std::strtod(value.c_str(),&end);
                ok=ok && end==value.c_str()+value.size() && errno!=ERANGE && std::isfinite(number);
                if(key=="zoom") { ok=ok&&(number==1||number==1.5||number==2||number==3); next.zoom=number; }
                if(key=="pan-x") { ok=ok&&number>=0&&number<=560; next.panX=number; }
                if(key=="pan-y") { ok=ok&&number>=0&&number<=160; next.panY=number; }
                if(key=="density") { ok=ok&&number>=0.25&&number<=16; next.density=number; }
            } else ok=false;
            if(!ok) { error="Invalid Dana view setting: "+word; return false; }
        }
        if(seen.empty()) { error="Use keyboard=, zoom=, pan-x=, pan-y=, pointer=, density=, physical= or page=."; return false; }
        *this=next; return true;
    }
    std::string Describe(const std::string& resolved) const {
        std::ostringstream out;
        out.precision(12);
        out<<"dana-view: keyboard="<<keyboard<<" resolved="<<resolved<<" zoom=";
        if(zoom==0)out<<"fit";else out<<zoom;
        out<<" pan-x="<<panX<<" pan-y="<<panY<<" pointer="<<pointer
           <<" density="<<density<<" physical="<<(physical?"on":"off")<<" page="<<page;
        return out.str();
    }
};

struct DanaViewLayout {
    SDL_Rect viewport{},keyboard{};
    SDL_FRect screen{};
    std::string resolved{"hidden"};
    double scale{1}, panX{0}, panY{0};
    bool portrait{true};
    static DanaViewLayout Make(int width,int height,const DanaViewState& state) {
        DanaViewLayout out;
        width=std::max(0,width);height=std::max(0,height);
        const double d=state.density;
        out.portrait=height>=width;
        out.resolved=state.keyboard;
        if(out.resolved=="auto") {
            if(state.physical)out.resolved="hidden";
            else out.resolved=width/d>=720 && height/d>=510?"full":"compact";
        }
        int keyboardWidth=width,keyboardHeight=0;
        if(out.resolved=="full") {
            keyboardWidth=std::min(width,int(std::max(0.,height-80*d)*600/292));
            keyboardHeight=danaKeyboardHeight(keyboardWidth);
        } else if(out.resolved=="compact") {
            keyboardWidth=std::min(width,int(780*d));
            keyboardHeight=int(std::lround((out.portrait?252:180)*d));
            keyboardHeight=std::min(keyboardHeight,std::max(0,height-int(40*d)));
        }
        out.keyboard={(width-keyboardWidth)/2,height-keyboardHeight,keyboardWidth,keyboardHeight};
        out.viewport={0,0,width,std::max(0,height-keyboardHeight)};
        out.scale=state.zoom==0 ? std::min(width/560.,out.viewport.h/160.) : state.zoom*d;
        out.scale=std::max(0.001,out.scale);
        out.panX=std::clamp(state.panX,0.,std::max(0.,560-width/out.scale));
        out.panY=std::clamp(state.panY,0.,std::max(0.,160-out.viewport.h/out.scale));
        out.screen={float(std::max(0.,(width-560*out.scale)/2)-out.panX*out.scale),
                    float(std::max(0.,(out.viewport.h-160*out.scale)/2)-out.panY*out.scale),
                    float(560*out.scale),float(160*out.scale)};
        return out;
    }
    bool GuestPoint(double x,double y,int& gx,int& gy) const {
        if(x<viewport.x||y<viewport.y||x>=viewport.x+viewport.w||y>=viewport.y+viewport.h)return false;
        const double px=(x-screen.x)*560/screen.w,py=(y-screen.y)*160/screen.h;
        if(px<0||py<0||px>=560||py>=160)return false;
        gx=int(std::floor(px));gy=int(std::floor(py));return true;
    }
};

struct DanaCompactKey {
    DanaKeyArt art;
    std::string action;
    std::vector<DanaScan5> chord;
};

// Approved compact rows, using the full keyboard's cap and legend primitives.
inline std::vector<DanaCompactKey> danaCompactKeys(const std::string& page,bool portrait) {
    std::vector<DanaCompactKey> keys;
    const int rowH=portrait?52:40,keyH=portrait?44:34,strip=portrait?40:12;
    auto add=[&](const char* name,const char* label,int x,int y,int w,int h,
                 const char* action="",const char* shifted="") {
        DanaKeyArt art{name,{x,y,w,h},label,"","",DanaKeyMark::none,false};
        const int index=danaKeyArtIndex(name);
        if(index>=0) { art=danaKeyboardArt().keys[index]; art.bounds={x,y,w,h}; }
        if(*label) { art.legend=label; art.upper=""; art.second=""; art.mark=DanaKeyMark::none; }
        DanaCompactKey key{art,action,{}};
        if(*shifted)key.chord.push_back(*DanaScan5::Named("lshift"));
        if(auto scan=DanaScan5::Named(name))key.chord.push_back(*scan);
        keys.push_back(std::move(key));
    };
    {
        const char* names[]={"tab","escape","ctrl","function","command","left","up","down","right"};
        for(int i=0;i<9;++i)add(names[i],"",portrait?i*43:390+(i%2)*60,
            portrait?0:(i/2)*34,portrait?43:60,34,(i>=2&&i<=4)?names[i]:"");
    }
    auto row=[&](const char* text,int n) {
        for(int i=0;text[i];++i) {
            static const char* letters[]={"q","w","e","r","t","y","u","i","o","p","a","s","d","f","g","h","j","k","l","z","x","c","v","b","n","m"};
            const char* name=nullptr;
            for(auto s:letters)if(s[0]==text[i])name=s;
            if(name)add(name,"",(n==1?19:0)+i*39,strip+n*rowH,39,keyH);
        }
    };
    if(page=="abc") {
        row("qwertyuiop",0);row("asdfghjkl",1);
        add("lshift","shift",0,strip+2*rowH,58,keyH,"lshift");
        const char* z[]={"z","x","c","v","b","n","m"};
        for(int i=0;i<7;++i)add(z[i],"",58+i*39,strip+2*rowH,39,keyH);
        add("backspace","",331,strip+2*rowH,59,keyH);
    } else if(page=="symbols"||page=="more") {
        const char* normal[3][10]={{"1","2","3","4","5","6","7","8","9","0"},
            {"2","3","4","5","7","-","=","9","0",nullptr},
            {"8","'","'",";",";","1","/",nullptr,nullptr,nullptr}};
        const char* labels[3][10]={{"1","2","3","4","5","6","7","8","9","0"},
            {"@","#","$","%","&","-","+","(",")",nullptr},
            {"*","\"","'",":",";","!","?",nullptr,nullptr,nullptr}};
        const char* more[3][10]={{"`","`","-","=","[","]","[","]","\\","\\"},
            {",",".","/","-","=",";","'","`",nullptr,nullptr},
            {",",".","\\","[","]",nullptr,nullptr,nullptr,nullptr,nullptr}};
        const char* moreLabels[3][10]={{"`","~","_","+","[","]","{","}","\\","|"},
            {"<",">","/","-","=",";","'","`",nullptr,nullptr},
            {",",".","\\","[","]",nullptr,nullptr,nullptr,nullptr,nullptr}};
        for(int r=0;r<3;++r)for(int c=0;c<10;++c) {
            const char* name=page=="more"?more[r][c]:normal[r][c];
            const char* label=page=="more"?moreLabels[r][c]:labels[r][c];
            if(name) {
                const bool shifted=std::strchr("~!@#$%^&*()_+{}|:\"<>?",label[0]);
                add(name,label,(r==2?58:0)+c*(r==1?43:39),strip+r*rowH,39,keyH,"",shifted?"shift":"");
            }
        }
        add("","=\\<",0,strip+2*rowH,58,keyH,page=="more"?"symbols":"more");
        add("backspace","",331,strip+2*rowH,59,keyH);
    } else {
        const char* apps[]={"calendar","address","todo","memo","find","menu","apps","send"};
        const char* fs[]={"f1","f2","f3","f4","f5","f6","f7","f8"};
        for(int i=0;i<8;++i) {add(apps[i],"",i*48,strip,48,keyH);add(fs[i],"",i*48,strip+rowH,48,keyH);}
        add("function","function",0,strip+2*rowH,58,keyH,"function");
        add("caps","",58,strip+2*rowH,54,keyH);add("power","on/off",112,strip+2*rowH,54,keyH);
        add("delete","",166,strip+2*rowH,54,keyH);
        add("up","PgUp",220,strip+2*rowH,54,keyH,"","function");
        keys.back().chord[0]=*DanaScan5::Named("function");
        add("down","PgDn",274,strip+2*rowH,54,keyH,"","function");
        keys.back().chord[0]=*DanaScan5::Named("function");
        add("backspace","",328,strip+2*rowH,62,keyH);
    }
    const int y=strip+3*rowH;
    add("",page=="abc"?"?123":"ABC",0,y,58,keyH,page=="abc"?"symbols":"abc");
    add("","Fn",58,y,39,keyH,"fn");
    add(",","",97,y,39,keyH);
    add("space","",136,y,157,keyH);
    add(".","",293,y,39,keyH);add("enter","",332,y,58,keyH);
    return keys;
}

// Pointer chords own scans before the screen source is aggregated. Failed releases retry.
class DanaScreenKeys {
    using Chord=std::vector<DanaScan5>;
    std::map<std::pair<int64_t,int64_t>,Chord> pointers;
    std::map<uint8_t,int> modifiers;
    std::array<bool,128> applied{};
    std::array<bool,128> Desired() const {
        std::array<bool,128> desired{};
        for(const auto& item:modifiers)if(item.second)desired[item.first]=true;
        for(const auto& item:pointers)for(auto scan:item.second)desired[scan.Code()]=true;
        return desired;
    }
  public:
    template<class Apply> bool Flush(Apply apply) {
        const auto desired=Desired();
        std::vector<DanaTransition5> batch;
        // Releases first let a saturated chord make room for the next press.
        for(const auto& key:danaKeys5)if(applied[key.scan]&&!desired[key.scan])batch.push_back({*DanaScan5::Named(key.name),false});
        for(const auto& key:danaKeys5)if(!applied[key.scan]&&desired[key.scan])batch.push_back({*DanaScan5::Named(key.name),true});
        if(!batch.empty()&&!apply(batch))return false;
        applied=desired;return true;
    }
    template<class Apply> bool Down(int64_t touch,int64_t id,const Chord& chord,Apply apply) {
        const auto key=std::make_pair(touch,id);
        if(pointers.count(key)||chord.empty()||!Flush(apply))return false;
        pointers[key]=chord;
        if(!Flush(apply)) {pointers.erase(key);return false;}
        // One-shot modifiers remain in this pointer's chord until its release.
        for(auto& modifier:modifiers)if(modifier.second==1) {
            auto& held=pointers[key];
            for(const auto& scan:danaKeys5)if(scan.scan==modifier.first)held.push_back(*DanaScan5::Named(scan.name));
            modifier.second=0;
        }
        return true;
    }
    template<class Apply> void Up(int64_t touch,int64_t id,Apply apply) { pointers.erase({touch,id});Flush(apply); }
    template<class Apply> bool Modifier(DanaScan5 scan,Apply apply) {
        const int old=modifiers[scan.Code()];modifiers[scan.Code()]=(old+1)%3;
        if(!Flush(apply)) { modifiers[scan.Code()]=old;return false; }return true;
    }
    template<class Apply> void Cancel(Apply apply) {pointers.clear();modifiers.clear();Flush(apply);}
    void Clear() {pointers.clear();modifiers.clear();applied={};}
    bool Owns(int64_t touch,int64_t id) const {return pointers.count({touch,id})!=0;}
    bool Held(DanaScan5 scan) const {return applied[scan.Code()];}
};
#endif
