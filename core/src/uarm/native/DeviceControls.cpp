#include "DeviceControls.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <tuple>
#include <vector>

namespace {
struct Rgb { float r, g, b; };
Rgb mix(Rgb a, Rgb b, float t) {return {a.r+(b.r-a.r)*t,a.g+(b.g-a.g)*t,a.b+(b.b-a.b)*t};}
Rgb operator*(Rgb a, float k) {return {a.r*k,a.g*k,a.b*k};}
Rgb operator+(Rgb a, Rgb b) {return {a.r+b.r,a.g+b.g,a.b+b.b};}
float clamp01(float v) {return std::min(1.f,std::max(0.f,v));}
float segment(float x, float y, float ax, float ay, float bx, float by) {
    const float px=x-ax,py=y-ay,qx=bx-ax,qy=by-ay;
    const float t=clamp01((px*qx+py*qy)/(qx*qx+qy*qy));
    return std::hypot(px-qx*t,py-qy*t);
}
float ring(float x, float y, float cx, float cy, float radius) {
    return std::fabs(std::hypot(x-cx,y-cy)-radius);
}
float ellipse(float x, float y, float rx, float ry) {
    const float k0=std::hypot(x/rx,y/ry),k1=std::hypot(x/(rx*rx),y/(ry*ry));
    return k1>0 ? std::fabs(k0*(k0-1)/k1) : rx;
}
// Signed distance to a rectangle given by its corners; negative inside.
float box(float x, float y, float x0, float y0, float x1, float y1) {
    const float qx=std::fabs(x-(x0+x1)*0.5f)-(x1-x0)*0.5f,qy=std::fabs(y-(y0+y1)*0.5f)-(y1-y0)*0.5f;
    return std::hypot(std::max(qx,0.f),std::max(qy,0.f))+std::min(std::max(qx,qy),0.f);
}
// Signed distance to a simple polygon, any winding or shape; negative inside.
template <size_t N>
float polygon(float x, float y, const std::array<std::array<float,2>,N>& p) {
    float d=1e9f,sign=1;
    for(size_t i=0,j=N-1;i<N;j=i++) {
        const auto& a=p[i];const auto& b=p[j];
        d=std::min(d,segment(x,y,a[0],a[1],b[0],b[1]));
        const bool c0=y>=a[1],c1=y<b[1],c2=(b[0]-a[0])*(y-a[1])>(b[1]-a[1])*(x-a[0]);
        if((c0 && c1 && c2) || (!c0 && !c1 && !c2))sign=-sign;
    }
    return sign*d;
}
// Distance to a circular arc running clockwise on screen from angle a0 to a1, in radians.
float arc(float x, float y, float cx, float cy, float r, float a0, float a1) {
    const float tau=6.2831853f;
    float a=std::atan2(y-cy,x-cx);
    while(a<a0)a+=tau;
    while(a>=a0+tau)a-=tau;
    if(a<=a1)return std::fabs(std::hypot(x-cx,y-cy)-r);
    return std::min(std::hypot(x-cx-r*std::cos(a0),y-cy-r*std::sin(a0)),
                    std::hypot(x-cx-r*std::cos(a1),y-cy-r*std::sin(a1)));
}
float degrees(float d) {return d*0.017453293f;}

// Distance to the thin line icons on the app keys, in strip units from the key centre.
float iconDistance(DeviceControlIcon icon, float x, float y) {
    float d=1e9f;
    auto line=[&](float ax,float ay,float bx,float by) {d=std::min(d,segment(x,y,ax,ay,bx,by));};
    switch(icon) {
        case DeviceControlIcon::home:
            line(-7,-0.5f,0,-7);line(0,-7,7,-0.5f);line(-5,-2.2f,-5,6);line(5,-2.2f,5,6);
            line(-5,6,5,6);line(-1.6f,6,-1.6f,1.8f);line(-1.6f,1.8f,1.6f,1.8f);line(1.6f,1.8f,1.6f,6);
            break;
        case DeviceControlIcon::calendar: {
            // A small grid with a clock face over its lower-left corner.
            const float cx=-3.6f,cy=2.6f,r=4.2f;
            if(std::hypot(x-cx,y-cy)>r+1.4f) {
                line(-2,-6,7.5f,-6);line(7.5f,-6,7.5f,4.5f);line(-2,4.5f,7.5f,4.5f);line(-2,-6,-2,4.5f);
                line(1.2f,-6,1.2f,4.5f);line(4.4f,-6,4.4f,4.5f);line(-2,-2.5f,7.5f,-2.5f);line(-2,1,7.5f,1);
            }
            d=std::min(d,ring(x,y,cx,cy,r));line(cx,cy,cx,cy-2.4f);line(cx,cy,cx+1.8f,cy);
            break;
        }
        case DeviceControlIcon::contacts: {
            const SDL_Rect card{-8,-6,16,12};
            d=std::min(d,std::fabs(deviceRoundedDistance(card,1.5f,x,y)));
            d=std::min(d,ring(x,y,-3.6f,-1.6f,1.9f));
            if(y<=4.4f)d=std::min(d,ring(x,y,-3.6f,4.4f,3.1f));
            line(1,-2.6f,5.6f,-2.6f);line(1,0.6f,5.6f,0.6f);line(1,3.6f,4.4f,3.6f);
            break;
        }
        case DeviceControlIcon::web:
            d=std::min(d,ring(x,y,0,0,7));d=std::min(d,ellipse(x,y,3,7));
            line(-7,0,7,0);line(0,-7,0,7);line(-6,-3.6f,6,-3.6f);line(-6,3.6f,6,3.6f);
            break;
        case DeviceControlIcon::tasks:
            // Two list rows: a ticked row and an open box, each with a line of text.
            line(-7.5f,-3.6f,-5.6f,-1.6f);line(-5.6f,-1.6f,-2.2f,-6.2f);line(0.5f,-3.4f,7.5f,-3.4f);
            d=std::min(d,std::fabs(box(x,y,-7.3f,1.6f,-3,5.9f)));line(0.5f,3.8f,7.5f,3.8f);
            break;
        case DeviceControlIcon::notes:
            // A handwritten loop and a pen stroke.
            d=arc(x,y,-2.6f,0.4f,4.6f,degrees(-30),degrees(250));
            d=std::min(d,arc(x,y,-1.6f,1,2.2f,degrees(140),degrees(330)));
            line(-1.6f+2.2f*std::cos(degrees(330)),1+2.2f*std::sin(degrees(330)),
                 -2.6f+4.6f*std::cos(degrees(-30)),0.4f+4.6f*std::sin(degrees(-30)));
            line(3,5.2f,8,-5.2f);
            break;
        case DeviceControlIcon::media: {
            // A camera seen from the front: body, viewfinder bump, lens and flash.
            const SDL_Rect body{-8,-4,16,11};
            d=std::fabs(deviceRoundedDistance(body,1.6f,x,y));
            if(y<-4) {line(-5.6f,-4,-5,-6.4f);line(-5,-6.4f,-1.6f,-6.4f);line(-1.6f,-6.4f,-1,-4);}
            d=std::min(d,ring(x,y,2.4f,1.6f,2.9f));
            d=std::min(d,std::hypot(x+4.6f,y+0.8f)-0.6f);
            break;
        }
        case DeviceControlIcon::music: {
            // Two beamed eighth notes with filled heads.
            line(-2.2f,3.6f,-2.2f,-5.4f);line(5,2.2f,5,-6.8f);
            line(-2.2f,-5.4f,5,-6.8f);d=std::min(d,segment(x,y,-2.2f,-4.2f,5,-5.6f)-0.3f);
            for(auto [hx,hy]:{std::pair<float,float>{-4.3f,4},{2.9f,2.6f}}) {
                // Heads tilt up to the right.
                const float c=0.9397f,sn=-0.342f,u=(x-hx)*c-(y-hy)*sn,v=(x-hx)*sn+(y-hy)*c;
                const float k=std::hypot(u/2.3f,v/1.6f);
                d=std::min(d,k<=1 ? 0.f : ellipse(u,v,2.3f,1.6f));
            }
            break;
        }
        case DeviceControlIcon::phone: {
            // A telephone handset lying diagonally, the ear and mouth cups facing up and right.
            const float cx=6.4f,cy=-6.4f,r=10.8f;
            d=arc(x,y,cx,cy,r,degrees(98),degrees(172))-0.8f;
            for(float a:{degrees(98),degrees(172)}) {
                const float ex=cx+r*std::cos(a),ey=cy+r*std::sin(a);
                d=std::min(d,segment(x,y,ex,ey,ex-std::cos(a)*2.6f,ey-std::sin(a)*2.6f)-1.7f);
            }
            break;
        }
        case DeviceControlIcon::memo: {
            // A note pad with lines of writing and a pencil across its corner.
            const float px0=7.8f,py0=-7.8f,px1=1.2f,py1=0.6f;
            const float pencil=segment(x,y,px0,py0,px1,py1);
            if(pencil>2.4f) {
                d=std::fabs(box(x,y,-7,-5.6f,3.4f,7.4f));
                line(-7,-5.6f,3.4f,-5.6f);line(-7,-4.4f,3.4f,-4.4f);
                for(float row:{-1.6f,1.4f,4.4f})line(-5,row,1.6f,row);
            }
            d=std::min(d,pencil-0.9f);
            const std::array<std::array<float,2>,3> tip{{{0.2f,-0.6f},{2.4f,1.6f},{-0.8f,2.6f}}};
            d=std::min(d,polygon(x,y,tip));
            break;
        }
        case DeviceControlIcon::graffiti:
            // A pen writing a looped stroke.
            d=arc(x,y,-3.4f,2.2f,3,degrees(60),degrees(330));
            line(-3.4f+3*std::cos(degrees(60)),2.2f+3*std::sin(degrees(60)),3,5.6f);
            line(7.4f,-7.4f,1.6f,-1.6f);
            d=std::min(d,segment(x,y,6.2f,-6.2f,2.6f,-2.6f)-1.1f);
            line(1.6f,-1.6f,0.4f,0.4f);
            break;
        case DeviceControlIcon::back:
            // A U-turn arrow pointing back to the left.
            d=arc(x,y,1,0,4.6f,degrees(-90),degrees(90));
            line(1,-4.6f,-6,-4.6f);line(1,4.6f,-3,4.6f);
            line(-6,-4.6f,-2.8f,-7.8f);line(-6,-4.6f,-2.8f,-1.4f);
            break;
        case DeviceControlIcon::inbox:
            // An envelope with its flap folded down to the middle.
            d=std::fabs(box(x,y,-7.5f,-5,7.5f,5.5f));
            line(-7.5f,-5,0,1);line(0,1,7.5f,-5);
            break;
        case DeviceControlIcon::start:
            // A menu: three lines, each with a dot before it.
            for(float row:{-4.5f,0.f,4.5f}) {
                d=std::min(d,std::hypot(x+6,y-row)-1.1f);
                line(-2.6f,row,6.5f,row);
            }
            break;
        case DeviceControlIcon::ok:
            // "OK" in a ring.
            d=ring(x,y,0,0,8);
            d=std::min(d,ellipse(x+2.9f,y,2.2f,3.4f));
            line(1.6f,-3.4f,1.6f,3.4f);line(1.6f,0.4f,5,-3.4f);line(2.6f,-0.6f,5,3.4f);
            break;
        case DeviceControlIcon::wireless:
            // A dot sending waves both ways.
            d=std::hypot(x,y)-1.6f;
            for(float r:{4.4f,7.6f}) {
                d=std::min(d,arc(x,y,0,0,r,degrees(-45),degrees(45)));
                d=std::min(d,arc(x,y,0,0,r,degrees(135),degrees(225)));
            }
            break;
        case DeviceControlIcon::taskSwitch: {
            // A hook curling round from the lower right to an arrowhead at the upper right.
            d=arc(x,y,-0.5f,0.5f,6.5f,degrees(30),degrees(300));
            const float tx=-0.5f+6.5f*std::cos(degrees(300)),ty=0.5f+6.5f*std::sin(degrees(300));
            const float ux=-std::sin(degrees(300)),uy=std::cos(degrees(300));
            line(tx,ty,tx-3.6f*ux-2.8f*uy,ty-3.6f*uy+2.8f*ux);
            line(tx,ty,tx-3.6f*ux+2.8f*uy,ty-3.6f*uy-2.8f*ux);
            break;
        }
        case DeviceControlIcon::today:
            // A handheld seen from the front: its outline and the edges of its screen.
            d=std::fabs(box(x,y,-4.5f,-7.5f,4.5f,7.5f));
            line(-4.5f,-4.6f,4.5f,-4.6f);line(-4.5f,4.4f,4.5f,4.4f);
            break;
        case DeviceControlIcon::calendarGrid:
            // A month grid, five columns by four rows.
            d=std::fabs(box(x,y,-7.5f,-6,7.5f,6));
            for(float gx:{-4.5f,-1.5f,1.5f,4.5f})line(gx,-6,gx,6);
            for(float gy:{-3.f,0.f,3.f})line(-7.5f,gy,7.5f,gy);
            break;
        default: break;
    }
    return d;
}

// Fine printed or moulded speckle, -0.5 to 0.5, on a half-unit grid.
float speckle(float x, float y) {
    uint32_t h=uint32_t(int(std::floor(x*2)))*73856093u^uint32_t(int(std::floor(y*2)))*19349663u;
    h^=h>>13;h*=0x5bd1e995u;h^=h>>15;
    return (h&1023u)/1023.f-0.5f;
}

// Float RGB image of the strip at device pixels; shapes are given in strip units.
class Canvas {
 public:
    Canvas(int width, int height, float scale)
        : w(int(std::lround(width*scale))), h(int(std::lround(height*scale))), s(scale), pixels(size_t(w)*h) {}
    float edge(float distance) const {return clamp01(0.5f-distance*s);}
    template <class Cover, class Colour>
    void paint(float x0, float y0, float x1, float y1, Cover cover, Colour colour) {
        const int X0=std::max(0,int(std::floor((std::max(x0,clipLeft)+originX)*s))),X1=std::min(w,int(std::ceil((std::min(x1,clipRight)+originX)*s)));
        const int Y0=std::max(0,int(std::floor((y0+originY)*s))),Y1=std::min(h,int(std::ceil((y1+originY)*s)));
        for(int Y=Y0;Y<Y1;++Y)for(int X=X0;X<X1;++X) {
            const float x=(X+0.5f)/s-originX,y=(Y+0.5f)/s-originY,c=cover(x,y);
            if(c<=0)continue;
            auto& p=pixels[size_t(Y)*w+X];p=mix(p,colour(x,y,p),std::min(c,1.f));
        }
    }
    int w, h;
    float s;
    float originX{0},originY{0};
    float clipLeft{-1e9f},clipRight{1e9f};
    std::vector<Rgb> pixels;
};

// Colours of a five-way: a raised outer rim, a dished face sloping down to the select pill.
struct NavigatorLook {
    Rgb seam, rimTop, rimBottom, faceTop, faceBottom, highlight;
    float rimWidth, shadow, dish, held;
    Rgb groove, pillTop, pillBottom, pillEdge;
};

// How far a five-way point belongs to the held directions, 0 to 1, blended softly across the
// diagonals; the hit test splits them hard along the same lines.
float navigatorHeld(const SDL_Rect& n, float x, float y, DeviceKeyMask pressed) {
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    const float dx=x-n.x-n.w*0.5f,dy=y-n.y-n.h*0.5f;
    const float across=(std::fabs(dx)*n.h-std::fabs(dy)*n.w)/std::hypot(float(n.w),float(n.h));
    const float sideways=clamp01(0.5f+across/6);
    return sideways*float(down(dx<0 ? keyIdLeft : keyIdRight))+(1-sideways)*float(down(dy<0 ? keyIdUp : keyIdDown));
}

void paintNavigator(Canvas& canvas, const DeviceControlProfile& profile, int ox, DeviceKeyMask pressed,
                    const NavigatorLook& look) {
    SDL_Rect nav=profile.navigator,pill=profile.select;
    nav.x+=ox;pill.x+=ox;
    const float corner=profile.navigatorCorner,pr=profile.selectCorner>0 ? profile.selectCorner : pill.h*0.5f;
    const float cy=nav.y+nav.h*0.5f;
    const float x0=nav.x-4,y0=nav.y-3,x1=nav.x+nav.w+4,y1=nav.y+nav.h+5;
    auto dn=[&](float x,float y) {return deviceRoundedDistance(nav,corner,x,y);};
    auto dp=[&](float x,float y) {return deviceRoundedDistance(pill,pr,x,y);};
    // Soft drop shadow and a dark seam round the ring.
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return clamp01((2.5f-dn(x,y-1.8f))/5)*look.shadow;},
                 [](float,float,Rgb) {return Rgb{0,0,0};});
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(dn(x,y)-1.4f);},
                 [&](float,float,Rgb) {return look.seam;});
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(dn(x,y));},[&](float x,float y,Rgb) {
        const float t=clamp01((y-nav.y)/nav.h),d=dn(x,y);
        Rgb c=mix(look.faceTop,look.faceBottom,t);
        // Raised bevelled rim rolling off at the outer edge, with a fine line where it meets the face.
        const float rim=clamp01(0.5f+(d+look.rimWidth)*canvas.s/1.5f);
        c=mix(c,mix(look.rimTop,look.rimBottom,t),rim);
        c=c*(1-0.25f*clamp01(1+d/1.2f))*(1-0.22f*canvas.edge(std::fabs(d+look.rimWidth)-0.35f));
        // The dish: its upper slope faces away from the light, the lower slope towards it.
        const float slope=std::pow(clamp01(1-(dp(x,y)-2)/10),1.5f)*(1-rim);
        c=c*(1+look.dish*slope*std::max(-1.f,std::min(1.f,(y-cy)/(nav.h*0.35f))));
        return c*(1-(1-look.held)*navigatorHeld(nav,x,y,pressed));
    });
    // A thin highlight along the top of the rim.
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
        return canvas.edge(std::fabs(dn(x,y)+0.7f)-0.35f)*0.6f*clamp01((cy-y)/(nav.h*0.5f))*
               (1-navigatorHeld(nav,x,y,pressed)*0.7f);
    },[&](float,float,Rgb) {return look.highlight;});
    // Groove round the pill-shaped select key; holding it presses it in.
    const bool held=(pressed&deviceKeyBit(keyIdSelect))!=0;
    const float sink=held ? 0.8f : 0;
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(dp(x,y)-1.8f);},
                 [&](float,float y,Rgb) {return mix(look.groove,look.groove*1.6f,clamp01((y-pill.y)/pill.h));});
    auto pillDistance=[&](float x,float y) {return dp(x,y-sink)+0.3f;};
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(pillDistance(x,y));},[&](float x,float y,Rgb) {
        const float t=clamp01((y-pill.y-sink)/pill.h);
        Rgb c=mix(look.pillTop,look.pillBottom,t);
        const float r=std::hypot((x-pill.x-pill.w*0.5f)/(pill.w*0.5f),(y-pill.y-sink-pill.h*0.3f)/(pill.h*0.5f));
        c=c*(1+0.12f*clamp01(1-r));
        // Held: darker, with a shadow under the groove's top edge.
        return held ? c*(0.5f+0.2f*clamp01(-pillDistance(x,y-1.5f)/3)) : c;
    });
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
        return canvas.edge(std::fabs(pillDistance(x,y)+0.8f)-0.35f)*(held?0.15f:0.55f)*
               clamp01((pill.y+pill.h*0.5f-y)/(pill.h*0.5f));
    },[&](float,float,Rgb) {return look.pillEdge;});
}

void paintTxControls(Canvas& canvas, const DeviceControlProfile& profile, int width, DeviceKeyMask pressed) {
    const float H=profile.height;
    const int ox=(width-profile.width)/2;
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    auto all=[&](float,float) {return 1.f;};
    auto shifted=[&](SDL_Rect r) {r.x+=int(ox);return r;};
    // Original vector artwork: a satin near-black face stepped down into a band below the screen.
    const float step=12;
    canvas.paint(0,0,width,H,all,[&](float x,float y,Rgb) {
        Rgb c=y<step ? mix(Rgb{27,30,39},Rgb{23,26,34},y/step)
                     : mix(Rgb{21,23,31},Rgb{12,13,18},(y-step)/(H-step));
        if(y>=step)c=c+Rgb{5,6,9}*std::exp(-std::pow((y-step-7)/6,2.f));
        const float t=std::fabs(x-width*0.5f)/(width*0.5f);
        return c*(1-0.18f*t*t);
    });
    canvas.paint(0,step-1,width,step+2,[&](float,float y) {return canvas.edge(std::fabs(y-step+0.5f)-0.5f)*0.55f;},
                 [](float,float,Rgb) {return Rgb{60,66,80};});
    canvas.paint(0,step,width,step+3,[&](float,float y) {return canvas.edge(std::fabs(y-step-0.8f)-0.4f)*0.7f;},
                 [](float,float,Rgb) {return Rgb{6,7,10};});
    canvas.paint(0,H-2,width,H,[&](float,float y) {return canvas.edge(std::fabs(y-H+0.6f)-0.6f)*0.6f;},
                 [](float,float,Rgb) {return Rgb{38,42,53};});

    for(size_t i=0;i<profile.count;++i) {
        const auto& button=profile.buttons[i];
        if(deviceNavigatorButton(profile,button))continue;
        const bool held=down(button.key);
        const SDL_Rect pocket=shifted(button.bounds);
        const float corner=deviceButtonCorner(profile,pocket);
        const float x0=pocket.x-2,y0=pocket.y-2,x1=pocket.x+pocket.w+2,y1=pocket.y+pocket.h+2;
        // Recessed pocket with a thin grey rim, lit along its lower lip.
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(deviceRoundedDistance(pocket,corner,x,y));},
                     [&](float,float y,Rgb) {return mix(Rgb{3,3,5},Rgb{11,12,16},clamp01((y-pocket.y)/pocket.h));});
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
            return canvas.edge(std::fabs(deviceRoundedDistance(pocket,corner,x,y))-0.45f);
        },[&](float,float y,Rgb) {return mix(Rgb{36,39,48},Rgb{72,76,88},clamp01((y-pocket.y)/pocket.h));});
        // Black domed key; holding it presses it into the pocket.
        const float sink=held ? 0.8f : 0;
        SDL_Rect key{pocket.x+3,pocket.y+3,pocket.w-6,pocket.h-6};
        const float kcorner=corner-2.5f,kcx=key.x+key.w*0.5f,kcy=key.y+key.h*0.5f+sink;
        auto keyDistance=[&](float x,float y) {return deviceRoundedDistance(key,kcorner,x,y-sink);};
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(keyDistance(x,y-1.2f))*(held?0.3f:0.7f);},
                     [](float,float,Rgb) {return Rgb{0,0,0};});
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(keyDistance(x,y));},
                     [&](float x,float y,Rgb) {
            const float r=std::hypot((x-kcx)/(key.w*0.5f),(y-kcy+key.h*0.3f)/(key.h*0.6f));
            Rgb c=Rgb{28,30,36}+Rgb{30,32,37}*std::pow(clamp01(1-r),2.f);
            c=mix(Rgb{9,10,13},c,clamp01(-keyDistance(x,y)/3.5f));
            // Held: darker, with an inner shadow under the pocket's top lip.
            return held ? c*(0.4f+0.25f*clamp01(-keyDistance(x,y-1.5f)/3)) : c;
        });
        // Thin lit edge around the key, strongest along the top.
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
            return canvas.edge(std::fabs(keyDistance(x,y)+0.8f)-0.35f)*(held?0.15f:0.25f+0.35f*clamp01((kcy-y)/(key.h*0.5f)));
        },[](float,float,Rgb) {return Rgb{72,76,86};});
        const Rgb ink=held ? Rgb{84,88,98} : Rgb{168,173,182};
        canvas.paint(kcx-10,kcy-10,kcx+10,kcy+10,[&](float x,float y) {
            return canvas.edge(iconDistance(button.icon,x-kcx,y-kcy)-0.55f);
        },[&](float,float,Rgb) {return ink;});
    }

    // Chrome ring: a bright outer edge round a darker reflective face, then a dark groove
    // round a light silver select key.
    if(profile.navigator.w>0)
        paintNavigator(canvas,profile,ox,pressed,{Rgb{4,5,7},Rgb{238,241,246},Rgb{150,154,162},Rgb{58,61,68},
                                                  Rgb{112,116,124},Rgb{255,255,255},4,0.75f,0.3f,0.6f,
                                                  Rgb{5,6,8},Rgb{228,231,236},Rgb{150,154,162},Rgb{255,255,255}});
}

SDL_Texture* canvasTexture(SDL_Renderer* renderer, const Canvas& canvas) {
    auto* surface=SDL_CreateRGBSurfaceWithFormat(0,canvas.w,canvas.h,32,SDL_PIXELFORMAT_RGBA32);
    if(!surface)return nullptr;
    for(int y=0;y<canvas.h;++y) {
        auto* row=static_cast<Uint8*>(surface->pixels)+y*surface->pitch;
        for(int x=0;x<canvas.w;++x) {
            const auto& p=canvas.pixels[size_t(y)*canvas.w+x];
            row[x*4]=Uint8(std::lround(std::min(255.f,std::max(0.f,p.r))));
            row[x*4+1]=Uint8(std::lround(std::min(255.f,std::max(0.f,p.g))));
            row[x*4+2]=Uint8(std::lround(std::min(255.f,std::max(0.f,p.b))));
            row[x*4+3]=255;
        }
    }
    auto* texture=SDL_CreateTextureFromSurface(renderer,surface);
    if(texture)SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_NONE);
    SDL_FreeSurface(surface);
    return texture;
}

// Stroke capitals and digits for the keyboard labels: x from the glyph centre, y from the
// baseline, 7 units high.
float glyphDistance(char c, float x, float y) {
    float d=1e9f;
    auto line=[&](float ax,float ay,float bx,float by) {d=std::min(d,segment(x,y,ax,ay,bx,by));};
    switch(c) {
        case 'A': line(-2.6f,0,0,-7);line(0,-7,2.6f,0);line(-1.6f,-2.4f,1.6f,-2.4f);break;
        case 'B':
            line(-2.2f,0,-2.2f,-7);line(-2.2f,-7,0.2f,-7);line(-2.2f,-3.6f,0.6f,-3.6f);line(-2.2f,0,0.6f,0);
            d=std::min({d,arc(x,y,0.2f,-5.3f,1.7f,degrees(-90),degrees(90)),arc(x,y,0.6f,-1.8f,1.8f,degrees(-90),degrees(90))});
            break;
        case 'C': d=arc(x,y,0.3f,-3.5f,3.5f,degrees(45),degrees(315));break;
        case 'a': d=ring(x,y,-0.4f,-2.5f,2.5f);line(2.1f,-5.2f,2.1f,0);break;
        case 'b': d=ring(x,y,0.4f,-2.5f,2.5f);line(-2.1f,-7.4f,-2.1f,0);break;
        case 'c': d=arc(x,y,0.3f,-2.5f,2.5f,degrees(45),degrees(315));break;
        case '1': line(0.4f,-7,0.4f,0);line(-1.6f,-5.4f,0.4f,-7);break;
        case '2': {
            const float cx=0,cy=-5,r=2;
            d=arc(x,y,cx,cy,r,degrees(180),degrees(390));
            line(cx+r*std::cos(degrees(30)),cy+r*std::sin(degrees(30)),-2.2f,0);line(-2.2f,0,2.4f,0);
            break;
        }
        case '3':
            d=std::min(arc(x,y,0,-5.2f,1.8f,degrees(200),degrees(450)),arc(x,y,0,-1.8f,1.9f,degrees(270),degrees(520)));
            break;
        default: break;
    }
    return d;
}
float textDistance(const char* text, float x, float y) {
    const float advance=6.2f;
    const size_t n=std::strlen(text);
    float d=1e9f;
    for(size_t i=0;i<n;++i)d=std::min(d,glyphDistance(text[i],x-(i-(n-1)*0.5f)*advance,y));
    return d;
}

