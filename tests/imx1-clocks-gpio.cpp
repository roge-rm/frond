#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "imx1_bootstrap.h"

int main() {
    Imx1Gpio gpio;
    for (unsigned port = 0; port < 4; ++port) {
        const uint32_t base = 0x0021c000 + port * 0x100;
        assert(gpio.interruptOutput(port) == Imx1GpioLine::clear);
        uint32_t value = 0xdeadbeef;
        assert(gpio.access(base + 0x24, 4, false, value) == Imx1GpioAccess::externalInput);
        assert(value == 0xdeadbeef);
        for (unsigned bit = 0; bit < 32; ++bit) {
            value = 1u << bit;
            assert(gpio.access(base + 0x30, 4, true, value) == Imx1GpioAccess::sideEffect);
            assert(gpio.interruptOutput(port) == Imx1GpioLine::clear);
            assert(gpio.access(base + 0x30, 4, false, value) == Imx1GpioAccess::ok && value == 0);
        }
        for (auto sample : {0u, 0xffffffffu}) {
            gpio.setInput(port, sample);
            assert(gpio.interruptOutput(port) == Imx1GpioLine::clear);
            value = 0xdeadbeef;
            assert(gpio.access(base + 0x34, 4, false, value) == Imx1GpioAccess::externalInput);
            assert(value == 0xdeadbeef);
        }
    }
    assert(gpio.interruptOutput(4) == Imx1GpioLine::unknown);
    Imx1Bootstrap soc;
    auto read = [&](uint32_t address, uint32_t expected) {
        uint32_t value = 0xdeadbeef;
        assert(soc.access(address, 4, false, value) == Imx1BootstrapAccess::ok);
        assert(value == expected);
    };
    auto write = [&](uint32_t address, uint32_t value, Imx1BootstrapAccess expected) {
        assert(soc.access(address, 4, true, value) == expected);
    };
    read(0x0021b000, 0x0f00ac03);
    read(0x0021b004, 0x003f1437);
    read(0x0021b008, 0);
    read(0x0021b00c, 0x043f1437);
    read(0x0021b020, 0x000b00bb);
    write(0x0021b000, 0x57000003, Imx1BootstrapAccess::ok);
    write(0x0021b020, 0x00100012, Imx1BootstrapAccess::ok);
    read(0x0021b000, 0x57000003);
    read(0x0021b020, 0x00100012);
    write(0x0021b000, 0x57800003, Imx1BootstrapAccess::reservedBits);
    write(0x0021b000, 0xc0000003, Imx1BootstrapAccess::reservedBits);
    write(0x0021b000, 0xe0000003, Imx1BootstrapAccess::reservedBits);
    write(0x0021b020, 0x00100112, Imx1BootstrapAccess::reservedBits);
    for (auto value : {0x57400003u, 0x57000001u, 0x57000002u})
        write(0x0021b000, value, Imx1BootstrapAccess::sideEffect);
    write(0x0021b000, 0x57010003, Imx1BootstrapAccess::externalInput);
    read(0x0021b000, 0x57000003);
    read(0x0021b020, 0x00100012);
    for (auto address : {0x0021b008u, 0x0021b00cu, 0x0021b010u}) {
        write(address, 0x80000000, Imx1BootstrapAccess::reservedBits);
        write(address, address == 0x0021b008 || address == 0x0021b010 ? 0x40 : 0x003f1438,
              Imx1BootstrapAccess::sideEffect);
    }
    write(0x0021b004, 0x80000000, Imx1BootstrapAccess::reservedBits);
    read(0x0021b004, 0x003f1437);
    read(0x0021b008, 0);
    read(0x0021b00c, 0x043f1437);
    uint32_t unknown = 0xdeadbeef;
    for (unsigned i = 0; i < 8; ++i)
        assert(soc.access(0x0021b010, 4, false, unknown) == Imx1BootstrapAccess::externalInput);
    assert(unknown == 0xdeadbeef);
    assert(soc.access(0x0021b014, 4, false, unknown) == Imx1BootstrapAccess::unmapped);

    constexpr uint32_t inUse[] = {0x00c3fffe, 0xffffffff, 0x0007ffff, 0xffffffff};
    for (unsigned port = 0; port < 4; ++port) {
        const uint32_t base = 0x0021c000 + port * 0x100;
        for (unsigned index = 0; index < 17; ++index) {
            const uint32_t address = base + index * 4;
            assert(Imx1Bootstrap::registerName(address));
            if (index == 9) {
                assert(soc.access(address, 4, false, unknown) == Imx1BootstrapAccess::externalInput);
                write(address, 0, Imx1BootstrapAccess::sideEffect);
                continue;
            }
            if (index == 13) {
                assert(soc.access(address, 4, false, unknown) == Imx1BootstrapAccess::externalInput);
                write(address, 0xffffffff, Imx1BootstrapAccess::ok);
                assert(soc.access(address, 4, false, unknown) == Imx1BootstrapAccess::externalInput);
                continue;
            }
            uint32_t expected = index >= 3 && index <= 6 ? 0xffffffff : 0;
            if (index == 8) expected = inUse[port];
            if (index == 16) expected = port == 2 ? 0xf910ffff : 0xffffffff;
            read(address, expected);
            if (index == 12) {
                write(address, 0, Imx1BootstrapAccess::ok);
                write(address, 1, Imx1BootstrapAccess::sideEffect);
                read(address, 0);
            } else if (index == 15) {
                write(address, 2, Imx1BootstrapAccess::reservedBits);
                write(address, 1, Imx1BootstrapAccess::sideEffect);
                write(address, 0, Imx1BootstrapAccess::ok);
                read(address, 0);
            } else {
                write(address, 0xa55a369c ^ port, Imx1BootstrapAccess::ok);
                read(address, 0xa55a369c ^ port);
            }
        }
        assert(soc.access(base + 0x44, 4, false, unknown) == Imx1BootstrapAccess::unmapped);
    }
    assert(unknown == 0xdeadbeef);
    soc.setGpioDInput(0x600);
    read(0x0021c324, 0x600);
    // Output and pull settings do not stand in for measured pin levels.
    assert(soc.access(0x0021c024, 4, false, unknown) == Imx1BootstrapAccess::externalInput);
    soc.setGpioInput(0, 0);
    read(0x0021c024, 0);
    soc.setGpioInput(0, 0x80);
    // Retaining a full sample is independent of direction, mux, output and pulls.
    for (auto offset : {0u, 0x1cu, 0x20u, 0x40u})
        write(0x0021c000 + offset, 0xffffffff, Imx1BootstrapAccess::ok);
    read(0x0021c024, 0x80);
    read(0x0021c324, 0x600);
    for (auto address : {0x0021c124u, 0x0021c224u})
        assert(soc.access(address, 4, false, unknown) == Imx1BootstrapAccess::externalInput);
    assert(unknown == 0xdeadbeef);
    assert(!std::strcmp(Imx1Bootstrap::registerName(0x0021b000), "CGM.CSCR"));
    assert(!std::strcmp(Imx1Bootstrap::registerName(0x0021c330), "GPIO.IMR_D"));
    for (auto address : {0x0021b000u, 0x0021b004u, 0x0021b010u, 0x0021b020u, 0x0021c000u, 0x0021c340u}) {
        for (auto size : {1u, 2u, 8u}) {
            assert(soc.access(address, size, false, unknown) == Imx1BootstrapAccess::width);
            write(address + 1, 0, Imx1BootstrapAccess::width);
        }
    }
    std::puts("PASS: static CGM and named GPIO startup state, explicit clock and pin limits");
}
