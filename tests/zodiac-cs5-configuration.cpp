#include <cassert>
#include <array>
#include <cstdio>
#include <initializer_list>
#include "imx1_cs5_configuration.h"
#include "zodiac-graphics-setup.h"

int main() {
    using Access = Imx1BootstrapAccess;
    Imx1Eim eim;
    auto configure = [&](uint32_t address, uint32_t value) {
        assert(eim.access(address, 4, true, value) == Imx1EimAccess::ok);
    };
    for (uint32_t pc : {0x200bab4cu, 0x200bab80u}) {
        Imx1Cs5Configuration state;
        auto reject = [&](uint32_t callPc, uint32_t address, uint32_t size, bool write,
                          uint32_t value, Access expected) {
            const bool selected = state.apertureSelected(), known = state.defaultsKnown();
            const auto defaults = state.defaultsValue();
            const uint32_t before = value;
            assert(state.access(eim, callPc, address, size, write, value) == expected);
            assert(value == before && state.apertureSelected() == selected && state.defaultsKnown() == known);
            assert(state.defaultsValue() == defaults);
        };
        configure(0x00220028, 0x3e00);
        configure(0x0022002c, 0x11111e00);
        reject(pc, 0x16000010, 1, false, 0x76543210, Access::sequence);
        configure(0x0022002c, 0x11111e01);
        reject(0x200babf8, 0x1600001c, 1, true, 0x0e, Access::sequence);
        reject(pc + 4, 0x16000010, 1, false, 0x76543210, Access::externalInput);
        reject(pc, 0x16000010, 1, true, 1, Access::externalInput);
        for (uint32_t size : {0u, 2u, 4u, 8u}) {
            reject(pc, 0x16000010, size, false, 0x76543210, Access::width);
            reject(0x200babf8, 0x1600001c, size, true, 0x0e, Access::width);
        }
        for (uint32_t address : {0x16000000u, 0x16000014u, 0x16000018u, 0x16000020u,
                                 0x16010008u, 0x17000010u})
            reject(pc, address, 1, false, 0x76543210, Access::unmapped);
        uint32_t value = 0x76543210;
        assert(state.access(eim, pc, 0x16000010, 1, false, value) == Access::ok && value == 1);
        assert(state.apertureSelected() && !state.defaultsKnown());
        reject(0x200babf8, 0x1600001c, 1, false, 0x76543210, Access::externalInput);
        reject(0x200babfc, 0x1600001c, 1, true, 0x0e, Access::externalInput);
        for (uint32_t defaults = 0; defaults < 256; ++defaults)
            if (defaults != 0x0e)
                reject(0x200babf8, 0x1600001c, 1, true, defaults, Access::sideEffect);
        value = 0x0e;
        assert(state.access(eim, 0x200babf8, 0x1600001c, 1, true, value) == Access::ok);
        assert(state.defaultsKnown() && state.defaultsValue() == 0x0e);
        for (uint32_t address : {0x16010000u, 0x16010004u}) {
            reject(0x200bac00, address, 4, false, 0x76543210, Access::externalInput);
            reject(0x200bac28, address, 4, false, 0xabcdef01, Access::externalInput);
            reject(0x200bac00, address, 4, true, 0x76543210, Access::externalInput);
            for (uint32_t size : {0u, 1u, 2u, 8u})
                reject(0x200bac00, address, size, false, 0x76543210, Access::width);
        }
        configure(0x00220028, 0x3e01);
        reject(pc, 0x16000010, 1, false, 0x76543210, Access::sequence);
    }
    {
        // b30, the chip's USB function register: reads 0 and takes 0 once the chip is set up (marked assumption).
        Imx1Eim wired;
        uint32_t upper = 0x3e00, lower = 0x11111e01;
        assert(wired.access(0x00220028, 4, true, upper) == Imx1EimAccess::ok);
        assert(wired.access(0x0022002c, 4, true, lower) == Imx1EimAccess::ok);
        Imx1Cs5Configuration chip(Imx1GraphicsProfile::virtual5744Compatible8MiB), plain;
        uint32_t value = 0x76543210;
        assert(chip.access(wired, 0x200c5348, 0x16010b30, 4, false, value) == Access::sequence && value == 0x76543210);
        assert(plain.access(wired, 0x200c5348, 0x16010b30, 4, false, value) == Access::unmapped);
        configureGraphicsInterface(chip, wired);
        assert(chip.access(wired, 0x200c5348, 0x16010b30, 4, false, value) == Access::ok && value == 0);
        assert(chip.access(wired, 0x200c537c, 0x16010b30, 4, true, value) == Access::ok);
        for (uint32_t on : {1u, 0x400u, 0x401u, 0x80000000u}) {
            value = on;
            assert(chip.access(wired, 0x200c537c, 0x16010b30, 4, true, value) == Access::sideEffect);
        }
        value = 0;
        assert(chip.access(wired, 0x200c5348, 0x16010b30, 2, false, value) == Access::width);
        value = 0x76543210;
        assert(chip.access(wired, 0x200c5348, 0x16010b30, 4, false, value) == Access::ok && value == 0);
    }
    {
        // PWRMGT (98) from player code with the command ring busy: the read and bit 0 go through, another bit
        // is refused, and the ROM's own clock writes still wait for an empty ring.
        Imx1Eim wired;
        uint32_t upper = 0x3e00, lower = 0x11111e01;
        assert(wired.access(0x00220028, 4, true, upper) == Imx1EimAccess::ok);
        assert(wired.access(0x0022002c, 4, true, lower) == Imx1EimAccess::ok);
        Imx1Cs5Configuration chip(Imx1GraphicsProfile::virtual5744Compatible8MiB);
        uint32_t value = 0x76543210;
        configureGraphicsInterface(chip, wired);
        configureGraphicsController(chip, wired);
        constexpr uint32_t player = 0x4053a32c, pwrmgt = 0x16010098;
        assert(chip.access(wired, 0x200acb98, 0x160113e8, 4, false, value) == Access::ok);
        for (auto op : {std::array<uint32_t, 3>{0x200acba8, 0x13e8, 0x400}, {0x200acbb4, 0x144, 0x2440},
                        {0x200acbc4, 0x10a8, 0x1fff1fff}, {0x200acbd0, 0x4dc, 0xc0000},
                        {0x200b8e80, 0x214, 0x10150000}, {0x200b8e88, 0x220, 0},
                        {0x200b8f04, 8, 0x04150000}, {0x200b8f10, 0x230, 0x80000000}, {0x200b8f20, 0x210, 0x0800000d}}) {
            value = op[2];
            assert(chip.access(wired, op[0], 0x16010000 + op[1], 4, true, value) == Access::ok);
        }
        value = 0;
        assert(chip.access(wired, 0x200c573c, 0x16010098, 4, true, value) == Access::ok);
        value = 0;
        assert(chip.access(wired, player, pwrmgt, 4, false, value) == Access::ok);
        const uint32_t idle = value;
        value = 4;
        assert(chip.access(wired, player, 0x16010220, 4, true, value) == Access::ok);
        assert(!chip.engineState().emptyRing());
        value = 0;
        assert(chip.access(wired, player, 0x16010140, 4, false, value) == Access::ok && value == 0x80000000);
        value = 0;
        assert(chip.access(wired, player, pwrmgt, 4, false, value) == Access::ok && value == idle);
        value = idle ^ 1;
        assert(chip.access(wired, player, pwrmgt, 4, true, value) == Access::ok);
        value = 0;
        assert(chip.access(wired, player, pwrmgt, 4, false, value) == Access::ok && value == (idle ^ 1));
        for (uint32_t bit : {2u, 4u, 0x100u, 0x80000000u}) {
            value = (idle ^ 1) ^ bit;
            assert(chip.access(wired, player, pwrmgt, 4, true, value) == Access::sideEffect);
            assert(value == ((idle ^ 1) ^ bit));
        }
        value = 0;
        assert(chip.access(wired, player, pwrmgt, 4, false, value) == Access::ok && value == (idle ^ 1));
        assert(chip.access(wired, 0x200c4c00, pwrmgt, 4, true, value) == Access::sequence);
    }
    {
        // The USB driver's chip words, the identity and its block wait for the same set-up as every other path:
        // too early they stop with sequence and leave the value alone.
        Imx1Eim wired;
        uint32_t upper = 0x3e00, lower = 0x11111e01;
        assert(wired.access(0x00220028, 4, true, upper) == Imx1EimAccess::ok);
        assert(wired.access(0x0022002c, 4, true, lower) == Imx1EimAccess::ok);
        Imx1Cs5Configuration chip(Imx1GraphicsProfile::virtual5744Compatible8MiB);
        const auto early = [&] {
            for (uint32_t address : {0x16010000u, 0x160100a8u, 0x16010b10u, 0x16010b1cu, 0x16010b30u, 0x16010200u,
                                     0x16201234u, 0x16200000u}) {
                for (bool write : {false, true}) {
                    if (write && address == 0x16010000) continue;  // the identity is never written
                    uint32_t value = 0x76543210;
                    assert(chip.access(wired, 0x20177e40, address, 4, write, value) == Access::sequence);
                    assert(value == 0x76543210);
                }
            }
        };
        early();
        configureGraphicsInterface(chip, wired);
        early();
        assert(!chip.usbState().used());
    }
    {
        // The enable's read-modify-write paths (20086dea, 200876ee) set, keep and clear the USB group (0x50);
        // other writers still can't.
        Imx1GraphicsInterrupts interrupts;
        uint32_t value = 0x51;
        assert(interrupts.access(0x20086e08, 0x16010200, 4, true, value) == Access::ok && interrupts.enableValue() == 0x51);
        value = 0;
        assert(interrupts.access(0x20087702, 0x16010200, 4, true, value) == Access::ok && interrupts.enableValue() == 0);
        value = 0x40;
        assert(interrupts.usbAccess(0x16010200, true, value) == Access::ok);
        for (uint32_t pc : {0x20086e08u, 0x20087702u}) {
            value = 0x41;
            assert(interrupts.access(pc, 0x16010200, 4, true, value) == Access::ok && interrupts.enableValue() == 0x41);
            value = 0x40;
            assert(interrupts.access(pc, 0x16010200, 4, true, value) == Access::ok && interrupts.enableValue() == 0x40);
        }
        value = 0x41;
        assert(interrupts.access(0x20086970, 0x16010200, 4, true, value) == Access::sideEffect);
        value = 0x40;
        assert(interrupts.access(0x200c4c00, 0x16010200, 4, true, value) == Access::externalInput);
        value = 0x21;
        assert(interrupts.access(0x20086e08, 0x16010200, 4, true, value) == Access::reservedBits);
    }
    std::puts("PASS: both native aperture paths, exact defaults transaction, unknown full identity/revision and transactional EIM/width/PC/value guards, b30 reads and takes 0");
}
