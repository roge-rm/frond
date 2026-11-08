#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "imx1_bootstrap.h"

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
    for (uint32_t setting : {2u,6u,0x26u}) write(mmc,setting,A::sideEffect);
    write(mmc,7,A::reservedBits); write(mmc,0x10,A::reservedBits); read(mmc,4);
    for (uint32_t setting : {0x400u,0x800u,0x100u,0x300u,7u})
        write(mmc+0xc,setting,A::sideEffect);
    read(mmc+0xc,0);
    write(mmc+4,0,A::sideEffect); read(mmc+4,0);
    for (uint32_t offset : {0x20u,0x28u,0x2cu,0x30u,0x34u,0x38u}) {
        value = 0xdeadbeef;
        assert(soc.access(mmc+offset,4,false,value) == A::externalInput && value == 0xdeadbeef);
        write(mmc+offset,0,A::externalInput);
    }
    for (uint32_t address : {psr0,psr1,mmc,mmc+4,mmc+8,mmc+0x24}) {
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
    puts("i.MX1 AIPI2/MMCSD idle startup: PASS");
}
