#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "imx1_cs5_reset.h"

int main() {
    using Access = Imx1BootstrapAccess;
    Imx1Eim eim;
    auto configure = [&](uint32_t address, uint32_t value) {
        assert(eim.access(address, 4, true, value) == Imx1EimAccess::ok);
    };
    Imx1Cs5Reset unknown(0xa5);
    uint32_t value = 0xabcdef;
    assert(unknown.access(eim, 0x200bac84, 0x16000018, 1, false, value) == Access::sequence);
    configure(0x00220028, 0x3e00);
    configure(0x0022002c, 0x11111e01);
    for (unsigned sample = 0; sample < 256; ++sample) {
        Imx1Cs5Reset state(sample);
        auto reject = [&](uint32_t pc, uint32_t address, uint32_t size, bool write, Access expected) {
            uint32_t v = 0x12345678;
            const auto phase = state.phaseValue(), status = unsigned(state.statusValue());
            assert(state.access(eim, pc, address, size, write, v) == expected);
            assert(v == 0x12345678 && phase == state.phaseValue() && status == state.statusValue());
        };
        reject(0x200bac84, 0x16000010, 1, false, Access::unmapped);
        reject(0x200bac84, 0x16000118, 1, false, Access::unmapped);
        for (unsigned size : {0u, 2u, 4u, 8u})
            reject(0x200bac84, 0x16000018, size, false, Access::width);
        reject(0x200bac80, 0x16000018, 1, false, Access::externalInput);
        reject(0x200bac84, 0x16000018, 1, true, Access::externalInput);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            value = 0xabcdef;
            assert(state.access(eim, 0x200bac84, 0x16000018, 1, false, value) == Access::ok);
            assert(value == (repeat == 0 ? sample : (sample & 0xf7)));
            value = (sample | 8) ^ 1;
            assert(state.access(eim, 0x200bac9c, 0x16000018, 1, true, value) == Access::sideEffect);
            assert(state.phaseValue() == 1);
            value = sample | 8;
            assert(state.access(eim, 0x200bac9c, 0x16000018, 1, true, value) == Access::ok);
            assert(state.access(eim, 0x200baca4, 0x16000018, 1, false, value) == Access::ok);
            assert(value == (sample | 8));
            assert((int32_t(value << 28) < 0));
            value &= 0xf7;
            assert(state.access(eim, 0x200bace8, 0x16000018, 1, true, value) == Access::ok);
            assert(state.access(eim, 0x200bacf0, 0x16000018, 1, false, value) == Access::ok);
            assert(value == (sample & 0xf7));
            assert((int32_t(value << 28) >= 0));
            assert(state.phaseValue() == 0);
        }
    }
    configure(0x0022002c, 0x11111e00);
    assert(unknown.access(eim, 0x200bac84, 0x16000018, 1, false, value) == Access::sequence);
    assert(unknown.phaseValue() == 0);
    std::puts("PASS: all 256 opaque samples preserve other bits and take identical reset poll branches; EIM, PC, width, phase and write guards retain state");
}
