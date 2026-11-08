#include <cassert>
#include <cstdio>
#include "imx1_graphics_acquisition.h"
#include "imx1_gpio.h"
using A = Imx1BootstrapAccess;
static void put(Imx1GraphicsAcquisition& s, uint32_t offset, uint32_t v, A expected=A::ok) {
    assert(s.access(0,0x16010000+offset,4,true,v)==expected);
}
static uint32_t get(Imx1GraphicsAcquisition& s, uint32_t offset) {
    uint32_t v=0xdeadbeef; assert(s.access(0,0x16010000+offset,4,false,v)==A::ok); return v;
}
static void configure(Imx1GraphicsAcquisition& s) {
    put(s,0xd08,0x2000); put(s,0xd08,0x2800); s.advance(10000);
    put(s,0xd08,0x2000); s.advance(10000);
    put(s,0xd10,0x2700a); put(s,0xd08,0x2400); put(s,0xd1c,8);
}
int main() {
    Imx1Gpio pins;
    auto pinPut = [&](uint32_t offset, uint32_t value) { assert(pins.access(0x0021c100+offset,4,true,value)==Imx1GpioAccess::ok); };
    pinPut(0x20,0x0f400000); pinPut(0x00,0x0a400000); pinPut(0x08,0x00cc0000); pinPut(0x1c,0x08000000);
    assert(pins.outputPattern(1,0x05000000,0x08000000,0x02000000));
    assert(!pins.outputPattern(1,0x0a000000,0x04000000,0x01000000));
    pinPut(0x00,0x05400000); pinPut(0x08,0x00ff0000); pinPut(0x1c,0x04000000);
    assert(pins.outputPattern(1,0x0a000000,0x04000000,0x01000000));
    pinPut(0x08,0); assert(!pins.outputPattern(1,0x0a000000,0x04000000,0x01000000));
    Imx1GraphicsAcquisition s; configure(s);
    put(s,0xd0c,0x400,A::externalInput); assert(get(s,0xd0c)==0);
    assert(s.setInput(128,900)); assert(!s.setInput(1024,1));
    put(s,0xd0c,0x400); assert(s.deadlineRemaining()==20000);
    auto partitioned=s;
    assert(s.setInput(23,24));
    for(int i=0;i<100;++i) assert(get(s,0xd10)==0x2700a);
    assert(s.deadlineRemaining()==20000);
    s.advance(19999); partitioned.advance(10000); partitioned.advance(9999);
    assert(s.queued()==0 && partitioned.queued()==0);
    put(s,0xd10,0x1700a,A::sequence); put(s,0xd08,0x2402,A::sequence);
    put(s,0xd1c,9,A::sideEffect); assert(get(s,0xd1c)==8);
    s.advance(1); partitioned.advance(1); assert(s.queued()==2);
    assert(get(s,0xd10)==0x22700a); put(s,0xd0c,0);
    assert(get(s,0xd14)==0x1000080); assert(get(partitioned,0xd14)==0x1000080);
    assert(get(s,0xd14)==0x2000384); assert(get(s,0xd10)==0x2700a);
    uint32_t v=0xabcdef; assert(s.access(0,0x16010d14,4,false,v)==A::externalInput && v==0xabcdef);
    put(s,0xd0c,0x400); s.advance(19999); put(s,0xd0c,0); s.advance(2); assert(!s.queued());
    put(s,0xd0c,0x400); s.advance(20000); put(s,0xd08,0x2c00); assert(!s.queued());
    put(s,0xd08,0x2400); put(s,0xd0c,0x400); s.advance(20000);
    assert(get(s,0xd14)==0x1000017); assert(get(s,0xd14)==0x2000018);
    s.reset(); assert(get(s,0xd08)==0 && get(s,0xd1c)==0);
    configure(s); put(s,0xd08,0x2402); put(s,0xd10,0x1700a); put(s,0xd0c,0x400,A::externalInput);
    assert(!s.deadlineRemaining() && !s.queued());
    for (uint32_t source : {0u,1u}) {
        s.selectSource(source); put(s,0xd0c,0x400); s.advance(20000);
        assert(s.queued()==1); put(s,0xd0c,0);
        put(s,0xd10,get(s,0xd10)); assert(s.queued()==1);
        assert(get(s,0xd14)==(0x0a000000 | (source ? 24 : 23)));
    }
    put(s,0xd08,0x2401,A::sideEffect); put(s,0xd10,0x3700a,A::sideEffect);
    assert(s.access(0,0x16010d08,2,false,v)==A::width);
    puts("PASS: virtual acquisition input capture, partitioned guest deadline, FIFO, reset, cancel and guards");
}
