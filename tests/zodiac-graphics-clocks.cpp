#include <cassert>
#include <cstdio>
#include "imx1_graphics_clocks.h"
using A = Imx1BootstrapAccess;
static void put(Imx1GraphicsClocks& c, uint32_t pc, uint32_t off, uint32_t v) {
    assert(c.access(pc,0x16010000+off,4,true,v) == A::ok);
}
static uint32_t sample(Imx1GraphicsClocks& c) {
    uint32_t v = 0xdeadbeef;
    assert(c.access(0x200c588c,0x16010094,4,false,v) == A::ok);
    return v;
}
static void begin(Imx1GraphicsClocks& c, uint32_t selector = 1) {
    put(c,0x200c5824,0x94,selector|0x200);
    assert((sample(c)>>16) == 0);
    put(c,0x200c5850,0x94,selector);
    put(c,0x200c587c,0x94,selector|0x100);
}
static void advance(Imx1GraphicsClocks& c, uint64_t n) {
    while (n) { auto step = n; if (c.nextEvent()) step = std::min(step,c.nextEvent()); c.advance(step); n -= step; }
}
int main() {
    Imx1GraphicsClocks c;
    put(c,0x200c45e4,0x80,0x3f);
    for (auto op : {std::array<uint32_t,3>{0x200c4e24,0xa4,0x1100020},
            {0x200c4f9c,0xa8,0x4b000200},{0x200c501c,0xac,2},{0x200c5060,0xb0,0x7f}}) {
        put(c,op[0],op[1],op[2]);
        auto bad=op[2]^0x40000;
        assert(c.access(op[0],0x16010000+op[1],4,true,bad)==A::sideEffect && c.word(op[1])==op[2]);
    }
    for (auto op : {std::array<uint32_t,3>{0x200c637c,0xa4,0x1100020},
            {0x200c6350,0xa8,0x4b000200},{0x200c632c,0xac,2}}) {
        uint32_t value=0;
        assert(c.access(op[0],0x16010000+op[1],4,false,value)==A::ok && value==op[2]);
        assert(c.access(0,0x16010000+op[1],4,false,value)==A::externalInput);
    }
    put(c,0x200c49ac,0x84,0x80100603);
    // DAC voltage, gain, current offset and range change the free-running VCO.
    uint64_t last = 0;
    for (uint32_t gain = 0; gain < 8; ++gain) {
        put(c,0x200c5930,0x88,0x4bd00240|(gain<<10));
        assert(c.pllRate().numerator > last); last=c.pllRate().numerator;
    }
    put(c,0x200c5930,0x88,0x4b700240);
    const auto low = c.pllRate().numerator;
    put(c,0x200c5930,0x88,0x4ba00240);
    const auto mid = c.pllRate().numerator;
    put(c,0x200c5930,0x88,0x4bd00240);
    const auto high = c.pllRate().numerator;
    assert(low < mid && mid < high);
    put(c,0x200c5930,0x88,0x4bd04240); assert(c.pllRate().numerator > high);
    put(c,0x200c5930,0x88,0x4bd02240); assert(c.pllRate().numerator < high*c.pllRate().denominator);
    put(c,0x200c5930,0x88,0x4bd00240);
    begin(c);
    advance(c,2000); // 10us, 33.6MHz / 4.
    assert(sample(c)>>16 == 84);
    auto partitioned = c;
    advance(c,4000);
    for (int i=0;i<4000;++i) advance(partitioned,1);
    assert(sample(c) == sample(partitioned));
    for (int i=0;i<1000;++i) assert(sample(c) == sample(partitioned));
    put(c,0x200c58c4,0x94,sample(c)&~0x100u);
    const auto stopped = sample(c); advance(c,1000); assert(sample(c)==stopped);
    put(c,0x200c587c,0x94,0xffff0101); assert(sample(c)==(stopped|0x100));
    advance(c,2000000);
    const auto completed = sample(c);
    advance(c,2000000); assert(sample(c) == completed);
    put(c,0x200c5824,0x94,0xffff0301); assert(sample(c)==0x301);
    advance(c,2000); assert(sample(c)==0x301);
    uint32_t invalid=0x104;
    assert(c.access(0x200c587c,0x16010094,4,true,invalid)==A::sideEffect);
    assert(sample(c)==0x301);
    invalid=0x101;
    assert(c.access(0,0x16010094,4,true,invalid)==A::externalInput);
    assert(c.access(0x200c587c,0x16010094,2,true,invalid)==A::width);
    // Closed-loop frequency follows independent reference and feedback ratios.
    for (auto setting : {std::array<uint32_t,3>{1,3,0}, {3,6,0}, {2,10,5}}) {
        put(c,0x200c5e0c,0x84,0x1000000|setting[0]|setting[1]<<8|setting[2]<<16);
        const uint64_t target=Imx1GraphicsClocks::referenceHz*((setting[1]+1)*8+setting[2])/((setting[0]+1)*8);
        const uint32_t gain = (target-8000000)/16000000;
        put(c,0x200c5ca8,0x88,0x4b000200 | gain<<10);
        assert(!c.pllRate().numerator && c.nextEvent());
        const auto delay=c.nextEvent(); advance(c,delay-1); assert(!c.pllRate().numerator);
        advance(c,1);
        const auto r=c.pllRate();
        assert(r.numerator*(setting[0]+1)*8 == Imx1GraphicsClocks::referenceHz*((setting[1]+1)*8+setting[2])*r.denominator);
        begin(c); advance(c,4000);
        assert((sample(c)>>16) == r.numerator*std::min<uint64_t>(4000,c.measurementTicks())/(r.denominator*Imx1GraphicsClocks::timeHz*4));
    }
    const auto r=c.pllRate();
    put(c,0x200c5ca8,0x88,0x4b000200);
    advance(c,c.nextEvent()); assert(!c.pllRate().numerator);
    put(c,0x200c5ca8,0x88,0x4b000e00);
    advance(c,c.nextEvent());
    put(c,0x200c55c0,0x8c,0x31); assert(c.systemRate().numerator==r.numerator && c.systemRate().denominator==r.denominator*4);
    put(c,0x200c4920,0x90,0x20); assert(c.pixelRate().numerator==Imx1GraphicsClocks::referenceHz && c.pixelRate().denominator==3);
    put(c,0x200c5ca8,0x88,0x4b000201); assert(!c.pllRate().numerator && !c.systemRate().numerator && c.pixelRate().numerator);
    put(c,0x200c5ca8,0x88,0xcb000200); assert(!c.pllRate().numerator);
    put(c,0x200c5ca8,0x88,0x4b000202); assert(!c.pllRate().numerator);
    put(c,0x200c5e0c,0x84,0x1f000603);
    begin(c); advance(c,4000); assert(!(sample(c)>>16));
    begin(c,2); advance(c,4000); assert(!(sample(c)>>16));
    begin(c,3); advance(c,4000); assert((sample(c)>>16)==10);
    put(c,0x200c45e4,0x80,0); begin(c,3); advance(c,4000); assert(!(sample(c)>>16));
    put(c,0x200c45e4,0x80,0x3f);
    // The programmed measurement window ends independently of host reads.
    uint16_t previous = 0;
    for (auto row : {std::array<uint32_t,3>{4,250,10},{9,500,21},{31,1600,67}}) {
        put(c,0x200c5e0c,0x84,row[0]<<24|0x00603);
        put(c,0x200c5930,0x88,0x4bd00240);
        begin(c); const auto window=c.measurementTicks(); assert(window==row[1]);
        advance(c,window/2); const auto partial=sample(c)>>16;
        advance(c,window-window/2); const auto final=sample(c);
        assert((final>>16)==row[2] && (final>>16)>partial && (final>>16)>previous); previous=final>>16;
        advance(c,window*20); assert(sample(c)==final);
        put(c,0x200c587c,0x94,0x101); advance(c,window); assert(sample(c)==final);
    }
    // Independent target ratios and gate durations need different VCO bands.
    uint32_t previousBand=~0u;
    for (auto config : {std::array<uint32_t,3>{3,6,128},{1,6,64},{1,13,32},{3,1,128}}) {
        put(c,0x200c5e0c,0x84,config[2]<<24|config[1]<<8|config[0]);
        const uint64_t targetN=Imx1GraphicsClocks::referenceHz*(config[1]+1),targetD=config[0]+1;
        const auto goal=targetN*c.measurementTicks()/(targetD*Imx1GraphicsClocks::timeHz*4);
        bool found=false;
        for (uint32_t range=0;range<2 && !found;++range) for (uint32_t gain=0;gain<8 && !found;++gain) for (uint32_t offset=0;offset<4 && !found;++offset) {
            const uint32_t band=range<<13|gain<<10|offset<<14;
            put(c,0x200c5930,0x88,0x4bd00240|band);begin(c);advance(c,c.measurementTicks());const auto upper=sample(c)>>16;
            put(c,0x200c5930,0x88,0x4b700240|band);begin(c);advance(c,c.measurementTicks());const auto lower=sample(c)>>16;
            if (upper<goal || lower>goal) continue;
            put(c,0x200c5ca8,0x88,0x4b000200|band);advance(c,c.nextEvent());
            const auto rate=c.pllRate(); assert(rate.numerator && rate.numerator*targetD==targetN*rate.denominator);
            assert(band!=previousBand);previousBand=band;found=true;
        }
        assert(found);
    }
    put(c,0x200c573c,0x98,4);
    put(c,0x200c56c0,0x8c,0x1810);
    assert(c.systemRate().denominator==4);
    put(c,0x200c573c,0x98,0);
    assert(c.systemRate().denominator==2);
    put(c,0x200c56c0,0x8c,0x1000);
    assert(c.systemRate().denominator==1);
    put(c,0x200c573c,0x98,4); assert(c.systemRate().denominator==3);
    {
        Imx1GraphicsClocks automatic;
        put(automatic,0x200c45e4,0x80,0x3f);
        put(automatic,0x200c49ac,0x84,0x25120b01);
        put(automatic,0x200c5930,0x88,0x4b001200);
        advance(automatic,automatic.nextEvent());
        put(automatic,0x200c47b0,0x8c,0x1831);
        put(automatic,0x200c4920,0x90,0x81);
        put(automatic,0x200c4c00,0x98,0x14001);
        put(automatic,0x200c1550,0x98,0x140c1);
        put(automatic,0x200c14e4,0x80,0xbf);
        put(automatic,0x200c15c0,0x80,0xa0000bf);
        put(automatic,0x200c15f0,0x8c,0x1831);
        const auto pixel=automatic.pixelRate(),pll=automatic.pllRate();
        auto measured=automatic;
        begin(measured,2);advance(measured,1900);assert((sample(measured)>>16)==19);
        assert(automatic.nextEvent()==400 && !automatic.automaticNormal());
        advance(automatic,399);assert(!automatic.automaticNormal());
        advance(automatic,1);assert(automatic.automaticNormal());
        assert(automatic.systemRate().numerator==16000000 && automatic.systemRate().denominator==4);
        assert(automatic.pixelRate().numerator==pixel.numerator && automatic.pixelRate().denominator==pixel.denominator);
        assert(automatic.pllRate().numerator==pll.numerator && automatic.pllRate().denominator==pll.denominator);
        for (int i=0;i<100;++i) sample(automatic);
        assert(automatic.automaticNormal());
        automatic.setGpuBusy(true);assert(automatic.nextEvent()==200);
        advance(automatic,199);assert(automatic.automaticNormal());
        automatic.setGpuBusy(true);advance(automatic,1);assert(!automatic.automaticNormal());
        advance(automatic,10000);assert(!automatic.automaticNormal());
        automatic.setGpuBusy(false);auto partitioned=automatic;
        advance(automatic,400);for (int i=0;i<400;++i) advance(partitioned,1);
        assert(automatic.automaticNormal() && partitioned.automaticNormal());
        put(automatic,0x200c1550,0x98,0x14001);
        assert(!automatic.automaticNormal() && !automatic.nextEvent());
        assert(automatic.systemRate().numerator*pll.denominator*4==pll.numerator*automatic.systemRate().denominator);
    }
    Imx1GraphicsFraction f,g;
    uint64_t whole=f.add(1001,7)+f.add(2001,11);
    uint64_t parts=0; for(int i=0;i<1001;++i) parts+=g.add(1,7); for(int i=0;i<2001;++i) parts+=g.add(1,11);
    assert(whole==parts && f.phase==g.phase && f.denominator==g.denominator);
    uint32_t pclk=0;
    assert(c.access(0x200c54a8,0x16010090,4,false,pclk)==A::ok && pclk==0x20);
    put(c,0x200c54c0,0x90,0x21);
    put(c,0x200c54e0,0x90,0x81);
    uint32_t invalidRmw=0x11;
    assert(c.access(0x200c54c0,0x16010090,4,true,invalidRmw)==A::sideEffect && c.word(0x90)==0x81);
    put(c,0x200c14e4,0x80,c.word(0x80)|0x80);
    put(c,0x200c1550,0x98,c.word(0x98)|1);
    put(c,0x200c165c,0x80,c.word(0x80)&0x7fffff);
    put(c,0x200c16ac,0x88,c.word(0x88)&~3u);
    const auto pll=c.pllRate();
    put(c,0x200c1174,0x88,c.word(0x88)&~4u);
    assert(c.pllRate().numerator==pll.numerator && c.pllRate().denominator==pll.denominator);
    put(c,0x200c1148,0x9c,c.word(0x9c)&~0x80u);
    uint32_t badPm=c.word(0x88)|4u;
    assert(c.access(0x200c1174,0x16010088,4,true,badPm)==A::sideEffect);
    c.reset(); assert(!c.programmed() && !c.count());
    std::puts("virtual graphics clock tests passed");
}