void paintE2Controls(Canvas& canvas, const DeviceControlProfile& profile, int width, DeviceKeyMask pressed) {
    const float H=profile.height;
    const int ox=(width-profile.width)/2;
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    auto all=[&](float,float) {return 1.f;};
    auto shifted=[&](SDL_Rect r) {r.x+=ox;return r;};
    // Original vector artwork: brushed silver face under the screen glass, stepped down into a
    // slightly darker band that holds the keys.
    const float step=13;
    canvas.paint(0,0,width,H,all,[&](float x,float y,Rgb) {
        Rgb c=y<step ? mix(Rgb{186,189,194},Rgb{174,178,183},y/step)
                     : mix(Rgb{166,170,176},Rgb{116,120,127},std::pow((y-step)/(H-step),1.3f));
        if(y>=step)c=c+Rgb{14,14,14}*std::exp(-std::pow((y-step-3)/4,2.f));
        c=c+Rgb{3,3,3}*std::sin(y*9.1f+std::sin(x*0.37f)*0.6f);
        const float t=std::fabs(x-width*0.5f)/(width*0.5f);
        return c*(1-0.16f*t*t);
    });
    // Lip of the screen glass, then the step down to the band.
    canvas.paint(0,0,width,3,[&](float,float y) {return canvas.edge(y-1.2f);},[](float,float,Rgb) {return Rgb{46,48,52};});
    canvas.paint(0,0,width,4,[&](float,float y) {return canvas.edge(std::fabs(y-1.9f)-0.4f)*0.8f;},
                 [](float,float,Rgb) {return Rgb{236,238,241};});
    canvas.paint(0,step-1,width,step+2,[&](float,float y) {return canvas.edge(std::fabs(y-step+0.5f)-0.45f)*0.8f;},
                 [](float,float,Rgb) {return Rgb{240,242,245};});
    canvas.paint(0,step,width,step+3,[&](float,float y) {return canvas.edge(std::fabs(y-step-0.7f)-0.4f)*0.6f;},
                 [](float,float,Rgb) {return Rgb{104,108,116};});
    canvas.paint(0,H-3,width,H,[&](float,float y) {return canvas.edge(std::fabs(y-H+0.8f)-0.8f)*0.55f;},
                 [](float,float,Rgb) {return Rgb{84,88,95};});

    for(size_t i=0;i<profile.count;++i) {
        const auto& button=profile.buttons[i];
        if(deviceNavigatorButton(profile,button))continue;
        const bool held=down(button.key);
        const SDL_Rect key=shifted(button.bounds);
        const float corner=deviceButtonCorner(profile,key),sink=held ? 0.7f : 0;
        const float x0=key.x-4,y0=key.y-4,x1=key.x+key.w+4,y1=key.y+key.h+6;
        const float kcx=key.x+key.w*0.5f,kcy=key.y+key.h*0.5f+sink;
        auto d=[&](float x,float y) {return deviceRoundedDistance(key,corner,x,y);};
        // Soft shadow and a dark seam round the key.
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return clamp01((2-d(x,y-1.4f))/4)*(held?0.15f:0.3f);},
                     [](float,float,Rgb) {return Rgb{30,32,36};});
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(d(x,y)-1.1f);},
                     [](float,float,Rgb) {return Rgb{78,81,87};});
        // Bright silver rim, lit along the top, round a grey face.
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(d(x,y));},[&](float,float y,Rgb) {
            const Rgb c=mix(Rgb{246,247,249},Rgb{156,160,166},clamp01((y-key.y)/key.h));
            return held ? c*0.82f : c;
        });
        auto face=[&](float x,float y) {return d(x,y-sink)+1.8f;};
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(face(x,y));},[&](float x,float y,Rgb) {
            const float t=clamp01((y-key.y-sink)/key.h);
            Rgb c=mix(Rgb{164,168,174},Rgb{136,140,147},t);
            const float r=std::hypot((x-kcx)/(key.w*0.45f),(y-key.y-sink-key.h*0.7f)/(key.h*0.35f));
            c=c+Rgb{14,14,15}*std::pow(clamp01(1-r),2.f);
            c=mix(Rgb{118,122,128},c,clamp01(-face(x,y)/2));
            // Held: darker, with a shadow under the rim's top edge.
            return held ? c*(0.7f+0.12f*clamp01(-face(x,y-1.5f)/3)) : c;
        });
        const Rgb ink=held ? Rgb{40,43,48} : Rgb{48,51,57};
        canvas.paint(kcx-11,kcy-11,kcx+11,kcy+11,[&](float x,float y) {
            return canvas.edge(iconDistance(button.icon,(x-kcx)/1.12f,(y-kcy)/1.12f)*1.12f-0.7f);
        },[&](float,float,Rgb) {return ink;});
    }

    // Polished silver five-way with a light silver pill in a dark groove.
    if(profile.navigator.w>0)
        paintNavigator(canvas,profile,ox,pressed,{Rgb{78,81,87},Rgb{248,249,251},Rgb{140,144,151},Rgb{150,154,161},
                                                  Rgb{236,238,241},Rgb{255,255,255},5,0.35f,0.12f,0.7f,
                                                  Rgb{58,61,68},Rgb{232,234,238},Rgb{174,178,184},Rgb{255,255,255}});
}

// Colours of a light silver key sitting in a dark outline groove.
struct SilverKeyLook {
    Rgb groove, faceTop, faceBottom, sheen, lip, ink, inkHeld;
    float grooveWidth, held;
    // How much the face darkens from its centre to its edge, for a domed cap; 0 for flat.
    float dome{0};
};

// A raised silver key of any rounded shape: a dark groove round it, a bright lip on the body
// below the groove, a lit face and its icon. Held keys sink and darken. inset draws the key
// that many units inside its rectangle on every side.
void paintSilverKey(Canvas& canvas, const SDL_Rect& key, float corner, bool held, DeviceControlIcon icon,
                    float iconScale, const SilverKeyLook& look, bool drawIcon=true, float inset=0) {
    const float sink=held ? 0.7f : 0,gw=look.grooveWidth;
    const float x0=key.x-gw-4,y0=key.y-gw-4,x1=key.x+key.w+gw+4,y1=key.y+key.h+gw+6;
    const float kcx=key.x+key.w*0.5f,kcy=key.y+key.h*0.5f+sink;
    auto d=[&](float x,float y) {return deviceRoundedDistance(key,corner,x,y)+inset;};
    // Bright lip where the body curves into the groove, strongest underneath.
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
        return canvas.edge(std::fabs(d(x,y)-gw-0.6f)-0.6f)*clamp01(0.15f+0.85f*(y-key.y)/key.h);
    },[&](float,float,Rgb) {return look.lip;});
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(d(x,y)-gw);},
                 [&](float,float,Rgb) {return look.groove;});
    // Face: lit along the top, a soft sheen across its middle, darkening into the groove.
    auto face=[&](float x,float y) {return d(x,y-sink);};
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(face(x,y)+0.3f);},[&](float x,float y,Rgb) {
        const float t=clamp01((y-key.y-sink)/key.h);
        Rgb c=mix(look.faceTop,look.faceBottom,t);
        const float r=std::hypot((x-kcx)/(key.w*0.5f),(y-key.y-sink-key.h*0.35f)/(key.h*0.45f));
        c=c+look.sheen*std::pow(clamp01(1-r),1.5f);
        if(look.dome>0) {
            // Lit from above: brightest a little over the centre, falling off towards the edges.
            const float rx=(x-kcx)/(key.w*0.5f-inset),ry=(y-kcy+key.h*0.12f)/(key.h*0.5f-inset);
            c=c*(1+look.dome*(0.3f-clamp01(rx*rx*0.6f+ry*ry)));
        }
        c=mix(c*0.72f,c,clamp01(-face(x,y)/2.2f));
        // Held: darker, with a shadow under the groove's top edge.
        return held ? c*(look.held+0.12f*clamp01(-face(x,y-1.6f)/3)) : c;
    });
    // Fine highlight just inside the top edge.
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
        return canvas.edge(std::fabs(face(x,y)+1.1f)-0.35f)*(held?0.15f:0.6f)*clamp01((kcy-y)/(key.h*0.5f));
    },[](float,float,Rgb) {return Rgb{252,253,255};});
    if(!drawIcon)return;
    const Rgb ink=held ? look.inkHeld : look.ink;
    const float reach=11*iconScale;
    canvas.paint(kcx-reach,kcy-reach,kcx+reach,kcy+reach,[&](float x,float y) {
        return canvas.edge(iconDistance(icon,(x-kcx)/iconScale,(y-kcy)/iconScale)*iconScale-0.65f*iconScale);
    },[&](float,float,Rgb) {return ink;});
}

// Original Zire 72 body: graphite-tinted satin silver. The Zire 72s is plain silver.
const Rgb zire72Body{126,128,137};

void paintZire72Controls(Canvas& canvas, const DeviceControlProfile& profile, int width, DeviceKeyMask pressed) {
    const float H=profile.height;
    const int ox=(width-profile.width)/2;
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    auto all=[&](float,float) {return 1.f;};
    auto shifted=[&](SDL_Rect r) {r.x+=ox;return r;};
    // Satin body under the silkscreen glass, a little darker towards the bottom and the sides.
    canvas.paint(0,0,width,H,all,[&](float x,float y,Rgb) {
        Rgb c=mix(zire72Body*1.06f,zire72Body*0.8f,std::pow(y/H,1.2f));
        c=c+Rgb{12,12,14}*std::exp(-std::pow((y-10)/5,2.f));
        c=c+Rgb{2.5f,2.5f,2.5f}*std::sin(y*8.3f+std::sin(x*0.29f)*0.7f);
        const float t=std::fabs(x-width*0.5f)/(width*0.5f);
        return c*(1-0.2f*t*t);
    });
    // The bright chamfer of the glass bezel, then its shadow.
    canvas.paint(0,0,width,3,[&](float,float y) {return canvas.edge(y-1);},[](float,float,Rgb) {return Rgb{40,43,50};});
    canvas.paint(0,0,width,8,[&](float,float y) {return canvas.edge(std::fabs(y-3.8f)-2.6f);},
                 [](float,float,Rgb) {return Rgb{226,231,239};});
    canvas.paint(0,0,width,8,[&](float,float y) {return canvas.edge(std::fabs(y-2)-0.6f)*0.8f;},
                 [](float,float,Rgb) {return Rgb{252,253,255};});
    canvas.paint(0,5,width,9,[&](float,float y) {return canvas.edge(std::fabs(y-6.8f)-0.5f)*0.4f;},
                 [](float,float,Rgb) {return Rgb{70,75,88};});
    canvas.paint(0,H-3,width,H,[&](float,float y) {return canvas.edge(std::fabs(y-H+0.8f)-0.8f)*0.5f;},
                 [](float,float,Rgb) {return Rgb{72,78,92};});

    const SilverKeyLook keys{Rgb{30,33,42},Rgb{216,220,228},Rgb{168,173,184},Rgb{12,12,13},Rgb{226,231,240},
                             Rgb{44,47,56},Rgb{36,38,46},1.7f,0.72f};
    for(size_t i=0;i<profile.count;++i) {
        const auto& button=profile.buttons[i];
        if(deviceNavigatorButton(profile,button))continue;
        const SDL_Rect key=shifted(button.bounds);
        paintSilverKey(canvas,key,deviceButtonCorner(profile,key),down(button.key),button.icon,1.0f,keys);
    }

    // Silver rounded-square five-way: a raised outer rim falling inwards to a light
    // rounded-rectangle select key.
    if(profile.navigator.w>0)
        paintNavigator(canvas,profile,ox,pressed,{Rgb{30,33,42},Rgb{240,243,249},Rgb{160,167,182},Rgb{118,125,140},
                                                  Rgb{186,192,204},Rgb{255,255,255},8,0.45f,0.3f,0.72f,
                                                  Rgb{34,37,46},Rgb{214,218,226},Rgb{164,169,180},Rgb{250,251,253}});
    // A dark step where the rim drops to the inner face.
    if(profile.navigator.w>0) {
        SDL_Rect nav=profile.navigator;nav.x+=ox;
        const float inner=profile.navigatorCorner-8;
        const SDL_Rect step{nav.x+8,nav.y+8,nav.w-16,nav.h-16};
        canvas.paint(nav.x,nav.y,nav.x+nav.w,nav.y+nav.h,[&](float x,float y) {
            return canvas.edge(std::fabs(deviceRoundedDistance(step,inner,x,y))-0.5f)*
                   (0.35f+0.4f*clamp01((nav.y+nav.h-y)/nav.h));
        },[](float,float,Rgb) {return Rgb{44,48,60};});
    }
}

// The m515's metal colour, whether its keys are black (silver otherwise) and the width of the dark
// ring round each app key.
struct M515Look {
    Rgb body;
    bool blackKeys;
    float ring;
};
// m515: satin silver metal with a faint mauve tint. m500: blue-grey silver with black keys.
// WorkPad c500: black with black keys. m505: a lighter, plain silver with thinner rings. m520:
// darker gunmetal.
const M515Look m515Look{Rgb{170,166,174},false,3.8f},m500Look{Rgb{158,163,172},true,3.8f},
               c500Look{Rgb{58,59,63},true,3.8f},m505Look{Rgb{180,181,184},false,3.f},
               m520Look{Rgb{136,138,144},false,3.8f};

void paintM515Controls(Canvas& canvas, const DeviceControlProfile& profile, int width, DeviceKeyMask pressed,
                       const M515Look& look515) {
    const Rgb body=look515.body;
    const float H=profile.height;
    const int ox=(width-profile.width)/2;
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    auto all=[&](float,float) {return 1.f;};
    auto shifted=[&](SDL_Rect r) {r.x+=ox;return r;};
    // Satin silver metal, lit from above.
    canvas.paint(0,0,width,H,all,[&](float x,float y,Rgb) {
        Rgb c=mix(body*1.04f,body*0.78f,std::pow(y/H,1.1f));
        c=c+Rgb{2,2,2}*std::sin(y*7.7f+std::sin(x*0.31f)*0.8f);
        const float t=std::fabs(x-width*0.5f)/(width*0.5f);
        return c*(1-0.22f*t*t);
    });
    // Lower edge of the silkscreen bezel.
    canvas.paint(0,0,width,4,[&](float,float y) {return canvas.edge(y-1.4f);},[](float,float,Rgb) {return Rgb{62,60,66};});
    canvas.paint(0,0,width,6,[&](float,float y) {return canvas.edge(std::fabs(y-2.6f)-1)*0.85f;},
                 [](float,float,Rgb) {return Rgb{226,224,230};});

    // Each key is a silver or black dome in a dark ring; the ring's outer edge is the hit area.
    const SilverKeyLook silver{Rgb{24,24,28},Rgb{224,222,228},Rgb{160,158,166},Rgb{18,18,18},Rgb{214,212,220},
                               Rgb{40,40,46},Rgb{30,30,36},look515.ring,0.7f};
    const SilverKeyLook black{Rgb{16,16,18},Rgb{78,78,84},Rgb{34,34,38},Rgb{12,12,13},Rgb{214,212,220},
                              Rgb{206,208,212},Rgb{130,132,136},look515.ring,0.6f};
    const SilverKeyLook& keys=look515.blackKeys ? black : silver;
    // The scroll keys sit in thinner rings.
    SilverKeyLook scroll=keys;
    scroll.grooveWidth=2.2f;scroll.groove=Rgb{52,51,58};
    auto paintRound=[&](const DeviceButton& button,bool icon) {
        const SDL_Rect hit=shifted(button.bounds);
        const auto& look=icon ? keys : scroll;
        const int g=int(look.grooveWidth+0.5f)+(icon ? 0 : 1);
        const SDL_Rect dome{hit.x+g,hit.y+g,hit.w-2*g,hit.h-2*g};
        paintSilverKey(canvas,dome,dome.w*0.5f,down(button.key),button.icon,1.5f,look,icon);
    };
    for(size_t i=0;i<profile.count;++i) {
        const auto& button=profile.buttons[i];
        if(button.key==keyIdUp || button.key==keyIdDown)continue;
        paintRound(button,true);
    }

    // Scroll keys: two plain domes stacked in a recessed pill housing.
    const DeviceButton* up=nullptr;const DeviceButton* dn=nullptr;
    for(size_t i=0;i<profile.count;++i) {
        if(profile.buttons[i].key==keyIdUp)up=&profile.buttons[i];
        if(profile.buttons[i].key==keyIdDown)dn=&profile.buttons[i];
    }
    if(up && dn) {
        const SDL_Rect a=shifted(up->bounds),b=shifted(dn->bounds);
        const SDL_Rect housing{a.x-5,a.y-5,a.w+10,b.y+b.h-a.y+10};
        const float hr=housing.w*0.5f;
        auto dh=[&](float x,float y) {return deviceRoundedDistance(housing,hr,x,y);};
        const float x0=housing.x-4,y0=housing.y-3,x1=housing.x+housing.w+4,y1=housing.y+housing.h+4;
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
            return canvas.edge(std::fabs(dh(x,y)-1.2f)-0.6f)*clamp01(0.2f+0.8f*(y-housing.y)/housing.h);
        },[](float,float,Rgb) {return Rgb{222,220,228};});
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(dh(x,y));},[&](float,float y,Rgb) {
            const Rgb c=mix(Rgb{104,102,110},Rgb{146,144,152},clamp01((y-housing.y)/housing.h));
        return look515.blackKeys ? mix(c,body,0.5f)*0.8f : c;
        });
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(std::fabs(dh(x,y))-0.5f)*0.7f;},
                     [](float,float,Rgb) {return Rgb{60,59,66};});
        paintRound(*up,false);
        paintRound(*dn,false);
    }
}

// The peanut-shaped IIIx scroll rocker: two round lobes joined by a narrower waist.
float iiixRocker(float x, float y, float cx, float top, float bottom, float grow) {
    const float r=15+grow,waist=8.5f+grow;
    const float a=std::hypot(x-cx,y-top)-r,b=std::hypot(x-cx,y-bottom)-r,c=segment(x,y,cx,top,cx,bottom)-waist;
    // Smooth union rounds the waist into the lobes.
    const float k=9,m=std::min(a,b),h=std::max(k-std::fabs(m-c),0.f)/k;
    return std::min(m,c)-h*h*k*0.25f;
}

// The IIIx and IIIc teal power key, with a small sun for the backlight.
void paintTealPower(Canvas& canvas, const DeviceControlProfile& profile, const DeviceButton* power, int ox,
                    DeviceKeyMask pressed) {
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    auto shifted=[&](SDL_Rect r) {r.x+=ox;return r;};
    const SDL_Rect r=shifted(power->bounds);
    const bool held=down(keyIdPower);
    const float corner=deviceButtonCorner(profile,*power),sink=held ? 0.8f : 0;
    auto dp=[&](float x,float y) {return deviceRoundedDistance(r,corner,x,y);};
    const float x0=r.x-3,y0=r.y-3,x1=r.x+r.w+3,y1=r.y+r.h+4;
    const float cx=r.x+r.w*0.5f,cy=r.y+r.h*0.38f+sink;
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(dp(x,y)-1.2f);},[](float,float,Rgb) {return Rgb{16,17,16};});
    auto face=[&](float x,float y) {return dp(x,y-sink)+0.2f;};
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(face(x,y));},[&](float x,float y,Rgb) {
        const float t=clamp01((y-r.y-sink)/r.h);
        Rgb c=mix(Rgb{44,170,150},Rgb{22,118,104},t);
        c=c+Rgb{30,30,30}*std::pow(clamp01(1-std::fabs(x-cx+1.5f)/4),2.f)*(1-t);
        c=mix(c*0.7f,c,clamp01(-face(x,y)/1.6f));
        return held ? c*(0.6f+0.15f*clamp01(-face(x,y-1.5f)/3)) : c;
    });
    const Rgb ink=held ? Rgb{40,110,98} : Rgb{150,220,206};
    canvas.paint(cx-5,cy-5,cx+5,cy+5,[&](float x,float y) {
        float d=ring(x,y,cx,cy,1.5f)-0.5f;
        for(int n=0;n<8;++n) {
            const float c=std::cos(degrees(45*n)),sn=std::sin(degrees(45*n));
            d=std::min(d,segment(x,y,cx+2.9f*c,cy+2.9f*sn,cx+4*c,cy+4*sn)-0.4f);
        }
        return canvas.edge(d);
    },[&](float,float,Rgb) {return ink;});
}

// Body plastic: IIIx dark charcoal, IIIe the same a hair warmer, IIIxe a faintly cooler slate.
const Rgb iiixBody{60,64,61},iiieBody{67,64,57},iiixeBody{61,65,68};

void paintIIIxControls(Canvas& canvas, const DeviceControlProfile& profile, int width, DeviceKeyMask pressed,
                       Rgb body) {
    const float H=profile.height;
    const int ox=(width-profile.width)/2;
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    auto all=[&](float,float) {return 1.f;};
    auto shifted=[&](SDL_Rect r) {r.x+=ox;return r;};
    // Textured plastic, lit from above.
    canvas.paint(0,0,width,H,all,[&](float x,float y,Rgb) {
        Rgb c=mix(body*1.08f,body*0.78f,std::pow(y/H,1.2f));
        c=c+Rgb{7,7,7}*speckle(x,y);
        const float t=std::fabs(x-width*0.5f)/(width*0.5f);
        return c*(1-0.16f*t*t);
    });
    // The lower edge of the screen window, then the rounded lip of the bezel.
    canvas.paint(0,0,width,3,[&](float,float y) {return canvas.edge(y-1.2f);},[](float,float,Rgb) {return Rgb{18,19,18};});
    canvas.paint(0,0,width,9,[&](float,float y) {return canvas.edge(std::fabs(y-3.6f)-1.2f)*0.55f;},
                 [](float,float,Rgb) {return Rgb{104,109,105};});

    for(size_t i=0;i<profile.count;++i) {
        const auto& button=profile.buttons[i];
        if(button.key==keyIdUp || button.key==keyIdDown || button.key==keyIdPower)continue;
        const bool held=down(button.key);
        // Dark oval pocket, the hit area, lit along its lower outer lip.
        const SDL_Rect pocket=shifted(button.bounds);
        const float corner=deviceButtonCorner(profile,button);
        auto dp=[&](float x,float y) {return deviceRoundedDistance(pocket,corner,x,y);};
        const float x0=pocket.x-3,y0=pocket.y-3,x1=pocket.x+pocket.w+3,y1=pocket.y+pocket.h+4;
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
            return canvas.edge(std::fabs(dp(x,y)-0.9f)-0.7f)*clamp01((y-pocket.y)/pocket.h-0.15f)*0.8f;
        },[](float,float,Rgb) {return Rgb{104,109,105};});
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(dp(x,y));},[&](float x,float y,Rgb) {
            return mix(Rgb{24,26,25},Rgb{38,41,39},clamp01((y-pocket.y)/pocket.h))+Rgb{4,4,4}*speckle(x,y);
        });
        // Grey dome; holding it presses it into the pocket.
        const float sink=held ? 0.8f : 0;
        const SDL_Rect key{pocket.x+5,pocket.y+4,pocket.w-10,pocket.h-8};
        const float kcorner=std::min(key.w,key.h)*0.5f,kcx=key.x+key.w*0.5f,kcy=key.y+key.h*0.5f+sink;
        auto dk=[&](float x,float y) {return deviceRoundedDistance(key,kcorner,x,y-sink);};
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(dk(x,y-1.6f))*(held?0.3f:0.75f);},
                     [](float,float,Rgb) {return Rgb{8,9,8};});
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(dk(x,y));},[&](float x,float y,Rgb) {
            const float r=std::hypot((x-kcx+key.w*0.12f)/(key.w*0.5f),(y-kcy+key.h*0.22f)/(key.h*0.55f));
            Rgb c=Rgb{50,54,52}+Rgb{34,36,35}*std::pow(clamp01(1-r),1.6f)+Rgb{3,3,3}*speckle(x,y);
            c=mix(Rgb{30,32,31},c,clamp01(-dk(x,y)/3));
            // Held: darker, with a shadow under the pocket's top lip.
            return held ? c*(0.55f+0.2f*clamp01(-dk(x,y-1.8f)/3)) : c;
        });
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
            return canvas.edge(std::fabs(dk(x,y)+0.8f)-0.35f)*(held?0.1f:0.45f)*clamp01((kcy-y)/(key.h*0.5f));
        },[](float,float,Rgb) {return Rgb{116,121,117};});
        const Rgb ink=held ? Rgb{110,114,110} : Rgb{196,200,194};
        const float k=1.65f,reach=11*k;
        canvas.paint(kcx-reach,kcy-reach,kcx+reach,kcy+reach,[&](float x,float y) {
            return canvas.edge(iconDistance(button.icon,(x-kcx)/k,(y-kcy)/k)*k-0.6f*k);
        },[&](float,float,Rgb) {return ink;});
    }

    // Scroll rocker: one peanut-shaped key in a matching groove, its top lobe rocking up and its
    // bottom lobe down.
    const DeviceButton* up=nullptr;const DeviceButton* dn=nullptr;const DeviceButton* power=nullptr;
    for(size_t i=0;i<profile.count;++i) {
        if(profile.buttons[i].key==keyIdUp)up=&profile.buttons[i];
        if(profile.buttons[i].key==keyIdDown)dn=&profile.buttons[i];
        if(profile.buttons[i].key==keyIdPower)power=&profile.buttons[i];
    }
    if(up && dn) {
        const SDL_Rect a=shifted(up->bounds),b=shifted(dn->bounds);
        const float cx=a.x+a.w*0.5f,top=a.y+15.5f,bottom=b.y+b.h-15.5f,split=b.y;
        const bool upHeld=down(keyIdUp),dnHeld=down(keyIdDown);
        auto dr=[&](float x,float y,float grow) {return iiixRocker(x,y,cx,top,bottom,grow);};
        // How far a point belongs to the held lobe, blended across the waist.
        auto heldAt=[&](float y) {
            const float t=clamp01(0.5f+(y-split)/6);
            return (1-t)*float(upHeld)+t*float(dnHeld);
        };
        const float x0=cx-22,y0=a.y-2,x1=cx+22,y1=b.y+b.h+2;
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
            return canvas.edge(std::fabs(dr(x,y,1.2f))-0.6f)*clamp01((y-a.y)/(b.y+b.h-a.y))*0.7f;
        },[](float,float,Rgb) {return Rgb{100,105,101};});
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(dr(x,y,0.4f));},[](float,float,Rgb) {return Rgb{16,17,16};});
        auto face=[&](float x,float y) {return dr(x,y-0.7f*heldAt(y),-1.4f);};
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(face(x,y));},[&](float x,float y,Rgb) {
            // Each lobe is domed with its own highlight; the waist dips.
            const float ly=y<split ? top : bottom;
            const float r=std::hypot((x-cx+3)/15,(y-ly+5)/15);
            Rgb c=Rgb{54,58,56}+Rgb{36,38,37}*std::pow(clamp01(1-r),1.6f)+Rgb{3,3,3}*speckle(x,y);
            c=c*(1-0.25f*std::exp(-std::pow((y-split)/5,2.f)));
            c=mix(Rgb{30,32,31},c,clamp01(-face(x,y)/3));
            return c*(1-0.42f*heldAt(y));
        });
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
            const float ly=y<split ? top : bottom;
            return canvas.edge(std::fabs(face(x,y)+0.8f)-0.35f)*0.45f*clamp01((ly-y)/10)*(1-0.8f*heldAt(y));
        },[](float,float,Rgb) {return Rgb{120,125,121};});
        // Small moulded chevrons on the lobes.
        for(int i=0;i<2;++i) {
            const float dir=i ? 1 : -1,yy=(i ? bottom : top)+dir*6.5f+0.7f*float(i ? dnHeld : upHeld);
            canvas.paint(cx-5,yy-4,cx+5,yy+4,[&](float x,float y) {
                return canvas.edge(std::min(segment(x,y,cx-2.6f,yy-dir*1.3f,cx,yy+dir*1.3f),
                                            segment(x,y,cx+2.6f,yy-dir*1.3f,cx,yy+dir*1.3f))-0.5f)*0.6f;
            },[](float,float,Rgb) {return Rgb{118,123,119};});
        }
    }

    // Teal power key at the far left.
    if(power)paintTealPower(canvas,profile,power,ox,pressed);
}

// A face with keys sunk in recesses, lit from above: its colour, the light along the bezel
// chamfer and recess edges, the recess walls in shadow and lit, and the flake in the finish.
// edge is a body colour round the face's sides and bottom, edgeWidth units wide (none at 0);
// plainScroll leaves the dimples off the scroll keys.
struct RecessFaceLook {
    Rgb face, chamfer, chamferHighlight, chamferShadow, bezel, foot, rollover, wallDark, wallLit, lip;
    float flake, sheen;
    Rgb edge{};
    float edgeWidth{0};
    bool plainScroll{false};
};
// Palm m125: blue-grey metallic face with a fine flake.
const RecessFaceLook m125Look{Rgb{148,155,168},Rgb{222,227,236},Rgb{252,253,255},Rgb{74,80,92},Rgb{34,37,43},
                              Rgb{40,43,50},Rgb{236,240,248},Rgb{58,63,74},Rgb{204,210,222},Rgb{244,247,252},7,12};
