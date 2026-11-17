#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
#include "imx1_graphics_sd.h"
#include "machine_state_archive.h"
#include "imx1_graphics_gpio.h"
#include "imx1_graphics_interrupts.h"
using A = Imx1BootstrapAccess;
// Each access and advance is followed by the card work, as the probe runs it on the committed model.
static void put(Imx1GraphicsSd& s, uint32_t pc, uint32_t address, uint32_t v, A expected=A::ok) {
    assert(s.access(pc,address,4,true,v)==expected);
    if (s.cardWorkPending()) s.runCardWork();
}
static uint32_t get(Imx1GraphicsSd& s, uint32_t pc, uint32_t address, uint32_t size=4) {
    uint32_t v=0xdeadbeef; assert(s.access(pc,address,size,false,v)==A::ok);
    if (s.cardWorkPending()) s.runCardWork();
    return v;
}
static void tick(Imx1GraphicsSd& s, uint64_t ticks) {
    s.advance(ticks);
    if (s.cardWorkPending()) s.runCardWork();
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
static void emptySlot() {
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

// A 1 MiB card image, 2048 sectors with a pattern of their own.
static std::vector<uint8_t> image(2048 * 512);
static bool sectorRead(uint32_t sector, void* buffer) {
    if (sector >= 2048) return false;
    std::memcpy(buffer, image.data() + size_t(sector) * 512, 512); return true;
}
static bool sectorWrite(uint32_t sector, const void* buffer) {
    if (sector >= 2048) return false;
    std::memcpy(image.data() + size_t(sector) * 512, buffer, 512); return true;
}
static uint8_t pattern(size_t at) { return uint8_t(at * 7 + (at >> 9) * 13 + 1); }

// One command as the driver's 204d2456 issues it, run to status1 bit 0; returns status2 then.
static uint32_t cardCommand(Imx1GraphicsSd& s, uint32_t value, uint32_t argument, uint32_t count = 0) {
    for (int i = 0; get(s,0x204d2476,0x1620203c,2) & 0x4000; ++i) { assert(i < 1000); tick(s, 100); }
    put(s,0x204d2178,0x1620203c,0);
    put(s,0x204d217a,0x16202038,0);
    put(s,0x204d2180,0x1620206c,0);
    put(s,0x204d2184,0x162021f0,0);
    put(s,0x204d218a,0x162021e8,0x700);
    if (count) {
        put(s,0x204d2502,0x1620204c,0x200);
        if (count > 1) { put(s,0x204d250a,0x16202010,0x100); put(s,0x204d250c,0x16202014,count); }
        else put(s,0x204d2512,0x16202010,0);
    }
    put(s,0x204d2518,0x16202008,argument & 0xffff);
    put(s,0x204d251c,0x1620200c,argument >> 16);
    put(s,0x204d251e,0x16202000,value);
    put(s,0x204d2310,0x16202048,get(s,0x204d2300,0x16202048,2) | 0x100);
    int polls = 0;
    while (!(get(s,0x204d252e,0x16202038,2) & 1)) { assert(++polls < 100000); tick(s, 50); }
    const uint32_t status1 = get(s,0x204d252e,0x16202038,2);
    put(s,0x204d254e,0x16202038,status1 & ~1u);
    return get(s,0x204d25a0,0x1620203c,2);
}
static uint32_t response(Imx1GraphicsSd& s, unsigned i) { return get(s,0x204d2626,0x16202018 + 4 * i,2); }
// Waits for status2 bit 8 (read) or 9 (write) as 204d23b8 and 204d2372 do; false on an error bit.
static bool waitBlock(Imx1GraphicsSd& s, uint32_t bit) {
    for (int i = 0; i < 100000; ++i) {
        const uint32_t status2 = get(s,0x204d23c2,0x1620203c,2);
        if (status2 & 0x807f) { put(s,0x204d23f4,0x1620203c,0); return false; }
        if (status2 & bit) { put(s,0x204d23f4,0x1620203c,0); return true; }
        tick(s, 50);
    }
    assert(false); return false;
}
static void readBlock(Imx1GraphicsSd& s, uint8_t* out, unsigned words = 128) {
    for (unsigned i = 0; i < words; ++i) {
        const uint32_t v = get(s,0x204d3736,0x16202060);
        for (unsigned b = 0; b < 4; ++b) out[i * 4 + b] = uint8_t(v >> (8 * b));
    }
}
static void writeBlock(Imx1GraphicsSd& s, const uint8_t* in) {
    for (unsigned i = 0; i < 128; ++i)
        put(s,0x204d2bbe,0x16202060,uint32_t(in[i*4]) | uint32_t(in[i*4+1]) << 8 | uint32_t(in[i*4+2]) << 16 | uint32_t(in[i*4+3]) << 24);
}
// The end of a multi-block transfer, status1 bit 2, as 204d23fe waits for and clears it.
static void waitMultiEnd(Imx1GraphicsSd& s) {
    int polls = 0;
    while (!(get(s,0x204d2408,0x16202038,2) & 4)) { assert(++polls < 100000); tick(s, 50); }
    put(s,0x204d2428,0x16202038,get(s,0x204d2408,0x16202038,2) & ~4u);
}
static void eventsOn(Imx1GraphicsSd& s) {
    put(s,0x204d22a0,0x16202040,(get(s,0x204d2274,0x16202040,2) & ~0x18u) | 0x305);
}
// The card init of 204d2748 for an SD card; returns the RCA.
static uint32_t initCard(Imx1GraphicsSd& s) {
    put(s,0x204d275a,0x16202004,0x200);
    put(s,0x204d2766,0x16202050,get(s,0x204d2764,0x16202050,2) | 0x8000);
    assert(cardCommand(s,0x300,0) == 0x80);
    cardCommand(s,0x437,0);
    assert(cardCommand(s,0x437,0) == 0x80);
    bool ready = false;
    for (int i = 0; i < 21 && !ready; ++i) {
        if (i) assert(cardCommand(s,0x437,0) == 0x80);
        assert(cardCommand(s,0x769,0x00700000) == 0x80);
        ready = response(s,1) & 0x8000;
    }
    assert(ready && ((response(s,1) >> 4) & 7));
    assert(cardCommand(s,0x602,0) == 0x80);
    assert((response(s,7) >> 8) == 0x3f);
    const uint32_t mid = response(s,7) & 0xff;
    (void)mid;
    assert(cardCommand(s,0x403,0) == 0x80);
    const uint32_t rca = response(s,1);
    assert(rca);
    assert(cardCommand(s,0x609,rca << 16) == 0x80);
    assert((response(s,7) >> 8) == 0x3f && !(response(s,0) & 0x30));
    assert(cardCommand(s,0x507,rca << 16) == 0x80);
    assert(cardCommand(s,0x410,0x200) == 0x80);
    put(s,0x204d2896,0x16202050,get(s,0x204d2894,0x16202050,2) & ~0x8000u);
    assert(cardCommand(s,0x437,rca << 16) == 0x80);
    assert(cardCommand(s,0x46a,0) == 0x80);
    assert(cardCommand(s,0x437,rca << 16) == 0x80);
    assert(cardCommand(s,0x446,2) == 0x80);
    put(s,0x204d2916,0x16202048,1);
    put(s,0x204d2310,0x16202048,0x101);
    for (int i = 0; !(get(s,0x204d2316,0x16202048,2) & 0x100); ++i) { assert(i < 1000); tick(s, 100); }
    return rca;
}
// Machine state round trip: state() and stateCard() with the card's bytes, into a fresh slot.
static std::vector<uint8_t> save(Imx1GraphicsSd& s) {
    std::vector<uint8_t> bytes, cardBytes;
    s.saveCard(cardBytes);
    StateWriter w(bytes);
    s.state(w); s.stateCard(w); w.DoVector(cardBytes, 1u << 20);
    return bytes;
}
static Imx1GraphicsSd load(const std::vector<uint8_t>& bytes, VSD* card) {
    Imx1GraphicsSd loaded;
    std::vector<uint8_t> loadedCard;
    StateReader r(bytes.data(), bytes.size());
    loaded.state(r); loaded.stateCard(r); r.DoVector(loadedCard, 1u << 20);
    assert(r.complete());
    assert(loaded.reattachCard(card, &loadedCard));
    return loaded;
}

static void cardSlot() {
    for (size_t i = 0; i < image.size(); ++i) image[i] = pattern(i);
    VSD* card = vsdInit(sectorRead, sectorWrite, 2048);
    assert(card);
    Imx1GraphicsSd s; initialisation(s);
    Imx1GraphicsInterrupts irq;

    // Card detect: insert and remove with card events on raise the interrupt; masked events latch nothing.
    assert(!s.attachCard(card));            // events still masked (0x31d)
    assert(get(s,0x204d3618,0x16010910) == 0x430004 && s.cardUsed());
    assert(!s.attachCard(nullptr));
    assert(get(s,0x204d3618,0x16010910) == 0x430000);
    eventsOn(s);
    uint32_t v = 0;
    assert(irq.access(0x204d22a4,0x16010200,4,false,v,s.cardEventsKnown()) == A::ok);
    v |= 0x02000000; assert(irq.access(0x204d22aa,0x16010200,4,true,v,s.cardEventsKnown()) == A::ok);
    assert(s.attachCard(card)); irq.raiseSd();
    assert(irq.interruptOutput() == Imx1GpioLine::asserted);
    // The handler: status1, 16200204, clear, acknowledge.
    assert(get(s,0x204d21a2,0x16202038,2) == 0x10);
    assert(get(s,0x204d21aa,0x16200204,2) == 0);
    put(s,0x204d2178,0x1620203c,0);
    put(s,0x204d217a,0x16202038,0);
    v = 0x02000000; assert(irq.access(0x204d21b6,0x16010204,4,true,v) == A::ok);
    assert(irq.interruptOutput() == Imx1GpioLine::clear && !irq.pendingValue());
    v = 0x01000000; assert(irq.access(0x204d21b6,0x16010204,4,true,v) == A::sideEffect);
    assert(get(s,0x204d3618,0x16010910) == 0x430004);
    assert(get(s,0x204d3728,0x16010910) == 0x430004);
    s.setWriteProtect(true); assert(get(s,0x204d3728,0x16010910) == 0x430005); s.setWriteProtect(false);
    // Power-up writes back the inputs it read.
    put(s,0x204d26b2,0x16010910,0x430004);
    assert(s.controlValue() == 0x430000);
    assert(s.attachCard(nullptr));
    assert(get(s,0x204d21a2,0x16202038,2) == 0x08 && get(s,0x204d3618,0x16010910) == 0x430000);
    put(s,0x204d217a,0x16202038,0);
    // With the slot empty a command times out as in the empty slot.
    assert(cardCommand(s,0x437,0) == 0xc0);
    assert(s.attachCard(card));
    put(s,0x204d217a,0x16202038,0);
    // Outside the driver the slot stays guarded.
    v = 0; assert(s.access(0x20001000,0x16202038,2,false,v) == A::externalInput);

    const uint32_t rca = initCard(s);
    (void)rca;
    uint8_t block[512 * 4];
    // Single block read of block 3.
    assert(cardCommand(s,0x5c11,3 << 9,1) == 0x80);
    assert(waitBlock(s,0x100));
    readBlock(s,block);
    assert(!std::memcmp(block, image.data() + 3 * 512, 512));
    v = 0; assert(s.access(0x204d3736,0x16202060,4,false,v) == A::sequence);
    // Multi-block read of blocks 5-7.
    assert(cardCommand(s,0x3c12,5 << 9,3) == 0x80);
    put(s,0x204d244c,0x16202038,get(s,0x204d2436,0x16202038,2) & ~4u);
    for (unsigned b = 0; b < 3; ++b) { assert(waitBlock(s,0x100)); readBlock(s,block + 512 * (b % 4)); }
    waitMultiEnd(s);
    assert(!std::memcmp(block, image.data() + 5 * 512, 3 * 512));
    // Single block write to block 9.
    uint8_t out[512 * 2];
    for (unsigned i = 0; i < sizeof(out); ++i) out[i] = uint8_t(0xa5 ^ i ^ (i >> 8));
    assert(cardCommand(s,0x4c18,9 << 9,1) == 0x280);
    assert(waitBlock(s,0x200));
    writeBlock(s,out);
    assert(!std::memcmp(image.data() + 9 * 512, out, 512));
    // Multi-block write to blocks 20-21.
    assert(cardCommand(s,0x2c19,20 << 9,2) == 0x280);
    for (unsigned b = 0; b < 2; ++b) { assert(waitBlock(s,0x200)); writeBlock(s,out + 512 * b); }
    waitMultiEnd(s);
    assert(!std::memcmp(image.data() + 20 * 512, out, 1024));
    // Read back what was written.
    assert(cardCommand(s,0x3c12,20 << 9,2) == 0x80);
    for (unsigned b = 0; b < 2; ++b) { assert(waitBlock(s,0x100)); readBlock(s,block + 512 * b); }
    waitMultiEnd(s);
    assert(!std::memcmp(block, out, 1024));
    // A block past the card's end fails with an error, and a multi-block one still ends.
    assert(cardCommand(s,0x3c12,2047u << 9,2) == 0x80);
    assert(waitBlock(s,0x100)); readBlock(s,block);
    assert(!waitBlock(s,0x100));
    waitMultiEnd(s);

    // Save part way through a multi-block read, load into a fresh slot, and both end the same.
    assert(cardCommand(s,0x3c12,30 << 9,4) == 0x80);
    assert(waitBlock(s,0x100)); readBlock(s,block);
    assert(waitBlock(s,0x100)); readBlock(s,block + 512, 60);
    const auto saved = save(s);
    readBlock(s,block + 512 * 1 + 240, 68);
    for (unsigned b = 2; b < 4; ++b) { assert(waitBlock(s,0x100)); readBlock(s,block + 512 * b); }
    waitMultiEnd(s);
    assert(!std::memcmp(block, image.data() + 30 * 512, 4 * 512));
    // The loaded slot puts the card back where the save left it.
    auto loaded = load(saved, card);
    uint8_t again[512 * 4];
    std::memcpy(again, block, 512 + 240);
    readBlock(loaded,again + 512 + 240, 68);
    for (unsigned b = 2; b < 4; ++b) { assert(waitBlock(loaded,0x100)); readBlock(loaded,again + 512 * b); }
    waitMultiEnd(loaded);
    assert(!std::memcmp(again, block, sizeof(block)));
    // And during a multi-block write.
    assert(cardCommand(s,0x2c19,40 << 9,2) == 0x280);
    assert(waitBlock(s,0x200)); writeBlock(s,out);
    auto writer = load(save(s), card);
    assert(waitBlock(writer,0x200)); writeBlock(writer,out + 512);
    waitMultiEnd(writer);
    assert(!std::memcmp(image.data() + 40 * 512, out, 1024));
    // Pulled during a read: the transfer ends with an error, and its end.
    assert(cardCommand(writer,0x3c12,50 << 9,3) == 0x80);
    assert(waitBlock(writer,0x100));
    assert(writer.attachCard(nullptr));
    assert(get(writer,0x204d23c2,0x1620203c,2) & 0x8000);
    waitMultiEnd(writer);
    // Saved mid-PIO-read, loaded with the card refused (none, or bytes it won't take): the driver sees an error.
    {
        Imx1GraphicsSd r; initialisation(r); eventsOn(r);
        assert(r.attachCard(card));
        initCard(r);
        assert(cardCommand(r,0x5c11,3 << 9,1) == 0x80);
        assert(waitBlock(r,0x100));
        std::vector<uint8_t> bytes, cardBytes;
        r.saveCard(cardBytes);
        StateWriter w(bytes);
        r.state(w); r.stateCard(w); w.DoVector(cardBytes, 1u << 20);
        for (int mode = 0; mode < 2; ++mode) {
            Imx1GraphicsSd l;
            std::vector<uint8_t> loadedCard;
            StateReader rd(bytes.data(), bytes.size());
            l.state(rd); l.stateCard(rd); rd.DoVector(loadedCard, 1u << 20);
            bool event = false;
            if (mode == 0) assert(!l.reattachCard(nullptr, &loadedCard, &event) && event && !l.cardPresent());
            else { loadedCard.resize(loadedCard.size() / 2); assert(!l.reattachCard(card, &loadedCard, &event)); }
            assert(get(l,0x204d21a2,0x16202038,2) & 4);
            assert(get(l,0x204d23c2,0x1620203c,2) & 0x8000);
        }
    }
    puts("PASS: slot 2 card detect and its interrupt, SD init and identify, PIO single and multi-block read and write, errors, save and load mid-transfer");
}

int main() {
    emptySlot();
    cardSlot();
}
