#include <cassert>
#include <cstdio>
#include "imx1_cs5_bus.h"
#include "imx1_graphics_memory.h"
#include "zodiac-graphics-setup.h"
using S = Imx1Cs5Bus::State;
using A = Imx1BootstrapAccess;
static void put(Imx1Bootstrap& s, uint32_t address, uint32_t value, uint32_t mode=0) {
    const auto status=s.access(address,4,true,value,mode);
    if (status!=A::ok) std::fprintf(stderr,"write %08x=%08x mode%u failed %s\n",address,value,mode,imx1BootstrapAccessName(status));
    assert(status == A::ok);
}
static uint32_t get(Imx1Bootstrap& s, uint32_t address) {
    uint32_t value=0;assert(s.access(address,4,false,value) == A::ok);return value;
}
int main() {
    for (auto mode : {Imx1Cs5Mode::dtack,Imx1Cs5Mode::wait}) {
        const bool initial = mode == Imx1Cs5Mode::dtack;
        for (uint32_t ack : {0u,1u,2u,3u,44u,1021u,1022u}) {
            Imx1Cs5Bus b(mode);
            assert(b.sample(0,true,initial) == S::waiting);
            if (ack) assert(b.sample(ack-1,true,initial) == S::waiting);
            assert(b.sample(ack,true,!initial) == (ack < 3 ? S::waiting : S::complete));
            assert(b.sample(ack < 3 ? 3 : ack,true,!initial) == S::complete);
        }
        Imx1Cs5Bus timeout(mode);
        assert(timeout.sample(1021,true,initial) == S::waiting);
        assert(timeout.sample(1022,true,initial) == S::timeout);
        assert(timeout.sample(1023,true,!initial) == S::timeout);
        Imx1Cs5Bus late(mode);assert(late.sample(1023,true,!initial)==S::timeout);
        Imx1Cs5Bus unknown(mode);
        assert(unknown.sample(0,false,initial) == S::unknown);
        assert(unknown.sample(3,true,!initial) == S::unknown);
    }
    Imx1Cs5Bus stale(Imx1Cs5Mode::dtack,false);
    assert(stale.sample(3,true,false) == S::waiting);
    assert(stale.sample(10,true,true) == S::waiting);
    assert(stale.sample(11,true,false) == S::complete);
    Imx1Cs5Bus fixed(Imx1Cs5Mode::fixed);
    assert(fixed.sample(62,true,false) == S::waiting);
    assert(fixed.sample(63,true,false) == S::complete);
    Imx1Eim eim;
    auto store=[&](uint32_t a,uint32_t v) {assert(eim.access(a,4,true,v)==Imx1EimAccess::ok);};
    store(0x0022002c,0x11111e01);
    for (auto word : {0x3e00u,0x3f00u,0x80003f00u}) {
        store(0x00220028,word);assert(eim.cs5DeviceConfigured());
        for (uint32_t n : {1u,2u,4u}) assert(eim.cs5Transfer(0x16800000,n,true)==Imx1EimAccess::ok);
    }
    for (auto dsz : {0u,1u,2u,3u,4u,5u}) {
        store(0x0022002c,0x11111801|(dsz<<8));
        assert(eim.cs5Transfer(0x16800000,1,true)==Imx1EimAccess::ok);
        assert(eim.cs5Transfer(0x16800000,4,true)==Imx1EimAccess::width);
        assert(eim.cs5Transfer(0x16800000,2,true)==(dsz<4?Imx1EimAccess::width:Imx1EimAccess::ok));
        assert(!eim.cs5DeviceConfigured());
    }
    store(0x0022002c,0x11111e11);assert(eim.cs5Transfer(0x16800000,4,true)==Imx1EimAccess::sideEffect);
    store(0x0022002c,0x11111e41);assert(eim.cs5Transfer(0x16800000,4,false,true)==Imx1EimAccess::sideEffect);
    store(0x0022002c,0x11111e00);assert(eim.cs5Transfer(0x16800000,4,false)==Imx1EimAccess::sideEffect);
    store(0x0022002c,0x11111e01);assert(eim.cs5Transfer(0x16800001,4,false)==Imx1EimAccess::width);
    Imx1Bootstrap soc;assert(soc.setGpioInput(0,0x80));
    assert(!soc.cs5PinsSelected(true));
    put(soc,0x0021c020,0);assert(soc.cs5PinsSelected(true));
    put(soc,0x0021c038,1u<<17);assert(!soc.cs5PinsSelected(true));
    put(soc,0x0021c038,0);put(soc,0x0021c000,1u<<17);assert(!soc.cs5PinsSelected(true));
    put(soc,0x0021c000,0);assert(soc.cs5PinsSelected(true));
    put(soc,0x0021c020,1u<<23);assert(!soc.cs5PinsSelected(false));
    // HCLK elapsed time preserves timer/reference fractions across CPU modes.
    Imx1Bootstrap bulk,split;
    for (auto* s : {&bulk,&split}) {
        s->setSystemPllScenario(true);
        put(*s,0x00202004,2);put(*s,0x00202008,1);put(*s,0x00202000,0x13);
        s->advanceCpuCycles(7,0,false);
    }
    bulk.advanceHclkCycles(1022);
    for (int i=0;i<1022;++i) split.advanceHclkCycles(1);
    for (auto a : {0x00202010u,0x00202014u,0x0021b000u}) assert(get(bulk,a)==get(split,a));
    assert(bulk.interruptLines().irq==split.interruptLines().irq);
    Imx1Bootstrap async;
    assert(async.setRtcInitialTime({1,12,34,56}));async.setSystemPllScenario(true);
    put(async,0x0021b000,0x57000003);put(async,0x0021b004,0x03e717bf);
    put(async,0x0021b000,0x57200003);async.advanceCpuCycles(6000,0,false);
    put(async,0x0021b00c,0x123638ad,3);put(async,0x0021b000,0x57400003,3);
    assert(async.finishSystemPllBusStall()==2*762752);
    put(async,0x0021b020,2);put(async,0x00202004,3,3);put(async,0x00202000,0x103,3);
    async.advanceCpuCycles(37,3,true);auto chunks=async;
    for (uint32_t divider : {0x5700ac03u,0x57008003u,0x57002c03u}) {
        put(async,0x0021b000,divider,3);put(chunks,0x0021b000,divider,3);
        async.advanceHclkCycles(1022);
        for (int i=0;i<1022;++i) chunks.advanceHclkCycles(1);
        async.advanceCpuCycles(137,3,true);
        for (int i=0;i<137;++i) chunks.advanceCpuCycles(1,3,true);
        for (uint32_t a : {0x00202010u,0x00204000u,0x00204004u,0x00204020u}) assert(get(async,a)==get(chunks,a));
    }
    Imx1Cs5Configuration c(Imx1GraphicsProfile::virtual5744Compatible8MiB);
    Imx1Eim configured;
    uint32_t setting=0x3e00;assert(configured.access(0x00220028,4,true,setting)==Imx1EimAccess::ok);
    setting=0x11111e01;assert(configured.access(0x0022002c,4,true,setting)==Imx1EimAccess::ok);
    configureGraphicsInterface(c,configured);configureGraphicsController(c,configured);
    Imx1GraphicsMemory memory;
    uint32_t v=0xaabbccdd;
    assert(memory.validate(c,configured,0x16800000,4)==A::ok);
    assert(!memory.dirty(1).pending);
    assert(memory.access(c,configured,0x16800000,4,true,v)==A::ok);
    const auto generation=memory.dirty(1).generation;
    for (int i=0;i<100;++i) assert(memory.validate(c,configured,0x16800000,4)==A::ok);
    assert(memory.dirty(1).generation==generation);
    assert(memory.validate(c,configured,0x16800001,4)==A::width);
    assert(memory.validate(c,configured,0x16080000,4)==A::unmapped);
    uint32_t datum=0;
    assert(c.access(configured,0x200acb98,0x160113e8,4,false,datum)==A::ok);
    for (auto op : {std::array<uint32_t,3>{0x200acba8,0x13e8,0x400},
            {0x200acbb4,0x144,0x2440},{0x200acbc4,0x10a8,0x1fff1fff},{0x200acbd0,0x4dc,0xc0000},
            {0x200b8e80,0x214,0x10150000},{0x200b8e88,0x220,0},{0x200b8f04,8,0x04150000},
            {0x200b8f10,0x230,0x80000000},{0x200b8f20,0x210,0x0800000d}}) {
        datum=op[2];assert(c.access(configured,op[0],0x16010000+op[1],4,true,datum)==A::ok);
    }
    setting=0x80003f00;assert(configured.access(0x00220028,4,true,setting)==Imx1EimAccess::ok);
    setting=0x11501e01;assert(configured.access(0x0022002c,4,true,setting)==Imx1EimAccess::ok);
    assert(c.memoryReady(configured));
    for (uint32_t pc : {0x20084650u,0x20084658u,0x20084660u,0x20084664u}) {
        datum=0;assert(c.access(configured,pc,0x16010000,4,false,datum)==A::ok && datum==0x57441002);
    }
    for (auto op : {std::array<uint32_t,3>{0x20086a08,0x24,0x20000},{0x20086a1a,0x28,0},
            {0x20086a2c,0x18,0xc000200},{0x20086a3c,0x184,0xf7},
            {0x20086a52,0x18,0xc000600},{0x20086a5e,0x20,0x100000},
            {0x20086970,0x200,0},{0x20086982,0x18,0xc000600}}) {
        datum=op[2]^0x80000000u;
        assert(c.access(configured,op[0],0x16010000+op[1],4,true,datum)==A::sideEffect);
        datum=op[2];assert(c.access(configured,op[0],0x16010000+op[1],4,true,datum)==A::ok);
    }
    assert(c.memoryReady(configured) && memory.dirty(1).generation==generation);
    datum=0;assert(memory.access(c,configured,0x16800000,4,false,datum)==A::ok && datum==0xaabbccdd);
    std::puts("PASS: CS5 edge/WAIT protocols, deadline, narrow ports, routing, elapsed HCLK partitions and pure VRAM admission");
}