// Palm m100: charcoal plastic with a faint texture; dimmer light on the recess edges; plain
// scroll keys.
const RecessFaceLook m100Look{Rgb{66,68,72},Rgb{112,116,122},Rgb{140,144,150},Rgb{30,31,34},Rgb{18,19,21},
                              Rgb{22,23,25},Rgb{118,122,128},Rgb{20,21,24},Rgb{92,96,102},Rgb{132,136,142},6,5,
                              Rgb{},0,true};
// Palm m130: a pale, nearly white silver face in a blue body.
const RecessFaceLook m130Look{Rgb{208,212,218},Rgb{236,239,244},Rgb{253,254,255},Rgb{110,116,128},Rgb{34,38,46},
                              Rgb{42,46,56},Rgb{246,248,252},Rgb{84,90,102},Rgb{222,226,234},Rgb{250,251,254},5,10,
                              Rgb{58,104,150},5};
// Palm m105: the m100's plastic a step lighter.
const RecessFaceLook m105Look{Rgb{94,96,100},Rgb{126,130,136},Rgb{152,156,162},Rgb{36,37,40},Rgb{18,19,21},
                              Rgb{26,27,30},Rgb{130,134,140},Rgb{24,25,28},Rgb{104,108,114},Rgb{144,148,154},6,5,
                              Rgb{},0,true};
// Palm V: darker, warmer pewter. Vx: lighter, neutral silver.
const RecessFaceLook vLook{Rgb{124,118,111},Rgb{196,190,182},Rgb{236,232,226},Rgb{70,66,62},Rgb{36,34,32},
                           Rgb{50,47,44},Rgb{214,208,200},Rgb{54,51,48},Rgb{180,174,166},Rgb{226,221,214},4,9};
const RecessFaceLook vxLook{Rgb{174,177,182},Rgb{226,228,232},Rgb{252,253,255},Rgb{92,94,100},Rgb{40,42,46},
                           Rgb{60,62,66},Rgb{240,242,246},Rgb{70,72,78},Rgb{214,216,222},Rgb{248,249,252},4,10};

// A recess in a face of any shape d (negative inside), from top to bottom: its upper wall in
// shadow, its lower wall lit, a bright lip along the bottom edge.
template <class Distance>
void paintRecess(Canvas& canvas, const RecessFaceLook& look, float x0, float y0, float x1, float y1, float top,
                 float bottom, Distance d) {
    // The face rolling over into the recess catches light along the top.
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
        return canvas.edge(std::fabs(d(x,y)-0.6f)-0.5f)*0.55f*clamp01(1-(y-top)/(bottom-top)*1.6f);
    },[&](float,float,Rgb) {return look.rollover;});
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(d(x,y));},[&](float x,float y,Rgb) {
        const float t=clamp01((y-top)/(bottom-top)),lit=t*t*(3-2*t);
        Rgb c=mix(look.wallDark,look.wallLit,clamp01((lit-0.25f)/0.65f));
        // The wall darkens down towards the floor.
        c=c*(1-0.3f*clamp01(-d(x,y)/7));
        return c+Rgb{5,5,6}*speckle(x,y);
    });
    // A bright lip along the lower edge where the wall meets the face.
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
        return canvas.edge(std::fabs(d(x,y)+0.9f)-0.45f)*clamp01(((y-top)/(bottom-top)-0.55f)*2.6f);
    },[&](float,float,Rgb) {return look.lip;});
}

// A black matte key of shape d at rest (negative inside), in a recess: a dark gap round it, a
// domed face lit from the upper left and a fine edge highlight along the top. Held keys sink and
// darken. Returns nothing; icons go on top.
template <class Distance>
void paintBlackKey(Canvas& canvas, const SDL_Rect& key, bool held, Distance d) {
    const float sink=held ? 0.8f : 0;
    const float x0=key.x-3,y0=key.y-3,x1=key.x+key.w+3,y1=key.y+key.h+4;
    const float kcx=key.x+key.w*0.5f,kcy=key.y+key.h*0.5f+sink;
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(d(x,y)-1.5f);},
                 [](float,float,Rgb) {return Rgb{12,13,15};});
    auto face=[&](float x,float y) {return d(x,y-sink);};
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(face(x,y));},[&](float x,float y,Rgb) {
        const float r=std::hypot((x-kcx+key.w*0.12f)/(key.w*0.55f),(y-kcy+key.h*0.24f)/(key.h*0.6f));
        Rgb c=Rgb{36,37,40}+Rgb{36,37,41}*std::pow(clamp01(1-r),1.7f)+Rgb{3,3,3}*speckle(x,y);
        c=mix(Rgb{18,19,21},c,clamp01(-face(x,y)/3.2f));
        // Held: darker, with a shadow under the recess's top edge.
        return held ? c*(0.55f+0.2f*clamp01(-face(x,y-1.8f)/3)) : c;
    });
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
        return canvas.edge(std::fabs(face(x,y)+0.8f)-0.35f)*(held?0.12f:0.5f)*clamp01((kcy-y)/(key.h*0.5f));
    },[](float,float,Rgb) {return Rgb{118,122,130};});
}

// Two round lobes joined by a narrower waist, for the m125 scroll well.
float m125Peanut(float x, float y, float cx, float top, float bottom, float r, float waist) {
    const float a=std::hypot(x-cx,y-top)-r,b=std::hypot(x-cx,y-bottom)-r,c=segment(x,y,cx,top,cx,bottom)-waist;
    // Smooth union rounds the waist into the lobes.
    const float k=10,m=std::min(a,b),h=std::max(k-std::fabs(m-c),0.f)/k;
    return std::min(m,c)-h*h*k*0.25f;
}

// App keys: a rounded pocket, the hit area, with a black key inset units in and a light icon
// scaled by k.
void paintRecessedAppKeys(Canvas& canvas, const DeviceControlProfile& profile, int ox, DeviceKeyMask pressed,
                          const RecessFaceLook& look, int inset=6, float k=1.22f) {
    for(size_t i=0;i<profile.count;++i) {
        const auto& button=profile.buttons[i];
        if(button.key==keyIdUp || button.key==keyIdDown)continue;
        const bool held=(pressed&deviceKeyBit(button.key))!=0;
        SDL_Rect pocket=button.bounds;pocket.x+=ox;
        const float pr=deviceButtonCorner(profile,button);
        auto dp=[&](float x,float y) {return deviceRoundedDistance(pocket,pr,x,y);};
        paintRecess(canvas,look,pocket.x-3,pocket.y-3,pocket.x+pocket.w+3,pocket.y+pocket.h+3,
                    float(pocket.y),float(pocket.y+pocket.h),dp);
        const SDL_Rect key{pocket.x+inset,pocket.y+inset,pocket.w-2*inset,pocket.h-2*inset};
        const float kr=std::min(key.w,key.h)*0.5f;
        paintBlackKey(canvas,key,held,[&](float x,float y) {return deviceRoundedDistance(key,kr,x,y);});
        const float kcx=key.x+key.w*0.5f,kcy=key.y+key.h*0.5f+(held ? 0.8f : 0);
        const Rgb ink=held ? Rgb{112,116,122} : Rgb{204,208,214};
        const float reach=11*k;
        canvas.paint(kcx-reach,kcy-reach,kcx+reach,kcy+reach,[&](float x,float y) {
            return canvas.edge(iconDistance(button.icon,(x-kcx)/k,(y-kcy)/k)*k-0.6f*k);
        },[&](float,float,Rgb) {return ink;});
    }
}

// The face under the silkscreen window: lit from above, darker to the bottom and the sides, then
// the window's lower edge and the bright chamfer of the face.
void paintRecessFace(Canvas& canvas, const RecessFaceLook& look, int width, float H) {
    canvas.paint(0,0,width,H,[](float,float) {return 1.f;},[&](float x,float y,Rgb) {
        Rgb c=mix(look.face*1.08f,look.face*0.7f,std::pow(y/H,1.2f));
        c=c+Rgb{1,1,1.05f}*look.sheen*std::exp(-std::pow((y-10)/6,2.f));
        c=c+Rgb{1,1,1.1f}*look.flake*speckle(x,y);
        const float t=std::fabs(x-width*0.5f)/(width*0.5f);
        return c*(1-0.2f*t*t);
    });
    canvas.paint(0,0,width,3,[&](float,float y) {return canvas.edge(y-1.2f);},[&](float,float,Rgb) {return look.bezel;});
    canvas.paint(0,0,width,8,[&](float,float y) {return canvas.edge(std::fabs(y-3.4f)-1.8f)*0.85f;},
                 [&](float,float,Rgb) {return look.chamfer;});
    canvas.paint(0,0,width,8,[&](float,float y) {return canvas.edge(std::fabs(y-2.1f)-0.5f)*0.85f;},
                 [&](float,float,Rgb) {return look.chamferHighlight;});
    canvas.paint(0,4,width,9,[&](float,float y) {return canvas.edge(std::fabs(y-5.9f)-0.5f)*0.3f;},
                 [&](float,float,Rgb) {return look.chamferShadow;});
    canvas.paint(0,H-3,width,H,[&](float,float y) {return canvas.edge(std::fabs(y-H+0.8f)-0.8f)*0.5f;},
                 [&](float,float,Rgb) {return look.foot;});
    if(look.edgeWidth<=0)return;
    // The body round the face: the face's rounded outline, then the body colour outside it, lit
    // along the face's rim.
    const float e=look.edgeWidth;
    const SDL_Rect face{int(e),-40,int(width-2*e),int(H+40-e)};
    auto d=[&](float x,float y) {return deviceRoundedDistance(face,18,x,y);};
    canvas.paint(0,0,width,H,[&](float x,float y) {return canvas.edge(-d(x,y));},[&](float x,float y,Rgb) {
        return mix(look.edge*1.15f,look.edge*0.7f,clamp01(d(x,y)/e))+Rgb{3,3,4}*speckle(x,y);
    });
    canvas.paint(0,0,width,H,[&](float x,float y) {return canvas.edge(std::fabs(d(x,y)-0.5f)-0.5f)*0.7f;},
                 [&](float,float,Rgb) {return look.chamferHighlight;});
}

void paintM125Controls(Canvas& canvas, const DeviceControlProfile& profile, int width, DeviceKeyMask pressed,
                       const RecessFaceLook& look) {
    const float H=profile.height;
    const int ox=(width-profile.width)/2;
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    auto shifted=[&](SDL_Rect r) {r.x+=ox;return r;};
    paintRecessFace(canvas,look,width,H);

    paintRecessedAppKeys(canvas,profile,ox,pressed,look);

    // Scroll keys: two black keys, round at their outer ends and squarer where they meet, each
    // with a dimple, in one peanut-shaped well.
    const DeviceButton* up=nullptr;const DeviceButton* dn=nullptr;
    for(size_t i=0;i<profile.count;++i) {
        if(profile.buttons[i].key==keyIdUp)up=&profile.buttons[i];
        if(profile.buttons[i].key==keyIdDown)dn=&profile.buttons[i];
    }
    if(!up || !dn)return;
    const SDL_Rect a=shifted(up->bounds),b=shifted(dn->bounds);
    const float cx=a.x+a.w*0.5f,lobe=a.w*0.5f,top=a.y+lobe,bottom=b.y+b.h-lobe;
    auto dw=[&](float x,float y) {return m125Peanut(x,y,cx,top,bottom,lobe+6,lobe);};
    const float wy0=a.y-6,wy1=b.y+b.h+6;
    paintRecess(canvas,look,cx-lobe-11,wy0-3,cx+lobe+11,wy1+3,wy0,wy1,dw);
    for(const auto* button:{up,dn}) {
        const SDL_Rect key=shifted(button->bounds);
        const bool held=down(button->key),isUp=button==up;
        // Round outer end, small corners at the inner end.
        const float outer=key.w*0.5f-0.5f,inner=5;
        auto dk=[&](float x,float y) {
            const bool upperHalf=y<key.y+key.h*0.5f;
            return deviceRoundedDistance(key,upperHalf==isUp ? outer : inner,x,y);
        };
        paintBlackKey(canvas,key,held,dk);
        if(look.plainScroll)continue;
        // A small dimple, shaded the other way from the key: dark at the top, lit at the bottom.
        const float sink=held ? 0.8f : 0,dcx=key.x+key.w*0.5f,dcy=key.y+key.h*0.5f+(isUp ? -1.5f : 1.5f)+sink;
        canvas.paint(dcx-6,dcy-6,dcx+6,dcy+6,[&](float x,float y) {return canvas.edge(std::hypot(x-dcx,y-dcy)-3.6f);},
                     [&](float,float y,Rgb) {
            const Rgb c=mix(Rgb{12,13,15},Rgb{46,48,53},clamp01((y-dcy+3.6f)/7.2f));
            return held ? c*0.7f : c;
        });
        canvas.paint(dcx-6,dcy-6,dcx+6,dcy+6,[&](float x,float y) {
            return canvas.edge(std::fabs(std::hypot(x-dcx,y-dcy)-3.4f)-0.35f)*(held?0.1f:0.4f)*clamp01((y-dcy)/3.6f);
        },[](float,float,Rgb) {return Rgb{110,114,122};});
    }
}

// The Palm V rocker: a tall rounded key pinched in at its waist, d negative inside.
float vRocker(float x, float y, const SDL_Rect& r, float corner, float pinch) {
    const float cy=r.y+r.h*0.5f;
    return deviceRoundedDistance(r,corner,x,y)+pinch*std::exp(-std::pow((y-cy)/(r.h*0.2f),2.f));
}

void paintVControls(Canvas& canvas, const DeviceControlProfile& profile, int width, DeviceKeyMask pressed,
                    const RecessFaceLook& vLook) {
    const float H=profile.height;
    const int ox=(width-profile.width)/2;
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    auto shifted=[&](SDL_Rect r) {r.x+=ox;return r;};
    paintRecessFace(canvas,vLook,width,H);
    // Small oval keys in thin collars.
    paintRecessedAppKeys(canvas,profile,ox,pressed,vLook,3,1.0f);

    // Rocker: one narrow black pill in a matching recess, a little waisted, its upper half rocking
    // up and its lower half down, with a fine seam across the waist.
    const DeviceButton* up=nullptr;const DeviceButton* dn=nullptr;
    for(size_t i=0;i<profile.count;++i) {
        if(profile.buttons[i].key==keyIdUp)up=&profile.buttons[i];
        if(profile.buttons[i].key==keyIdDown)dn=&profile.buttons[i];
    }
    if(!up || !dn)return;
    const SDL_Rect shape=shifted(up->shape),b=shifted(dn->bounds);
    const float corner=deviceButtonCorner(profile,*up),split=float(b.y);
    auto well=[&](float x,float y) {return vRocker(x,y,shape,corner,1.2f)-0.5f;};
    paintRecess(canvas,vLook,shape.x-4,shape.y-4,shape.x+shape.w+4,shape.y+shape.h+4,
                float(shape.y),float(shape.y+shape.h),well);
    const SDL_Rect key{shape.x+3,shape.y+3,shape.w-6,shape.h-6};
    for(int half=0;half<2;++half) {
        const SDL_Rect r=half ? SDL_Rect{key.x,int(split),key.w,key.y+key.h-int(split)} : SDL_Rect{key.x,key.y,key.w,int(split)-key.y};
        const bool held=down(half ? keyIdDown : keyIdUp);
        paintBlackKey(canvas,r,held,[&](float x,float y) {
            const float d=vRocker(x,y,key,corner-3,1.2f);
            return std::max(d,half ? split+0.4f-y : y-split+0.4f);
        });
    }
}

void paintIIIcControls(Canvas& canvas, const DeviceControlProfile& profile, int width, DeviceKeyMask pressed) {
    const float H=profile.height;
    const int ox=(width-profile.width)/2;
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    auto shifted=[&](SDL_Rect r) {r.x+=ox;return r;};
    // Blue-black textured plastic, lit from above.
    const Rgb body{44,47,57};
    canvas.paint(0,0,width,H,[](float,float) {return 1.f;},[&](float x,float y,Rgb) {
        Rgb c=mix(body*1.1f,body*0.75f,std::pow(y/H,1.2f));
        c=c+Rgb{6,6,7}*speckle(x,y);
        const float t=std::fabs(x-width*0.5f)/(width*0.5f);
        return c*(1-0.16f*t*t);
    });
    canvas.paint(0,0,width,3,[&](float,float y) {return canvas.edge(y-1.2f);},[](float,float,Rgb) {return Rgb{14,15,19};});
    canvas.paint(0,0,width,9,[&](float,float y) {return canvas.edge(std::fabs(y-3.6f)-1.2f)*0.5f;},
                 [](float,float,Rgb) {return Rgb{92,97,110};});

    // Dark oval keys, each in a matte dark grey collar.
    const DeviceButton* up=nullptr;const DeviceButton* dn=nullptr;const DeviceButton* power=nullptr;
    for(size_t i=0;i<profile.count;++i) {
        const auto& button=profile.buttons[i];
        if(button.key==keyIdUp)up=&button;
        if(button.key==keyIdDown)dn=&button;
        if(button.key==keyIdPower)power=&button;
        if(button.key==keyIdUp || button.key==keyIdDown || button.key==keyIdPower)continue;
        const bool held=down(button.key);
        const SDL_Rect pocket=shifted(button.bounds);
        const float pr=deviceButtonCorner(profile,button);
        auto dp=[&](float x,float y) {return deviceRoundedDistance(pocket,pr,x,y);};
        const float x0=pocket.x-3,y0=pocket.y-3,x1=pocket.x+pocket.w+3,y1=pocket.y+pocket.h+4;
        // The collar, a little lighter along the bottom, with a faint lit lip.
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(dp(x,y));},[&](float x,float y,Rgb) {
            return mix(Rgb{50,53,62},Rgb{66,70,80},clamp01((y-pocket.y)/pocket.h))+Rgb{4,4,5}*speckle(x,y);
        });
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
            return canvas.edge(std::fabs(dp(x,y)+0.6f)-0.4f)*0.5f*clamp01((y-pocket.y)/pocket.h-0.3f);
        },[](float,float,Rgb) {return Rgb{96,101,114};});
        const SDL_Rect key{pocket.x+5,pocket.y+5,pocket.w-10,pocket.h-10};
        const float kr=std::min(key.w,key.h)*0.5f;
        paintBlackKey(canvas,key,held,[&](float x,float y) {return deviceRoundedDistance(key,kr,x,y);});
        const float kcx=key.x+key.w*0.5f,kcy=key.y+key.h*0.5f+(held ? 0.8f : 0);
        const Rgb ink=held ? Rgb{108,114,124} : Rgb{198,204,214};
        const float k=1.3f,reach=11*k;
        canvas.paint(kcx-reach,kcy-reach,kcx+reach,kcy+reach,[&](float x,float y) {
            return canvas.edge(iconDistance(button.icon,(x-kcx)/k,(y-kcy)/k)*k-0.6f*k);
        },[&](float,float,Rgb) {return ink;});
    }

    // Scroll keys: two small separate oval pebbles, one over the other, each in a thin collar.
    if(up && dn) {
        for(const auto* button:{up,dn}) {
            const SDL_Rect key=shifted(button->bounds);
            const float kr=deviceButtonCorner(profile,*button);
            auto dk=[&](float x,float y) {return deviceRoundedDistance(key,kr,x,y);};
            canvas.paint(key.x-4,key.y-4,key.x+key.w+4,key.y+key.h+4,[&](float x,float y) {return canvas.edge(dk(x,y)-2.2f);},
                         [&](float x,float y,Rgb) {return Rgb{54,57,66}+Rgb{4,4,5}*speckle(x,y);});
            paintBlackKey(canvas,key,down(button->key),dk);
        }
    }
    if(power)paintTealPower(canvas,profile,power,ox,pressed);
}

// Glyphs on the silkscreen discs, filled: signed distance in LCD pixels from the disc centre.
float silkscreenDiscGlyph(SilkscreenIcon icon, float x, float y) {
    switch(icon) {
        case SilkscreenIcon::home: {
            // A house seen from the front left: gable end, roof running back, side wall with a door.
            const std::array<std::array<float,2>,5> gable{{{-12,1},{-5,-7},{2,1},{2,11},{-12,11}}};
            const std::array<std::array<float,2>,4> roof{{{-4,-9},{6,-12},{13,-4},{3,-1}}};
            const std::array<std::array<float,2>,4> side{{{4,1},{12,-2},{12,8},{4,11}}};
            float d=std::min({polygon(x,y,gable),polygon(x,y,roof),polygon(x,y,side),box(x,y,8,-14,10.5f,-8)});
            d=std::max(d,-(segment(x,y,-6,-7.6f,3.6f,-0.2f)-0.8f));
            d=std::max(d,-box(x,y,6.2f,3,9.4f,10));
            return std::max(d,-box(x,y,-8,3,-4,7));
        }
        case SilkscreenIcon::menu: {
            // A pull-down: a title bar with a small arrow over a list.
            const std::array<std::array<float,2>,3> arrow{{{-8.2f,-8.4f},{-4,-8.4f},{-6.1f,-6.2f}}};
            float d=std::max(box(x,y,-10,-10.5f,10,-4.5f),-polygon(x,y,arrow));
            float list=box(x,y,-7,-2.5f,7,10.5f);
            for(float row:{0.9f,4.3f,7.7f})list=std::max(list,-box(x,y,-8,row-0.6f,8,row+0.6f));
            return std::min(d,list);
        }
        case SilkscreenIcon::favorites: {
            std::array<std::array<float,2>,10> p;
            for(int i=0;i<10;++i) {
                const float a=degrees(-90+36*i),r=i%2 ? 5 : 11.5f;
                p[i]={r*std::cos(a),r*std::sin(a)+1};
            }
            return polygon(x,y,p);
        }
        case SilkscreenIcon::calc: {
            // Four rounded keys in a square, with plus, divide, times and equals cut out.
            float d=1e9f;
            for(float kx:{-5.4f,5.4f})for(float ky:{-5.4f,5.4f})d=std::min(d,box(x,y,kx-2.6f,ky-2.6f,kx+2.6f,ky+2.6f)-2);
            auto cut=[&](float v) {d=std::max(d,-(v-0.75f));};
            cut(std::min(segment(x,y,-8,-5.4f,-2.8f,-5.4f),segment(x,y,-5.4f,-8,-5.4f,-2.8f)));
            cut(std::min({segment(x,y,2.8f,-5.4f,8,-5.4f),std::hypot(x-5.4f,y+7.9f)-0.3f,std::hypot(x-5.4f,y+2.9f)-0.3f}));
            cut(std::min(segment(x,y,-7.4f,3.4f,-3.4f,7.4f),segment(x,y,-7.4f,7.4f,-3.4f,3.4f)));
            cut(std::min(segment(x,y,2.8f,4.1f,8,4.1f),segment(x,y,2.8f,6.7f,8,6.7f)));
            return d;
        }
        case SilkscreenIcon::find:
            return std::min(ring(x,y,2.5f,-2.5f,6.3f)-1.8f,segment(x,y,-2.6f,2.6f,-8.6f,8.6f)-2.3f);
        default: return 1e9f;
    }
}

// Printed silkscreen colours and sizes; mark scales the clock and brightness glyphs and label the
// abc and 123 labels (mark when 0).
struct SilkscreenLook {
    Rgb panelTop, panelBottom, edge, ink, discTop, discBottom, glyph;
    float vignette, line, discInset, mark, label{0};
};

void paintT415Silkscreen(Canvas& canvas, const DeviceSilkscreen& silk);

