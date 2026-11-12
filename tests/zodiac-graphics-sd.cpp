#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "imx1_graphics_sd.h"
#include "imx1_graphics_gpio.h"
#include "imx1_graphics_interrupts.h"
using A = Imx1BootstrapAccess;
static void put(Imx1GraphicsSd& s, uint32_t pc, uint32_t address, uint32_t v, A expected=A::ok) {
    assert(s.access(pc,address,4,true,v)==expected);
}
static uint32_t get(Imx1GraphicsSd& s, uint32_t pc, uint32_t address, uint32_t size=4) {
    uint32_t v=0xdeadbeef; assert(s.access(pc,address,size,false,v)==A::ok); return v;
}
static void initialisation(Imx1GraphicsSd& s) {
    assert(get(s,0x204d26a2,0x16010910)==0);
    put(s,0x204d26b2,0x16010910,0x430000);
    assert(get(s,0x204d26b4,0x16010910)==0x430000);
    put(s,0x204d26ba,0x16010910,0x430000);
    assert(get(s,0x204d26bc,0x1601090c)==0);
    put(s,0x204d26c2,0x1601090c,0x8000000);
    put(s,0x204d26cc,0x162021c0,0);
    auto nativeDelay = s;
    // Captured native reset writes are 19,853 instruction entries apart.
    // Replay that spacing with one reference tick per entry.
    nativeDelay.advance(59861351 - 59841498);
    put(nativeDelay,0x204d26d8,0x162021c0,7);
    put(s,0x204d26d8,0x162021c0,7,A::sequence);
    s.advance(31);
    put(s,0x204d26d8,0x162021c0,7,A::sequence);
    s.advance(1);
    put(s,0x204d26d8,0x162021c0,7);
    s.advance(20000);
    put(s,0x204d2178,0x1620203c,0);
    put(s,0x204d217a,0x16202038,0);
    put(s,0x204d2180,0x1620206c,0);
    put(s,0x204d2184,0x162021f0,0);
    put(s,0x204d218a,0x162021e8,0x700);
    assert(get(s,0x204d22c4,0x16202040,2)==0);
    put(s,0x204d22f0,0x16202040,0x31d);
    put(s,0x204d364e,0x16202048,0x40);
    assert(get(s,0x204d2300,0x16202048,2)==0x40);
    put(s,0x204d2310,0x16202048,0x140);
    for(int i=0;i<100;++i) assert(get(s,0x204d2316,0x16202048,2)==0x40);
    s.advance(19999); assert(get(s,0x204d2316,0x16202048,2)==0x40);
    s.advance(1); assert(get(s,0x204d2316,0x16202048,2)==0x140);
    assert(get(s,0x204d3618,0x16010910)==0x430000);
}
static void command(Imx1GraphicsSd& s, uint32_t cmd) {
    put(s,0x204d2178,0x1620203c,0);
    put(s,0x204d217a,0x16202038,0);
    put(s,0x204d2180,0x1620206c,0);
    put(s,0x204d2184,0x162021f0,0);
    put(s,0x204d218a,0x162021e8,0x700);
    put(s,0x204d2518,0x16202008,0);
    put(s,0x204d251c,0x1620200c,0);
    put(s,0x204d251e,0x16202000,cmd);
    assert(get(s,0x204d2300,0x16202048,2)==0x140);
    put(s,0x204d2310,0x16202048,0x140);
}
int main() {
    Imx1GraphicsSd s; initialisation(s);
    uint32_t v=0xabcdef;
    assert(s.access(0,0x16010910,4,false,v)==A::externalInput && v==0xabcdef);
    assert(s.access(0x204d26a2,0x16010910,2,false,v)==A::width && v==0xabcdef);
    put(s,0x204d26b2,0x16010910,0x430004,A::sideEffect);
    assert(s.controlValue()==0x430000);
    put(s,0x204d251e,0x16202000,0x411,A::externalInput); assert(!s.pending());
    command(s,0x300); auto partitioned=s;
    for(int i=0;i<100;++i) assert(get(s,0x204d252e,0x16202038,2)==0);
    assert(get(s,0x204d2476,0x1620203c,2)==0x4000);
    const uint64_t cmdTicks=48*2*65;
    s.advance(cmdTicks-1); partitioned.advance(2000); partitioned.advance(cmdTicks-2001);
    assert(s.pending() && partitioned.pending());
    s.advance(1); partitioned.advance(1);
    assert(!s.pending() && !partitioned.pending());
    assert(get(s,0x204d252e,0x16202038,2)==1);
    assert(get(s,0x204d25a0,0x1620203c,2)==0x80);
    put(s,0x204d254e,0x16202038,0);
    for(uint32_t cmd : {0x437u,0x701u}) {
        command(s,cmd); s.advance(64*2*65-1); assert(s.pending());
        s.advance(1); assert(get(s,0x204d252e,0x16202038,2)==1);
        assert(get(s,0x204d25a0,0x1620203c,2)==0xc0);
        put(s,0x204d2178,0x1620203c,0);
        assert(get(s,0x204d25a0,0x1620203c,2)==0x80);
    }
    command(s,0x437); put(s,0x204d2736,0x16202048,0); s.advance(1000000); assert(s.pending());
    put(s,0x204d273e,0x16010910,0x8000); assert(!s.pending() && !s.clockValue());
    s.reset(); assert(s.controlValue()==0 && s.statusValue()==0x80); initialisation(s);
    Imx1GraphicsGpio gpio;
    v=0xffffffff; assert(gpio.access(0x200b99c4,0x160104bc,4,true,v)==A::ok);
    v=0x12345678; assert(gpio.access(0x200b9a14,0x160104b8,4,true,v)==A::ok);
    assert(gpio.access(0x204d26f4,0x160104b8,4,false,v)==A::ok);
    v=(v & ~0x4000u)|0x40000000; assert(gpio.access(0x204d2708,0x160104b8,4,true,v)==A::ok);
    assert(gpio.bank(0).data==0x52341678);
    v^=1; assert(gpio.access(0x204d2708,0x160104b8,4,true,v)==A::sideEffect);
    Imx1GraphicsInterrupts irq; v=0;
    assert(irq.access(0x204d22a4,0x16010200,4,false,v)==A::externalInput);
    irq.observeDisplay(true,true); v=1;
    assert(irq.access(0x20087702,0x16010200,4,true,v)==A::ok);
    const auto pending = irq.pendingValue();
    const auto output = irq.interruptOutput();
    assert(get(s,0x204d2274,0x16202040,2)==0x31d);
    put(s,0x204d22a0,0x16202040,0x305);
    assert(s.cardEventsKnown());
    assert(irq.access(0x204d22a4,0x16010200,4,false,v,s.cardEventsKnown())==A::ok && v==1);
    v|=0x02000000;
    assert(irq.access(0x204d22aa,0x16010200,4,true,v,s.cardEventsKnown())==A::ok);
    assert(irq.enableValue()==0x02000001 && irq.pendingValue()==pending && irq.interruptOutput()==output);
    v=0;
    assert(irq.access(0x204d22aa,0x16010200,4,true,v,true)==A::sideEffect);
    assert(irq.access(0x204d22b8,0x16010200,4,false,v)==A::ok);
    v&=~0x02000000u;
    assert(irq.access(0x204d22be,0x16010200,4,true,v)==A::ok);
    assert(irq.enableValue()==1 && irq.pendingValue()==pending && irq.interruptOutput()==output);
    v=0; assert(irq.access(0x204d22be,0x16010200,4,true,v)==A::sideEffect);
    put(s,0x204d22a0,0x16202040,0x304,A::sideEffect);
    assert(get(s,0x204d2274,0x16202040,2)==0x305);
    put(s,0x204d22f0,0x16202040,0x31d);
    assert(get(s,0x204d22c4,0x16202040,2)==0x31d);
    s.reset(); assert(!s.cardEventsKnown());
    puts("PASS: complete virtual empty SD init, guest reset/clock deadlines, CMD0, response timeouts, cancellation and preserved GPIO/IRQ state");
}
