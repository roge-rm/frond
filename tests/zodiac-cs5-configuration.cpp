#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "imx1_cs5_configuration.h"

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
    std::puts("PASS: both native aperture paths, exact defaults transaction, unknown full identity/revision and transactional EIM/width/PC/value guards");
}