void paintSilkscreen(Canvas& canvas, const DeviceSilkscreen& silk) {
    if(silk.style==SilkscreenStyle::t415)return paintT415Silkscreen(canvas,silk);
    const float W=silk.width,H=silk.height;
    // E2: dark grey print with light grey discs. Zire 72: a bluer grey with silver discs.
    // m515: light grey-green print with pale discs and one Graffiti box. IIIx: pale speckled
    // sage with dark discs and a narrower box. m125: pale grey-green with black discs, the m515's
    // box and corner glyphs, its labels set at a slant. V: greener, with the IIIx's box and the
    // m125's slanted labels. m500: the m515's layout printed light with dark discs. Every label is
    // small and set at a slant, abc falling to the right and 123 rising.
    const bool iiix=silk.style==SilkscreenStyle::iiix,m125=silk.style==SilkscreenStyle::m125;
    // IIIc: printed light on a dark panel, with the IIIx's box and smaller discs.
    const bool v=silk.style==SilkscreenStyle::v,iiic=silk.style==SilkscreenStyle::iiic;
    const bool m500=silk.style==SilkscreenStyle::m500,m515=silk.style==SilkscreenStyle::m515 || m500;
    const bool narrow=iiix || v || iiic,slanted=m125 || v || iiix || iiic || m515,iiixLabels=iiix || iiic;
    const SilkscreenLook look=
        iiic ? SilkscreenLook{Rgb{62,65,74},Rgb{54,57,66},Rgb{24,26,30},Rgb{196,200,208},
                              Rgb{214,218,224},Rgb{188,192,200},Rgb{44,47,56},0.07f,0.8f,12,1.25f,0.85f} :
        m500 ? SilkscreenLook{Rgb{184,192,182},Rgb{172,180,170},Rgb{70,74,68},Rgb{34,37,34},
                              Rgb{58,61,60},Rgb{30,32,32},Rgb{196,202,192},0.06f,1,2,1.4f,1} :
        v ? SilkscreenLook{Rgb{182,192,166},Rgb{170,180,154},Rgb{56,60,54},Rgb{26,28,25},
                           Rgb{42,45,41},Rgb{20,22,20},Rgb{188,198,176},0.06f,1,6,1.3f} :
        m125 ? SilkscreenLook{Rgb{178,186,178},Rgb{166,174,166},Rgb{56,60,57},Rgb{24,26,25},
                              Rgb{46,49,48},Rgb{20,22,21},Rgb{186,194,186},0.06f,1,2,1.4f} :
        iiix ? SilkscreenLook{Rgb{178,188,164},Rgb{166,176,152},Rgb{60,64,58},Rgb{34,37,34},
                              Rgb{58,62,60},Rgb{34,37,36},Rgb{188,198,176},0.07f,0.8f,6,1.25f,0.85f} :
        silk.style==SilkscreenStyle::zire72 ? SilkscreenLook{Rgb{82,87,100},Rgb{74,79,92},Rgb{36,39,46},Rgb{178,183,194},
                                                             Rgb{196,200,208},Rgb{170,175,184},Rgb{50,54,64},0.08f,0.5f,4,1}
      : m515 ? SilkscreenLook{Rgb{132,139,128},Rgb{122,129,118},Rgb{70,74,68},Rgb{236,240,232},
                              Rgb{228,231,224},Rgb{204,208,200},Rgb{70,75,68},0.06f,1,2,1.4f,1}
      : SilkscreenLook{Rgb{68,72,76},Rgb{62,66,70},Rgb{38,40,43},Rgb{158,162,165},
                       Rgb{172,175,177},Rgb{156,159,161},Rgb{72,76,80},0.06f,0.45f,4,1};
    const bool oneBox=m515 || narrow || m125;
    const Rgb ink=look.ink;
    // Printed panel over the digitizer.
    canvas.paint(0,0,W,H,[](float,float) {return 1.f;},[&](float x,float y,Rgb) {
        Rgb c=mix(look.panelTop,look.panelBottom,y/H);
        if(iiix || iiic || slanted)c=c+Rgb{12,12,12}*(iiix ? 1 : 0.5f)*speckle(x,y);
        const float t=std::fabs(x-W*0.5f)/(W*0.5f);
        return c*(1-look.vignette*t*t);
    });
    canvas.paint(0,0,W,1.5f*silk.unit,[&](float,float y) {return canvas.edge(y-0.6f*silk.unit);},[&](float,float,Rgb) {return look.edge;});
    // Graffiti box, the printed bounds of the letter and number areas.
    // Box lines on odd units sit on LCD pixel centres at 2 units per pixel.
    const float inset=narrow ? -7 : 5;
    const float bx0=silk.letters.x-inset,bx1=silk.numbers.x+silk.numbers.w+inset,by0=3,by1=H-3,bc=narrow ? 6 : 9;
    if(oneBox) {
        // One rounded box with triangles at the top and bottom marking the split.
        const SDL_Rect frame{int(bx0),int(by0),int(bx1-bx0),int(by1-by0)};
        const float split=silk.numbers.x;
        const std::array<std::array<float,2>,3> top{{{split-4.5f,by0},{split+4.5f,by0},{split,by0+6}}};
        const std::array<std::array<float,2>,3> bottom{{{split-4.5f,by1},{split+4.5f,by1},{split,by1-6}}};
        canvas.paint(0,0,W,H,[&](float x,float y) {
            return canvas.edge(std::min({std::fabs(deviceRoundedDistance(frame,bc,x,y))-look.line,
                                         polygon(x,y,top),polygon(x,y,bottom)}));
        },[&](float,float,Rgb) {return ink;});
    } else {
        // Thin lines between the button columns and the Graffiti areas, and ticks splitting
        // letters from numbers at the top and bottom.
        auto lines=[&](float x,float y) {
            // Lines sit on LCD pixel centres so they stay sharp at normal size.
            const float split=silk.numbers.x+0.5f;
            const float tick=std::min(box(x,y,split-look.line,0,split+look.line,13),box(x,y,split-look.line,H-15,split+look.line,H));
            return std::min({tick,std::fabs(x-(silk.letters.x-5.5f))-look.line,std::fabs(x-(silk.numbers.x+silk.numbers.w+7.5f))-look.line});
        };
        canvas.paint(0,0,W,H,[&](float x,float y) {return canvas.edge(lines(x,y));},[&](float,float,Rgb) {return ink;});
    }
    const float k=look.mark;
    for(size_t i=0;i<silk.count;++i) {
        const auto& button=silk.buttons[i];
        SDL_Rect r=button.bounds;
        r.h=std::min(r.y+r.h,silk.height)-r.y;
        const float cx=r.x+r.w*0.5f;
        switch(button.icon) {
            case SilkscreenIcon::clock:
            case SilkscreenIcon::brightness:
            case SilkscreenIcon::contrast: {
                // Small glyph near the top of its rectangle; inside the box's corners on the m515.
                const float gx=oneBox ? (button.icon==SilkscreenIcon::clock ? bx0+9 : bx1-9) : cx;
                const float cy=oneBox ? by0+9 : r.y+7;
                canvas.paint(gx-8*k,cy-8*k,gx+8*k,cy+8*k,[&](float x,float y) {
                    x=(x-gx)/k;y=(y-cy)/k;
                    float d;
                    if(button.icon==SilkscreenIcon::clock) {
                        d=std::min({ring(x,y,0,0,4.4f),segment(x,y,0,0,0,-2.8f),segment(x,y,0,0,2,0.9f)});
                    } else if(button.icon==SilkscreenIcon::contrast) {
                        // A circle, its right half filled.
                        d=ring(x,y,0,0,4.4f);
                        if(x>0)d=std::min(d,std::hypot(x,y)-4.4f);
                        d=std::min(d,std::max(std::fabs(x)-0.2f,std::hypot(x,y)-4.4f));
                    } else {
                        d=ring(x,y,0,0,2.1f);
                        for(int n=0;n<8;++n)d=std::min(d,std::hypot(x-4.6f*std::cos(degrees(45*n)),y-4.6f*std::sin(degrees(45*n)))-0.3f);
                    }
                    return canvas.edge(d*k-0.5f*k);
                },[&](float,float,Rgb) {return ink;});
                break;
            }
            case SilkscreenIcon::letters:
            case SilkscreenIcon::numbers: {
                const bool letters=button.icon==SilkscreenIcon::letters;
                const char* text=letters ? (oneBox ? "abc" : "ABC") : "123";
                // Set in the box's lower corners, each with a dot; the IIIx's a little smaller,
                // with the dot below the first character.
                const float lk=look.label>0 ? look.label : k;
                const float tx=oneBox ? (letters ? bx0+(iiixLabels ? 15 : m515 ? 16 : 19) : bx1-(iiixLabels ? 15 : m515 ? 16 : 19)) : cx;
                const float base=oneBox ? by1-(iiixLabels ? 7 : slanted ? 9 : 6) : r.y+r.h-6;
                const float slant=slanted ? degrees(letters ? 24 : -24) : 0,cs=std::cos(slant),sn=std::sin(slant);
                canvas.paint(tx-14*lk,base-12*lk,tx+14*lk,base+6*lk,[&](float x,float y) {
                    const float u=(x-tx)*cs+(y-base)*sn,v=-(x-tx)*sn+(y-base)*cs;
                    return canvas.edge(textDistance(text,u/lk,v/lk)*lk-0.75f*lk);
                },[&](float,float,Rgb) {return ink;});
                if(oneBox) {
                    const float dx=letters ? bx0+(iiixLabels ? 7 : 6) : bx1-(iiixLabels ? 7 : 6),dy=by1-(iiixLabels ? 4.5f : slanted ? 5 : 6);
                    canvas.paint(dx-3,dy-3,dx+3,dy+3,[&](float x,float y) {return canvas.edge(std::hypot(x-dx,y-dy)-1.8f);},
                                 [&](float,float,Rgb) {return ink;});
                }
                break;
            }
            default: {
                // Lighter disc with a dark glyph, centred on the reachable rectangle.
                const float cy=r.y+r.h*0.5f,R=std::min(r.w,r.h)*0.5f-look.discInset;
                canvas.paint(cx-R-1,cy-R-1,cx+R+1,cy+R+1,[&](float x,float y) {
                    return canvas.edge(std::hypot(x-cx,y-cy)-R);
                },[&](float x,float y,Rgb) {
                    return mix(look.discTop,look.discBottom,clamp01((y-cy+R)/(2*R)))*(1-0.04f*clamp01((x-cx)/R));
                });
                canvas.paint(cx-R,cy-R,cx+R,cy+R,[&](float x,float y) {
                    return canvas.edge(silkscreenDiscGlyph(button.icon,(x-cx)*19/R,(y-cy)*19/R)*R/19);
                },[&](float,float,Rgb) {return look.glyph;});
            }
        }
    }
}

// T415 silkscreen, one unit per LCD pixel: a black panel with Home, Menu, Calc and Find as pale
// tiles with the glyphs in dark lines, a thin light Graffiti box with the split marked top and
// bottom, a small light a and 1 in the box's lower corners and the contrast mark in the bottom
// left corner.
void paintT415Silkscreen(Canvas& canvas, const DeviceSilkscreen& silk) {
    const float W=silk.width,H=silk.height;
    const Rgb panelTop{26,27,30},panelBottom{16,17,19},ink{34,40,46},print{196,200,204},tileTop{170,180,186},tileBottom{156,166,172};
    canvas.paint(0,0,W,H,[](float,float) {return 1.f;},[&](float x,float y,Rgb) {
        const float t=std::fabs(x-W*0.5f)/(W*0.5f);
        return mix(panelTop,panelBottom,y/H)*(1-0.05f*t*t);
    });
    // The LCD bezel's lower edge: a dark line, then a faint lip.
    canvas.paint(0,0,W,1.5f,[&](float,float y) {return canvas.edge(y-0.6f);},[](float,float,Rgb) {return Rgb{6,6,7};});
    canvas.paint(0,1,W,3.5f,[&](float,float y) {return canvas.edge(std::fabs(y-2.2f)-0.5f)*0.8f;},[](float,float,Rgb) {return Rgb{86,88,94};});
    // The Graffiti box round both areas, with small triangles at the split.
    const float bx0=silk.letters.x,bx1=silk.numbers.x+silk.numbers.w,by0=silk.letters.y-2,by1=H-4;
    const SDL_Rect frame{int(bx0),int(by0),int(bx1-bx0),int(by1-by0)};
    const float split=silk.numbers.x;
    const std::array<std::array<float,2>,3> top{{{split-4,by0},{split+4,by0},{split,by0+5}}};
    const std::array<std::array<float,2>,3> bottom{{{split-4,by1},{split+4,by1},{split,by1-5}}};
    canvas.paint(0,0,W,H,[&](float x,float y) {
        return canvas.edge(std::min({std::fabs(deviceRoundedDistance(frame,5,x,y))-0.6f,polygon(x,y,top),polygon(x,y,bottom)}));
    },[&](float,float,Rgb) {return print;});
    for(size_t i=0;i<silk.count;++i) {
        const auto& button=silk.buttons[i];
        SDL_Rect r=button.bounds;
        r.h=std::min(r.y+r.h,silk.height)-r.y;
        const float cx=r.x+r.w*0.5f,cy=r.y+r.h*0.5f;
        switch(button.icon) {
            case SilkscreenIcon::letters:
            case SilkscreenIcon::numbers: {
                // Kept inside the box; the ROM's 123 area runs past its right edge.
                const bool letters=button.icon==SilkscreenIcon::letters;
                const char* text=letters ? "a" : "1";
                const float k=1.f,base=by1-3;
                const float cx=letters ? bx0+7 : bx1-7;
                canvas.paint(cx-14*k,base-10*k,cx+14*k,base+4*k,[&](float x,float y) {
                    return canvas.edge(textDistance(text,(x-cx)/k,(y-base)/k)*k-0.6f*k);
                },[&](float,float,Rgb) {return print;});
                break;
            }
            case SilkscreenIcon::contrast: {
                // A small circle, its right half filled, on the panel's left edge.
                const float gx=5,gy=cy;
                canvas.paint(gx-5,gy-5,gx+5,gy+5,[&](float x,float y) {
                    x-=gx;y-=gy;
                    float d=std::fabs(std::hypot(x,y)-3.2f)-0.5f;
                    if(x>0)d=std::min(d,std::hypot(x,y)-3.2f);
                    return canvas.edge(d);
                },[&](float,float,Rgb) {return print;});
                break;
            }
            default: {
                // A pale tile with a dark glyph: Home and Find in outline,
                // Calc as four signs split by a cross.
                const float R=std::min(r.w,r.h)*0.5f-1.5f;
                const SDL_Rect tile{int(std::lround(cx-R)),int(std::lround(cy-R)),int(std::lround(2*R)),int(std::lround(2*R))};
                canvas.paint(cx-R-1,cy-R-1,cx+R+1,cy+R+1,[&](float x,float y) {
                    return canvas.edge(deviceRoundedDistance(tile,2.5f,x,y));
                },[&](float,float y,Rgb) {return mix(tileTop,tileBottom,clamp01((y-cy+R)/(2*R)));});
                const float g=R*1.05f;
                canvas.paint(cx-R,cy-R,cx+R,cy+R,[&](float x,float y) {
                    const float u=(x-cx)*19/g,v=(y-cy)*19/g;
                    float d;
                    if(button.icon==SilkscreenIcon::calc) {
                        const float a=7.5f;
                        d=std::min({std::max(std::fabs(u)-0.6f,std::fabs(v)-16.f),std::max(std::fabs(v)-0.6f,std::fabs(u)-16.f),
                                    segment(u,v,-a-4,-a,-a+4,-a)-1.4f,segment(u,v,-a,-a-4,-a,-a+4)-1.4f,
                                    segment(u,v,a-4,-a,a+4,-a)-1.4f,std::hypot(u-a,v+a-3.4f)-1.5f,std::hypot(u-a,v+a+3.4f)-1.5f,
                                    segment(u,v,-a-3,a-3,-a+3,a+3)-1.4f,segment(u,v,-a-3,a+3,-a+3,a-3)-1.4f,
                                    segment(u,v,a-4,a,a+4,a)-1.4f});
                    } else {
                        d=silkscreenDiscGlyph(button.icon,u,v);
                        if(button.icon!=SilkscreenIcon::menu)d=std::fabs(d)-1.2f;
                    }
                    return canvas.edge(d*g/19);
                },[&](float,float,Rgb) {return ink;});
            }
        }
    }
}

// Stroke font for the TG50 keyboard: a squared-off face with cut corners. Each glyph is
// polylines split by '|', points "x,y" on a cell 4 wide with capitals from y 0 to the baseline
// at 6; lowercase is 4 high and descenders reach 8.
const char* tg50GlyphStrokes(char c) {
    switch(c) {
        case 'A': return "0,6 0,1 1,0 3,0 4,1 4,6|0,3.5 4,3.5";
        case 'B': return "0,3 3,3 4,4 4,5 3,6 0,6 0,0 3,0 4,1 4,2 3,3";
        case 'C': return "4,0 1,0 0,1 0,5 1,6 4,6";
        case 'D': return "0,0 3,0 4,1 4,5 3,6 0,6 0,0";
        case 'E': return "4,0 0,0 0,6 4,6|0,3 3,3";
        case 'F': return "4,0 0,0 0,6|0,3 3,3";
        case 'G': return "4,1 3,0 1,0 0,1 0,5 1,6 3,6 4,5 4,3.4 2.2,3.4";
        case 'H': return "0,0 0,6|4,0 4,6|0,3 4,3";
        case 'I': return "0.8,0 3.2,0|2,0 2,6|0.8,6 3.2,6";
        case 'J': return "4,0 4,5 3,6 1,6 0,5";
        case 'K': return "0,0 0,6|0,3.6 4,0|1.5,2.3 4,6";
        case 'L': return "0,0 0,6 4,6";
        case 'M': return "0,6 0,0 2,3 4,0 4,6";
        case 'N': return "0,6 0,0 4,6 4,0";
        case 'O': case '0': return "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0";
        case 'P': return "0,6 0,0 3,0 4,1 4,2.4 3,3.4 0,3.4";
        case 'Q': return "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0|2.6,4.6 4.2,6.2";
        case 'R': return "0,6 0,0 3,0 4,1 4,2.4 3,3.4 0,3.4|2.2,3.4 4,6";
        case 'S': return "4,1 3,0 1,0 0,1 0,2 1,3 3,3 4,4 4,5 3,6 1,6 0,5";
        case 'T': return "0,0 4,0|2,0 2,6";
        case 'U': return "0,0 0,5 1,6 3,6 4,5 4,0";
        case 'V': return "0,0 2,6 4,0";
        case 'W': return "0,0 1,6 2,2.6 3,6 4,0";
        case 'X': return "0,0 4,6|4,0 0,6";
        case 'Y': return "0,0 2,3 4,0|2,3 2,6";
        case 'Z': return "0,0 4,0 0,6 4,6";
        case '1': return "1,1 2,0 2,6";
        case '2': return "0,1 1,0 3,0 4,1 4,2.2 0,6 4,6";
        case '3': return "0,0 4,0 2,2.6 3,2.6 4,3.6 4,5 3,6 1,6 0,5";
        case '4': return "3,6 3,0 0,4 4,4";
        case '5': return "4,0 0,0 0,2.6 3,2.6 4,3.6 4,5 3,6 0,6";
        case '6': return "3.4,0 1,0 0,1 0,5 1,6 3,6 4,5 4,3.6 3,2.6 0,2.6";
        case '7': return "0,0 4,0 1.4,6";
        case '8': return "1,3 0,2 0,1 1,0 3,0 4,1 4,2 3,3 1,3 0,4 0,5 1,6 3,6 4,5 4,4 3,3";
        case '9': return "4,3.4 1,3.4 0,2.4 0,1 1,0 3,0 4,1 4,5 3,6 0.6,6";
        case 'a': return "0.4,2 3,2 3.6,2.6 3.6,6 1,6 0.4,5.4 0.4,4.6 1,4 3.6,4";
        case 'b': return "0.4,0 0.4,6 3,6 3.6,5.4 3.6,2.6 3,2 0.4,2";
        case 'c': return "3.6,2 1,2 0.4,2.6 0.4,5.4 1,6 3.6,6";
        case 'e': return "0.4,4 3.6,4 3.6,2.6 3,2 1,2 0.4,2.6 0.4,5.4 1,6 3.6,6";
        case 'f': return "3.4,0 2.2,0 1.6,0.6 1.6,6|0.4,2 3,2";
        case 'g': return "3.6,6 1,6 0.4,5.4 0.4,2.6 1,2 3.6,2 3.6,7.4 3,8 0.6,8";
        case 'h': return "0.4,0 0.4,6|0.4,2 3,2 3.6,2.6 3.6,6";
        case 'i': return "2,2 2,6|2,0.4 2,0.6";
        case 'k': return "0.6,0 0.6,6|3.6,2 0.6,4.4|1.8,3.6 3.6,6";
        case 'l': return "1.6,0 1.6,5.4 2.2,6 2.8,6";
        case 'n': return "0.4,6 0.4,2 3,2 3.6,2.6 3.6,6";
        case 'o': return "1,2 3,2 3.6,2.6 3.6,5.4 3,6 1,6 0.4,5.4 0.4,2.6 1,2";
        case 'p': return "0.4,8 0.4,2 3,2 3.6,2.6 3.6,5.4 3,6 0.4,6";
        case 'r': return "0.4,6 0.4,2|0.4,3.2 1.6,2 3.4,2";
        case 's': return "3.6,2 1,2 0.4,2.6 0.4,3.4 1,4 3,4 3.6,4.6 3.6,5.4 3,6 0.4,6";
        case 't': return "1.6,0.4 1.6,5.4 2.2,6 3.2,6|0.4,2 3.2,2";
        case '#': return "1.4,0.6 1,5.4|3,0.6 2.6,5.4|0,2.2 4,2.2|0,3.8 4,3.8";
        case '<': return "4,0.6 0,3 4,5.4";
        case '>': return "0,0.6 4,3 0,5.4";
        case '(': return "3,-0.4 1.8,0.8 1.8,5.2 3,6.4";
        case ')': return "1,-0.4 2.2,0.8 2.2,5.2 1,6.4";
        case '\\': return "0,0 4,6";
        case '/': return "4,0 0,6";
        case '$': return "4,1.4 3,0.6 1,0.6 0,1.6 0,2.4 1,3 3,3 4,3.6 4,4.4 3,5.4 1,5.4 0,4.6|2,-0.4 2,6.4";
        case '\'': return "2,0 2,2";
        case '~': return "0,3.4 1,2.4 3,3.6 4,2.6";
        case '%': return "4,0 0,6|0.4,0.4 1.6,0.4 1.6,1.6 0.4,1.6 0.4,0.4|2.4,4.4 3.6,4.4 3.6,5.6 2.4,5.6 2.4,4.4";
        case '+': return "2,1 2,5|0,3 4,3";
        case '*': return "2,0.8 2,5.2|0.2,1.9 3.8,4.1|0.2,4.1 3.8,1.9";
        case '=': return "0,2 4,2|0,4 4,4";
        case '@': return "3,4.2 3,2 1.6,2 1,2.6 1,3.6 1.6,4.2 4,4.2 4,1 3,0 1,0 0,1 0,5 1,6 3.6,6";
        case '_': return "0,6.4 4,6.4";
        case '-': return "0.6,3.2 3.4,3.2";
        case ';': return "2,2 2,2.2|2.2,4.8 1.4,6.8";
        case ',': return "2.2,5 1.4,7";
        case ':': return "2,2 2,2.2|2,5.6 2,5.8";
        case '.': return "2,5.6 2,5.8";
        // Used by the Dana legends.
        case 'd': return "3.6,0 3.6,6 1,6 0.4,5.4 0.4,2.6 1,2 3.6,2";
        case 'j': return "2.6,2 2.6,7.4 2,8 0.6,8|2.6,0.4 2.6,0.6";
        case 'm': return "0,6 0,2 1.6,2 2,2.4 2,6|2,2.4 2.4,2 3.6,2 4,2.4 4,6";
        case 'q': return "3.6,8 3.6,2 1,2 0.4,2.6 0.4,5.4 1,6 3.6,6";
        case 'u': return "0.4,2 0.4,5.4 1,6 3.6,6 3.6,2";
        case 'v': return "0.4,2 2,6 3.6,2";
        case 'w': return "0,2 1,6 2,3.4 3,6 4,2";
        case 'x': return "0.4,2 3.6,6|3.6,2 0.4,6";
        case 'y': return "0.4,2 0.4,5.4 1,6 3.6,6|3.6,2 3.6,7.4 3,8 0.6,8";
        case 'z': return "0.4,2 3.6,2 0.4,6 3.6,6";
        case '`': return "1.4,0 2.4,1.4";
        case '!': return "2,0 2,4|2,5.6 2,5.8";
        case '^': return "0.6,2 2,0 3.4,2";
        case '&': return "4,6 1,2 1,0.8 1.8,0 2.6,0 3.2,0.8 3.2,1.6 0,4 0,5.2 0.8,6 2.4,6 4,3.6";
        case '{': return "3,-0.4 2.2,0 2,0.6 2,2.4 1.2,3 2,3.6 2,5.4 2.2,6 3,6.4";
        case '}': return "1,-0.4 1.8,0 2,0.6 2,2.4 2.8,3 2,3.6 2,5.4 1.8,6 1,6.4";
        case '[': return "3,-0.4 1.8,-0.4 1.8,6.4 3,6.4";
        case ']': return "1,-0.4 2.2,-0.4 2.2,6.4 1,6.4";
        case '|': return "2,-0.4 2,6.4";
        case '"': return "1.4,0 1.4,2|2.6,0 2.6,2";
        case '?': return "0,1 1,0 3,0 4,1 4,2 2,3.4 2,4.2|2,5.6 2,5.8";
        default: return nullptr;
    }
}

struct Tg50Glyph {
    std::vector<std::array<float,4>> segments;
    float left, advance;
};
const Tg50Glyph& tg50Glyph(char c) {
    // Parsed once; C++ makes the static's set-up thread-safe.
    static const auto glyphs=[] {
        std::array<Tg50Glyph,128> g{};
        for(int i=0;i<128;++i) {
            g[i].left=0;g[i].advance=3;
            const char* p=tg50GlyphStrokes(char(i));
            if(!p)continue;
            float minX=1e9f,maxX=-1e9f,px=0,py=0;bool first=true;
            while(*p) {
                char* end;
                const float x=std::strtof(p,&end);p=end+1;
                const float y=std::strtof(p,&end);p=end;
                if(!first)g[i].segments.push_back({px,py,x,y});
                minX=std::min(minX,x);maxX=std::max(maxX,x);
                px=x;py=y;first=false;
                if(*p=='|')first=true;
                if(*p)++p;
            }
            g[i].left=minX;g[i].advance=maxX-minX+1.8f;
        }
        return g;
    }();
    return glyphs[size_t(c)&127];
}
// Distance from (x,y) to a line of text, both in cells of the font scaled by size, centred
// across x with the cap height from y 0 to 6.
float tg50TextDistance(const char* text, float x, float y, float size) {
    float total=0;
    for(const char* c=text;*c;++c)total+=tg50Glyph(*c).advance;
    float pen=-(total-1.8f)*0.5f*size,d=1e9f;
    for(const char* c=text;*c;++c) {
        const auto& g=tg50Glyph(*c);
        const float gx=(x-pen)/size+g.left;
        for(const auto& s:g.segments)d=std::min(d,segment(gx,y/size,s[0],s[1],s[2],s[3]));
        pen+=g.advance*size;
    }
    return d*size;
}
float tg50TextWidth(const char* text, float size) {
    float total=0;
    for(const char* c=text;*c;++c)total+=tg50Glyph(*c).advance;
    return (total-1.8f)*size;
}

// What's printed on and under each TG50 keyboard key, in tg50Key order (drive 2-7, column
// 0-6): the face, a second character at its lower right, the blue symbol under the key, a red
// one, and a drawn mark for keys without words.
enum class Tg50Mark { none, blueBar, redBar, up, down, left, right, enter };
struct Tg50Legend { const char* face; const char* second; const char* blue; const char* red; Tg50Mark mark; };
const Tg50Legend tg50Legends[42]={
    {"Caps","","","",Tg50Mark::none},{"Q","","1","",Tg50Mark::none},{"W","","2","",Tg50Mark::none},
    {"E","","3","",Tg50Mark::none},{"R","","4","",Tg50Mark::none},{"T","","5","",Tg50Mark::none},
    {"H","","\\","",Tg50Mark::none},
    {"Tab","","","",Tg50Mark::none},{"Y","","6","",Tg50Mark::none},{"U","","7","",Tg50Mark::none},
    {"I","","8","",Tg50Mark::none},{"O","","9","",Tg50Mark::none},{"P","","0","",Tg50Mark::none},
    {"J","","$","",Tg50Mark::none},
    {"Shift","","","",Tg50Mark::none},{"A","","#","",Tg50Mark::none},{"S","","<","",Tg50Mark::none},
    {"D","",">","",Tg50Mark::none},{"F","","(","",Tg50Mark::none},{"G","",")","",Tg50Mark::none},
    {"K","","'","",Tg50Mark::none},
    {"Ctrl","","","",Tg50Mark::none},{"Z","","","",Tg50Mark::none},{"X","","","",Tg50Mark::none},
    {"C","","%","",Tg50Mark::none},{"V","","+","",Tg50Mark::none},{"B","","/","",Tg50Mark::none},
    {"L","","~","",Tg50Mark::none},
    {"","","","",Tg50Mark::blueBar},{"N","","*","",Tg50Mark::none},{"M","","=","",Tg50Mark::none},
    {";",",","","",Tg50Mark::none},{":",".","","",Tg50Mark::none},{"","","","",Tg50Mark::up},
    {"","","","",Tg50Mark::down},
    {"","","","",Tg50Mark::redBar},{"Space","","","",Tg50Mark::none},{"@","_","-","",Tg50Mark::none},
    {"","","","",Tg50Mark::left},{"","","","",Tg50Mark::right},{"","","","",Tg50Mark::enter},
    {"BS","","","Del",Tg50Mark::none},
};
const Rgb tg50Blue{38,78,178},tg50Red{196,44,40};

// Colours of a Jog Dial wheel: dark at its sides, light across its crown, and its outline.
struct JogWheelLook { Rgb side, crown, edge; };

// Side tab Jog Dial and Back key in strip units, ox to the left: silver up and down keys with
// chevrons either side of a ridged wheel that presses in, then a silver Back key.
void paintJogKeys(Canvas& canvas, const DeviceControlProfile& profile, int ox, DeviceKeyMask pressed,
                  const SilverKeyLook& keys, Rgb ink, Rgb glow, const JogWheelLook& look) {
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    auto shifted=[&](SDL_Rect r) {r.x+=ox;return r;};
    const DeviceButton* jog[3]={nullptr,nullptr,nullptr};const DeviceButton* back=nullptr;
    for(size_t i=0;i<profile.count;++i) {
        const auto& b=profile.buttons[i];
        if(b.key==keyIdJogUp)jog[0]=&b;
        if(b.key==keyIdJogPress)jog[1]=&b;
        if(b.key==keyIdJogDown)jog[2]=&b;
        if(b.key==keyIdBack)back=&b;
    }
    if(jog[0] && jog[1] && jog[2]) {
        for(int part : {0,2}) {
            const auto& button=*jog[part];
            const SDL_Rect r=shifted(button.bounds);
            paintSilverKey(canvas,r,6,down(button.key),button.icon,0.62f,keys,false);
            const float cx=r.x+r.w*0.5f,cy=r.y+r.h*0.5f,dir=part ? 1.f : -1.f;
            canvas.paint(cx-6,cy-4,cx+6,cy+4,[&](float x,float y) {
                return canvas.edge(std::min(segment(x,y,cx-4,cy-dir*2,cx,cy+dir*2),
                                            segment(x,y,cx+4,cy-dir*2,cx,cy+dir*2))-0.7f);
            },[&](float,float,Rgb) {return down(button.key) ? glow : ink;});
        }
        const SDL_Rect wheel=shifted(jog[1]->bounds);
        const bool held=down(keyIdJogPress);
        auto d=[&](float x,float y) {return deviceRoundedDistance(wheel,deviceButtonCorner(profile,*jog[1]),x,y);};
        canvas.paint(wheel.x,wheel.y,wheel.x+wheel.w,wheel.y+wheel.h,
            [&](float x,float y) {return canvas.edge(d(x,y));},[&](float x,float y,Rgb) {
                const float across=clamp01((x-wheel.x)/wheel.w);
                const float end=clamp01(std::min(y-wheel.y,wheel.y+wheel.h-y)/9);
                const float ridge=0.82f+0.18f*std::cos(y*2.4f);
                Rgb col=mix(look.side,look.crown,std::sin(across*3.14159265f));
                return col*(ridge*(0.65f+0.35f*end)*(held ? 0.62f : 1.f));
            });
        canvas.paint(wheel.x,wheel.y,wheel.x+wheel.w,wheel.y+wheel.h,
            [&](float x,float y) {return canvas.edge(std::fabs(d(x,y)+0.8f)-0.4f)*0.65f;},
            [&](float,float,Rgb) {return look.edge;});
    }
    if(back) {
        const SDL_Rect r=shifted(back->bounds);
        paintSilverKey(canvas,r,deviceButtonCorner(profile,*back),down(keyIdBack),back->icon,0.62f,keys);
    }
}

