#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "imx1_bootstrap.h"

int main() {
    constexpr uint32_t gccr = 0x0021b810;
    Imx1Bootstrap soc;
    uint32_t setup = 0x57000003;
    assert(soc.access(0x0021b000, 4, true, setup) == Imx1BootstrapAccess::ok);
    setup = 2;
    assert(soc.access(0x0021b020, 4, true, setup) == Imx1BootstrapAccess::ok);
    auto read = [&](uint32_t address, uint32_t expected) {
        uint32_t value = 0xdeadbeef;
        assert(soc.access(address, 4, false, value, 3) == Imx1BootstrapAccess::ok);
        assert(value == expected);
    };
    auto write = [&](uint32_t address, uint32_t value,
                     Imx1BootstrapAccess expected = Imx1BootstrapAccess::ok) {
        assert(soc.access(address, 4, true, value, 3) == expected);
    };
    assert(!std::strcmp(Imx1Bootstrap::registerName(gccr), "SCM.GCCR"));
    read(gccr, 0x2f);
    // Each peripheral enable retains configuration, without supplying a device response.
    for (unsigned bit : {0u, 1u, 2u, 3u, 5u, 6u}) {
        write(gccr, 1u << bit);
        read(gccr, 1u << bit);
    }
    write(gccr, 0x6f);
    for (unsigned bit = 7; bit < 32; ++bit) {
        write(gccr, 0x6f | (1u << bit), Imx1BootstrapAccess::reservedBits);
        read(gccr, 0x6f);
    }
    // An unqualified BROM override rejects the whole combined change.
    write(gccr, 0x10, Imx1BootstrapAccess::externalInput);
    read(gccr, 0x6f);
    uint32_t unknown = 0xdeadbeef;
    for (uint32_t size : {0u, 1u, 2u, 8u}) {
        assert(soc.access(gccr, size, false, unknown, 3) == Imx1BootstrapAccess::width);
        assert(soc.access(gccr, size, true, unknown, 3) == Imx1BootstrapAccess::width);
    }
    for (unsigned offset = 1; offset < 4; ++offset) {
        assert(soc.access(gccr + offset, 4, false, unknown, 3) == Imx1BootstrapAccess::width);
        assert(soc.access(gccr + offset, 4, true, unknown, 3) == Imx1BootstrapAccess::width);
    }
    assert(unknown == 0xdeadbeef);
    read(gccr, 0x6f);
    for (auto address : {0x0021b800u, 0x0021b804u, 0x0021b808u, 0x0021b80cu, 0x0021b814u,
                         0x00208010u, 0x00212000u, 0x00214000u, 0x00222000u}) {
        unknown = 0xdeadbeef;
        assert(soc.access(address, 4, false, unknown, 3) == Imx1BootstrapAccess::unmapped);
        assert(unknown == 0xdeadbeef);
        assert(!soc.instructionWord(address, unknown));
    }
    assert(!soc.instructionWord(gccr, unknown));
    assert(!soc.instructionRead(gccr, 2, unknown));
    read(gccr, 0x6f);
    // Table 8-5 does not gate Timer 1. Retained and rejected writes leave its phase alone.
    write(0x0021b000, 0x57000003);
    write(0x0021b020, 2);
    write(0x00202004, 31);
    write(0x00202000, 0x103);
    soc.advanceFastBusCycles(95);
    read(0x00202010, 0);
    write(gccr, 0);
    for (unsigned i = 0; i < 100; ++i) read(gccr, 0);
    write(gccr, 0x1f, Imx1BootstrapAccess::externalInput);
    write(gccr, 0x80, Imx1BootstrapAccess::reservedBits);
    soc.advanceFastBusCycles(1);
    read(0x00202010, 1);
    write(gccr, 0x6f);
    soc.advanceFastBusCycles(96);
    read(0x00202010, 2);
    // SCM access must not weaken asynchronous CGM guards.
    write(0x00202000, 0);
    write(0x0021b000, 0x57000403, Imx1BootstrapAccess::sideEffect);
    read(0x0021b000, 0x57000003);
    std::puts("PASS: SCM GCCR reset/configuration, transactional guards, fetch isolation and Timer 1 phase");
}
