#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "imx1_cs5_configuration.h"

int main() {
    using Access = Imx1BootstrapAccess;
    using Profile = Imx1GraphicsProfile;
    assert(imx1GraphicsProfileValid(Profile::strict));
    assert(imx1GraphicsProfileValid(Profile::virtual5744Compatible8MiB));
    assert(!imx1GraphicsProfileValid(static_cast<Profile>(255)));
    Imx1Eim eim;
    uint32_t value = 0x3e00;
    assert(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
    value = 0x11111e01;
    assert(eim.access(0x0022002c, 4, true, value) == Imx1EimAccess::ok);
    Imx1Cs5Configuration invalid(static_cast<Profile>(255));
    value = 0xaabbccdd;
    assert(invalid.access(eim, 0x200bab4c, 0x16000010, 1, false, value) == Access::sequence);
    assert(value == 0xaabbccdd && !invalid.apertureSelected() && !invalid.defaultsKnown());
    for (auto profile : {Profile::strict, Profile::virtual5744Compatible8MiB}) {
        Imx1Cs5Configuration state(profile);
        auto reject = [&](uint32_t pc, uint32_t address, uint32_t size, bool write, Access expected) {
            const auto selected = state.apertureSelected(), known = state.defaultsKnown();
            const auto defaults = state.defaultsValue();
            uint32_t v = 0xabcdef01;
            assert(state.access(eim, pc, address, size, write, v) == expected);
            assert(v == 0xabcdef01 && selected == state.apertureSelected());
            assert(known == state.defaultsKnown() && defaults == state.defaultsValue());
        };
        reject(0x200bac00, 0x16010000, 4, false,
               profile == Profile::strict ? Access::externalInput : Access::sequence);
        value = 0;
        assert(state.access(eim, 0x200bab4c, 0x16000010, 1, false, value) == Access::ok && value == 1);
        value = 0x0e;
        assert(state.access(eim, 0x200babf8, 0x1600001c, 1, true, value) == Access::ok);
        for (uint32_t address : {0x16010000u, 0x16010004u}) {
            const auto pc = address == 0x16010000 ? 0x200bac00 : 0x200bac28;
            for (uint32_t size : {0u, 1u, 2u, 8u}) reject(pc, address, size, false, Access::width);
            reject(pc + 4, address, 4, false, Access::externalInput);
            reject(pc, address, 4, true, Access::externalInput);
            if (profile == Profile::strict) reject(pc, address, 4, false, Access::externalInput);
            else {
                value = 0xffffffff;
                assert(state.access(eim, pc, address, 4, false, value) == Access::ok);
                if (address == 0x16010000) {
                    assert((value & 0xffff) == 0x1002 && (value >> 16) == 0x5744);
                } else assert(value == 0);
            }
        }
        if (profile == Profile::virtual5744Compatible8MiB) {
            auto read = [&](uint32_t pc, uint32_t address) {
                uint32_t v = 0xdeadbeef;
                assert(state.access(eim, pc, address, 4, false, v) == Access::ok);
                return v;
            };
            auto store = [&](uint32_t pc, uint32_t address, uint32_t v) {
                assert(state.access(eim, pc, address, 4, true, v) == Access::ok);
            };
            for (const auto address : {0x16010020u, 0x16010024u, 0x16010028u, 0x16010018u, 0x16010908u, 0x1601019cu}) {
                reject(0x200c2500, address, 2, false, Access::width);
                reject(0x1234, address, 4, false, Access::externalInput);
            }
            value = 0x3e01;
            assert(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
            reject(0x200c2500, 0x16010020, 4, false, Access::sequence);
            value = 0x3e00;
            assert(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
            assert(read(0x200c2500, 0x16010020) == 0);
            store(0x200c2534, 0x16010020, 0x3ff);
            assert(read(0x200c2500, 0x16010020) == 0x3ff);
            reject(0x200c2534, 0x16010020, 4, true, Access::sideEffect);
            assert(read(0x200c2500, 0x16010020) == 0x3ff);
            store(0x200c24e8, 0x16010908, 0x155);
            store(0x200c249c, 0x16010908, 0x55555);
            store(0x200c2450, 0x16010908, 0x15555555);
            assert(read(0x200c24b4, 0x16010908) == 0x15555555);
            assert(read(0x200c2468, 0x16010908) == 0x15555555);
            assert(read(0x200c241c, 0x16010908) == 0x15555555);
            reject(0x200c24e8, 0x16010908, 4, true, Access::sideEffect);
            assert(read(0x200c24b4, 0x16010908) == 0x15555555);
            store(0x200c67c4, 0x1601019c, 0x10011001);
            store(0x200c681c, 0x1601019c, 0x30213021);
            store(0x200c6874, 0x1601019c, 0x73217321);
            assert(read(0x200c6790, 0x1601019c) == 0x73217321);
            assert(read(0x200c67e8, 0x1601019c) == 0x73217321);
            assert(read(0x200c6840, 0x1601019c) == 0x73217321);
            reject(0x200c67c4, 0x1601019c, 4, true, Access::sideEffect);
            assert(read(0x200c6790, 0x1601019c) == 0x73217321);
            reject(0x200c6888, 0x1601019c, 4, true, Access::sequence);
            store(0x200c5098, 0x16010028, 0x280);
            store(0x200c50a8, 0x16010024, 0x20000);
            store(0x200c50bc, 0x16010018, 0xc000000);
            assert(read(0x200c508c, 0x16010028) == 0x280);
            assert(read(0x200c50a0, 0x16010024) == 0x20000);
            assert(read(0x200c50b0, 0x16010018) == 0xc000000);
            reject(0x200c50dc, 0x16000014, 1, true, Access::sideEffect);
            value = 0x11;
            assert(state.access(eim, 0x200c50dc, 0x16000014, 1, true, value) == Access::ok);
            reject(0x200c45e4, 0x16010080, 4, true, Access::sequence);
            state.reset();
            reject(0x200c2500, 0x16010020, 4, false, Access::sequence);
            value = 0;
            assert(state.access(eim, 0x200bab4c, 0x16000010, 1, false, value) == Access::ok);
            value = 0x0e;
            assert(state.access(eim, 0x200babf8, 0x1600001c, 1, true, value) == Access::ok);
            assert(read(0x200c2500, 0x16010020) == 0);
            assert(read(0x200c24b4, 0x16010908) == 0);
            assert(read(0x200c6790, 0x1601019c) == 0);
            assert(read(0x200c508c, 0x16010028) == 0);
            assert(read(0x200c50a0, 0x16010024) == 0);
            assert(read(0x200c50b0, 0x16010018) == 0);
        }
        state.reset();
        assert(!state.apertureSelected() && !state.defaultsKnown() && state.defaultsValue() == 0);
    }
    std::puts("PASS: strict and virtual identity/revision, native access guards, rejection preserves state and reset clears configuration");
}
