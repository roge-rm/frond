#include <cassert>
#include <cstdio>
#include "zodiac-graphics-setup.h"

int main() {
    using A = Imx1BootstrapAccess;
    Imx1Eim eim;
    uint32_t value = 0x3e00;
    assert(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
    value = 0x11111e01;
    assert(eim.access(0x0022002c, 4, true, value) == Imx1EimAccess::ok);
    Imx1Cs5Configuration state(Imx1GraphicsProfile::virtual5744Compatible8MiB), strict;
    value = 0xdeadbeef;
    assert(state.access(eim, 0x200acb98, 0x160113e8, 4, false, value) == A::sequence);
    assert(value == 0xdeadbeef && !state.engineState().setupComplete());
    assert(strict.access(eim, 0x200acb98, 0x160113e8, 4, false, value) == A::unmapped);
    configureGraphicsInterface(state, eim);
    configureGraphicsController(state, eim);
    struct Operation { uint32_t pc, address, value; bool write; };
    for (auto op : {Operation{0x200acb98, 0x160113e8, 0, false},
            {0x200acba8, 0x160113e8, 0x400, true}, {0x200acbb4, 0x16010144, 0x2440, true},
            {0x200acbc4, 0x160110a8, 0x1fff1fff, true}, {0x200acbd0, 0x160104dc, 0xc0000, true},
            {0x200b8e80, 0x16010214, 0x10150000, true}, {0x200b8e88, 0x16010220, 0, true},
            {0x200b8f04, 0x16010008, 0x04150000, true},
            {0x200b8f10, 0x16010230, 0x80000000, true}, {0x200b8f20, 0x16010210, 0x0800000d, true},
            {0x200b02ac, 0x16011280, 0, false}, {0x200b02fc, 0x16011280, 0x10000, true}}) {
        value = op.write ? op.value : 0xdeadbeef;
        auto before = value;
        assert(state.access(eim, 0, op.address, 4, op.write, value) != A::ok);
        assert(value == before);
        assert(state.access(eim, op.pc, op.address, 2, op.write, value) == A::width);
        assert(value == before);
        if (op.write) {
            value ^= 1;
            assert(state.access(eim, op.pc, op.address, 4, true, value) != A::ok);
            value = op.value;
        }
        if (op.address == 0x16010214) {
            auto unalignedRing = state;
            value = 0x10150004;
            assert(unalignedRing.access(eim, op.pc, op.address, 4, true, value) == A::ok);
            value = 0;
            assert(unalignedRing.access(eim, 0x200b8e88, 0x16010220, 4, true, value) == A::ok);
            value = 0x04150004;
            assert(unalignedRing.access(eim, 0x200b8f04, 0x16010008, 4, true, value) == A::sideEffect);
            assert(unalignedRing.engineState().ringBytes() == 0 && !unalignedRing.engineState().emptyRing());
            for (auto rejected : {0x1015fffcu, 0x10800001u, 0x00100001u, 0x20150000u, 0x00080000u}) {
                value = rejected;
                assert(state.access(eim, op.pc, op.address, 4, true, value) == A::sideEffect);
                assert(value == rejected && state.engineState().ringBaseWord() == 0);
            }
            value = op.value;
        }
        if (op.address == 0x16010008) {
            value = 0x05150000;
            assert(state.access(eim, op.pc, op.address, 4, true, value) == A::sideEffect);
            assert(state.engineState().ringBytes() == 0);
            value = op.value;
        }
        assert(state.access(eim, op.pc, op.address, 4, op.write, value) == A::ok);
        assert(value == op.value);
    }
    assert(state.engineState().setupComplete());
    assert(state.engineState().clipContains(8191, 8191));
    assert(!state.engineState().clipContains(8192, 0));
    assert(!state.engineState().clipContains(0, 8192));
    assert(state.engineState().emptyRing());
    assert(state.engineState().ringAddress() == 0x150000 && state.engineState().ringBytes() == 65536);
    assert(state.engineState().ringBaseWord() == 0x10150000);
    value = 0;
    assert(state.access(eim, 0x200b02fc, 0x16011280, 4, true, value) == A::sequence);
    assert(state.access(eim, 0x200b02ac, 0x16011280, 4, false, value) == A::ok && value == 0x10000);
    value = 0;
    assert(state.access(eim, 0x200b02fc, 0x16011280, 4, true, value) == A::sideEffect);
    assert(state.engineState().nativeOptionWord() == 0x10000);
    value = 0x78;
    assert(state.access(eim, 0x200aeb68, 0x160104d8, 4, true, value) == A::ok);
    assert(!state.displayState().lcdEnabled());
    value = 1;
    assert(state.access(eim, 0x200b8e88, 0x16010220, 4, true, value) == A::sequence);
    assert(state.engineState().emptyRing());
    assert(state.access(eim, 0, 0x16010218, 4, true, value) == A::unmapped);
    assert(state.access(eim, 0, 0x16010140, 4, false, value) == A::unmapped);
    value = 0x3e01;
    assert(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
    value = 0xdeadbeef;
    assert(state.access(eim, 0x200b02ac, 0x16011280, 4, false, value) == A::sequence && value == 0xdeadbeef);
    assert(state.engineState().nativeOptionWord() == 0x10000 && state.engineState().emptyRing());
    value = 0x3e00;
    assert(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
    state.reset();
    assert(!state.engineState().setupComplete() && !state.engineState().emptyRing());
    assert(state.engineState().ringAddress() == 0 && state.engineState().ringBytes() == 0);
    assert(state.engineState().nativeOptionWord() == 0);
    std::puts("PASS: native engine setup, clipping, bounded empty ring, guards and reset");
}