// CLIE PEG-TG50: brushed champagne-silver body, an app row of round and slim silver keys, a
// silver QWERTY with printed legends and a left-side Jog Dial. The strip canvas is in strip
// units; the side tab canvas, beside the LCD or left of the keyboard, is in side tab units.
void paintTg50Controls(Canvas& canvas, const DeviceControlProfile& profile, int width, DeviceKeyMask pressed,
                       const DeviceControlView& view, const DeviceControlTransform& transform, bool sidebar=false) {
    const int ox=(width-profile.width)/2;
    const float H=view.keyboard ? profile.height : profile.height-profile.keyboardHeight;
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    auto shifted=[&](SDL_Rect r) {r.x+=ox;return r;};
    const Rgb background{26,26,28},body{184,180,171};
    // Window background round the body: beside the side tab, and below a hidden keyboard.
    canvas.paint(-canvas.originX,-canvas.originY,canvas.w/canvas.s-canvas.originX,canvas.h/canvas.s-canvas.originY,[](float,float) {return 1.f;},
                 [&](float,float,Rgb) {return background;});
    // Brushed metal: lit from above with fine horizontal streaks.
    auto brushed=[&](float x,float y,float top,float bottom,Rgb base) {
        Rgb c=mix(base*1.05f,base*0.84f,std::pow(clamp01((y-top)/(bottom-top)),1.2f));
        c=c+Rgb{5,5,5}*speckle(x*0.05f,y*1.5f)+Rgb{2,2,2}*std::sin(y*5.3f+std::sin(x*0.07f)*1.4f);
        return c;
    };
    // Canvas units per strip unit, and where the strip's top edge sits on this canvas.
    const float k=sidebar ? transform.scale*profile.unit : 1.f;
    const float top=sidebar ? -transform.sidebarTop*profile.unit : 0.f;
    const float left=-canvas.originX,right=canvas.w/canvas.s-canvas.originX;
    // Texture coordinates in window pixels from the body's top left, shared by both canvases.
    canvas.paint(left,-canvas.originY,right,top+H*k,[](float,float) {return 1.f;},[&](float x,float y,Rgb) {
        const float px=sidebar ? x/profile.unit+transform.sidebarLeft : transform.left+x*transform.scale;
        const float py=sidebar ? y/profile.unit+transform.sidebarTop : y*transform.scale;
        const float bodyWidth=transform.left+profile.width*transform.scale;
        const float t=std::fabs(px-bodyWidth*0.5f)/(bodyWidth*0.5f);
        return brushed(px,py,0,H*transform.scale,body)*(1-0.12f*t*t);
    });
    // The screen bezel's lower edge, then the body's foot.
    canvas.paint(left,top,right,top+3*k,[&](float,float y) {return canvas.edge(y-top-1.2f*k);},[](float,float,Rgb) {return Rgb{40,40,42};});
    canvas.paint(left,top,right,top+5*k,[&](float,float y) {return canvas.edge(std::fabs(y-top-2.2f*k)-0.6f*k)*0.8f;},
                 [](float,float,Rgb) {return Rgb{238,236,230};});
    canvas.paint(left,top+(H-3)*k,right,top+H*k,[&](float,float y) {return canvas.edge(std::fabs(y-top-(H-0.8f)*k)-0.8f*k)*0.6f;},
                 [](float,float,Rgb) {return Rgb{96,92,86};});

    const SilverKeyLook keys{Rgb{92,90,88},Rgb{240,239,236},Rgb{196,194,190},Rgb{14,14,14},Rgb{226,223,216},
                             Rgb{60,60,66},Rgb{50,50,56},1.1f,0.74f};
    // Keyboard caps: slightly domed, drawn inside their hit areas so the rows show clear gaps.
    SilverKeyLook caps=keys;caps.dome=0.3f;
    const float capInset=1.3f;
    const Rgb ink{62,62,68},glow{226,146,44};
    const DeviceButton* up=nullptr;const DeviceButton* dn=nullptr;
    for(size_t i=0;i<profile.count;++i) {
        if(profile.buttons[i].key==keyIdUp)up=&profile.buttons[i];
        if(profile.buttons[i].key==keyIdDown)dn=&profile.buttons[i];
    }
    // Icons printed above the app keys, and the bar that holds the slim keys.
    auto printed=[&](DeviceControlIcon icon,float cx,float cy,float k) {
        canvas.paint(cx-10*k,cy-10*k,cx+10*k,cy+10*k,[&](float x,float y) {
            return canvas.edge(iconDistance(icon,(x-cx)/k,(y-cy)/k)*k-0.55f);
        },[&](float,float,Rgb) {return ink;});
    };
    if(up && dn) {
        const SDL_Rect a=shifted(up->bounds);
        const float cx=a.x+a.w*0.5f;
        // Scroll mark: a small up and down triangle.
        canvas.paint(cx-5,4,cx+5,17,[&](float x,float y) {
            const std::array<std::array<float,2>,3> t0{{{cx-3,9},{cx+3,9},{cx,5.4f}}},t1{{{cx-3,11.4f},{cx+3,11.4f},{cx,15}}};
            return canvas.edge(std::min(polygon(x,y,t0),polygon(x,y,t1)));
        },[&](float,float,Rgb) {return ink;});
    }
    // Menu and Find marks printed by the round keys, each with a dash towards the bar.
    for(size_t i=0;i<profile.count;++i) {
        const auto& b=profile.buttons[i];
        if(b.key!=keyIdHome && b.key!=keyIdGraffiti)continue;
        const SDL_Rect r=shifted(b.bounds);
        const bool menu=b.key==keyIdHome;
        const float cx=menu ? r.x+r.w+3.5f : r.x-3.5f,cy=10.2f,dash=menu ? 1.f : -1.f;
        canvas.paint(cx-9,cy-5,cx+9,cy+5,[&](float x,float y) {
            float d=segment(x,y,cx+dash*4.8f,cy,cx+dash*7.6f,cy)-0.6f;
            if(menu) {
                // A title bar over three lines.
                d=std::min(d,box(x,y,cx-2.6f,cy-3.6f,cx+2.6f,cy-1.8f));
                for(float ly:{-0.2f,1.6f,3.4f})d=std::min(d,segment(x,y,cx-2.3f,cy+ly,cx+2.3f,cy+ly)-0.42f);
            } else {
                // A magnifier: a ring and its handle.
                d=std::min(d,std::fabs(std::hypot(x-cx-0.7f,y-cy+0.7f)-2.3f)-0.55f);
                d=std::min(d,segment(x,y,cx-0.9f,cy+0.9f,cx-3.4f,cy+3.4f)-0.7f);
            }
            return canvas.edge(d);
        },[&](float,float,Rgb) {return ink;});
    }
    SDL_Rect bar{0,0,0,0};
    for(size_t i=0;i<profile.count;++i) {
        const auto& b=profile.buttons[i];
        if(b.section!=DeviceControlSection::main)continue;
        const SDL_Rect r=shifted(b.bounds);
        if(b.key!=keyIdUp && b.key!=keyIdDown)printed(b.icon,r.x+r.w*0.5f,10.2f,b.bounds.w<30 ? 0.52f : 0.5f);
        if(b.bounds.w>30) {
            // The bar spans the slim keys.
            if(!bar.w) bar=r;
            const int x1=std::max(bar.x+bar.w,r.x+r.w),y1=std::max(bar.y+bar.h,r.y+r.h);
            bar.x=std::min(bar.x,r.x);bar.y=std::min(bar.y,r.y);bar.w=x1-bar.x;bar.h=y1-bar.y;
        }
    }
    if(bar.w) {
        const SDL_Rect tray{bar.x-4,bar.y-4,bar.w+8,bar.h+8};
        const float tc=tray.h*0.5f;
        auto d=[&](float x,float y) {return deviceRoundedDistance(tray,tc,x,y);};
        const float x0=tray.x-3,y0=tray.y-3,x1=tray.x+tray.w+3,y1=tray.y+tray.h+4;
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return clamp01((2-d(x,y-1.5f))/4)*0.35f;},
                     [](float,float,Rgb) {return Rgb{60,58,54};});
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(d(x,y));},[&](float,float y,Rgb) {
            return mix(Rgb{214,212,206},Rgb{150,148,146},clamp01((y-tray.y)/tray.h));
        });
        canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(d(x,y)+1.6f);},[&](float,float y,Rgb) {
            return mix(Rgb{112,114,122},Rgb{150,152,160},clamp01((y-tray.y)/tray.h));
        });
    }
    // Slim keys: silver pills with a pale light line along the middle.
    const SilverKeyLook slim{Rgb{70,70,76},Rgb{236,236,236},Rgb{184,184,188},Rgb{16,16,18},Rgb{170,172,180},
                             ink,ink,1.0f,0.72f};
    auto lightLine=[&](const SDL_Rect& r,float sink,bool held) {
        const float y=r.y+r.h*0.5f+sink,inset=r.h*0.5f+1;
        canvas.paint(r.x,y-2,r.x+r.w,y+2,[&](float x,float yy) {
            return canvas.edge(segment(x,yy,r.x+inset,y,r.x+r.w-inset,y)-0.7f)*(held?0.4f:0.85f);
        },[](float,float,Rgb) {return Rgb{214,232,250};});
    };
    for(size_t i=0;i<profile.count;++i) {
        const auto& b=profile.buttons[i];
        if(b.section!=DeviceControlSection::main || b.key==keyIdUp || b.key==keyIdDown)continue;
        const SDL_Rect r=shifted(b.bounds);
        const bool held=down(b.key);
        const float corner=deviceButtonCorner(profile,b);
        if(b.bounds.w<30) {
            // Round dimpled key in a dark ring.
            const SDL_Rect dome{r.x+2,r.y+2,r.w-4,r.h-4};
            canvas.paint(r.x-1,r.y-1,r.x+r.w+1,r.y+r.h+2,[&](float x,float y) {
                return canvas.edge(deviceRoundedDistance(r,corner,x,y));
            },[](float,float,Rgb) {return Rgb{84,82,80};});
            paintSilverKey(canvas,dome,dome.w*0.5f,held,b.icon,1,keys,false);
            const float cx=dome.x+dome.w*0.5f,cy=dome.y+dome.h*0.5f+(held?0.7f:0);
            canvas.paint(cx-7,cy-7,cx+7,cy+7,[&](float x,float y) {return canvas.edge(std::hypot(x-cx,y-cy)-5.2f);},
                         [&](float x,float y,Rgb p) {return p*(0.86f+0.1f*clamp01((y-cy+5)/10)+0.02f*(x-cx)/5);});
            canvas.paint(cx-7,cy-7,cx+7,cy+7,[&](float x,float y) {
                return canvas.edge(std::fabs(std::hypot(x-cx,y-cy)-5.4f)-0.3f)*0.6f*clamp01((y-cy)/5);
            },[](float,float,Rgb) {return Rgb{252,252,250};});
            continue;
        }
        paintSilverKey(canvas,r,corner,held,b.icon,1,slim,false);
        lightLine(r,held?0.7f:0,held);
    }
    if(up && dn) {
        // The scroll rocker: one pill split across its middle; a held half sinks and darkens.
        const SDL_Rect a=shifted(up->bounds),b=shifted(dn->bounds);
        // Drawn on the shape the two halves hit-test against.
        const SDL_Rect pill=up->shape.w>0 ? shifted(up->shape) : SDL_Rect{a.x,a.y,a.w,b.y+b.h-a.y};
        const float corner=deviceButtonCorner(profile,*up);
        paintSilverKey(canvas,pill,corner,false,DeviceControlIcon::up,1,slim,false);
        for(int half=0;half<2;++half) {
            if(!down(half ? keyIdDown : keyIdUp))continue;
            const float top=half ? b.y : a.y-2,bottom=half ? b.y+b.h+2 : a.y+a.h;
            canvas.paint(pill.x,top,pill.x+pill.w,bottom,[&](float x,float y) {
                return canvas.edge(deviceRoundedDistance(pill,corner,x,y)+0.3f);
            },[&](float,float y,Rgb p) {return p*(half ? 0.66f : 0.66f+0.1f*clamp01((y-a.y)/a.h));});
        }
        lightLine(pill,0,false);
        // The split.
        canvas.paint(pill.x,b.y-1,pill.x+pill.w,b.y+1,[&](float x,float y) {
            return canvas.edge(std::fabs(y-b.y)-0.35f)*canvas.edge(deviceRoundedDistance(pill,corner,x,y)+1.5f)*0.7f;
        },[](float,float,Rgb) {return Rgb{90,90,96};});
    }

    // Keyboard: silver keys with dark legends that light amber while held, blue and red
    // symbols printed on the body under them.
    if(view.keyboard) {
        for(size_t i=0;i<profile.count;++i) {
            const auto& b=profile.buttons[i];
            if(b.section!=DeviceControlSection::keyboard)continue;
            const int index=b.key-keyIdKeyboard;
            if(index<0 || index>=42)continue;
            const auto& legend=tg50Legends[index];
            const SDL_Rect r=shifted(b.bounds);
            const bool held=down(b.key);
            paintSilverKey(canvas,r,deviceButtonCorner(profile,b),held,b.icon,1,caps,false,capInset);
            const float sink=held ? 0.7f : 0,cx=r.x+r.w*0.5f,cy=r.y+r.h*0.5f+sink;
            const Rgb face=held ? glow : ink;
            auto text=[&](const char* t,float tx,float ty,float size,float weight,Rgb colour) {
                const float hw=tg50TextWidth(t,size)*0.5f+2,top=ty-2*size,bottom=ty+8*size+1;
                canvas.paint(tx-hw,top,tx+hw,bottom,[&](float x,float y) {
                    return canvas.edge(tg50TextDistance(t,x-tx,y-ty,size)-weight);
                },[&](float,float,Rgb) {return colour;});
            };
            if(*legend.second) {
                // Two symbols: one at the upper left, the other at the lower right.
                text(legend.face,r.x+8,r.y+3.5f+sink,1.1f,0.65f,face);
                text(legend.second,r.x+r.w-8,r.y+r.h-11.5f+sink,1.1f,0.65f,face);
            } else if(*legend.face) {
                const bool word=std::strlen(legend.face)>1;
                const float size=word ? 1.05f : 1.45f;
                text(legend.face,cx,cy-3*size,size,word ? 0.6f : 0.75f,face);
            }
            // Shapes for the keys without words: the shift bars, arrows and Enter.
            auto shape=[&](auto distance,Rgb colour) {
                canvas.paint(r.x,r.y,r.x+r.w,r.y+r.h+1,[&](float x,float y) {return canvas.edge(distance(x-cx,y-cy));},
                             [&](float,float,Rgb) {return colour;});
            };
            auto triangle=[&](float dx,float dy,float s) {
                // Points along (dx,dy), centred on the key, s across.
                return [=](float x,float y) {
                    const std::array<std::array<float,2>,3> t{{{dx*s*0.6f,dy*s*0.6f},
                        {-dx*s*0.4f-dy*s*0.6f,-dy*s*0.4f+dx*s*0.6f},{-dx*s*0.4f+dy*s*0.6f,-dy*s*0.4f-dx*s*0.6f}}};
                    return polygon(x,y,t);
                };
            };
            switch(legend.mark) {
                case Tg50Mark::blueBar: case Tg50Mark::redBar: {
                    const Rgb c=legend.mark==Tg50Mark::blueBar ? tg50Blue : tg50Red;
                    shape([&](float x,float y) {return segment(x,y,-r.w*0.3f,0,r.w*0.3f,0)-1.3f;},held ? c*0.8f : c);
                    break;
                }
                case Tg50Mark::up: case Tg50Mark::down: {
                    // The arrows page up and down on the red layer.
                    const float dir=legend.mark==Tg50Mark::up ? -1 : 1;
                    shape([&](float x,float y) {return triangle(0,dir,6)(x,y+3.6f);},face);
                    text(dir<0 ? "PgUp" : "PgDn",cx,cy+1.6f,0.62f,0.42f,held ? glow : tg50Red);
                    break;
                }
                case Tg50Mark::left: case Tg50Mark::right:
                    shape(triangle(legend.mark==Tg50Mark::left ? -1 : 1,0,6.4f),face);
                    break;
                case Tg50Mark::enter:
                    // Return arrow: down from the right, then left to a head.
                    shape([&](float x,float y) {
                        float d=std::min(segment(x,y,7,-4,7,1.6f),segment(x,y,7,1.6f,-6,1.6f))-0.75f;
                        return std::min(d,std::min(segment(x,y,-7.4f,1.6f,-3.8f,-1.6f),segment(x,y,-7.4f,1.6f,-3.8f,4.8f))-0.75f);
                    },face);
                    break;
                case Tg50Mark::none: break;
            }
            // Printed under the key, towards its right.
            // Digits under the top row are printed dark, the other symbols blue.
            if(*legend.blue)text(legend.blue,r.x+r.w-7,r.y+r.h+3.2f,0.72f,0.5f,
                                 std::isdigit(static_cast<unsigned char>(*legend.blue)) ? ink : tg50Blue);
            if(*legend.red)text(legend.red,r.x+r.w-9,r.y+r.h+3.2f,0.72f,0.5f,tg50Red);
        }
    }

    // The side controls use the same body paint and texture coordinates as the keyboard.
    // Clipped to the tab. Each key's groove and lip reach 2.3 units out, inside its 4 unit margins.
    if(sidebar && view.sideTab && profile.sideTabWidth>0) {
        canvas.clipLeft=profile.sideTab.x;
        canvas.clipRight=profile.sideTab.x+profile.sideTab.w;
        paintJogKeys(canvas,profile,ox,pressed,keys,ink,glow,{Rgb{54,62,52},Rgb{170,179,159},Rgb{55,59,51}});
    }
}

// CLIE PEG-T415: light silver metal lit from above. Under the LCD a black surround holds a dark
// brushed metal strip with the key icons stamped on it, then four wide, flat chrome slats edge to
// edge round a narrow rocker split across its middle. The side tab canvas (sidebar) is the same
// metal beside the LCD, with the Jog Dial and Back.
const Rgb t415Body{186,188,191};
const SilverKeyLook t415Keys{Rgb{78,80,86},Rgb{238,239,240},Rgb{184,186,190},Rgb{14,14,15},Rgb{246,246,246},
                             Rgb{70,72,78},Rgb{44,45,50},1.3f,0.7f};

// Brightness down a T415 chrome slat, t 0 at its top to 1 at its bottom: a shaded top edge, a
// bright upper ridge, a darker step, a bright lower ridge and a dark foot.
float t415Chrome(float t) {
    auto band=[](float t,float a,float b,float soft) {return clamp01((t-a)/soft)*clamp01((b-t)/soft);};
    return 0.22f+0.7f*band(t,0.2f,0.48f,0.06f)*(1-0.25f*(t-0.2f)/0.28f)+0.2f*band(t,0.48f,0.74f,0.05f)
           +0.62f*band(t,0.74f,0.95f,0.05f);
}

void paintT415Controls(Canvas& canvas, const DeviceControlProfile& profile, int width, DeviceKeyMask pressed,
                       bool sidebar=false, bool besideTab=false) {
    const float H=profile.height;
    const int ox=(width-profile.width)/2;
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    auto shifted=[&](SDL_Rect r) {r.x+=ox;return r;};
    const float left=-canvas.originX,right=canvas.w/canvas.s-canvas.originX;
    const float top=-canvas.originY,bottom=canvas.h/canvas.s-canvas.originY;
    // Metal with fine horizontal brushing, lit from above below the LCD; the side tab shades
    // towards the body's left edge.
    canvas.paint(left,top,right,bottom,[](float,float) {return 1.f;},[&](float x,float y,Rgb) {
        Rgb c=sidebar ? t415Body*(0.9f+0.1f*clamp01((x-left)/16))
                      : mix(t415Body,t415Body*0.84f,std::pow(clamp01(y/H),1.3f));
        return c+Rgb{4,4,4}*speckle(x*0.05f,y*1.5f)+Rgb{2,2,2}*std::sin(y*5.3f+std::sin(x*0.07f)*1.4f);
    });
    if(sidebar) {
        // The LCD bezel's left edge, then the Jog Dial and Back clipped to the tab.
        canvas.paint(right-3,top,right,bottom,[&](float x,float) {return canvas.edge(std::fabs(x-right+1)-0.8f)*0.7f;},
                     [](float,float,Rgb) {return Rgb{58,60,64};});
        if(profile.sideTabWidth>0) {
            canvas.clipLeft=profile.sideTab.x;
            canvas.clipRight=profile.sideTab.x+profile.sideTab.w;
            paintJogKeys(canvas,profile,ox,pressed,t415Keys,Rgb{64,66,72},Rgb{226,146,44},
                         {Rgb{62,64,68},Rgb{184,188,194},Rgb{60,62,66}});
        }
        return;
    }
    // The black surround carries on from round the LCD and ends in rounded corners, with a
    // bright chamfer on the metal below it. Beside the side tab it runs on into the tab, which
    // rounds the left corner.
    const SDL_Rect surround=besideTab ? SDL_Rect{ox-40,-20,profile.width+40,112} : SDL_Rect{ox,-20,profile.width,112};
    auto ds=[&](float x,float y) {return deviceRoundedDistance(surround,12,x,y);};
    canvas.paint(left,0,right,H,[&](float x,float y) {
        return canvas.edge(std::fabs(ds(x,y)-1.2f)-0.8f)*clamp01((y-60)/20);
    },[](float,float,Rgb) {return Rgb{236,237,239};});
    canvas.paint(left,0,right,H,[&](float x,float y) {return canvas.edge(ds(x,y));},[&](float x,float y,Rgb) {
        return Rgb{22,22,24}+Rgb{3,3,3}*speckle(x,y);
    });

    // The metal strip: dark brushed metal, lit along its top edge.
    const SDL_Rect strip{ox+4,3,profile.width-8,34};
    auto dm=[&](float x,float y) {return deviceRoundedDistance(strip,2,x,y);};
    canvas.paint(strip.x,strip.y,strip.x+strip.w,strip.y+strip.h,[&](float x,float y) {return canvas.edge(dm(x,y));},
                 [&](float x,float y,Rgb) {
        const float t=clamp01((y-strip.y)/strip.h);
        Rgb c=mix(Rgb{128,121,112},Rgb{92,87,80},t);
        c=c+Rgb{14,14,14}*speckle(x*0.04f,y*2)+Rgb{7,7,7}*std::sin(y*4.1f+std::sin(x*0.05f)*1.7f);
        // Darker towards its ends.
        const float e=std::fabs(x-ox-profile.width*0.5f)/(profile.width*0.5f);
        return c*(1-0.3f*std::pow(e,4.f));
    });
    canvas.paint(strip.x,strip.y,strip.x+strip.w,strip.y+3,[&](float,float y) {
        return canvas.edge(std::fabs(y-strip.y-0.6f)-0.4f)*0.6f;
    },[](float,float,Rgb) {return Rgb{170,166,160};});

    // Each key's icon stamped dark into the strip above it, lit along its lower edge; arrows over
    // and under the rocker.
    const Rgb stamp{30,28,26};
    for(size_t i=0;i<profile.count;++i) {
        const auto& b=profile.buttons[i];
        if(b.section!=DeviceControlSection::main || b.key==keyIdUp || b.key==keyIdDown)continue;
        const SDL_Rect k=shifted(b.bounds);
        const float cx=k.x+k.w*0.5f,cy=strip.y+strip.h*0.5f-1,s=1.35f;
        auto di=[&](float x,float y) {return iconDistance(b.icon,(x-cx)/s,(y-cy)/s)*s-0.55f*s;};
        canvas.paint(cx-14,cy-14,cx+14,cy+14,[&](float x,float y) {return canvas.edge(di(x,y-0.8f))*0.5f;},
                     [](float,float,Rgb) {return Rgb{176,170,160};});
        canvas.paint(cx-14,cy-14,cx+14,cy+14,[&](float x,float y) {return canvas.edge(di(x,y));},
                     [&](float,float,Rgb) {return stamp;});
    }
    const float mid=ox+profile.width*0.5f,sb=float(strip.y+strip.h);
    const std::array<std::array<float,2>,3> upMark{{{mid-4,sb-3},{mid+4,sb-3},{mid,sb-9}}};
    canvas.paint(mid-6,strip.y,mid+6,strip.y+strip.h,[&](float x,float y) {return canvas.edge(polygon(x,y,upMark));},
                 [&](float,float,Rgb) {return stamp;});

    // The slat row's dark well.
    const SDL_Rect row{ox+2,38,profile.width-4,42};
    canvas.paint(row.x,row.y,row.x+row.w,row.y+row.h,[&](float x,float y) {return canvas.edge(deviceRoundedDistance(row,3,x,y));},
                 [](float,float,Rgb) {return Rgb{10,10,11};});
    // A chrome slat or rocker half: horizontal ridges, a cooler tint in the dark steps and a glint
    // towards each end. Held keys sink a little and darken.
    auto chrome=[&](const SDL_Rect& r,float corner,bool held,float y0,float y1) {
        const float sink=held ? 0.8f : 0;
        auto d=[&](float x,float y) {return std::max(deviceRoundedDistance(r,corner,x,y-sink),std::max(y0-y,y-y1));};
        canvas.paint(r.x,y0,r.x+r.w,y1+1,[&](float x,float y) {return canvas.edge(d(x,y));},[&](float x,float y,Rgb) {
            const float v=t415Chrome(clamp01((y-sink-r.y)/r.h));
            const float across=(x-r.x)/r.w,glint=0.06f*std::sin(across*3.14159265f*2+0.6f);
            Rgb c=Rgb{238,240,244}*clamp01(v+glint)+Rgb{0,2,6}*(1-v);
            c=mix(c*0.6f,c,clamp01(-d(x,y)/1.5f));
            return held ? c*0.68f : c;
        });
    };
    for(size_t i=0;i<profile.count;++i) {
        const auto& b=profile.buttons[i];
        if(b.section!=DeviceControlSection::main || b.key==keyIdUp || b.key==keyIdDown)continue;
        const SDL_Rect k=shifted(b.bounds);
        chrome(k,deviceButtonCorner(profile,b),down(b.key),float(k.y)-1,float(k.y+k.h)+1);
    }

    // The rocker: a narrow chrome key between two thin bright posts, rocking at its middle,
    // with a down arrow on the ledge below it.
    const DeviceButton* up=nullptr;const DeviceButton* dn=nullptr;
    for(size_t i=0;i<profile.count;++i) {
        if(profile.buttons[i].key==keyIdUp)up=&profile.buttons[i];
        if(profile.buttons[i].key==keyIdDown)dn=&profile.buttons[i];
    }
    if(!up || !dn)return;
    const SDL_Rect shape=shifted(up->shape),b=shifted(dn->bounds);
    const float split=float(b.y);
    for(float px:{shape.x-2.5f,shape.x+shape.w+2.5f}) {
        canvas.paint(px-2,row.y,px+2,row.y+row.h,[&](float x,float y) {
            return canvas.edge(std::max(std::fabs(x-px)-0.8f,std::fabs(y-row.y-row.h*0.5f)-row.h*0.5f+2));
        },[&](float,float y,Rgb) {return Rgb{220,222,226}*(0.55f+0.45f*t415Chrome(clamp01((y-row.y)/row.h)));});
    }
    chrome(shape,deviceButtonCorner(profile,*up),down(keyIdUp),float(shape.y)-1,split-0.5f);
    chrome(shape,deviceButtonCorner(profile,*up),down(keyIdDown),split+0.5f,float(shape.y+shape.h)+1);
    const float rb=float(row.y+row.h);
    const std::array<std::array<float,2>,3> downMark{{{mid-4,rb+3},{mid+4,rb+3},{mid,rb+8}}};
    canvas.paint(mid-6,rb,mid+6,rb+10,[&](float x,float y) {return canvas.edge(polygon(x,y,downMark));},
                 [&](float,float,Rgb) {return Rgb{150,148,146};});
}

