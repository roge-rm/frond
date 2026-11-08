#include <cassert>
#include <cstdio>
#include "imx1_graphics_memory.h"
#include "zodiac-graphics-setup.h"

int main() {
    using Access = Imx1BootstrapAccess;
    Imx1Eim eim;
    uint32_t value = 0x3e00;
    assert(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
    value = 0x11111e01;
    assert(eim.access(0x0022002c, 4, true, value) == Imx1EimAccess::ok);
    Imx1Cs5Configuration state(Imx1GraphicsProfile::virtual5744Compatible8MiB), strict;
    Imx1GraphicsMemory memory;
    auto blocked = [&] {
        value = 0xdeadbeef;
        assert(!state.memoryReady(eim));
        assert(memory.access(state, eim, 0x16800000, 4, true, value) == Access::sequence);
        assert(!memory.dirty(1).pending && value == 0xdeadbeef);
    };
    auto store = [&](uint32_t pc, uint32_t address, uint32_t word, Access expected) {
        value = word;
        const auto actual=state.access(eim,pc,address,4,true,value);
        if (actual!=expected) std::fprintf(stderr,"MC pc=%08x address=%08x value=%08x actual=%u expected=%u\n",pc,address,word,unsigned(actual),unsigned(expected));
        assert(actual==expected);
        assert(value == word);
    };
    blocked();
    store(0x200b31b0, 0x160101a4, 0x7800, Access::sequence);
    store(0x200acb8c, 0x160101a4, 0x7800, Access::sequence);
    configureGraphicsInterface(state, eim);
    blocked();
    store(0x200b31bc, 0x16010180, 0x100007, Access::sequence);
    store(0x200c6888, 0x1601019c, 0x7bbb7444, Access::sequence);
    for (auto address : {0x160101a4u, 0x16010180u}) {
        value = 0xdeadbeef;
        assert(strict.access(eim, 0x200b31b0, address, 4, true, value) == Access::unmapped);
        assert(state.access(eim, 0, address, 4, true, value) == Access::externalInput);
        assert(state.access(eim, 0x200b31b0, address, 2, true, value) == Access::width);
        assert(state.access(eim, 0x200b31b0, address, 4, false, value) == Access::externalInput);
        assert(value == 0xdeadbeef);
    }
    store(0x200b31b0, 0x160101a4, 0x7801, Access::sideEffect);
    blocked();
    store(0x200b31b0, 0x160101a4, 0x7800, Access::ok);
    blocked();
    store(0x200b31bc, 0x16010180, 0x100006, Access::sideEffect);
    blocked();
    store(0x200b31bc, 0x16010180, 0x100007, Access::ok);
    blocked();
    value = 0xdeadbeef;
    assert(state.access(eim, 0x200c4380, 0x160101b8, 4, false, value) == Access::sequence && value == 0xdeadbeef);
    assert(state.access(eim, 0x200c4114, 0x160101b8, 4, true, value) == Access::externalInput && value == 0xdeadbeef);
    configureGraphicsExternal(state, eim);
    value = 0xdeadbeef;
    assert(state.access(eim, 0x200c4380, 0x160101b8, 4, false, value) == Access::ok && value == 0);
    state.advanceCpuCycles(31);
    value = 0xdeadbeef;
    assert(state.access(eim, 0x200c4380, 0x160101b8, 4, false, value) == Access::ok && value == 0);
    state.advanceCpuCycles(1);
    assert(state.access(eim, 0x200c4380, 0x160101b8, 4, false, value) == Access::ok && value == 1);
    store(0x200c6888, 0x1601019c, 0x7bbb7445, Access::sideEffect);
    value = 0;
    assert(state.access(eim, 0x200c6790, 0x1601019c, 4, false, value) == Access::ok && value == 0);
    blocked();
    store(0x200c6888, 0x1601019c, 0x7bbb7444, Access::ok);
    assert(state.memoryReady(eim));
    value = 0x12345678;
    assert(memory.access(state, eim, 0x16800000, 4, true, value) == Access::ok);
    assert(memory.dirty(1).pending);
    store(0x200acb8c, 0x160101a4, 0x7801, Access::sideEffect);
    store(0x200acb8c, 0x160101a4, 0x7800, Access::ok);
    assert(state.memoryReady(eim) && memory.dirty(1).pending);
    value = 0;
    assert(memory.access(state, eim, 0x16800000, 4, false, value) == Access::ok && value == 0x12345678);
    value = 0;
    assert(state.access(eim, 0x200c6840, 0x1601019c, 4, false, value) == Access::ok && value == 0x7bbb7444);
    store(0x200c5f20,0x16010194,0x1545,Access::ok);
    const auto savedLegacy=memory.dirty(1);
    const auto savedScanout=memory.dirty(1,Imx1GraphicsMemory::Consumer::scanout);
    const auto savedUpload=memory.dirty(1,Imx1GraphicsMemory::Consumer::upload);
    const auto savedPersistence=memory.dirty(1,Imx1GraphicsMemory::Consumer::persistence);
    struct Repeat { uint32_t pc,offset,value; bool write; };
    for (auto op : {Repeat{0x200c41c4,0x1b0,0,false}, {0x200c41d4,0x1b0,0,true},
            {0x200c41e8,0x194,0x1545,false},{0x200c4220,0x194,0x22001545,true},
            {0x200c4234,0x18c,0x0964000d,false},{0x200c423c,0x18c,0x0964000d,true},
            {0x200c4250,0x198,0x01600021,false},{0x200c4264,0x18c,0x0964000d,false},
            {0x200c42c0,0x18c,0x0964000d,true},{0x200c42d4,0x18c,0x0964000d,false},
            {0x200c42e4,0x18c,0x0964000d,true},{0x200c42f8,0x18c,0x0964000d,false},
            {0x200c4304,0x18c,0x0964000d,true},{0x200c4318,0x18c,0x0964000d,false},
            {0x200c4320,0x18c,0x0964000d,true},{0x200c4334,0x198,0x01600021,true},
            {0x200c434c,0x198,0x81600021,true},{0x200c4364,0x198,0x01600021,true}}) {
        if (op.pc==0x200c4220) {
            assert(state.memoryReady(eim));
            value=op.value^1;
            assert(state.access(eim,op.pc,0x16010000+op.offset,4,true,value)==Access::sideEffect);
            assert(state.memoryReady(eim));
            value=0;assert(memory.access(state,eim,0x16800000,4,false,value)==Access::ok && value==0x12345678);
        }
        value=op.write?op.value:0xdeadbeef;
        assert(state.access(eim,op.pc,0x16010000+op.offset,4,op.write,value)==Access::ok && value==op.value);
    }
    assert(memory.dirty(1).generation==savedLegacy.generation);
    assert(memory.dirty(1,Imx1GraphicsMemory::Consumer::scanout).generation==savedScanout.generation);
    assert(memory.dirty(1,Imx1GraphicsMemory::Consumer::upload).generation==savedUpload.generation);
    assert(memory.dirty(1,Imx1GraphicsMemory::Consumer::persistence).generation==savedPersistence.generation);
    assert(!state.memoryReady(eim));
    state.advanceCpuCycles(31);assert(!state.memoryReady(eim));
    state.advanceCpuCycles(1);assert(state.memoryReady(eim));
    value=0;assert(memory.access(state,eim,0x16800000,4,false,value)==Access::ok && value==0x12345678);
    store(0x200c6888, 0x1601019c, 0x7bbb7444, Access::sequence);
    store(0x200c67c4, 0x1601019c, 0x7bbb7445, Access::ok);
    assert(!state.memoryReady(eim));
    value = 0xdeadbeef;
    assert(memory.access(state, eim, 0x16800000, 4, false, value) == Access::sequence && value == 0xdeadbeef);
    store(0x200c67c4, 0x1601019c, 0x7bbb7444, Access::ok);
    value = 0x3e01;
    assert(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
    assert(!state.memoryReady(eim));
    store(0x200b31b0, 0x160101a4, 0x7800, Access::sequence);
    value = 0x3e00;
    assert(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
    assert(state.memoryReady(eim));
    state.reset();
    assert(!state.memoryReady(eim));
    assert(memory.dirty(1).pending);
    configureGraphicsInterface(state, eim);
    configureGraphicsController(state, eim);
    value = 0;
    assert(memory.access(state, eim, 0x16800000, 4, false, value) == Access::ok && value == 0x12345678);
    std::puts("PASS: native MC sequence gates shared VRAM, strict/width/caller/value/order guards, IO readback and reset retention");
}
