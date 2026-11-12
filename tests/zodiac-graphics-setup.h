#ifndef FROND_TEST_ZODIAC_GRAPHICS_SETUP_H
#define FROND_TEST_ZODIAC_GRAPHICS_SETUP_H

#include <cassert>
#include <initializer_list>
#include "imx1_cs5_configuration.h"

inline void configureGraphicsInterface(Imx1Cs5Configuration& state, const Imx1Eim& eim) {
    struct Write { uint32_t pc, address, size, value; };
    for (auto write : {Write{0x200bab4c, 0x16000010, 1, 0},
                       {0x200babf8, 0x1600001c, 1, 0xe},
                       {0x200c50bc, 0x16010018, 4, 0xc000000},
                       {0x200c50dc, 0x16000014, 1, 0x11},
                       {0x200c50f4, 0x16010188, 4, 0x15ff1000},
                       {0x200c5130, 0x16010190, 4, 0xffff8000}}) {
        assert(state.access(eim, write.pc, write.address, write.size,
                            write.address != 0x16000010, write.value) == Imx1BootstrapAccess::ok);
    }
}

inline void configureGraphicsExternal(Imx1Cs5Configuration& state, const Imx1Eim& eim) {
    struct Operation { uint32_t pc, offset, value; bool write; };
    for (auto op : {Operation{0x200c4114, 0x1b8, 0x0, false},
                    {0x200c4130, 0x1b0, 0x0, false},
                    {0x200c413c, 0x1b0, 0xff0000, true},
                    {0x200c4144, 0x194, 0x0, false},
                    {0x200c4150, 0x194, 0xff000000, true},
                    {0x200c4158, 0x18c, 0x0, false},
                    {0x200c4164, 0x18c, 0x80000, true},
                    {0x200c4178, 0x18c, 0x80000, false},
                    {0x200c4188, 0x18c, 0x70080000, true},
                    {0x200c4190, 0x18c, 0x70080000, false},
                    {0x200c4198, 0x18c, 0x70080001, true},
                    {0x200c41c4, 0x1b0, 0xff0000, false},
                    {0x200c41d4, 0x1b0, 0x0, true},
                    {0x200c41e8, 0x194, 0xff000000, false},
                    {0x200c4220, 0x194, 0x15001545, true},
                    {0x200c4234, 0x18c, 0x70080001, false},
                    {0x200c423c, 0x18c, 0x70000001, true},
                    {0x200c4250, 0x198, 0x0, false},
                    {0x200c4264, 0x18c, 0x70000001, false},
                    {0x200c42c0, 0x18c, 0x7000000d, true},
                    {0x200c42d4, 0x18c, 0x7000000d, false},
                    {0x200c42e4, 0x18c, 0x7964000d, true},
                    {0x200c42f8, 0x18c, 0x7964000d, false},
                    {0x200c4304, 0x18c, 0x964000d, true},
                    {0x200c4318, 0x18c, 0x964000d, false},
                    {0x200c4320, 0x18c, 0x964000d, true},
                    {0x200c4334, 0x198, 0x1600021, true},
                    {0x200c434c, 0x198, 0x81600021, true},
                    {0x200c4364, 0x198, 0x1600021, true}}) {
        uint32_t value = op.write ? op.value : 0xdeadbeef;
        const uint32_t saved = value;
        assert(state.access(eim, 0, 0x16010000 + op.offset, 4, op.write, value) == Imx1BootstrapAccess::externalInput);
        assert(state.access(eim, op.pc, 0x16010000 + op.offset, 2, op.write, value) == Imx1BootstrapAccess::width);
        assert(value == saved);
        if (op.write) {
            value ^= 0x10000;
            assert(state.access(eim, op.pc, 0x16010000 + op.offset, 4, true, value) == Imx1BootstrapAccess::sideEffect);
            value = op.value;
        }
        assert(state.access(eim, op.pc, 0x16010000 + op.offset, 4, op.write, value) == Imx1BootstrapAccess::ok);
        assert(value == op.value);
    }
}

inline void configureGraphicsController(Imx1Cs5Configuration& state, const Imx1Eim& eim) {
    uint32_t value = 0x7800;
    assert(state.access(eim, 0x200b31b0, 0x160101a4, 4, true, value) == Imx1BootstrapAccess::ok);
    value = 0x100007;
    assert(state.access(eim, 0x200b31bc, 0x16010180, 4, true, value) == Imx1BootstrapAccess::ok);
    configureGraphicsExternal(state, eim);
    state.advanceCpuCycles(32);
    value = 0x7bbb7444;
    assert(state.access(eim, 0x200c6888, 0x1601019c, 4, true, value) == Imx1BootstrapAccess::ok);
}
#endif
