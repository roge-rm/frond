#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>
#include "imx1_bootstrap.h"
#include "imx1_mmcsd.h"

int main() {
    using A = Imx1BootstrapAccess;
    constexpr uint32_t psr0 = 0x00210000, psr1 = psr0 + 4, mmc = 0x00214000;
    Imx1Bootstrap soc;
    auto read = [&](uint32_t address, uint32_t expected) {
        uint32_t value = 0xdeadbeef;
        assert(soc.access(address,4,false,value) == A::ok && value == expected);
    };
    auto write = [&](uint32_t address, uint32_t value, A expected = A::ok) {
        assert(soc.access(address,4,true,value) == expected);
    };
    read(psr0,0xffffc410); read(psr1,0xfffffbef);
    write(mmc,8,A::sideEffect);
    // Native cold/resume helper selects the 32-bit MMCSD peripheral bus.
    write(psr0,0xffffc400); write(psr1,0xfffffbff); write(psr1,0xfffffbff);
    read(psr0,0xffffc400); read(psr1,0xfffffbff);
    for (unsigned bit = 1; bit < 16; ++bit) {
        if (bit == 4) continue;
        write(psr0,0xffffc400 ^ (1u << bit),A::sideEffect);
        write(psr1,0xfffffbff ^ (1u << bit),A::sideEffect);
    }
    // Hardware ignores the reserved read-only fields, including AIPI's own size.
    write(psr0,0xc401); write(psr1,0xfbfe);
    read(psr0,0xffffc400); read(psr1,0xfffffbff);
    read(mmc,0); read(mmc+8,0x36); read(mmc+0x10,0x40); read(mmc+0x14,0xffff);
    uint32_t value = 0xdeadbeef;
    assert(soc.access(mmc+4,4,false,value) == A::externalInput && value == 0xdeadbeef);
    write(0x00223008,35,A::externalInput);
    soc.setMmcsdEmptySlot(true);
    read(mmc+4,0);
    write(mmc,0xd,A::sequence); read(mmc,0);
    write(mmc,5,A::sequence); read(mmc,0);
    // RM20.6.1 reset/enable handshake, then all remaining native startup registers.
    auto startup = [&] {
        write(mmc,8); read(mmc,0);
        write(mmc,5,A::sequence);
        write(mmc,0xd); read(mmc,4);
        for (unsigned i = 0; i < 8; ++i) {
            write(mmc+0x10,0xff,A::sequence);
            write(mmc,5); read(mmc+4,0);
        }
        write(mmc,5); read(mmc+4,0);
        write(mmc+8,0x36); write(mmc+0x10,0xff); write(mmc+0x14,0x2db4);
        write(mmc+0x18,0x200); write(mmc+0x24,0x7f);
        read(mmc+8,0x36); read(mmc+0x10,0xff); read(mmc+0x14,0x2db4);
        read(mmc+0x18,0x200); read(mmc+0x24,0x7f);
    };
    startup();
    // Constructor and resume enable MMCSD interrupt source 35, with no producer active.
    write(0x00223008,35); read(0x00223040,0xffffffff);
    const auto pending = soc.interruptPending();
    assert(!pending.normal && !pending.fast && !pending.unknownNormal && !pending.unknownFast);
    startup();
    write(psr0,0xffffc410,A::sideEffect); read(psr0,0xffffc400);
    for (uint32_t setting : {0u,0x3fu}) { write(mmc+8,setting); read(mmc+8,setting); }
    write(mmc+8,0x40,A::reservedBits); read(mmc+8,0x3f);
    write(mmc,2,A::sideEffect); write(mmc,6,A::sequence);
    write(mmc,0x26,A::sideEffect);
    write(mmc,7,A::reservedBits); write(mmc,0x10,A::reservedBits); read(mmc,4);
    for (uint32_t setting : {0x400u,0x800u,0x100u,0x300u,7u})
        write(mmc+0xc,setting,A::sideEffect);
    read(mmc+0xc,0);
    write(mmc+4,0,A::sideEffect); read(mmc+4,0);
    for (uint32_t offset : {0x20u,0x34u,0x38u}) {
        value = 0xdeadbeef;
        assert(soc.access(mmc+offset,4,false,value) == A::externalInput && value == 0xdeadbeef);
        write(mmc+offset,0,A::externalInput);
    }
    for (uint32_t address : {psr0,psr1,mmc,mmc+4,mmc+8,mmc+0x24,mmc+0x28,mmc+0x2c,mmc+0x30}) {
        assert(Imx1Bootstrap::registerName(address));
        for (uint32_t size : {0u,1u,2u,8u}) {
            value = 0xdeadbeef;
            assert(soc.access(address,size,false,value) == A::width && value == 0xdeadbeef);
            assert(soc.access(address,size,true,value) == A::width);
        }
        for (unsigned lane = 1; lane < 4; ++lane) {
            assert(soc.access(address+lane,4,false,value) == A::width);
            assert(soc.access(address+lane,4,true,value) == A::width);
        }
        assert(!soc.instructionWord(address,value));
        assert(!soc.instructionRead(address,2,value));
    }
    for (uint32_t address : {0x00200000u,psr0+8,psr0+0xc,psr0+0x10,mmc+0x3c}) {
        value = 0xdeadbeef;
        assert(soc.access(address,4,false,value) == A::unmapped && value == 0xdeadbeef);
    }
    auto copy = soc;
    write(mmc+0x10,0x10);
    assert(copy.access(mmc+0x10,4,false,value) == A::ok && value == 0xff);
    Imx1Bootstrap fresh;
    assert(fresh.access(psr0,4,false,value) == A::ok && value == 0xffffc410);
    soc.advanceHclkCycles(1000); read(mmc+4,0);
    startup();
    constexpr uint64_t irq35 = uint64_t(1) << 35;
    write(mmc+0x24,0x78);
    auto stage = [&](uint32_t command, uint32_t format) {
        write(mmc+0x28,command); write(mmc+0x2c,0xff); write(mmc+0x30,0xc000);
        write(mmc+0xc,format);
    };
    // An idle PERCLK2 divider write discards the old divider's partial period.
    auto retimed = soc;
    auto retimeWrite = [&](uint32_t address, uint32_t setting) {
        assert(retimed.access(address,4,true,setting) == A::ok);
    };
    retimeWrite(mmc+8,0);
    retimeWrite(0x0021b020,0x000b00fb);
    retimed.advanceHclkCycles(1);
    retimeWrite(0x0021b020,0x000b000b);
    retimeWrite(mmc+0x28,0); retimeWrite(mmc+0x2c,0); retimeWrite(mmc+0x30,0);
    retimeWrite(mmc+0xc,0x80); retimeWrite(mmc,6);
    retimed.advanceHclkCycles(10);
    assert(retimed.access(mmc+4,4,false,value) == A::ok && value == 0x100);
    assert(!(retimed.interruptPending().normal & irq35));
    retimed.advanceHclkCycles(1);
    assert(retimed.access(mmc+4,4,false,value) == A::ok && value == 0x2100);
    assert(retimed.interruptPending().normal & irq35);
    stage(0,0x80);
    write(mmc,6); read(mmc+4,0x100);
    assert(soc.idleWakePossible());
    write(mmc+8,0,A::sideEffect); read(mmc+8,0x36);
    write(mmc+0x28,55,A::sideEffect); read(mmc+0x28,0);
    write(mmc,5,A::sideEffect);
    write(0x0021b020,0x000b00ab,A::sideEffect);
    soc.advanceHclkCycles(320*128-1); read(mmc+4,0x100);
    assert(!(soc.interruptPending().normal & irq35));
    auto inFlight = soc;
    soc.advanceHclkCycles(1); read(mmc+4,0x2100);
    assert(soc.interruptPending().normal & irq35);
    assert(!(inFlight.interruptPending().normal & irq35));
    inFlight.advanceHclkCycles(1);
    assert(inFlight.interruptPending().normal & irq35);
    write(mmc+0x24,0x78); read(mmc+4,0x2100);
    assert(!(soc.interruptPending().normal & irq35));
    auto stopClock = [&] {
        write(mmc,5); read(mmc+4,0x100);
        soc.advanceHclkCycles(320*8-1); read(mmc+4,0x100);
        soc.advanceHclkCycles(1); read(mmc+4,0);
    };
    stopClock();
    for (uint32_t command : {55u,1u,1u}) {
        stage(command,command == 55 ? 1 : 3);
        read(mmc+0x2c,0xff); read(mmc+0x30,0xc000);
        write(mmc,6); read(mmc+4,0x100);
        soc.advanceHclkCycles(320*(48+255)-1); read(mmc+4,0x100);
        soc.advanceHclkCycles(1); read(mmc+4,0x2102);
        assert(soc.interruptPending().normal & irq35);
        write(mmc+0x24,0x78); read(mmc+4,0x2102);
        assert(!(soc.interruptPending().normal & irq35));
        stopClock();
    }
    // The no-card response timer uses the selected clock rate and timeout.
    write(mmc+8,0); write(mmc+0x10,7); write(mmc+0x24,0x7f);
    stage(55,1); write(mmc,6);
    soc.advanceHclkCycles(54); read(mmc+4,0x100);
    soc.advanceHclkCycles(1); read(mmc+4,0x2102);
    assert(!(soc.interruptPending().normal & irq35));
    write(mmc+0x24,0x78);
    assert(!(soc.interruptPending().normal & irq35));
    // Reset clears in-flight timing, status and the interrupt latch.
    write(mmc,8); read(mmc+4,0);
    soc.advanceHclkCycles(100000); read(mmc+4,0);
    assert(!(soc.interruptPending().normal & irq35));
    startup(); write(mmc+0x24,0x78);
    for (auto pair : {std::pair<uint32_t,uint32_t>{41,3},{17,9},{0,0},{0,0xc0},{55,3}}) {
        stage(pair.first,pair.second); write(mmc,6,A::sideEffect); read(mmc+4,0);
    }
    stage(55,1); write(mmc+0x10,0); write(mmc,6,A::sideEffect); read(mmc+4,0);
    write(mmc+0x10,2); soc.setMmcsdEmptySlot(false);
    write(mmc,6,A::externalInput);
    assert(soc.access(mmc+4,4,false,value) == A::externalInput);
    soc.setMmcsdEmptySlot(true); write(mmc,6);
    soc.advanceHclkCycles(320*50); read(mmc+4,0x2102);
    for (uint32_t offset : {0x34u,0x38u}) {
        value = 0xdeadbeef;
        assert(soc.access(mmc+offset,4,false,value) == A::externalInput && value == 0xdeadbeef);
    }
    // ENABLE gates the internal command/tail clocks, including their fractional phase.
    Imx1Mmcsd gated;
    gated.setEmptySlot(true);
    auto gw = [&](uint32_t address, uint32_t setting, Imx1MmcsdAccess expected = Imx1MmcsdAccess::ok) {
        assert(gated.access(address,4,true,setting) == expected);
    };
    auto gr = [&](uint32_t address, uint32_t expected) {
        uint32_t result = 0xdeadbeef;
        assert(gated.access(address,4,false,result) == Imx1MmcsdAccess::ok && result == expected);
    };
    gw(psr0,0xffffc400); gw(psr1,0xfffffbff);
    gw(mmc,8); gw(mmc,13);
    for (unsigned i = 0; i < 8; ++i) gw(mmc,5);
    gw(mmc+8,0x36); gw(mmc+0x24,0x78);
    gw(mmc+0x28,0); gw(mmc+0x2c,0xff); gw(mmc+0x30,0xc000); gw(mmc+0xc,0x80);
    gw(mmc,0); gr(mmc,0); gr(mmc+0x2c,0xff); gr(mmc+8,0x36);
    gw(mmc,4); gw(mmc,6);
    gated.advancePerclk2(319); gr(mmc+4,0x100);
    assert(gated.canInterrupt());
    gw(mmc,0); gr(mmc,0); gr(mmc+4,0);
    assert(!gated.canInterrupt() && !gated.interruptOutput());
    gated.advancePerclk2(1000000); gr(mmc+4,0);
    gw(mmc+8,0,Imx1MmcsdAccess::sideEffect);
    gw(mmc,4); gr(mmc,4); gr(mmc+4,0x100);
    gated.advancePerclk2(320*127); gr(mmc+4,0x100);
    gated.advancePerclk2(1); gr(mmc+4,0x2100);
    assert(gated.interruptOutput() && !gated.canInterrupt());
    // Disabling preserves a latched interrupt; the mask register acknowledges it.
    gw(mmc,0); gr(mmc+4,0x2000); assert(gated.interruptOutput());
    gated.advancePerclk2(1000000); assert(gated.interruptOutput());
    gw(mmc+0x24,0x78); assert(!gated.interruptOutput());
    gw(mmc,5); gated.advancePerclk2(320*3+319); gr(mmc+4,0x100);
    gw(mmc,0); gr(mmc+4,0); gated.advancePerclk2(1000000);
    auto gatedCopy = gated;
    gw(mmc,5); gated.advancePerclk2(320*4); gr(mmc+4,0x100);
    gated.advancePerclk2(1); gr(mmc+4,0); gr(mmc,4);
    uint32_t setting = 4;
    assert(gatedCopy.access(mmc,4,true,setting) == Imx1MmcsdAccess::ok);
    gatedCopy.advancePerclk2(320*4+1);
    assert(gatedCopy.access(mmc+4,4,false,value) == Imx1MmcsdAccess::ok && value == 0);
    // Bootstrap WFI prediction follows the module gate and retains timing on resume.
    startup(); write(mmc+0x24,0x78); stage(0,0x80); write(mmc,6);
    assert(soc.idleWakePossible()); write(mmc,0); assert(!soc.idleWakePossible());
    soc.advanceHclkCycles(100000); read(mmc+4,0);
    write(mmc,4); assert(soc.idleWakePossible());
    soc.advanceHclkCycles(320*128); assert(soc.interruptPending().normal & irq35);
    write(mmc,5); write(mmc,0); read(mmc,0); read(mmc+4,0);
    soc.advanceHclkCycles(320*20); read(mmc+4,0);
    write(0x0021b020,0x000b00ab,A::sideEffect);
    write(mmc,8); read(mmc,0); read(mmc+4,0);
    assert(!soc.idleWakePossible() && !(soc.interruptPending().normal & irq35));
    puts("i.MX1 AIPI2/MMCSD startup and timed empty-slot discovery: PASS");
}