// T415 side tab beside the silkscreen and strip, in side tab units: the black body they sit in,
// with the silkscreen's dark top edge, then the strip's metal foot under one rounded bottom
// corner. stripTop is the strip's top on this canvas and k its window pixels per strip unit.
void paintT415BesideTab(Canvas& canvas, const DeviceControlProfile& profile, DeviceKeyMask pressed,
                        float stripTop, float k) {
    const float left=-canvas.originX,right=canvas.w/canvas.s-canvas.originX;
    const float top=-canvas.originY,bottom=canvas.h/canvas.s-canvas.originY;
    const float H=profile.height;
    canvas.paint(left,top,right,bottom,[](float,float) {return 1.f;},[&](float x,float y,Rgb) {
        const float sy=(y-stripTop)/k;
        Rgb c=mix(t415Body,t415Body*0.84f,std::pow(clamp01(sy/H),1.3f));
        return c+Rgb{4,4,4}*speckle(x*0.05f,sy*1.5f)+Rgb{2,2,2}*std::sin(sy*5.3f+std::sin(x*0.07f)*1.4f);
    });
    // The surround as on the strip, its bottom at strip row 92, open to the right and above.
    const float x0=left,x1=right+40,y0=top-40,y1=stripTop+92*k,radius=12*k;
    auto ds=[&](float x,float y) {
        const float qx=std::fabs(x-(x0+x1)*0.5f)-(x1-x0)*0.5f+radius,qy=std::fabs(y-(y0+y1)*0.5f)-(y1-y0)*0.5f+radius;
        return std::hypot(std::max(qx,0.f),std::max(qy,0.f))+std::min(std::max(qx,qy),0.f)-radius;
    };
    canvas.paint(left,top,right,bottom,[&](float x,float y) {
        return canvas.edge(std::fabs(ds(x,y)-1.2f*k)-0.8f*k)*clamp01((y-stripTop-60*k)/(20*k));
    },[](float,float,Rgb) {return Rgb{236,237,239};});
    // Beside the silkscreen the body takes the panel's shading at its edge, below it the surround's.
    canvas.paint(left,top,right,bottom,[&](float x,float y) {return canvas.edge(ds(x,y));},[&](float x,float y,Rgb) {
        if(y<stripTop)return mix(Rgb{26,27,30},Rgb{16,17,19},clamp01((y-top)/(stripTop-top)))*0.95f;
        return Rgb{22,22,24}+Rgb{3,3,3}*speckle(x,y);
    });
    // The silkscreen's top edge carried across, at its scale.
    canvas.paint(left,top,right,top+1.5f*k,[&](float,float y) {return canvas.edge(y-top-0.6f*k);},[](float,float,Rgb) {return Rgb{6,6,7};});
    canvas.paint(left,top+k,right,top+3.5f*k,[&](float,float y) {return canvas.edge(std::fabs(y-top-2.2f*k)-0.5f*k)*0.8f;},
                 [](float,float,Rgb) {return Rgb{86,88,94};});
    if(profile.sideTabWidth>0) {
        canvas.clipLeft=profile.sideTab.x;
        canvas.clipRight=profile.sideTab.x+profile.sideTab.w;
        paintJogKeys(canvas,profile,0,pressed,t415Keys,Rgb{64,66,72},Rgb{226,146,44},
                     {Rgb{62,64,68},Rgb{184,188,194},Rgb{60,62,66}});
    }
}

// Windows Mobile iPAQs. The body fills the strip, darker at its edge, and its lower corners
// round off into the window background.
const Rgb ipaqBackground{18,19,22};
// The shade runs from top to bottom over span units, the body's height unless given.
void paintIpaqBody(Canvas& canvas, float width, float H, float corner, Rgb top, Rgb bottom, float grain, float span=0) {
    if(span<=0)span=H;
    const SDL_Rect body{0,-int(corner)-2,int(width),int(H+corner)+2};
    canvas.paint(0,0,width,H,[](float,float) {return 1.f;},[](float,float,Rgb) {return ipaqBackground;});
    auto d=[&](float x,float y) {return deviceRoundedDistance(body,corner,x,y);};
    canvas.paint(0,0,width,H,[&](float x,float y) {return canvas.edge(d(x,y));},[&](float x,float y,Rgb) {
        Rgb c=mix(top,bottom,std::pow(clamp01(y/span),1.2f))+Rgb{grain,grain,grain}*speckle(x,y);
        const float t=std::fabs(x-width*0.5f)/(width*0.5f);
        // The body curves away towards its outline.
        return c*(1-0.12f*t*t)*(1-0.3f*clamp01(1+d(x,y)/5));
    });
}

// A round dark key in a raised metal ring, hit by its whole ring: the ring lit from above, a
// dark groove, a glossy dome with a soft highlight and a light icon. Held keys sink and darken.
void paintIpaqRoundKey(Canvas& canvas, const SDL_Rect& hit, bool held, DeviceControlIcon icon, float iconScale) {
    const float cx=hit.x+hit.w*0.5f,cy=hit.y+hit.h*0.5f,R=hit.w*0.5f,dome=R*0.78f,sink=held ? 0.8f : 0;
    const float x0=hit.x-4,y0=hit.y-4,x1=hit.x+hit.w+4,y1=hit.y+hit.h+5;
    auto dr=[&](float x,float y) {return std::hypot(x-cx,y-cy)-R;};
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return clamp01((2-dr(x,y-1.6f))/4)*0.35f;},
                 [](float,float,Rgb) {return Rgb{0,0,0};});
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(dr(x,y));},[&](float x,float y,Rgb) {
        // Bright metal falling off to the lower half, with a lit lip at the very bottom.
        const float t=clamp01((y-hit.y)/hit.h);
        Rgb c=mix(Rgb{248,249,252},Rgb{128,132,140},std::pow(t,0.8f));
        return mix(c,Rgb{214,218,226},clamp01((t-0.86f)*7)*clamp01(1+dr(x,y)/1.5f));
    });
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(std::hypot(x-cx,y-cy)-dome-1.4f);},
                 [](float,float,Rgb) {return Rgb{22,23,27};});
    auto dk=[&](float x,float y) {return std::hypot(x-cx,y-cy-sink)-dome;};
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(dk(x,y));},[&](float x,float y,Rgb) {
        const float r=std::hypot((x-cx+dome*0.15f)/dome,(y-cy-sink+dome*0.35f)/(dome*0.8f));
        Rgb c=Rgb{38,40,46}+Rgb{40,42,48}*std::pow(clamp01(1-r),1.6f);
        c=mix(Rgb{16,17,20},c,clamp01(-dk(x,y)/3));
        return held ? c*(0.5f+0.15f*clamp01(-dk(x,y-1.6f)/3)) : c;
    });
    // A fine highlight round the top of the dome.
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
        return canvas.edge(std::fabs(dk(x,y)+1)-0.4f)*(held ? 0.1f : 0.5f)*clamp01((cy-y)/dome);
    },[](float,float,Rgb) {return Rgb{128,132,142};});
    const Rgb ink=held ? Rgb{96,99,106} : Rgb{206,209,216};
    const float k=iconScale,reach=11*k,icy=cy+sink;
    canvas.paint(cx-reach,icy-reach,cx+reach,icy+reach,[&](float x,float y) {
        return canvas.edge(iconDistance(icon,(x-cx)/k,(y-icy)/k)*k-0.6f*k);
    },[&](float,float,Rgb) {return ink;});
}

// rx1950 five-way: a near-square mirror chrome pad in a thin silver ring, four small ticks and a
// dark glossy centre key.
void paintIpaqChromeNavigator(Canvas& canvas, const DeviceControlProfile& profile, int ox, DeviceKeyMask pressed) {
    SDL_Rect n=profile.navigator,sel=profile.select;n.x+=ox;sel.x+=ox;
    const float corner=profile.navigatorCorner,ring=4,cx=n.x+n.w*0.5f,cy=n.y+n.h*0.5f;
    const SDL_Rect pad{n.x+int(ring),n.y+int(ring),n.w-2*int(ring),n.h-2*int(ring)};
    auto dn=[&](float x,float y) {return deviceRoundedDistance(n,corner,x,y);};
    auto dp=[&](float x,float y) {return deviceRoundedDistance(pad,corner-ring,x,y);};
    const float x0=n.x-4,y0=n.y-3,x1=n.x+n.w+4,y1=n.y+n.h+6;
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return clamp01((2.5f-dn(x,y-2))/5)*0.5f;},
                 [](float,float,Rgb) {return Rgb{0,0,0};});
    // A thin satin silver ring with a dark seam inside it.
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(dn(x,y));},[&](float,float y,Rgb) {
        return mix(Rgb{236,238,242},Rgb{150,154,162},clamp01((y-n.y)/n.h));
    });
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(dp(x,y)-1);},[](float,float,Rgb) {return Rgb{40,42,48};});
    // Mirror chrome: a bright band along the top, dark reflections across the middle and a lit
    // lower lip, a little darker towards the sides, dished towards the centre key.
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(dp(x,y));},[&](float x,float y,Rgb) {
        const float t=clamp01((y-pad.y)/pad.h),side=std::fabs(x-cx)/(pad.w*0.5f);
        Rgb c=t<0.22f ? mix(Rgb{240,242,246},Rgb{74,77,86},t/0.22f)
            : t<0.55f ? mix(Rgb{74,77,86},Rgb{128,132,142},(t-0.22f)/0.33f)
            : t<0.85f ? mix(Rgb{128,132,142},Rgb{64,66,74},(t-0.55f)/0.3f)
                      : mix(Rgb{64,66,74},Rgb{214,217,224},(t-0.85f)/0.15f);
        c=c*(1-0.25f*side*side);
        // A bright rolled edge just inside the seam.
        c=mix(c,Rgb{248,249,252},canvas.edge(std::fabs(dp(x,y)+1.5f)-0.6f)*0.6f*(t<0.5f ? 1.f : 0.5f));
        return c*(1-0.5f*navigatorHeld(n,x,y,pressed));
    });
    // Small tick marks halfway out from the centre key, one per direction.
    const float ax=(sel.w*0.5f+pad.w*0.5f)*0.5f+2,ay=(sel.h*0.5f+pad.h*0.5f)*0.5f+2;
    for(auto [dx,dy]:{std::pair<float,float>{0,-1},{0,1},{-1,0},{1,0}}) {
        const float tx=cx+dx*ax,ty=cy+dy*ay;
        canvas.paint(tx-6,ty-6,tx+6,ty+6,[&](float x,float y) {
            return canvas.edge(segment(x,y,tx-dx*3.5f,ty-dy*3.5f,tx+dx*3.5f,ty+dy*3.5f)-0.7f)*0.85f;
        },[](float,float,Rgb) {return Rgb{30,31,36};});
    }
    // The centre key: dark glossy, in a bright groove, with a soft highlight high on its face.
    const bool held=(pressed&deviceKeyBit(keyIdSelect))!=0;
    const float sc=profile.selectCorner,sink=held ? 1.f : 0;
    auto ds=[&](float x,float y) {return deviceRoundedDistance(sel,sc,x,y);};
    canvas.paint(sel.x-4,sel.y-4,sel.x+sel.w+4,sel.y+sel.h+5,[&](float x,float y) {return canvas.edge(ds(x,y)-2.2f);},
                 [&](float,float y,Rgb) {return mix(Rgb{236,238,242},Rgb{120,124,132},clamp01((y-sel.y)/sel.h));});
    canvas.paint(sel.x-4,sel.y-4,sel.x+sel.w+4,sel.y+sel.h+5,[&](float x,float y) {return canvas.edge(ds(x,y-sink));},
                 [&](float x,float y,Rgb) {
        const float t=clamp01((y-sel.y-sink)/sel.h);
        Rgb c=mix(Rgb{28,29,34},Rgb{70,73,80},t);
        const float r=std::hypot((x-sel.x-sel.w*0.5f)/(sel.w*0.34f),(y-sel.y-sink-sel.h*0.28f)/(sel.h*0.16f));
        c=mix(c,Rgb{236,238,244},clamp01(1-r)*0.8f);
        return held ? c*0.45f : c;
    });
}

// iPAQ rx1950: satin silver, round dark keys in bright rings close round a chrome five-way.
void paintRx1950Controls(Canvas& canvas, const DeviceControlProfile& profile, int width, DeviceKeyMask pressed) {
    const float H=profile.height;
    const int ox=(width-profile.width)/2;
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    paintIpaqBody(canvas,width,H,44,Rgb{208,211,217},Rgb{150,154,162},6);
    // The lower edge of the screen's bezel.
    canvas.paint(0,0,width,6,[&](float,float y) {return canvas.edge(y-1.6f);},[](float,float,Rgb) {return Rgb{70,73,80};});
    canvas.paint(0,0,width,8,[&](float,float y) {return canvas.edge(std::fabs(y-3.2f)-1.2f)*0.8f;},
                 [](float,float,Rgb) {return Rgb{244,246,250};});
    for(size_t i=0;i<profile.count;++i) {
        const auto& b=profile.buttons[i];
        if(deviceNavigatorButton(profile,b))continue;
        SDL_Rect r=b.bounds;r.x+=ox;
        paintIpaqRoundKey(canvas,r,down(b.key),b.icon,1.35f);
    }
    paintIpaqChromeNavigator(canvas,profile,ox,pressed);
}

// What's printed on each h4350 keyboard key, in h4350Key order: the black face, the blue
// function character at its upper left, and a drawn mark.
enum class H4350Mark { none, del, enter, up, down, left, right, fn, start, today, accents };
struct H4350Legend { const char* face; const char* blue; H4350Mark mark; };
const H4350Legend h4350Legends[39]={
    {"Q","",H4350Mark::none},{"W","@",H4350Mark::none},{"E","$",H4350Mark::none},{"R","%",H4350Mark::none},
    {"T","~",H4350Mark::none},{"Y","-",H4350Mark::none},{"U","1",H4350Mark::none},{"I","2",H4350Mark::none},
    {"O","3",H4350Mark::none},{"P","/",H4350Mark::none},
    {"A","(",H4350Mark::none},{"S",")",H4350Mark::none},{"D",":",H4350Mark::none},{"F","=",H4350Mark::none},
    {"G","",H4350Mark::up},{"H","+",H4350Mark::none},{"J","4",H4350Mark::none},{"K","5",H4350Mark::none},
    {"L","6",H4350Mark::none},{"","Del",H4350Mark::del},
    {"Shift","Lock",H4350Mark::none},{"Z","&",H4350Mark::none},{"X",";",H4350Mark::none},{"C","",H4350Mark::left},
    {"V","",H4350Mark::down},{"B","",H4350Mark::right},{"N","7",H4350Mark::none},{"M","8",H4350Mark::none},
    {"!","9",H4350Mark::none},{"","OK",H4350Mark::enter},
    {"","",H4350Mark::fn},{"Tab","",H4350Mark::none},{"","",H4350Mark::start},{"","",H4350Mark::today},
    {"","",H4350Mark::accents},{".","*",H4350Mark::none},{",","0",H4350Mark::none},{"?","#",H4350Mark::none},
    {"'","\"",H4350Mark::none}};

// iPAQ h4350: bluish silver above, the round keys and chrome five-way in a recessed blue chrome
// band, then a slate blue-grey panel whose top edge sags in the middle like the key rows, with
// pale silver domed keys, black legends and blue function characters. The body tapers a little
// to big rounded lower corners. Hidden, the row ends in the panel's rounded corners. Everything
// above the row's lower 32 units is drawn the same either way.
void paintH4350Controls(Canvas& canvas, const DeviceControlProfile& profile, int width, DeviceKeyMask pressed,
                        const DeviceControlView& view) {
    const float H=view.keyboard ? profile.height : profile.height-profile.keyboardHeight;
    const float W=width,row=profile.height-profile.keyboardHeight;
    const int ox=(width-profile.width)/2;
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    // Body outline: straight sides down the row, then a slight taper and wide lower corners.
    const float corner=view.keyboard ? 40 : 30;
    auto body=[&](float x,float y) {
        const float inset=view.keyboard ? 5*clamp01((y-row)/(H-row)) : 0;
        const SDL_Rect r{0,-int(corner)-2,int(W),int(H+corner)+2};
        return deviceRoundedDistance(r,corner,W*0.5f+std::fabs(x-W*0.5f)+inset,y);
    };
    canvas.paint(0,0,W,H,[](float,float) {return 1.f;},[](float,float,Rgb) {return ipaqBackground;});
    canvas.paint(0,0,W,H,[&](float x,float y) {return canvas.edge(body(x,y));},[&](float x,float y,Rgb) {
        Rgb c=mix(Rgb{204,213,230},Rgb{170,180,202},std::pow(clamp01(y/row),1.2f))+Rgb{5,5,5}*speckle(x,y);
        const float t=std::fabs(x-W*0.5f)/(W*0.5f);
        return c*(1-0.1f*t*t)*(1-0.3f*clamp01(1+body(x,y)/5));
    });
    // The lower edge of the screen's bezel.
    canvas.paint(0,0,W,6,[&](float,float y) {return canvas.edge(y-1.6f);},[](float,float,Rgb) {return Rgb{64,68,78};});
    canvas.paint(0,0,W,8,[&](float,float y) {return canvas.edge(std::fabs(y-3.2f)-1.2f)*0.8f;},
                 [](float,float,Rgb) {return Rgb{242,245,250};});

    // The recessed band: blue chrome with a bright reflection near its top, its sides closing in.
    const float bandTop=22;
    auto band=[&](float x,float y) {
        const float inset=20+8*clamp01((y-bandTop)/70);
        const SDL_Rect r{0,int(bandTop),int(W),int(H)};
        return deviceRoundedDistance(r,16,W*0.5f+std::fabs(x-W*0.5f)+inset,y);
    };
    const float bandBottom=std::min(H,110.f);
    canvas.paint(0,bandTop-3,W,bandBottom,[&](float x,float y) {
        return canvas.edge(std::fabs(band(x,y)-0.8f)-0.8f)*0.7f*canvas.edge(body(x,y));
    },[](float,float,Rgb) {return Rgb{84,94,116};});
    canvas.paint(0,bandTop-3,W,bandBottom,[&](float x,float y) {return canvas.edge(band(x,y))*canvas.edge(body(x,y));},
                 [&](float x,float y,Rgb) {
        const float t=clamp01((y-bandTop)/60);
        Rgb c=mix(Rgb{104,138,186},Rgb{50,78,122},t);
        c=c+Rgb{70,74,70}*std::exp(-std::pow((y-bandTop-8)/3.5f,2.f));
        // Shadow under the band's top lip.
        c=c*(1-0.4f*clamp01(1+band(x,y)/3)*clamp01((bandTop+6-y)/6));
        return c+Rgb{4,4,5}*speckle(x,y);
    });

    // The slate keyboard panel, its top edge sagging between the outer keys.
    auto panelTop=[&](float x) {const float u=(x-W*0.5f)/(W*0.5f);return 66+22*(1-u*u);};
    canvas.paint(0,60,W,H,[&](float x,float y) {return canvas.edge(panelTop(x)-y)*canvas.edge(body(x,y));},
                 [&](float x,float y,Rgb) {
        Rgb c=mix(Rgb{104,112,128},Rgb{80,87,102},clamp01((y-60)/264))+Rgb{4,4,5}*speckle(x,y);
        // A lit rounded edge along the top, the body darkening towards its outline.
        c=mix(c,Rgb{150,158,176},clamp01(1-(y-panelTop(x))/2.5f)*0.7f);
        return c*(1-0.3f*clamp01(1+body(x,y)/5));
    });
    canvas.paint(0,60,W,H,[&](float x,float y) {return canvas.edge(std::fabs(y-panelTop(x)+1.2f)-0.6f)*0.6f;},
                 [](float,float,Rgb) {return Rgb{24,30,44};});

    for(size_t i=0;i<profile.count;++i) {
        const auto& b=profile.buttons[i];
        if(b.section!=DeviceControlSection::main || deviceNavigatorButton(profile,b))continue;
        SDL_Rect r=b.bounds;r.x+=ox;
        paintIpaqRoundKey(canvas,r,down(b.key),b.icon,1.f);
    }
    // The five-way sits in a dark rounded-square surround.
    {
        SDL_Rect s=profile.navigator;s.x+=ox-5;s.y-=5;s.w+=10;s.h+=10;
        const float sc=profile.navigatorCorner+5;
        auto ds=[&](float x,float y) {return deviceRoundedDistance(s,sc,x,y);};
        canvas.paint(s.x-3,s.y-3,s.x+s.w+3,s.y+s.h+5,[&](float x,float y) {return clamp01((2-ds(x,y-1.6f))/4)*0.35f;},
                     [](float,float,Rgb) {return Rgb{0,0,0};});
        canvas.paint(s.x-3,s.y-3,s.x+s.w+3,s.y+s.h+5,[&](float x,float y) {return canvas.edge(ds(x,y));},
                     [&](float,float y,Rgb) {return mix(Rgb{40,44,54},Rgb{14,15,19},clamp01((y-s.y)/s.h));});
        canvas.paint(s.x-3,s.y-3,s.x+s.w+3,s.y+s.h+5,[&](float x,float y) {
            return canvas.edge(std::fabs(ds(x,y)+0.7f)-0.35f)*0.5f*clamp01((s.y+s.h*0.5f-y)/(s.h*0.5f));
        },[](float,float,Rgb) {return Rgb{140,150,170};});
    }
    // Chrome that mirrors the blue band: a bright rim, a blue dish and a pale select key.
    paintNavigator(canvas,profile,ox,pressed,{Rgb{16,18,24},Rgb{228,236,248},Rgb{96,110,136},Rgb{92,122,170},
                                              Rgb{44,62,98},Rgb{255,255,255},9,0.45f,0.35f,0.6f,
                                              Rgb{14,16,22},Rgb{214,226,244},Rgb{84,104,140},Rgb{255,255,255}});
    if(!view.keyboard)return;

    const SilverKeyLook keys{Rgb{40,44,54},Rgb{242,245,250},Rgb{168,176,192},Rgb{14,14,16},Rgb{132,140,158},
                             Rgb{22,24,30},Rgb{22,24,30},1.3f,0.66f};
    const Rgb blue{26,104,196};
    for(size_t i=0;i<profile.count;++i) {
        const auto& b=profile.buttons[i];
        const int index=b.key-keyIdKeyboard;
        if(b.section!=DeviceControlSection::keyboard || index<0 || index>=39)continue;
        SDL_Rect r=b.bounds;r.x+=ox;
        const bool held=down(b.key);
        const float kc=deviceButtonCorner(profile,b);
        // A soft shadow on the panel below each dome.
        auto dk=[&](float x,float y) {return deviceRoundedDistance(r,kc,x,y);};
        canvas.paint(r.x-4,r.y-2,r.x+r.w+4,r.y+r.h+6,[&](float x,float y) {return clamp01((3-dk(x,y-2.2f))/5)*0.4f;},
                     [](float,float,Rgb) {return Rgb{0,0,0};});
        paintSilverKey(canvas,r,kc,held,b.icon,1,keys,false);
        const float sink=held ? 0.7f : 0,cx=r.x+r.w*0.5f,cy=r.y+r.h*0.5f+sink;
        const Rgb ink=held ? Rgb{58,60,66} : Rgb{22,24,30};
        const Rgb fn=held ? blue*0.6f : blue;
        const auto& legend=h4350Legends[index];
        auto text=[&](const char* s,float gx,float gy,float size,float weight,Rgb colour) {
            const float hw=tg50TextWidth(s,size)*0.5f+2,ty=gy-3*size;
            canvas.paint(gx-hw,ty-2*size-1,gx+hw,ty+8*size+1,[&](float x,float y) {
                return canvas.edge(tg50TextDistance(s,x-gx,y-ty,size)-weight);
            },[&](float,float,Rgb) {return colour;});
        };
        auto mark=[&](float gx,float gy,float reach,Rgb colour,auto distance) {
            canvas.paint(gx-reach,gy-reach,gx+reach,gy+reach,[&](float x,float y) {
                return canvas.edge(distance(x-gx,y-gy));
            },[&](float,float,Rgb) {return colour;});
        };
        const bool word=std::strlen(legend.face)>1,small=*legend.blue!=0 || (legend.mark>=H4350Mark::up && legend.mark<=H4350Mark::right);
        if(word && *legend.blue) {
            // Shift, with Lock above it in blue.
            text(legend.blue,cx,cy-5.5f,0.72f,0.42f,fn);
            text(legend.face,cx,cy+4.5f,0.85f,0.5f,ink);
        } else if(word) {
            text(legend.face,cx,cy,0.95f,0.55f,ink);
        } else if(*legend.face) {
            text(legend.face,small ? cx+2.5f : cx,small ? cy+1.5f : cy,1.55f,0.8f,ink);
            if(*legend.blue)text(legend.blue,cx-7.5f,cy-7,0.85f,0.5f,fn);
        }
        switch(legend.mark) {
            case H4350Mark::del: case H4350Mark::enter: {
                // The blue word above, a black arrow under it: left for Del, a return for OK.
                text(legend.blue,cx,cy-5.5f,0.85f,0.5f,fn);
                const bool enter=legend.mark==H4350Mark::enter;
                mark(cx,cy+4.5f,10,ink,[&](float u,float v) {
                    float d=enter ? std::min(segment(u,v,6,-4,6,1),segment(u,v,6,1,-6,1)) : segment(u,v,-6,0,6,0);
                    const float hx=-6,hy=enter ? 1.f : 0.f;
                    d=std::min({d,segment(u,v,hx,hy,hx+3,hy-2.6f),segment(u,v,hx,hy,hx+3,hy+2.6f)});
                    return d-0.75f;
                });
                break;
            }
            case H4350Mark::up: case H4350Mark::down: case H4350Mark::left: case H4350Mark::right: {
                // A small blue triangle where the function character goes.
                const float a=legend.mark==H4350Mark::up ? -1.5708f : legend.mark==H4350Mark::down ? 1.5708f
                             : legend.mark==H4350Mark::left ? 3.14159f : 0.f;
                const float ca=std::cos(a),sa=std::sin(a);
                mark(cx-7.5f,cy-6.5f,6,fn,[&](float u,float v) {
                    const float px=u*ca+v*sa,py=-u*sa+v*ca;
                    return std::max({-px-2.2f,px*0.5f+std::fabs(py)*0.87f-1.8f})-0.2f;
                });
                break;
            }
            case H4350Mark::fn:
                mark(cx,cy,9,fn,[&](float u,float v) {return std::hypot(u,v)-5.8f;});
                break;
            case H4350Mark::start: case H4350Mark::today: {
                const auto icon=legend.mark==H4350Mark::start ? DeviceControlIcon::start : DeviceControlIcon::today;
                const float k=0.78f;
                mark(cx,cy,11,ink,[&](float u,float v) {return iconDistance(icon,u/k,v/k)*k-0.6f;});
                break;
            }
            case H4350Mark::accents: {
                // The accent key's a-acute and u-diaeresis in blue at the left of the space bar.
                const float gx=r.x+16,gy=cy+1;
                text("au",gx,gy,1.1f,0.6f,fn);
                mark(gx,gy,12,fn,[&](float u,float v) {
                    return std::min({segment(u,v,-3.8f,-3.6f,-2.2f,-5.6f),std::hypot(u-1.9f,v+4.4f)-0.8f,
                                     std::hypot(u-4.5f,v+4.4f)-0.8f})-0.4f;
                });
                break;
            }
            default: break;
        }
    }
}

