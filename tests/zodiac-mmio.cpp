#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "imx1_bootstrap.h"

int main() {
    Imx1Bootstrap devices;
    auto read = [&](uint32_t address, uint32_t expected) {
        uint32_t value = 0xdeadbeef;
        assert(devices.access(address, 4, false, value) == Imx1BootstrapAccess::ok);
        assert(value == expected);
    };
    auto write = [&](uint32_t address, uint32_t value, Imx1BootstrapAccess expected) {
        assert(devices.access(address, 4, true, value) == expected);
    };
    read(0x00300000, 0);
    write(0x00300000, 0x12345678, Imx1BootstrapAccess::ok);
    uint32_t byte = 0xab;
    assert(devices.access(0x00300001, 1, true, byte) == Imx1BootstrapAccess::ok);
    read(0x00300000, 0x1234ab78);
    uint32_t instruction = 0;
    assert(devices.instructionWord(0x00300000, instruction) && instruction == 0x1234ab78);
    assert(!devices.instructionWord(0x00300001, instruction));
    assert(!devices.instructionWord(0x00205034, instruction));
    write(0x0031fffc, 0x87654321, Imx1BootstrapAccess::ok);
    read(0x00320000, 0x87654321); read(0x003ffffc, 0x87654321);
    write(0x00320000, 0, Imx1BootstrapAccess::sideEffect);
    read(0x0031fffc, 0x87654321);
    byte = 0xdeadbeef;
    assert(devices.access(0x0031fffe, 4, false, byte) == Imx1BootstrapAccess::width);
    assert(byte == 0xdeadbeef);
    assert(devices.access(0x00320000, 2, false, byte) == Imx1BootstrapAccess::width);
    assert(devices.access(0x00300000, 8, true, byte) == Imx1BootstrapAccess::width);
    read(0x00205034, 0);
    read(0x00220000, 0x3e00);
    uint32_t unknown = 0xdeadbeef;
    assert(devices.access(0x00220004, 4, false, unknown) == Imx1BootstrapAccess::externalInput);
    assert(unknown == 0xdeadbeef);
    write(0x00220004, 0x11110d01, Imx1BootstrapAccess::ok);
    read(0x00220004, 0x11110d01);
    write(0x00220004, 0x11110d00, Imx1BootstrapAccess::sideEffect);
    read(0x00220004, 0x11110d01);
    write(0x00205034, 0, Imx1BootstrapAccess::ok);
    write(0x00205034, 2, Imx1BootstrapAccess::sideEffect);
    write(0x00205034, 4, Imx1BootstrapAccess::reservedBits);
    read(0x00205034, 0);
    read(0x00220010, 0); read(0x00220014, 0x0802);
    write(0x00220010, 0x200, Imx1BootstrapAccess::ok);
    write(0x00220014, 0x11110601, Imx1BootstrapAccess::ok);
    read(0x00220010, 0x200); read(0x00220014, 0x11110601);
    write(0x00220010, 0x80000000, Imx1BootstrapAccess::reservedBits);
    write(0x00220010, 0x00100000, Imx1BootstrapAccess::sideEffect);
    write(0x00220014, 0x80, Imx1BootstrapAccess::reservedBits);
    read(0x00220010, 0x200); read(0x00220014, 0x11110601);
    read(0x0021c300, 0); read(0x0021c320, 0xffffffff);
    write(0x0021c300, 1, Imx1BootstrapAccess::ok);
    read(0x0021c300, 1);
    write(0x0021c320, 0, Imx1BootstrapAccess::ok);
    read(0x0021c320, 0);
    uint32_t value = 0xdeadbeef;
    assert(devices.access(0x0021c324, 4, false, value) == Imx1BootstrapAccess::externalInput);
    assert(value == 0xdeadbeef);
    assert(!devices.setGpioDInput(0x40));
    Imx1Bootstrap sampled;
    assert(sampled.setGpioDInput(0x40));
    uint32_t sample = 0;
    assert(sampled.access(0x0021c324, 4, false, sample) == Imx1BootstrapAccess::ok && sample == 0x40);
    assert(!sampled.setGpioDInput(0));
    write(0x0021c324, 0, Imx1BootstrapAccess::sideEffect);
    for (uint32_t size : {1u, 2u, 8u})
        assert(devices.access(0x00205034, size, false, value) == Imx1BootstrapAccess::width);
    assert(devices.access(0x00205035, 4, false, value) == Imx1BootstrapAccess::width);
    assert(devices.access(0x00205030, 4, false, value) == Imx1BootstrapAccess::unmapped);
    write(0x00221000, 0x92120200, Imx1BootstrapAccess::memoryUnavailable);
    read(0x00221000, 0x01000300);
    assert(devices.access(0x00221004, 4, false, value) == Imx1BootstrapAccess::externalInput);
    devices.setRamMiB(128);
    write(0x00221000, 0x8212d200, Imx1BootstrapAccess::sequence);
    for (uint32_t bank : {0u, 1u}) {
        const uint32_t control = 0x00221000 + bank * 4;
        const uint32_t memory = 0x08000000 + bank * 0x04000000;
        write(control, 0xa2120200, Imx1BootstrapAccess::sequence);
        write(control, 0x92120200, Imx1BootstrapAccess::ok);
        assert(!devices.instructionWord(memory + 0x00200000, instruction));
        write(control, 0xa2120200, Imx1BootstrapAccess::sequence);
        assert(devices.access(memory, 4, false, value) == Imx1BootstrapAccess::sequence);
        assert(devices.access(memory + 0x00200000, 4, false, value) == Imx1BootstrapAccess::command);
        assert(value == 0);
        write(control, 0xa2120200, Imx1BootstrapAccess::ok);
        for (unsigned i = 0; i < 7; ++i)
            assert(devices.access(memory, 4, false, value) == Imx1BootstrapAccess::command);
        write(control, 0xb2120200, Imx1BootstrapAccess::sequence);
        assert(devices.access(memory, 4, false, value) == Imx1BootstrapAccess::command);
        write(control, 0xb2120200, Imx1BootstrapAccess::ok);
        assert(devices.access(memory + 0x00111800, 4, false, value) == Imx1BootstrapAccess::command);
        write(control, 0x8212d200, Imx1BootstrapAccess::ok);
        write(memory, 0xaa55aa55 + bank, Imx1BootstrapAccess::ok);
        read(memory, 0xaa55aa55 + bank);
        value = 0x5a;
        assert(devices.access(memory + 1, 1, true, value) == Imx1BootstrapAccess::ok);
        read(memory, 0xaa555a55 + bank);
    }
    read(0x08000000, 0xaa555a55);
    read(0x0c000000, 0xaa555a56);
    Imx1Bootstrap small;
    small.setRamMiB(32);
    value = 0x92120200;
    assert(small.access(0x00221000, 4, true, value) == Imx1BootstrapAccess::sideEffect);
    value = 0x91120200;
    assert(small.access(0x00221000, 4, true, value) == Imx1BootstrapAccess::ok);
    assert(small.access(0x0c000000, 4, false, value) == Imx1BootstrapAccess::memoryUnavailable);
    {
        Imx1Bootstrap soc;
        for (bool write : {false, true}) {
            assert(soc.cacheMemoryRange(0x00300000, 32, write));
            assert(soc.cacheMemoryRange(0x0031ffe0, 32, write));
            assert(!soc.cacheMemoryRange(0x0031fff0, 32, write));
            for (uint32_t address : {0x00320000u, 0x00217000u, 0x08000000u, 0x10000000u, 0xfffffff0u})
                assert(!soc.cacheMemoryRange(address, 32, write));
        }
        soc.setRamMiB(32);
        uint32_t value = 0x91120200;
        assert(soc.access(0x00221000, 4, true, value) == Imx1BootstrapAccess::ok);
        assert(!soc.cacheMemoryRange(0x08000000, 32, false));
        auto access = [&](uint32_t pa, uint32_t v, bool write) {
            assert(soc.access(pa, 4, write, v) == (pa >= 0x08000000 ? Imx1BootstrapAccess::command : Imx1BootstrapAccess::ok));
        };
        access(0x08200000, 0, false); access(0x00221000, 0xa1120200, true);
        for (unsigned i = 0; i < 8; ++i) access(0x08000000, 0, false);
        access(0x00221000, 0xb1120200, true); access(0x08111800, 0, false);
        access(0x00221000, 0x81129200, true);
        for (bool write : {false, true}) {
            assert(soc.cacheMemoryRange(0x08000000, 32, write));
            assert(soc.cacheMemoryRange(0x09ffffe0, 32, write));
            assert(!soc.cacheMemoryRange(0x09fffff0, 32, write));
            assert(!soc.cacheMemoryRange(0x0a000000, 32, write));
            assert(!soc.cacheMemoryRange(0x0c000000, 32, write));
        }
        access(0x00221000, 0x91120200, true);
        assert(!soc.cacheMemoryRange(0x08000000, 32, false));
    }
    std::puts("PASS: documented bootstrap register state and explicit unsupported operations");
}
