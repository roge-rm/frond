#include <cassert>
#include <cstdio>
#include "imx1_cs5_configuration.h"
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
    value = 0;
    assert(state.access(eim, 0x200bab4c, 0x16000010, 1, false, value) == Access::ok);
    value = 0xe;
    assert(state.access(eim, 0x200babf8, 0x1600001c, 1, true, value) == Access::ok);
    const uint32_t offsets[]{0x188, 0x190, 0x1bc, 0x1c0, 0x14, 0x10};
    const uint32_t settings[]{0x15ff1000, 0xffff8000, 0x7fff4000, 0x203f2000, 0x40000, 0x5ffff};
    const uint32_t writePcs[]{0x200c50f4, 0x200c5130, 0x200c5168, 0x200c51a0, 0x200c51d8, 0x200c51fc};
    const uint32_t readPcs[]{0x200c50fc, 0x200c5138, 0x200c5170, 0x200c51a8, 0x200c51e4, 0x200c5204};
    for (unsigned i = 0; i < 6; ++i) {
        const auto address = 0x16010000 + offsets[i];
        value = 0xdeadbeef;
        assert(state.access(eim, readPcs[i], address, 4, false, value) == Access::sequence);
        assert(value == 0xdeadbeef);
        assert(strict.access(eim, writePcs[i], address, 4, true, value) == Access::unmapped);
        value = settings[i];
        assert(state.access(eim, 0, address, 4, true, value) == Access::externalInput);
        assert(state.access(eim, writePcs[i], address, 2, true, value) == Access::width);
        value ^= 1;
        assert(state.access(eim, writePcs[i], address, 4, true, value) == Access::sideEffect);
        value = 0xdeadbeef;
        assert(state.access(eim, readPcs[i], address, 4, false, value) == Access::sequence);
        value = settings[i];
        assert(state.access(eim, writePcs[i], address, 4, true, value) == Access::ok);
        value = 0;
        assert(state.access(eim, readPcs[i], address, 4, false, value) == Access::ok && value == settings[i]);
    }
    const auto& windows = state.memoryWindows();
    assert(windows.internal().known && windows.internal().first == 0x100000 && windows.internal().last == 0x15ffff);
    assert(windows.external().known && windows.external().first == 0x800000 && windows.external().last == 0xffffff);
    assert(windows.aperture1().known && windows.aperture1().first == 0x400000 && windows.aperture1().last == 0x7fffff);
    assert(windows.aperture2().known && windows.aperture2().first == 0x200000 && windows.aperture2().last == 0x203fff);
    assert(windows.wrap().known && windows.wrap().first == 0x80000 && windows.wrap().last == 0xbffff);
    static_assert(Imx1GraphicsMemory::internalBytes == Imx1Cs5Configuration::virtualInternalBytes);
    static_assert(Imx1GraphicsMemory::externalBytes == Imx1Cs5Configuration::virtualExternalBytes);
    Imx1GraphicsMemory memory;
    configureGraphicsInterface(state, eim);
    configureGraphicsController(state, eim);
    value = 0xdeadbeef;
    assert(memory.access(state, eim, 0x1615fffc, 4, false, value) == Access::ok && value == 0);
    assert(memory.access(state, eim, 0x16fffffc, 4, false, value) == Access::ok && value == 0);
    for (auto base : {0x16100000u, 0x16800000u}) {
        value = 0x12345678;
        assert(memory.access(strict, eim, base, 4, true, value) == Access::sequence);
        assert(memory.access(state, eim, base + 1, 4, true, value) == Access::width);
        assert(memory.access(state, eim, base, 8, true, value) == Access::width);
        assert(memory.access(state, eim, base, 4, true, value) == Access::ok);
        for (unsigned i = 0; i < 4; ++i) {
            value = 0;
            assert(memory.access(state, eim, base + i, 1, false, value) == Access::ok);
            assert(value == ((0x12345678u >> (i * 8)) & 0xff));
        }
        value = 0xabcd;
        assert(memory.access(state, eim, base + 2, 2, true, value) == Access::ok);
        assert(memory.access(state, eim, base, 4, false, value) == Access::ok && value == 0xabcd5678);
    }
    uint32_t converted = 0xdeadbeef;
    assert(Imx1GraphicsMemory::cpuToGpu(0x16801234, converted) && converted == 0x801234);
    assert(Imx1GraphicsMemory::gpuToCpu(0x100080, converted) && converted == 0x16100080);
    assert(!Imx1GraphicsMemory::cpuToGpu(0x16080000, converted) && converted == 0x16100080);
    assert(!Imx1GraphicsMemory::gpuToCpu(0xfffffffe, converted) && converted == 0x16100080);
    value = 0x89abcdef;
    assert(memory.accessGpu(state, eim, 0x800080, 4, true, value) == Access::ok);
    assert(memory.access(state, eim, 0x16800080, 4, false, value) == Access::ok && value == 0x89abcdef);
    value = 0x1357;
    assert(memory.access(state, eim, 0x16100080, 2, true, value) == Access::ok);
    assert(memory.accessGpu(state, eim, 0x100080, 2, false, value) == Access::ok && value == 0x1357);
    value = 0x9876;
    assert(memory.access(state, eim, 0x1615fffe, 2, true, value) == Access::ok);
    assert(memory.access(state, eim, 0x16fffffe, 2, true, value) == Access::ok);
    assert(memory.dirty(0).first == 0 && memory.dirty(0).end == Imx1GraphicsMemory::internalBytes);
    assert(memory.dirty(1).first == 0 && memory.dirty(1).end == Imx1GraphicsMemory::externalBytes);
    for (auto address : {0x16080000u, 0x16200000u, 0x16400000u, 0x16160000u, 0x17000000u, 0xffffffffu}) {
        value = 0xdeadbeef;
        assert(memory.access(state, eim, address, 4, true, value) == Access::unmapped);
        assert(value == 0xdeadbeef);
    }
    memory.clearDirty(0);
    value = 0x78;
    assert(memory.access(state, eim, 0x16100020, 1, true, value) == Access::ok);
    assert(memory.dirty(0).pending && memory.dirty(0).first == 0x20 && memory.dirty(0).end == 0x21);
    value = 0;
    assert(state.access(eim, 0x200baf88, 0x16010250, 4, true, value) == Access::sequence);
    value = 0x401f0000;
    assert(state.access(eim, 0x200baf54, 0x16010240, 4, true, value) == Access::ok);
    value = 1;
    assert(state.access(eim, 0x200baf60, 0x16010244, 4, true, value) == Access::sideEffect);
    value = 0;
    assert(state.access(eim, 0x200baf60, 0x16010244, 4, true, value) == Access::ok);
    for (unsigned i = 0; i < 256; ++i) {
        value = i ^ 0x12345678;
        assert(state.access(eim, 0x200baf88, 0x16010250, 4, true, value) == Access::sequence);
        assert(state.access(eim, 0x200baf7c, 0x1601024c, 4, true, value) == Access::ok);
        assert(state.lookupEntry(i).firstKnown && !state.lookupEntry(i).secondKnown);
        assert(state.access(eim, 0x200baf7c, 0x1601024c, 4, true, value) == Access::sequence);
        value = ~value;
        assert(state.access(eim, 0x200baf88, 0x16010250, 4, true, value) == Access::ok);
        const auto entry = state.lookupEntry(i);
        assert(entry.first == (i ^ 0x12345678) && entry.second == value && entry.secondKnown);
    }
    assert(state.access(eim, 0x200baf7c, 0x1601024c, 4, true, value) == Access::sequence);
    assert(state.access(eim, 0x200baf7c, 0x1601024c, 4, false, value) == Access::externalInput);
    assert(state.access(eim, 0x200baf7c, 0x1601024c, 1, true, value) == Access::width);
    state.reset();
    value = 0xdeadbeef;
    assert(memory.access(state, eim, 0x16100020, 1, false, value) == Access::sequence && value == 0xdeadbeef);
    assert(memory.dirty(0).pending && memory.dirty(0).first == 0x20);
    assert(!state.lookupEntry(0).firstKnown && !state.lookupEntry(255).secondKnown);
    assert(!windows.internal().known && !windows.external().known && !windows.wrap().known);
    configureGraphicsInterface(state, eim);
    configureGraphicsController(state, eim);
    assert(memory.access(state, eim, 0x16100020, 1, false, value) == Access::ok && value == 0x78);
    assert(memory.access(state, eim, 0x16800000, 4, false, value) == Access::ok && value == 0xabcd5678);
    std::puts("PASS: graphics windows, bounded CPU/GPU storage, little-endian widths, dirty spans, guards, reset retention and lookup pairs");
}