// iPAQ 210: gloss black, a thin strip of four flat keys split by seams, each with a lighter
// lip along its bottom, and a polished chrome shield five-way with a recessed select key.
void paintIpaq210Controls(Canvas& canvas, const DeviceControlProfile& profile, int width, DeviceKeyMask pressed) {
    const float H=profile.height;
    const int ox=(width-profile.width)/2;
    auto down=[&](KeyId key) {return (pressed&deviceKeyBit(key))!=0;};
    paintIpaqBody(canvas,width,H,30,Rgb{30,31,35},Rgb{12,12,15},3);
    // A soft gloss reflection across the top edge.
    canvas.paint(0,0,width,10,[](float,float) {return 1.f;},[&](float,float y,Rgb c) {
        return c+Rgb{8,8,10}*std::exp(-std::pow((y-3)/3,2.f));
    });
    // The key strip: near black with a lit top edge and a dark lower one.
    const float b0=8,b1=84;
    canvas.paint(0,b0-1,width,b1+1,[&](float,float y) {return canvas.edge(std::fabs(y-(b0+b1)*0.5f)-(b1-b0)*0.5f);},
                 [](float,float,Rgb) {return Rgb{8,8,10};});
    canvas.paint(0,b0-2,width,b0+2,[&](float,float y) {return canvas.edge(std::fabs(y-b0)-0.5f)*0.6f;},
                 [](float,float,Rgb) {return Rgb{78,81,90};});
    canvas.paint(0,b1-2,width,b1+2,[&](float,float y) {return canvas.edge(std::fabs(y-b1)-0.5f)*0.5f;},
                 [](float,float,Rgb) {return Rgb{60,62,70};});
    for(size_t i=0;i<profile.count;++i) {
        const auto& b=profile.buttons[i];
        if(deviceNavigatorButton(profile,b))continue;
        SDL_Rect r=b.bounds;r.x+=ox;
        const bool held=down(b.key);
        const float corner=deviceButtonCorner(profile,r),sink=held ? 0.8f : 0,lip=r.y+r.h*0.7f;
        auto d=[&](float x,float y) {return deviceRoundedDistance(r,corner,x,y);};
        // Flat face, then a lighter matte lip along the bottom.
        canvas.paint(r.x-2,r.y-2,r.x+r.w+2,r.y+r.h+2,[&](float x,float y) {return canvas.edge(d(x,y));},[&](float x,float y,Rgb) {
            Rgb c=y<lip ? mix(Rgb{22,23,27},Rgb{15,16,19},clamp01((y-r.y)/(lip-r.y)))
                        : mix(Rgb{40,41,46},Rgb{32,33,37},clamp01((y-lip)/(r.y+r.h-lip)));
            c=c*(1-0.6f*canvas.edge(std::fabs(y-lip)-0.5f))+Rgb{3,3,3}*speckle(x,y);
            return held ? c*(0.45f+0.15f*clamp01(-d(x,y-2)/4)) : c;
        });
        canvas.paint(r.x,r.y-1,r.x+r.w,r.y+2,[&](float x,float y) {
            return canvas.edge(std::fabs(y-r.y-0.8f)-0.35f)*canvas.edge(d(x,y))*(held ? 0.1f : 0.35f);
        },[](float,float,Rgb) {return Rgb{96,100,110};});
        const Rgb ink=held ? Rgb{92,95,102} : Rgb{206,209,216};
        const float k=1.35f,cx=r.x+r.w*0.5f,cy=r.y+(lip-r.y)*0.5f+sink,reach=11*k;
        canvas.paint(cx-reach,cy-reach,cx+reach,cy+reach,[&](float x,float y) {
            return canvas.edge(iconDistance(b.icon,(x-cx)/k,(y-cy)/k)*k-0.6f*k);
        },[&](float,float,Rgb) {return ink;});
    }
    // The chrome shield: wide, small top corners, a slight taper to big rounded lower corners.
    SDL_Rect n=profile.navigator,sel=profile.select;n.x+=ox;sel.x+=ox;
    const float ncx=n.x+n.w*0.5f,ncy=n.y+n.h*0.5f,topCorner=5,bottomCorner=38,taper=0.04f,rim=11;
    auto shield=[&](float x,float y) {
        const float half=n.w*0.5f*(1-taper*clamp01((y-n.y)/n.h)),r=y<ncy ? topCorner : bottomCorner;
        const float qx=std::fabs(x-ncx)-half+r,qy=std::fabs(y-ncy)-n.h*0.5f+r;
        return std::hypot(std::max(qx,0.f),std::max(qy,0.f))+std::min(std::max(qx,qy),0.f)-r;
    };
    const float x0=n.x-4,y0=n.y-3,x1=n.x+n.w+4,y1=n.y+n.h+6;
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return clamp01((2.5f-shield(x,y-2.2f))/5)*0.7f;},
                 [](float,float,Rgb) {return Rgb{0,0,0};});
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(shield(x,y)-1.3f);},
                 [](float,float,Rgb) {return Rgb{4,4,6};});
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {return canvas.edge(shield(x,y));},[&](float x,float y,Rgb) {
        const float t=clamp01((y-n.y)/n.h),d=shield(x,y),side=std::fabs(x-ncx)/(n.w*0.5f);
        // Polished chrome: bright on top, a darker band across the middle, light again lower down.
        Rgb c=t<0.4f ? mix(Rgb{246,247,250},Rgb{184,187,194},t/0.4f) : mix(Rgb{184,187,194},Rgb{220,222,227},(t-0.4f)/0.6f);
        // The bevelled rim: lit along the top, in shadow down the sides and bright on the bottom lip.
        if(d>-rim) {
            const float lit=y<n.y+rim+2 ? 1.06f : t>0.82f ? 1.02f : 0.68f+0.1f*(1-side)*(x<ncx ? 1.4f : 0.6f);
            c=c*lit;
        }
        c=c*(1-0.35f*canvas.edge(std::fabs(d+rim)-0.4f))*(1-0.3f*clamp01(1+d/1.2f));
        return c*(1-0.5f*navigatorHeld(n,x,y,pressed));
    });
    // A highlight along the top edge and a fine light line inside the rim's crease.
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
        return canvas.edge(std::fabs(shield(x,y)+0.8f)-0.35f)*0.7f*clamp01((ncy-y)/(n.h*0.5f))*
               (1-navigatorHeld(n,x,y,pressed)*0.7f);
    },[](float,float,Rgb) {return Rgb{255,255,255};});
    canvas.paint(x0,y0,x1,y1,[&](float x,float y) {
        return canvas.edge(std::fabs(shield(x,y)+rim+1.1f)-0.35f)*0.5f*(1-navigatorHeld(n,x,y,pressed)*0.7f);
    },[](float,float,Rgb) {return Rgb{255,255,255};});
    // Select: a dark recessed rounded rectangle with a lighter inner face low inside it.
    const bool held=down(keyIdSelect);
    const float sc=profile.selectCorner,sink=held ? 1.f : 0;
    auto ds=[&](float x,float y) {return deviceRoundedDistance(sel,sc,x,y);};
    canvas.paint(sel.x-3,sel.y-3,sel.x+sel.w+3,sel.y+sel.h+3,[&](float x,float y) {return canvas.edge(ds(x,y)-1.5f);},
                 [](float,float,Rgb) {return Rgb{70,72,78};});
    canvas.paint(sel.x-3,sel.y-3,sel.x+sel.w+3,sel.y+sel.h+3,[&](float x,float y) {return canvas.edge(ds(x,y));},
                 [&](float,float y,Rgb) {return mix(Rgb{12,12,15},Rgb{30,31,36},clamp01((y-sel.y)/sel.h));});
    // The recess's upper wall catches a crescent of light.
    canvas.paint(sel.x,sel.y,sel.x+sel.w,sel.y+sel.h*0.5f,[&](float x,float y) {
        const float u=(x-sel.x-sel.w*0.5f)/(sel.w*0.36f),v=(y-sel.y-sel.h*0.36f)/(sel.h*0.2f);
        return clamp01(1-std::fabs(std::hypot(u,v)-1)*3)*clamp01(-v)*canvas.edge(ds(x,y)+2)*(held ? 0.2f : 0.7f);
    },[](float,float,Rgb) {return Rgb{214,216,222};});
    const SDL_Rect face{sel.x+5,sel.y+int(sel.h*0.4f),sel.w-10,sel.h-int(sel.h*0.4f)-5};
    auto df=[&](float x,float y) {return deviceRoundedDistance(face,4,x,y-sink);};
    canvas.paint(face.x-1,face.y-1,face.x+face.w+1,face.y+face.h+2,[&](float x,float y) {return canvas.edge(df(x,y));},
                 [&](float,float y,Rgb) {
        const Rgb c=mix(Rgb{214,216,222},Rgb{156,160,168},clamp01((y-face.y-sink)/face.h));
        return held ? c*0.5f : c;
    });
}

// AlphaSmart Dana: black moulded body, dark grey sculpted keys in a near-black well, white
// italic legends with the function layer in yellow. The top row's caps are a lighter grey.
const Rgb danaWhite{236,236,230},danaYellow{238,202,74},danaGlow{226,146,44};

// Distance to italic text from the stroke font, centred on x like tg50TextDistance.
float danaTextDistance(const char* text, float x, float y, float size) {
    return tg50TextDistance(text,x-0.2f*(6*size-y),y,size);
}

void paintDanaKey(Canvas& canvas, const DanaKeyArt& key, bool held) {
    const SDL_Rect& b=key.bounds;
    const SDL_Rect cap{b.x+2,b.y+2,b.w-4,b.h-4};
    const float sink=held ? 1.2f : 0;
    const Rgb skirt=key.topRow ? Rgb{86,90,100} : Rgb{58,60,67};
    const Rgb faceTop=key.topRow ? Rgb{104,108,119} : Rgb{70,72,80};
    const Rgb faceBottom=key.topRow ? Rgb{84,88,98} : Rgb{54,56,63};
    // Cap outline and skirt: the top skirt catches the light, the bottom one is in shadow.
    auto outer=[&](float x,float y) {return deviceRoundedDistance(cap,3.5f,x,y);};
    const SDL_Rect face{cap.x+4,cap.y+int(held ? 2 : 1),cap.w-8,cap.h-int(held ? 4 : 6)};
    auto top=[&](float x,float y) {return deviceRoundedDistance(face,3,x,y-sink*0.4f);};
    canvas.paint(cap.x-1,cap.y-1,cap.x+cap.w+1,cap.y+cap.h+1,[&](float x,float y) {return canvas.edge(outer(x,y));},
                 [&](float x,float y,Rgb) {
        const float down=clamp01((y-cap.y)/cap.h),side=std::fabs(x-cap.x-cap.w*0.5f)/(cap.w*0.5f);
        Rgb c=mix(skirt*1.22f,skirt*0.62f,down)*(1-0.18f*side*side);
        return held ? c*0.62f : c;
    });
    // Top face, slightly dished: brighter at the back, darker towards the typist.
    canvas.paint(face.x-1,face.y-1,face.x+face.w+1,face.y+face.h+2,[&](float x,float y) {return canvas.edge(top(x,y));},
                 [&](float x,float y,Rgb) {
        const float t=clamp01((y-face.y)/face.h);
        const float dish=std::pow(std::fabs(x-face.x-face.w*0.5f)/(face.w*0.5f),2.f);
        Rgb c=mix(faceTop,faceBottom,t)*(0.96f+0.06f*dish)+Rgb{3,3,3}*speckle(x,y);
        // Held: darker, with the well's shadow over its top edge.
        return held ? c*(0.55f+0.12f*clamp01((y-face.y-2)/5)) : c;
    });
    canvas.paint(face.x,face.y-1,face.x+face.w,face.y+3,[&](float x,float y) {
        return canvas.edge(std::fabs(top(x,y)+0.7f)-0.3f)*clamp01((face.y+2-y)/2)*(held ? 0.12f : 0.45f);
    },[](float,float,Rgb) {return Rgb{190,194,206};});

    // Legends glow amber while held, as on the other keyboards; yellow ones stay yellow.
    const Rgb ink=held ? danaGlow : danaWhite,second=held ? danaYellow*0.85f : danaYellow;
    const float fx=face.x+2.5f,fw=face.w-4.f,fy=face.y+sink*0.4f;
    auto text=[&](const char* t,float tx,float ty,float size,Rgb colour,bool centre) {
        // Shrinks to fit the face; centre puts x mid-line, otherwise x is the left edge.
        size=std::min(size,fw/std::max(1.f,tg50TextWidth(t,1)+1.5f));
        const float w=tg50TextWidth(t,size),cx=centre ? tx : tx+w*0.5f;
        const float weight=std::max(0.42f,0.42f*size+0.08f);
        canvas.paint(cx-w*0.5f-3,ty-2*size-1,cx+w*0.5f+3,ty+8*size+1,[&](float x,float y) {
            return canvas.edge(danaTextDistance(t,x-cx,y-ty,size)-weight);
        },[&](float,float,Rgb) {return colour;});
    };
    auto lines=[&](const char* t,float tx,float ty,float size,Rgb colour,bool centre) {
        std::string all(t);size_t at=0;int row=0;
        while(at<=all.size()) {
            const size_t end=std::min(all.find('\n',at),all.size());
            text(all.substr(at,end-at).c_str(),tx,ty+row*8.6f*size,size,colour,centre);
            at=end+1;++row;
        }
    };
    auto shape=[&](auto distance,Rgb colour) {
        canvas.paint(face.x-2,face.y-2,face.x+face.w+2,face.y+face.h+3,[&](float x,float y) {return canvas.edge(distance(x,y));},
                     [&](float,float,Rgb) {return colour;});
    };
    // Thin arrow from (ax,ay) to its head at (hx,hy).
    auto arrow=[&](float ax,float ay,float hx,float hy,float head) {
        const float len=std::hypot(hx-ax,hy-ay),ux=(hx-ax)/len,uy=(hy-ay)/len;
        return [=](float x,float y) {
            float d=segment(x,y,ax,ay,hx,hy);
            d=std::min(d,segment(x,y,hx,hy,hx-ux*head-uy*head*0.6f,hy-uy*head+ux*head*0.6f));
            d=std::min(d,segment(x,y,hx,hy,hx-ux*head+uy*head*0.6f,hy-uy*head-ux*head*0.6f));
            return d-0.5f;
        };
    };
    const float cx=face.x+face.w*0.5f,cy=fy+face.h*0.5f;
    const size_t chars=std::strlen(key.legend);
    if(key.topRow) {
        // Words centred, the function layer on a second line.
        const bool two=*key.second || key.mark==DanaKeyMark::apps;
        const float size=chars<=2 ? 1.0f : 0.62f;
        lines(key.legend,cx,two ? fy+2.4f : cy-3*size,size,ink,true);
        if(*key.second)lines(key.second,cx,fy+10.4f,0.62f,second,true);
    } else if(*key.upper) {
        text(key.upper,fx,fy+2.2f,1.0f,ink,false);
        text(key.legend,fx,fy+15.f,1.0f,ink,false);
    } else if(chars==1) {
        text(key.legend,fx+0.5f,fy+3,1.3f,ink,false);
    } else if(*key.legend) {
        lines(key.legend,fx,fy+2.f,0.66f,ink,false);
    }
    const bool arrowKey=key.mark>=DanaKeyMark::up && key.mark<=DanaKeyMark::right;
    if(!key.topRow && *key.second && !arrowKey) {
        // Function-layer word at the lower right.
        const float size=std::min(0.62f,fw/std::max(1.f,tg50TextWidth(key.second,1)+1.5f));
        text(key.second,face.x+face.w-2-tg50TextWidth(key.second,size)*0.5f,face.y+face.h-6*size-2+sink*0.4f,size,second,true);
    }
    switch(key.mark) {
        case DanaKeyMark::tab:
            shape([&](float x,float y) {
                const float l=fx+1,r=fx+13,a=fy+13,c=fy+17;
                return std::min({arrow(r,a,l,a,2.4f)(x,y),segment(x,y,l-0.6f,a-2.2f,l-0.6f,a+1.6f)-0.5f,
                                 arrow(l,c,r,c,2.4f)(x,y),segment(x,y,r+0.6f,c-1.6f,r+0.6f,c+2.2f)-0.5f});
            },ink);
            break;
        case DanaKeyMark::backspace:
            shape([&](float x,float y) {return arrow(fx+18,fy+15,fx+1,fy+15,2.6f)(x,y);},ink);
            break;
        case DanaKeyMark::enter:
            shape([&](float x,float y) {
                const float l=fx+1,r=fx+15,ay=fy+16;
                return std::min(arrow(r,ay,l,ay,2.6f)(x,y),segment(x,y,r,ay,r,ay-4)-0.5f);
            },ink);
            break;
        case DanaKeyMark::up: case DanaKeyMark::down: case DanaKeyMark::left: case DanaKeyMark::right: {
            // Up has its function word above the arrow, the others below it.
            const bool up=key.mark==DanaKeyMark::up;
            const float size=0.6f,word=up ? fy+2.f : face.y+face.h-6*size-2.5f+sink*0.4f;
            text(key.second,cx,word,size,second,true);
            const float ax=cx,ay=up ? fy+14.5f : fy+6.5f;
            float dx=0,dy=0;
            if(key.mark==DanaKeyMark::up)dy=-1;
            if(key.mark==DanaKeyMark::down)dy=1;
            if(key.mark==DanaKeyMark::left)dx=-1;
            if(key.mark==DanaKeyMark::right)dx=1;
            const float half=dx!=0 ? 6.f : 4.f;
            shape([&](float x,float y) {return arrow(ax-dx*half,ay-dy*half,ax+dx*half,ay+dy*half,2.4f)(x,y);},ink);
            break;
        }
        case DanaKeyMark::command:
            // Command loop sign, then the yellow check.
            shape([&](float x,float y) {
                // Four side lines run on past the square into a loop at each corner.
                const float px=fx+5,py=fy+6,h=1.7f,r=1.3f,e=h+r;
                float d=1e9f;
                for(float k:{-h,h}) {
                    d=std::min({d,segment(x,y,px+k,py-e,px+k,py+e),segment(x,y,px-e,py+k,px+e,py+k)});
                    for(float m:{-e,e})d=std::min(d,ring(x,y,px+(k<0 ? -e : e),py+m,r));
                }
                return d-0.45f;
            },ink);
            shape([&](float x,float y) {
                const float px=fx+18,py=fy+8;
                return std::min(segment(x,y,px-3,py,px-1,py+2.6f),segment(x,y,px-1,py+2.6f,px+3,py-3.4f))-0.55f;
            },second);
            text("cmd",fx,face.y+face.h-6*0.66f-2.5f+sink*0.4f,0.66f,ink,false);
            break;
        case DanaKeyMark::apps: {
            const float k=0.42f,ix=cx,iy=fy+13.5f;
            shape([&](float x,float y) {return iconDistance(DeviceControlIcon::home,(x-ix)/k,(y-iy)/k)*k-0.5f;},second);
            break;
        }
        case DanaKeyMark::none: break;
    }
}

void paintDanaKeyboard(Canvas& canvas, const DanaKeysHeld& held) {
    const auto& art=danaKeyboardArt();
    const float W=art.width,H=art.height;
    // Body: black moulded plastic with a faint blue cast, lit from above.
    canvas.paint(0,0,W,H,[](float,float) {return 1.f;},[&](float x,float y,Rgb) {
        const float t=clamp01(y/H),side=std::fabs(x-W*0.5f)/(W*0.5f);
        return mix(Rgb{62,66,80},Rgb{40,43,53},t)*(1-0.1f*side*side)+Rgb{4,4,5}*speckle(x*0.7f,y*0.7f);
    });
    // The LCD bezel's lower edge, and the rounded foot.
    canvas.paint(0,0,W,4,[&](float,float y) {return canvas.edge(y-1.2f);},[](float,float,Rgb) {return Rgb{20,21,26};});
    canvas.paint(0,0,W,5,[&](float,float y) {return canvas.edge(std::fabs(y-2.4f)-0.5f)*0.55f;},[](float,float,Rgb) {return Rgb{120,126,142};});
    canvas.paint(0,H-4,W,H,[&](float,float y) {return canvas.edge(std::fabs(y-H+1.2f)-1.f)*0.7f;},[](float,float,Rgb) {return Rgb{24,26,32};});
    // The well, with a pale lip where the body meets it below.
    const SDL_Rect a=art.well,b=art.arrowWell;
    auto well=[&](float x,float y) {return std::min(deviceRoundedDistance(a,3,x,y),deviceRoundedDistance(b,3,x,y));};
    canvas.paint(0,a.y-3,W,b.y+b.h+3,[&](float x,float y) {
        return canvas.edge(std::fabs(well(x,y)-1.1f)-0.5f)*clamp01((y-a.y)/(b.y+b.h-a.y))*0.5f;
    },[](float,float,Rgb) {return Rgb{118,124,140};});
    canvas.paint(0,a.y-1,W,b.y+b.h+1,[&](float x,float y) {return canvas.edge(well(x,y));},[](float,float,Rgb) {return Rgb{14,15,19};});
    for(size_t i=0;i<art.count;++i)paintDanaKey(canvas,art.keys[i],held.Held(i));
}

