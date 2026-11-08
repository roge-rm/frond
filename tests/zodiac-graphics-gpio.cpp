#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "imx1_cs5_configuration.h"

int main() {
    using Access = Imx1BootstrapAccess;
    using Profile = Imx1GraphicsProfile;
    Imx1Eim eim;
    uint32_t value = 0x3e00;
    assert(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
    value = 0x11111e01;
    assert(eim.access(0x0022002c, 4, true, value) == Imx1EimAccess::ok);
    Imx1Cs5Configuration state(Profile::virtual5744Compatible8MiB), strict;
    auto same = [](Imx1GraphicsGpio::Bank a, Imx1GraphicsGpio::Bank b) {
        return a.control == b.control && a.data == b.data &&
               a.controlKnown == b.controlKnown && a.dataKnown == b.dataKnown;
    };
    auto reject = [&](uint32_t pc, uint32_t address, uint32_t size, bool write, uint32_t v, Access expected) {
        const auto a = state.gpioBank(0), b = state.gpioBank(1);
        const auto before = v;
        assert(state.access(eim, pc, address, size, write, v) == expected);
        assert(v == before && same(a, state.gpioBank(0)) && same(b, state.gpioBank(1)));
    };
    auto write = [&](uint32_t pc, uint32_t address, uint32_t v) {
        assert(state.access(eim, pc, address, 4, true, v) == Access::ok);
    };
    auto read = [&](uint32_t pc, uint32_t address, uint32_t expected) {
        uint32_t v = 0xdeadbeef;
        assert(state.access(eim, pc, address, 4, false, v) == Access::ok && v == expected);
    };
    auto configure = [&] {
        value = 0;
        assert(state.access(eim, 0x200bab4c, 0x16000010, 1, false, value) == Access::ok);
        value = 0x0e;
        assert(state.access(eim, 0x200babf8, 0x1600001c, 1, true, value) == Access::ok);
    };
    reject(0x200b99c4, 0x160104bc, 4, true, 0xffff1fff, Access::sequence);
    configure();
    reject(0x200b9a14, 0x160104b8, 4, true, 0x01ff0008, Access::sequence);
    for (const auto address : {0x160104b8u, 0x160104bcu, 0x160104c0u,
                               0x16010528u, 0x1601052cu, 0x16010530u}) {
        value = 0xffff1fff;
        assert(strict.access(eim, 0x200b99c4, address, 4, true, value) == Access::unmapped);
        assert(value == 0xffff1fff && !strict.gpioBank(0).controlKnown);
        for (auto size : {0u, 1u, 2u, 8u})
            reject(0x200b99c4, address, size, true, 0xffff1fff, Access::width);
        reject(0x1234, address, 4, true, 0xffff1fff, Access::externalInput);
        reject(0x1234, address, 4, false, 0x12345678, Access::externalInput);
    }
    for (const auto address : {0x160104b7u, 0x160104b9u, 0x16010534u, 0x1601007cu})
        reject(0x200b99c4, address, 4, true, 0xffff1fff, Access::unmapped);
    reject(0x200b99c4, 0x16010080, 4, true, 0xffff1fff, Access::sequence);
    reject(0x200b99c4, 0x160104bc, 4, true, 0xfffe1fff, Access::sideEffect);
    reject(0x200b9cb4, 0x160104bc, 4, false, 0xabcdef01, Access::sequence);
    value = 0x3e01;
    assert(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
    reject(0x200b99c4, 0x160104bc, 4, true, 0xffff1fff, Access::sequence);
    value = 0x3e00;
    assert(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
    write(0x200b99c4, 0x160104bc, 0xffff1fff);
    write(0x200b99f0, 0x1601052c, 0xffff8001);
    assert(state.gpioBank(0).padKnown() == 0 && state.gpioBank(1).padKnown() == 0);
    write(0x200b9a14, 0x160104b8, 0x01ff0008);
    write(0x200b9a34, 0x16010528, 0x80010001);
    assert(state.gpioBank(0).direction() == 0x1ff && state.gpioBank(0).latch() == 8);
    assert(state.gpioBank(0).pullDown() == 0x1fff);
    assert(state.gpioBank(0).padKnown() == 0x1ff && state.gpioBank(0).padValue() == 8);
    assert(state.gpioBank(1).padKnown() == 0x8001 && state.gpioBank(1).padValue() == 1);
    read(0x200b9cb4, 0x160104bc, 0xffff1fff);
    read(0x200b9b80, 0x1601052c, 0xffff8001);
    read(0x200b9b50, 0x160104b8, 0x01ff0008);
    read(0x200b9c78, 0x16010528, 0x80010001);
    reject(0x200b9b5c,0x160104b8,4,true,0x01ff0038,Access::sideEffect);
    write(0x200b9b5c,0x160104b8,0x01ff0018);
    assert(state.gpioBank(0).padValue() == 0x18);
    reject(0x200b9b5c,0x160104b8,4,true,0x01ff0008,Access::sequence);
    read(0x200b9b50,0x160104b8,0x01ff0018);
    write(0x200b9b5c,0x160104b8,0x01ff0008);
    reject(0x200b9b8c,0x1601052c,4,true,0xfffe8001,Access::sideEffect);
    write(0x200b9b8c,0x1601052c,0xffff8000);
    // Latch state survives direction changes, input pads remain unknown.
    write(0x200b9a14, 0x160104b8, 0x00000008);
    assert(state.gpioBank(0).latch() == 8 && state.gpioBank(0).padKnown() == 0);
    write(0x200b9a14, 0x160104b8, 0x00080008);
    assert(state.gpioBank(0).padKnown() == 8 && state.gpioBank(0).padValue() == 8);
    write(0x200b9a34, 0x16010528, 0xffffa55a);
    assert(state.gpioBank(1).padKnown() == 0xffff && state.gpioBank(1).padValue() == 0xa55a);
    reject(0x200b9cd4, 0x160104c0, 4, false, 0xabcdef01, Access::externalInput);
    reject(0x200b9cd4, 0x16010530, 4, false, 0xabcdef01, Access::externalInput);
    reject(0x200b99c4, 0x160104bc, 4, true, 0, Access::sideEffect);
    state.reset();
    for (unsigned i = 0; i < 2; ++i) {
        const auto bank = state.gpioBank(i);
        assert(bank.control == 0 && bank.data == 0 && !bank.controlKnown && !bank.dataKnown);
        assert(bank.padKnown() == 0 && bank.padValue() == 0);
    }
    reject(0x200b9c78, 0x160104b8, 4, false, 0xabcdef01, Access::sequence);
    configure();
    reject(0x200b9c78, 0x160104b8, 4, false, 0xabcdef01, Access::sequence);
    write(0x200b99c4, 0x160104bc, 0xffff0000);
    write(0x200b9a14, 0x160104b8, 0x80008000);
    assert(state.gpioBank(0).padKnown() == 0x8000 && state.gpioBank(0).padValue() == 0x8000);
    std::puts("PASS: virtual GPIO bank state, logical output pads, stored readback, unknown inputs, strict/width/PC/EIM guards and reset");
}
