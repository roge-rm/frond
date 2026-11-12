#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "imx1_bootstrap.h"

int main() {
    Imx1Bootstrap soc;
    auto read = [&](uint32_t address, uint32_t expected) {
        uint32_t value = 0xdeadbeef;
        assert(soc.access(address, 4, false, value) == Imx1BootstrapAccess::ok);
        assert(value == expected);
    };
    auto write = [&](uint32_t address, uint32_t value, Imx1BootstrapAccess expected) {
        assert(soc.access(address, 4, true, value) == expected);
    };
    uint32_t unknown = 0xdeadbeef;
    read(0x00220000, 0x3e00);
    assert(soc.access(0x00220004, 4, false, unknown) == Imx1BootstrapAccess::externalInput);
    assert(unknown == 0xdeadbeef);
    write(0x00220004, 0x11110d00, Imx1BootstrapAccess::sideEffect);
    assert(soc.access(0x00220004, 4, false, unknown) == Imx1BootstrapAccess::externalInput);
    write(0x00220004, 0x11110d01, Imx1BootstrapAccess::ok);
    for (unsigned chip = 0; chip < 6; ++chip) {
        const uint32_t base = 0x00220000 + chip * 8;
        read(base, chip ? 0 : 0x3e00);
        read(base + 4, chip ? 0x0802 : 0x11110d01);
        const uint32_t upper = chip == 4 ? 0x3e70 : 0x3e00;
        const uint32_t lower = chip == 4 ? 0xffff4301 : chip == 5 ? 0x11111e01 : 0x11110d01;
        write(base, upper, Imx1BootstrapAccess::ok);
        write(base + 4, lower, Imx1BootstrapAccess::ok);
        read(base, upper); read(base + 4, lower);
        write(base, 0x80000000, chip == 5 ? Imx1BootstrapAccess::sideEffect : Imx1BootstrapAccess::reservedBits);
        for (uint32_t bad : {0x40000000u, 0x80u})
            write(base, bad, Imx1BootstrapAccess::reservedBits);
        for (uint32_t bad : {0x00100000u, 0x00200000u, 0x00300000u})
            write(base, bad, Imx1BootstrapAccess::sideEffect);
        write(base, 0x3f00, chip == 5 ? Imx1BootstrapAccess::ok : Imx1BootstrapAccess::sideEffect);
        if (chip == 5) write(base,upper,Imx1BootstrapAccess::ok);
        for (uint32_t bad : {0x80u, 0x20u, 0x08u, 0x04u})
            write(base + 4, lower | bad, Imx1BootstrapAccess::reservedBits);
        read(base, upper); read(base + 4, lower);
        for (uint32_t size : {1u, 2u, 8u}) {
            assert(soc.access(base, size, false, unknown) == Imx1BootstrapAccess::width);
            assert(soc.access(base + 4, size, true, unknown) == Imx1BootstrapAccess::width);
        }
        assert(soc.access(base + 1, 4, false, unknown) == Imx1BootstrapAccess::width);
        assert(soc.access(base + 5, 4, true, unknown) == Imx1BootstrapAccess::width);
        assert(unknown == 0xdeadbeef);
        read(base, upper); read(base + 4, lower);
    }
    // CS0 bus width, protection and enable remain fixed by the qualified flash path.
    for (uint32_t value : {0x11110d00u, 0x11110e01u, 0x11110d41u, 0x11110d11u})
        write(0x00220004, value, Imx1BootstrapAccess::sideEffect);
    write(0x00220004, 0x11110d03, Imx1BootstrapAccess::reservedBits);
    read(0x00220004, 0x11110d01);
    // PA, byte controls, port width and protection are retained for unconnected chip selects.
    for (unsigned chip = 1; chip < 6; ++chip) {
        write(0x00220004 + chip * 8, 0xffffff53, Imx1BootstrapAccess::ok);
        read(0x00220004 + chip * 8, 0xffffff53);
    }
    read(0x00220030, 0);
    write(0x00220030, 0, Imx1BootstrapAccess::ok);
    write(0x00220030, 4, Imx1BootstrapAccess::sideEffect);
    write(0x00220030, 1, Imx1BootstrapAccess::reservedBits);
    write(0x00220030, 8, Imx1BootstrapAccess::reservedBits);
    read(0x00220030, 0);
    assert(soc.access(0x00220030, 2, false, unknown) == Imx1BootstrapAccess::width);
    assert(soc.access(0x00220031, 4, true, unknown) == Imx1BootstrapAccess::width);
    assert(soc.access(0x00220034, 4, false, unknown) == Imx1BootstrapAccess::unmapped);
    assert(soc.access(0x0021fffc, 4, false, unknown) == Imx1BootstrapAccess::unmapped);
    // Configuration does not supply an attached device or turn an unknown bus read into zero.
    for (uint32_t address : {0x12000000u, 0x13000000u, 0x14000000u, 0x15000000u, 0x16000000u}) {
        assert(soc.access(address, 4, false, unknown) == Imx1BootstrapAccess::unmapped);
        assert(unknown == 0xdeadbeef);
        write(address, 0, Imx1BootstrapAccess::unmapped);
        uint32_t instruction = 0xdeadbeef;
        assert(!soc.instructionWord(address, instruction));
    }
    assert(!std::strcmp(Imx1Bootstrap::registerName(0x00220020), "EIM.CS4U"));
    assert(!std::strcmp(Imx1Bootstrap::registerName(0x0022002c), "EIM.CS5L"));
    std::puts("PASS: EIM CS0-5 reset/configuration, rejected writes retain state and unknown devices stay explicit");
}