// Tapwave Zodiac: original vector art on a rounded matte charcoal body with a black bezel where it
// meets the LCD. Round power, Function and Home keys, the stick as a dark dimpled knob on a light
// grey collar in a small well that follows the held position, four silver Action keys in a dark
// round recess with coloured dots, speaker slots, and slim trigger tabs and a Bluetooth lamp as
// stand-ins for the keys on the top edge. Paints the body from x0 to x1 and 0 to h and the keys of
// one section authored from x0 to x1, in the section's own units; screenHeight places around
// keys on a panel taller than authored.
float smoothMax(float a, float b, float k) {return 0.5f*(a+b+std::sqrt((a-b)*(a-b)+k*k));}
// Signed distance to one Action key: the part of the recess's quarter facing (ux,uy) inside an
// outer circle and a wider circle further out, kept off the diagonals; rounded tips.
float zodiacActionKey(float x, float y, float cx, float cy, float ux, float uy, float outer) {
    const float inner=outer*0.32f,tip=0.70f;
    const float far=(outer*outer-inner*inner)/(2*(outer*std::cos(tip)-inner)),wide=far-inner;
    const float dx=x-cx,dy=y-cy;
    const float a=std::hypot(dx,dy)-outer;
    const float b=std::hypot(dx-ux*far,dy-uy*far)-wide;
    const float gap=outer*0.07f-std::min(std::fabs(dx+dy),std::fabs(dx-dy))*0.70710678f;
    const float side=-(dx*ux+dy*uy);
    return smoothMax(smoothMax(smoothMax(a,b,3.f),gap,3.f),side,1.f);
}
void paintZodiacControls(Canvas& canvas, const DeviceControlProfile& profile, DeviceKeyMask pressed,
                         DeviceStick stick, DeviceControlSection section, float x0, float x1, float h,
                         int screenHeight=0) {
    auto down=[&](KeyId key) {return key!=keyInvalid && (pressed&deviceKeyBit(key))!=0;};
    const bool around=section==DeviceControlSection::around;
    // Around keys below the triggers move down this far on a taller panel, as deviceAroundTop.
    const float top=around ? float(std::max(0,screenHeight*profile.unit-profile.panelHeight)/2) : 0;
    // The body: square where it meets the LCD (past the panel's inner edge, or above the strip),
    // rounded at the outer corners. Around, the right panel is authored from x 150.
    const bool right=around && x0>0;
    const float bezel=3;
    const float bl=around ? (right ? x0-40 : x0) : x0,br=around ? (right ? x1 : x1+40) : x1;
    const float bt=around ? 0 : -40,bb=h;
    auto body=[&](float x,float y) {
        const float r=y<(bt+bb)*0.5f ? 26.f : 44.f;
        const SDL_Rect rect{int(bl),int(bt),int(br-bl),int(bb-bt)};
        return deviceRoundedDistance(rect,r,x,y);
    };
    // Distance from the LCD's edge, where the black bezel sits.
    auto fromLcd=[&](float x,float y) {return around ? (right ? x-x0 : x1-x) : y;};
    canvas.paint(x0,0,x1,h,[](float,float) {return 1.f;},[](float,float,Rgb) {return Rgb{0,0,0};});
    canvas.paint(x0,0,x1,h,[&](float x,float y) {return canvas.edge(body(x,y));},[&](float x,float y,Rgb) {
        Rgb c=mix(Rgb{60,62,67},Rgb{40,41,45},std::pow(clamp01(y/h),1.1f));
        // Matte, falling off towards the rounded outer edge and the bezel.
        c=c*(1-0.28f*clamp01(1+body(x,y)/16))*(1-0.18f*clamp01(1-(fromLcd(x,y)-bezel)/10));
        return c+Rgb{3,3,3}*speckle(x,y);
    });
    // A soft sheen along the outer edge, strongest at the top.
    canvas.paint(x0,0,x1,h,[&](float x,float y) {
        return canvas.edge(std::fabs(body(x,y)+1.4f)-0.6f)*(0.18f+0.25f*clamp01(1-y/h));
    },[](float,float,Rgb) {return Rgb{128,132,140};});
    // The black bezel round the LCD with a faint lip where the body starts.
    canvas.paint(x0,0,x1,h,[&](float x,float y) {return canvas.edge(fromLcd(x,y)-bezel);},
                 [](float,float,Rgb) {return Rgb{6,6,8};});
    canvas.paint(x0,0,x1,h,[&](float x,float y) {return canvas.edge(std::fabs(fromLcd(x,y)-bezel-0.6f)-0.4f)*0.5f;},
                 [](float,float,Rgb) {return Rgb{100,104,112};});
    // Speaker slots: three short slanted slots below the left keys and below the Action keys.
    auto slots=[&](float sx,float sy) {
        for(int i=-1;i<=1;++i) {
            const float ax=sx+i*5.5f+1.3f,ay=sy-5.5f,bx=sx+i*5.5f-1.3f,by=sy+5.5f;
            canvas.paint(sx-12,sy-8,sx+12,sy+9,[&](float x,float y) {return canvas.edge(segment(x,y-0.8f,ax,ay,bx,by)-1.3f)*0.6f;},
                         [](float,float,Rgb) {return Rgb{104,108,116};});
            canvas.paint(sx-12,sy-8,sx+12,sy+9,[&](float x,float y) {return canvas.edge(segment(x,y,ax,ay,bx,by)-1.3f);},
                         [&](float,float y,Rgb) {return mix(Rgb{6,6,8},Rgb{20,21,24},clamp01((y-sy+4)/9));});
        }
    };
    if(around) {if(right) slots(244,246+top); else slots(50,246+top);}
    else {slots(81,182);slots(400,182);}
    // A dark rim and inner shadow round a pressed key, and a light ring just outside it.
    auto pressedRim=[&](float bx0,float by0,float bx1,float by1,auto d) {
        canvas.paint(bx0,by0,bx1,by1,[&](float x,float y) {return canvas.edge(std::fabs(d(x,y)-1.6f)-0.9f)*0.85f;},
                     [](float,float,Rgb) {return Rgb{196,206,222};});
    };
    auto innerShadow=[&](float bx0,float by0,float bx1,float by1,auto d,float strength) {
        canvas.paint(bx0,by0,bx1,by1,[&](float x,float y) {
            const float inside=-d(x,y);
            return inside>0 ? clamp01(1-inside/4.5f)*clamp01(0.4f-d(x,y+2.5f)/3)*strength : 0.f;
        },[](float,float,Rgb) {return Rgb{0,0,0};});
    };
    bool recessDone=false;
    for(size_t i=0;i<profile.count;++i) {
        const auto& b=profile.buttons[i];
        if(b.section!=section || b.bounds.x<x0 || b.bounds.x>=x1)continue;
        SDL_Rect r=b.bounds;
        const float dy=around ? float(deviceAroundTop(profile,b,screenHeight)) : 0;
        r.y+=int(dy);
        const float cx=r.x+r.w*0.5f,cy=r.y+r.h*0.5f;
        const float bx0=r.x-8,by0=r.y-8,bx1=r.x+r.w+8,by1=r.y+r.h+10;
        const bool held=down(b.key);
        if(b.icon==DeviceControlIcon::stick) {
            const float well=r.w*0.5f,knob=r.w*deviceStickKnob,collar=knob*1.22f,travel=r.w*deviceStickTravel;
            const float kx=cx+stick.x*travel,ky=cy+stick.y*travel;
            // A low raised ring round the well, then the dark well with a lit lower lip.
            canvas.paint(bx0-6,by0-6,bx1+6,by1+6,[&](float x,float y) {
                return clamp01((well+9-std::hypot(x-cx,y-cy-2))/9)*0.35f;
            },[](float,float,Rgb) {return Rgb{0,0,0};});
            canvas.paint(bx0-6,by0-6,bx1+6,by1+6,[&](float x,float y) {
                return canvas.edge(std::fabs(std::hypot(x-cx,y-cy)-well-3)-2.5f)*clamp01((cy-y)/well+0.3f)*0.35f;
            },[](float,float,Rgb) {return Rgb{92,96,104};});
            // The outer part of the well slopes down from the body into the dark middle.
            canvas.paint(bx0,by0,bx1,by1,[&](float x,float y) {return canvas.edge(std::hypot(x-cx,y-cy)-well);},
                         [&](float x,float y,Rgb) {
                const float slope=clamp01((std::hypot(x-cx,y-cy)-well*0.8f)/(well*0.2f));
                const Rgb deep=mix(Rgb{10,10,12},Rgb{24,25,28},clamp01((y-r.y)/r.h));
                return mix(deep,mix(Rgb{24,25,28},Rgb{46,48,52},clamp01((y-r.y)/r.h)),slope*slope);
            });
            canvas.paint(bx0,by0,bx1,by1,[&](float x,float y) {
                return canvas.edge(std::fabs(std::hypot(x-cx,y-cy)-well+0.6f)-0.5f)*clamp01((y-cy)/well)*0.8f;
            },[](float,float,Rgb) {return Rgb{110,114,122};});
            // The collar slides with the knob under the well's lip; its shadow first.
            auto inWell=[&](float x,float y) {return canvas.edge(std::hypot(x-cx,y-cy)-well+0.8f);};
            canvas.paint(bx0,by0,bx1,by1,[&](float x,float y) {
                return clamp01((collar+3-std::hypot(x-kx,y-ky-3))/6)*0.7f*inWell(x,y);
            },[](float,float,Rgb) {return Rgb{0,0,0};});
            canvas.paint(bx0,by0,bx1,by1,[&](float x,float y) {return canvas.edge(std::hypot(x-kx,y-ky)-collar)*inWell(x,y);},
                         [&](float x,float y,Rgb) {
                // Light grey, lit from the lower right as in the photos' crescent.
                const float t=clamp01(((x-kx)+(y-ky))/(2.4f*collar)+0.5f);
                const Rgb c=mix(Rgb{70,72,78},Rgb{204,206,210},t*t);
                return held ? c*0.75f : c;
            });
            // The knob: dark, slightly domed, with a dimpled face.
            canvas.paint(bx0,by0,bx1,by1,[&](float x,float y) {return canvas.edge(std::hypot(x-kx,y-ky+0.6f)-knob-0.8f)*0.8f;},
                         [](float,float,Rgb) {return Rgb{14,14,16};});
            canvas.paint(bx0,by0,bx1,by1,[&](float x,float y) {return canvas.edge(std::hypot(x-kx,y-ky)-knob);},
                         [&](float x,float y,Rgb) {
                const float d=std::hypot(x-kx,y-ky)/knob;
                Rgb c=mix(Rgb{80,82,88},Rgb{44,46,50},clamp01((y-ky+knob)/(2*knob)));
                c=c*(1-0.22f*clamp01(d*d*d));
                // Dimples: one in the middle and two rings on eight spokes.
                float dimple=std::hypot(x-kx,y-ky);
                for(int k=0;k<8;++k) {
                    const float a=degrees(22.5f+45*k),ca=std::cos(a),sa=std::sin(a);
                    for(float ring:{0.36f,0.66f})
                        dimple=std::min(dimple,std::hypot(x-kx-ca*ring*knob,y-ky-sa*ring*knob));
                }
                c=c*(1-0.45f*canvas.edge(dimple-knob*0.055f));
                return held ? c*0.62f : stick.held ? c*0.88f : c;
            });
            canvas.paint(bx0,by0,bx1,by1,[&](float x,float y) {
                return canvas.edge(std::fabs(std::hypot(x-kx,y-ky)-knob*0.93f)-0.45f)*clamp01((ky-y)/knob)*0.45f;
            },[](float,float,Rgb) {return Rgb{150,154,162};});
            continue;
        }
        if(b.icon==DeviceControlIcon::action && b.shape.w>0) {
            const float rx=b.shape.x+b.shape.w*0.5f,ry=b.shape.y+dy+b.shape.h*0.5f,recess=b.shape.w*0.5f;
            const float outer=recess-6;
            const float kx=cx-rx,ky=cy-ry,kl=std::hypot(kx,ky),ux=kx/kl,uy=ky/kl;
            if(!recessDone) {
                recessDone=true;
                const float px0=rx-recess-10,py0=ry-recess-10,px1=rx+recess+10,py1=ry+recess+12;
                // A raised ring round the dark recess, a lit lower lip and a shadow under the top edge.
                canvas.paint(px0,py0,px1,py1,[&](float x,float y) {
                    return canvas.edge(std::fabs(std::hypot(x-rx,y-ry)-recess-3)-2.5f)*clamp01((ry-y)/recess+0.3f)*0.35f;
                },[](float,float,Rgb) {return Rgb{92,96,104};});
                canvas.paint(px0,py0,px1,py1,[&](float x,float y) {return canvas.edge(std::hypot(x-rx,y-ry)-recess);},
                             [&](float,float y,Rgb) {return mix(Rgb{14,14,16},Rgb{30,31,35},clamp01((y-ry+recess)/(2*recess)));});
                canvas.paint(px0,py0,px1,py1,[&](float x,float y) {
                    return canvas.edge(std::fabs(std::hypot(x-rx,y-ry)-recess+0.6f)-0.5f)*clamp01((y-ry)/recess)*0.8f;
                },[](float,float,Rgb) {return Rgb{110,114,122};});
            }
            const float sink=held ? 0.8f : 0;
            auto d=[&](float x,float y) {return zodiacActionKey(x,y-sink,rx,ry,ux,uy,outer);};
            const float ax0=rx-recess,ay0=ry-recess,ax1=rx+recess,ay1=ry+recess;
            // Cast shadow, dark seam, then the silver face: lighter towards the outside with a
            // shaded hollow inside, as on the real keys.
            canvas.paint(ax0,ay0,ax1,ay1,[&](float x,float y) {return clamp01((2-d(x,y-1.8f))/4)*(held ? 0.2f : 0.6f);},
                         [](float,float,Rgb) {return Rgb{0,0,0};});
            canvas.paint(ax0,ay0,ax1,ay1,[&](float x,float y) {return canvas.edge(d(x,y)-1.1f);},
                         [](float,float,Rgb) {return Rgb{8,8,10};});
            canvas.paint(ax0,ay0,ax1,ay1,[&](float x,float y) {return canvas.edge(d(x,y));},[&](float x,float y,Rgb) {
                const float along=((x-rx)*ux+(y-ry-sink)*uy)/outer;
                Rgb c=mix(Rgb{150,153,158},Rgb{214,216,220},clamp01((along-0.3f)/0.6f));
                c=c*(1-0.12f*clamp01((y-ry)/outer+0.5f));
                // The hollow: a soft darker band across the key's middle.
                c=c*(1-0.14f*clamp01(1-std::fabs(along-0.52f)/0.18f));
                c=c+Rgb{5,5,5}*speckle(x*1.5f,y*1.5f);
                return held ? c*0.58f : c;
            });
            // A bright edge on the upper side of each key.
            canvas.paint(ax0,ay0,ax1,ay1,[&](float x,float y) {
                return canvas.edge(std::fabs(d(x,y)+0.9f)-0.4f)*clamp01((ry-y)/outer+0.6f)*(held ? 0.1f : 0.55f);
            },[](float,float,Rgb) {return Rgb{244,246,250};});
            if(held) {innerShadow(ax0,ay0,ax1,ay1,d,0.55f);pressedRim(ax0,ay0,ax1,ay1,d);}
            // The coloured dot: blue top, yellow right, green bottom, red left.
            const Rgb dot=b.key==keyIdHard4 ? Rgb{48,72,200} : b.key==keyIdHard2 ? Rgb{214,170,20}
                        : b.key==keyIdHard1 ? Rgb{40,160,70} : Rgb{204,44,40};
            const float dx0=rx+ux*outer*0.6f,dy0=ry+sink+uy*outer*0.6f;
            canvas.paint(dx0-4,dy0-4,dx0+4,dy0+4,[&](float x,float y) {return canvas.edge(std::hypot(x-dx0,y-dy0)-1.9f);},
                         [&](float,float,Rgb) {return held ? dot*0.6f : dot;});
            continue;
        }
        const float corner=deviceButtonCorner(profile,b),sink=held ? 0.8f : 0;
        auto d=[&](float x,float y) {return deviceRoundedDistance(r,corner,x,y);};
        auto dk=[&](float x,float y) {return d(x,y-sink);};
        const bool silver=b.icon==DeviceControlIcon::function;
        const bool round=b.icon!=DeviceControlIcon::trigger;
        // Round keys sit in a shallow dished ring.
        if(round) {
            canvas.paint(bx0,by0,bx1,by1,[&](float x,float y) {return canvas.edge(d(x,y)-3.2f);},
                         [&](float,float y,Rgb) {return mix(Rgb{22,23,26},Rgb{44,46,50},clamp01((y-r.y+3)/(r.h+6)));});
        }
        canvas.paint(bx0,by0,bx1,by1,[&](float x,float y) {return clamp01((2.5f-d(x,y-1.8f))/5)*(held ? 0.2f : 0.55f);},
                     [](float,float,Rgb) {return Rgb{0,0,0};});
        canvas.paint(bx0,by0,bx1,by1,[&](float x,float y) {return canvas.edge(d(x,y)-1.1f);},
                     [](float,float,Rgb) {return Rgb{8,8,10};});
        const Rgb hi=silver ? Rgb{222,224,228} : b.icon==DeviceControlIcon::trigger ? Rgb{88,91,97} : Rgb{86,89,95};
        const Rgb lo=silver ? Rgb{148,151,157} : Rgb{40,42,46};
        canvas.paint(bx0,by0,bx1,by1,[&](float x,float y) {return canvas.edge(dk(x,y));},[&](float x,float y,Rgb) {
            Rgb c=mix(hi,lo,clamp01((y-r.y-sink)/r.h));
            if(round) {
                // Slightly domed: a soft spot up and to the left.
                const float g=std::hypot((x-cx+r.w*0.18f)/(r.w*0.36f),(y-cy-sink+r.h*0.2f)/(r.h*0.28f));
                c=c+(silver ? Rgb{24,24,26} : Rgb{22,23,26})*std::pow(clamp01(1-g),2.f);
            }
            return held ? c*0.58f : c;
        });
        canvas.paint(bx0,by0,bx1,by1,[&](float x,float y) {
            return canvas.edge(std::fabs(dk(x,y)+0.8f)-0.35f)*clamp01((cy-y)/(r.h*0.5f))*(held ? 0.1f : 0.5f);
        },[&](float,float,Rgb) {return silver ? Rgb{250,250,252} : Rgb{150,154,162};});
        if(held) {innerShadow(bx0,by0,bx1,by1,dk,0.6f);pressedRim(bx0,by0,bx1,by1,d);}
        const float ix=cx,iy=cy+sink;
        const Rgb ink=held ? Rgb{120,124,132} : Rgb{184,188,196};
        if(b.icon==DeviceControlIcon::function) {
            canvas.paint(ix-4,iy-4,ix+4,iy+4,[&](float x,float y) {return canvas.edge(std::hypot(x-ix,y-iy)-1.9f);},
                         [&](float,float,Rgb) {return held ? Rgb{20,20,22} : Rgb{36,37,40};});
        }
        if(b.icon==DeviceControlIcon::home) {
            // A house outline: a pentagon with a pitched roof.
            const std::array<std::array<float,2>,5> house{{{0,-6.2f},{6,-1.2f},{6,5.6f},{-6,5.6f},{-6,-1.2f}}};
            canvas.paint(ix-10,iy-10,ix+10,iy+10,[&](float x,float y) {
                return canvas.edge(std::fabs(polygon(x-ix,y-iy,house))-0.85f);
            },[&](float,float,Rgb) {return ink;});
        }
        if(b.icon==DeviceControlIcon::power) {
            // The power symbol: a ring open at the top with a bar through the gap.
            canvas.paint(ix-10,iy-10,ix+10,iy+10,[&](float x,float y) {
                const float ring=arc(x,y,ix,iy+0.6f,5.4f,degrees(-50),degrees(230));
                return canvas.edge(std::min(ring,segment(x,y,ix,iy-6.4f,ix,iy-0.4f))-0.85f);
            },[&](float,float,Rgb) {return ink;});
        }
        if(b.icon==DeviceControlIcon::bluetooth) {
            // A blue lamp in the middle with a soft glow round it.
            const Rgb lamp=held ? Rgb{50,88,170} : Rgb{86,148,255};
            canvas.paint(ix-8,iy-8,ix+8,iy+8,[&](float x,float y) {
                return clamp01(1-std::hypot(x-ix,y-iy)/7)*0.35f;
            },[&](float,float,Rgb) {return lamp;});
            canvas.paint(ix-5,iy-5,ix+5,iy+5,[&](float x,float y) {return canvas.edge(std::hypot(x-ix,y-iy)-3.f);},
                         [&](float,float,Rgb) {return lamp;});
        }
    }
}
}  // namespace

void drawDeviceControls(SDL_Renderer* renderer, const DeviceControlProfile& profile,
                        int width, int height, float scale, DeviceKeyMask pressed,
                        const DeviceControlView& view, DeviceStick stick) {
    if(!profile.count || !deviceControlsUnits(profile,view))return;
    // The canvas is in strip units, scale/unit device pixels each; the side tab widens it.
    const int u=profile.unit;
    const DeviceControlTransform transform(profile,width,view);
    const bool tg50=profile.style==DeviceControlStyle::tg50;
    // The TG50 and Zodiac strips scale to the window; the canvas stays in strip units.
    const bool scaled=tg50 || profile.style==DeviceControlStyle::zodiac;
    // Beside the buttons the strip is drawn at its scale right of the side tab, from screenHeight+top.
    const bool beside=deviceSideTabBesideButtons(profile,view);
    Canvas canvas(scaled || beside ? profile.width : deviceControlsWidth(profile,width,view)*u,
                  deviceControlsUnits(profile,view),scale*transform.scale);
    // Allocate the same integer device rectangle as the cached renderer target.
    if(scaled) {
        canvas.w=int(deviceControlsWidth(profile,width,view)*scale);
        canvas.h=int(deviceControlsHeight(profile,width,view)*scale);
        canvas.pixels.resize(size_t(canvas.w)*canvas.h);
    }
    const int stripX=beside ? int(std::lround(transform.left*scale)) : 0;
    if(beside) {
        canvas.w=int(width*scale)-stripX;
        canvas.h=int((deviceControlsHeight(profile,width,view)-transform.top)*scale);
        canvas.pixels.resize(size_t(canvas.w)*canvas.h);
    }
    canvas.originX=scaled ? transform.left/transform.scale : beside ? 0 : deviceScreenLeft(profile,view)*u;
    switch(profile.style) {
        case DeviceControlStyle::e2: paintE2Controls(canvas,profile,width*u,pressed);break;
        case DeviceControlStyle::zire72: paintZire72Controls(canvas,profile,width*u,pressed);break;
        case DeviceControlStyle::m515: paintM515Controls(canvas,profile,width*u,pressed,m515Look);break;
        case DeviceControlStyle::iiix: paintIIIxControls(canvas,profile,width*u,pressed,iiixBody);break;
        case DeviceControlStyle::iiie: paintIIIxControls(canvas,profile,width*u,pressed,iiieBody);break;
        case DeviceControlStyle::iiixe: paintIIIxControls(canvas,profile,width*u,pressed,iiixeBody);break;
        case DeviceControlStyle::m125: paintM125Controls(canvas,profile,width*u,pressed,m125Look);break;
        case DeviceControlStyle::m100: paintM125Controls(canvas,profile,width*u,pressed,m100Look);break;
        case DeviceControlStyle::m105: paintM125Controls(canvas,profile,width*u,pressed,m105Look);break;
        case DeviceControlStyle::v: paintVControls(canvas,profile,width*u,pressed,vLook);break;
        case DeviceControlStyle::vx: paintVControls(canvas,profile,width*u,pressed,vxLook);break;
        case DeviceControlStyle::m500: paintM515Controls(canvas,profile,width*u,pressed,m500Look);break;
        case DeviceControlStyle::c500: paintM515Controls(canvas,profile,width*u,pressed,c500Look);break;
        case DeviceControlStyle::m505: paintM515Controls(canvas,profile,width*u,pressed,m505Look);break;
        case DeviceControlStyle::m520: paintM515Controls(canvas,profile,width*u,pressed,m520Look);break;
        case DeviceControlStyle::m130: paintM125Controls(canvas,profile,width*u,pressed,m130Look);break;
        case DeviceControlStyle::iiic: paintIIIcControls(canvas,profile,width*u,pressed);break;
        case DeviceControlStyle::tg50: paintTg50Controls(canvas,profile,profile.width,pressed,view,transform);break;
        case DeviceControlStyle::zodiac:
            paintZodiacControls(canvas,profile,pressed,stick,DeviceControlSection::below,0,float(profile.width),profile.height);
            break;
        case DeviceControlStyle::ipaq210: paintIpaq210Controls(canvas,profile,width*u,pressed);break;
        case DeviceControlStyle::rx1950: paintRx1950Controls(canvas,profile,width*u,pressed);break;
        case DeviceControlStyle::h4350: paintH4350Controls(canvas,profile,width*u,pressed,view);break;
        case DeviceControlStyle::t415: paintT415Controls(canvas,profile,beside ? profile.width : width*u,pressed,false,beside);break;
        default: paintTxControls(canvas,profile,width*u,pressed);
    }
    if(auto* texture=canvasTexture(renderer,canvas)) {
        const SDL_Rect dest{stripX,int(std::lround((height+transform.top)*scale)),canvas.w,canvas.h};
        SDL_RenderCopy(renderer,texture,nullptr,&dest);
        SDL_DestroyTexture(texture);
    }
    if(!deviceSideTabBelowScreen(profile,view))return;
    // The side tab left of the keyboard, on its own canvas in side tab units.
    const int tabWidth=int(transform.left);
    Canvas tab(tabWidth*u,0,scale/u);
    tab.w=int(tabWidth*scale);tab.h=canvas.h;
    tab.pixels.resize(size_t(tab.w)*tab.h);
    tab.originX=tabWidth*u;
    tab.originY=transform.sidebarTop*u;
    paintTg50Controls(tab,profile,profile.width,pressed,view,transform,true);
    if(auto* texture=canvasTexture(renderer,tab)) {
        const SDL_Rect dest{0,int(std::lround(height*scale)),tab.w,tab.h};
        SDL_RenderCopy(renderer,texture,nullptr,&dest);
        SDL_DestroyTexture(texture);
    }
}

void drawDeviceSidebar(SDL_Renderer* renderer, const DeviceControlProfile& profile,
                       int screenWidth, int screenHeight, float scale, DeviceKeyMask pressed,
                       const DeviceControlView& view, DeviceStick stick) {
    if(deviceSideTabBesideButtons(profile,view)) {
        // Left of the silkscreen and strip, from the LCD's bottom to the strip's, in side tab units.
        const DeviceControlTransform transform(profile,screenWidth,view);
        const int u=profile.unit,tabWidth=int(transform.left);
        const int lcdBottom=screenHeight-profile.silkscreenHeight/u;
        const int rows=screenHeight+deviceControlsHeight(profile,screenWidth,view)-lcdBottom;
        Canvas tab(tabWidth*u,0,scale/u);
        tab.w=int(std::lround(transform.left*scale));tab.h=int(rows*scale);
        tab.pixels.resize(size_t(tab.w)*tab.h);
        tab.originX=tabWidth*u;
        tab.originY=(lcdBottom-screenHeight-transform.sidebarTop)*-u;
        if(profile.style==DeviceControlStyle::t415)
            paintT415BesideTab(tab,profile,pressed,float((transform.top-transform.sidebarTop)*u),transform.scale*u);
        else paintTg50Controls(tab,profile,profile.width,pressed,view,transform,true);
        if(auto* texture=canvasTexture(renderer,tab)) {
            const SDL_Rect dest{0,int(std::lround(lcdBottom*scale)),tab.w,tab.h};
            SDL_RenderCopy(renderer,texture,nullptr,&dest);
            SDL_DestroyTexture(texture);
        }
        return;
    }
    const int left=deviceScreenLeft(profile,view);
    if(!left)return;
    if(deviceControlsAround(profile,view)) {
        // One canvas per panel, each the screen's height, in strip units from the panel's
        // authored left edge; the right one ends on the window's integer right edge.
        const int right=int(left*scale)+int(screenWidth*scale);
        for(bool second:{false,true}) {
            const int x0=second ? profile.panelWidth : 0;
            Canvas canvas(profile.panelWidth,screenHeight*profile.unit,scale/profile.unit);
            canvas.w=second ? int(deviceControlsWidth(profile,screenWidth,view)*scale)-right : int(left*scale);
            canvas.h=int(screenHeight*scale);
            canvas.pixels.assign(size_t(canvas.w)*canvas.h,Rgb{0,0,0});
            canvas.originX=-float(x0);
            paintZodiacControls(canvas,profile,pressed,stick,DeviceControlSection::around,float(x0),
                                float(x0+profile.panelWidth),float(screenHeight*profile.unit),screenHeight);
            const int x=second ? right : 0;
            if(auto* texture=canvasTexture(renderer,canvas)) {
                const SDL_Rect dest{x,0,canvas.w,canvas.h};
                SDL_RenderCopy(renderer,texture,nullptr,&dest);
                SDL_DestroyTexture(texture);
            }
        }
        return;
    }
    Canvas canvas(left*profile.unit,screenHeight*profile.unit,scale/profile.unit);
    // Match the LCD's integer left edge at fractional scales.
    canvas.w=int(left*scale);
    canvas.pixels.resize(size_t(canvas.w)*canvas.h);
    canvas.originX=left*profile.unit;
    canvas.originY=screenHeight*profile.unit;
    const DeviceControlTransform transform(profile,screenWidth,view);
    if(profile.style==DeviceControlStyle::t415)paintT415Controls(canvas,profile,screenWidth*profile.unit,pressed,true);
    else paintTg50Controls(canvas,profile,profile.width,pressed,view,transform,true);
    if(auto* texture=canvasTexture(renderer,canvas)) {
        const SDL_Rect dest{0,0,canvas.w,canvas.h};
        SDL_RenderCopy(renderer,texture,nullptr,&dest);
        SDL_DestroyTexture(texture);
    }
}

SDL_Texture* createDeviceSilkscreen(SDL_Renderer* renderer, const DeviceSilkscreen& silkscreen, float scale) {
    if(!silkscreen.count)return nullptr;
    Canvas canvas(silkscreen.width,silkscreen.height,scale/silkscreen.unit);
    paintSilkscreen(canvas,silkscreen);
    return canvasTexture(renderer,canvas);
}

const DanaKeyboardArt& danaKeyboardArt() {
    // Rows follow the Dana User's Guide front panel figure: 17 equal top keys, then a 14.5-key
    // wide board with the arrows' lower row standing out below its right end.
    using M=DanaKeyMark;
    static const DanaKeyArt keys[] = {
        {"power",{11,10,34,34},"on/off","","",M::none,true},
        {"f1",{45,10,34,34},"F1","","",M::none,true},{"f2",{79,10,34,34},"F2","","",M::none,true},
        {"f3",{113,10,34,34},"F3","","",M::none,true},{"f4",{147,10,34,34},"F4","","",M::none,true},
        {"f5",{181,10,34,34},"F5","","",M::none,true},{"f6",{215,10,34,34},"F6","","",M::none,true},
        {"f7",{249,10,34,34},"F7","","",M::none,true},{"f8",{283,10,34,34},"F8","","",M::none,true},
        {"calendar",{317,10,34,34},"date","","new",M::none,true},
        {"address",{351,10,34,34},"address","","cancel",M::none,true},
        {"todo",{385,10,34,34},"to do","","done",M::none,true},
        {"memo",{419,10,34,34},"memo","","note",M::none,true},
        {"find",{453,10,34,34},"find","","contrast",M::none,true},
        {"menu",{487,10,34,34},"menu","","calc",M::none,true},
        {"apps",{521,10,34,34},"apps","","",M::apps,true},
        {"send",{555,10,34,34},"send","","sync",M::none,true},

        {"`",{10,46,40,40},"`","~","",M::none},{"1",{50,46,40,40},"1","!","",M::none},
        {"2",{90,46,40,40},"2","@","",M::none},{"3",{130,46,40,40},"3","#","",M::none},
        {"4",{170,46,40,40},"4","$","",M::none},{"5",{210,46,40,40},"5","%","",M::none},
        {"6",{250,46,40,40},"6","^","",M::none},{"7",{290,46,40,40},"7","&","",M::none},
        {"8",{330,46,40,40},"8","*","",M::none},{"9",{370,46,40,40},"9","(","",M::none},
        {"0",{410,46,40,40},"0",")","",M::none},{"-",{450,46,40,40},"-","_","",M::none},
        {"=",{490,46,40,40},"=","+","",M::none},
        {"backspace",{530,46,60,40},"backspace","","",M::backspace},

        {"tab",{10,86,60,40},"tab","","",M::tab},
        {"q",{70,86,40,40},"Q","","",M::none},{"w",{110,86,40,40},"W","","",M::none},
        {"e",{150,86,40,40},"E","","",M::none},{"r",{190,86,40,40},"R","","",M::none},
        {"t",{230,86,40,40},"T","","",M::none},{"y",{270,86,40,40},"Y","","",M::none},
        {"u",{310,86,40,40},"U","","",M::none},{"i",{350,86,40,40},"I","","",M::none},
        {"o",{390,86,40,40},"O","","",M::none},{"p",{430,86,40,40},"P","","",M::none},
        {"[",{470,86,40,40},"[","{","",M::none},{"]",{510,86,40,40},"]","}","",M::none},
        {"\\",{550,86,40,40},"\\","|","",M::none},

        {"caps",{10,126,70,40},"caps lock","","",M::none},
        {"a",{80,126,40,40},"A","","",M::none},{"s",{120,126,40,40},"S","","",M::none},
        {"d",{160,126,40,40},"D","","",M::none},{"f",{200,126,40,40},"F","","",M::none},
        {"g",{240,126,40,40},"G","","",M::none},{"h",{280,126,40,40},"H","","",M::none},
        {"j",{320,126,40,40},"J","","",M::none},{"k",{360,126,40,40},"K","","",M::none},
        {"l",{400,126,40,40},"L","","",M::none},{";",{440,126,40,40},";",":","",M::none},
        {"'",{480,126,40,40},"'","\"","",M::none},
        {"enter",{520,126,70,40},"enter","","ok",M::enter},

        {"lshift",{10,166,90,40},"shift","","",M::none},
        {"z",{100,166,40,40},"Z","","",M::none},{"x",{140,166,40,40},"X","","",M::none},
        {"c",{180,166,40,40},"C","","",M::none},{"v",{220,166,40,40},"V","","",M::none},
        {"b",{260,166,40,40},"B","","",M::none},{"n",{300,166,40,40},"N","","",M::none},
        {"m",{340,166,40,40},"M","","",M::none},{",",{380,166,40,40},",","<","",M::none},
        {".",{420,166,40,40},".",">","",M::none},{"/",{460,166,40,40},"/","?","",M::none},
        {"rshift",{500,166,90,40},"shift","","",M::none},

        {"ctrl",{10,206,50,40},"ctrl","","",M::none},
        {"function",{60,206,50,40},"alt\noption","","function",M::none},
        {"command",{110,206,50,40},"","","",M::command},
        {"space",{160,206,220,40},"","","",M::none},
        {"escape",{380,206,40,40},"esc","","",M::none},
        {"delete",{420,206,40,40},"del","","delete...",M::none},
        {"up",{503,206,44,40},"","","PgUp",M::up},
        {"left",{460,246,43,34},"","","show",M::left},
        {"down",{503,246,44,34},"","","PgDn",M::down},
        {"right",{547,246,43,34},"","","details",M::right},
    };
    static const DanaKeyboardArt art{600,292,keys,std::size(keys),{8,8,584,240},{458,244,134,38}};
    return art;
}

void drawDanaKeyboard(SDL_Renderer* renderer, const SDL_Rect& dest, const DanaKeysHeld& held) {
    const auto& art=danaKeyboardArt();
    if(dest.w<=0 || dest.h<=0)return;
    Canvas canvas(art.width,art.height,float(dest.w)/art.width);
    canvas.w=dest.w;canvas.h=dest.h;
    canvas.pixels.resize(size_t(canvas.w)*canvas.h);
    paintDanaKeyboard(canvas,held);
    if(auto* texture=canvasTexture(renderer,canvas)) {
        SDL_RenderCopy(renderer,texture,nullptr,&dest);
        SDL_DestroyTexture(texture);
    }
}

void drawDanaKeyCaps(SDL_Renderer* renderer,const SDL_Rect& dest,int width,int height,
                     const DanaKeyArt* keys,size_t count,const std::vector<bool>& held) {
    if(dest.w<=0||dest.h<=0||width<=0||height<=0)return;
    Canvas canvas(width,height,float(dest.w)/width);
    for(size_t i=0;i<count;++i)paintDanaKey(canvas,keys[i],i<held.size()&&held[i]);
    if(auto* texture=canvasTexture(renderer,canvas)) {
        SDL_RenderCopy(renderer,texture,nullptr,&dest);
        SDL_DestroyTexture(texture);
    }
}
