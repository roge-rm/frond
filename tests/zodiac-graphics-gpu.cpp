// The host GPU path of the Zodiac graphics engine against the CPU model: every fill kind the GPU takes, sources in
// the same bank and the other one, CPU accesses between fills onto pages the GPU holds newer, and the
// differential mode. Exits 77 where the host has no usable GPU.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "zodiac-graphics-setup.h"
#include "imx1_graphics_commands.h"
#include "imx1_graphics_gpu_gles.h"

using A = Imx1BootstrapAccess;
using C = Imx1GraphicsCommands;

static Imx1Eim eim;
static Imx1Cs5Configuration* state;
static uint64_t r = 0x9e3779b97f4a7c15ull;
static uint32_t next(uint32_t n) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; return uint32_t(r % n); }
static const uint16_t palette[4] = {0x0000, 0xffff, 0xf800, 0x07e0};

static std::unique_ptr<Imx1GraphicsGpu> makeGpu(bool differential) {
    std::string error;
    const uint32_t banks[2] = {Imx1GraphicsMemory::internalBytes, Imx1GraphicsMemory::externalBytes};
    auto backend = imx1MakeGraphicsGpuBackend(banks, error);
    if (!backend) {
        std::printf("SKIP: no GPU (%s)\n", error.c_str());
        std::exit(77);
    }
    return std::make_unique<Imx1GraphicsGpu>(std::move(backend), differential);
}

// Both memories, every byte of each region, read the CPU's way (so GPU-newer pages come back).
struct Region { uint32_t base, bytes; };
static void compareRegions(Imx1GraphicsMemory& gpu, Imx1GraphicsMemory& cpu, const std::vector<Region>& regions,
                           const char* what, unsigned kind, unsigned round) {
    for (const auto& region : regions)
        for (uint32_t i = 0; i < region.bytes; i += 2) {
            uint32_t a = 0, b = 0;
            assert(gpu.accessGpu(*state, eim, region.base + i, 2, false, a) == A::ok);
            assert(cpu.accessGpu(*state, eim, region.base + i, 2, false, b) == A::ok);
            if (a != b) {
                std::fprintf(stderr, "FAIL: %s kind %u round %u differs at %x: gpu %04x, cpu %04x\n", what, kind, round,
                             region.base + i, a, b);
                std::exit(1);
            }
        }
}

// Random fills of every kind on two memories, one with the GPU attached. Destinations in bank 0 or bank 1;
// sources in either, a third of them around the destination (those stay on the CPU). Between fills, CPU writes and
// reads land on random pages, some of them ones the GPU holds newer. Every fourth fill the regions are compared.
static void checkFills() {
    static Imx1GraphicsMemory gpuMemory, cpuMemory;
    auto gpu = makeGpu(false);
    std::printf("renderer: %s\n", gpu->renderer().c_str());
    const std::vector<Region> regions = {{0x100000, 0x6000}, {0x900000, 0x6000}};
    for (const auto& region : regions)
        for (uint32_t i = 0; i < region.bytes; i += 2) {
            uint32_t word = next(3) ? palette[next(4)] : next(0x10000);
            assert(gpuMemory.accessGpu(*state, eim, region.base + i, 2, true, word) == A::ok);
            assert(cpuMemory.accessGpu(*state, eim, region.base + i, 2, true, word) == A::ok);
        }
    gpu->attach(&gpuMemory);
    unsigned fills = 0, kinds = 0, cpuFills = 0;
    for (unsigned kind = 0; kind < 128; ++kind) {
        const bool pixels = kind & 1, copy = kind & 2, keyed = copy && (kind & 4), doubled = copy && (kind & 8);
        const bool invert = kind & 16, a4 = kind & 32, painted = keyed && (kind & 64);
        if ((!copy && (kind & (4 | 8 | 64))) || (a4 && doubled) || (pixels && copy)) continue;
        ++kinds;
        for (unsigned round = 0; round < 48; ++round) {
            const auto& region = regions[next(2)];
            C::Fill f{};
            f.width = 1 + next(80);
            f.height = 1 + next(24);
            // Some fills are one long row or have pitches across texture rows; some rows overlap (CPU).
            f.pitch = next(8) ? f.width * 2 + next(600) * 2 : next(2) ? 4096 + next(64) * 2 : 2 + next(f.width) * 2;
            const uint64_t span = uint64_t(f.height - 1) * f.pitch + f.width * 2;
            if (span + 128 > region.bytes) f.height = 1;
            f.base = region.base + 64 + next((region.bytes - 64 - (f.height - 1) * f.pitch - f.width * 2) / 2) * 2;
            f.keyed = keyed; f.plain = copy && !keyed; f.a4 = a4; f.invert = invert; f.doubled = doubled;
            f.painted = painted; f.paint = next(0x10000); f.keyEqual = next(2);
            f.colour = a4 ? next(17) : keyed ? palette[next(4)] : next(4) ? next(0x10000) : 0x10000 | next(0x10000);
            if (copy || a4) {
                const auto& from = round % 3 == 2 ? region : regions[next(2)];
                f.sourcePitch = f.width * 2 + next(30) * 2;
                f.sourceX = doubled ? next(5) : 0; f.sourceY = doubled ? next(5) : 0;
                const uint32_t sourceSpan = (f.sourceY + f.height) * f.sourcePitch + (f.sourceX + f.width) * 2 + 8;
                f.sourceBase = round % 3 == 2 && f.base >= region.base + 64 ? f.base - 64 + next(64) * 2 :
                    from.base + next((from.bytes - sourceSpan) / 2) * 2;
            }
            if (pixels || (a4 && next(2)))
                for (uint32_t i = 0; i < f.width * f.height; ++i)
                    f.pixels.push_back(uint16_t(a4 ? (next(3) ? next(2) * 15 : next(16)) : next(0x10000)));
            const uint64_t before = gpu->statistics().fills;
            assert(C::completeFill(*state, eim, gpuMemory, f) == A::ok);
            assert(C::completeFillCpu(*state, eim, cpuMemory, f) == A::ok);
            if (gpu->statistics().fills == before) ++cpuFills;
            // A few CPU writes and reads, anywhere in the regions.
            for (unsigned i = next(4); i; --i) {
                const auto& at = regions[next(2)];
                const uint32_t address = at.base + next(at.bytes / 2) * 2;
                uint32_t word = next(0x10000), a = 0, b = 0;
                if (next(2)) {
                    assert(gpuMemory.accessGpu(*state, eim, address, 2, true, word) == A::ok);
                    assert(cpuMemory.accessGpu(*state, eim, address, 2, true, word) == A::ok);
                } else {
                    assert(gpuMemory.accessGpu(*state, eim, address, 2, false, a) == A::ok);
                    assert(cpuMemory.accessGpu(*state, eim, address, 2, false, b) == A::ok);
                    if (a != b) {
                        std::fprintf(stderr, "FAIL: read kind %u round %u at %x: gpu %04x, cpu %04x\n", kind, round,
                                     address, a, b);
                        std::exit(1);
                    }
                }
            }
            if (round % 4 == 3) compareRegions(gpuMemory, cpuMemory, regions, "fill", kind, round);
            ++fills;
        }
    }
    compareRegions(gpuMemory, cpuMemory, regions, "end", 0, 0);
    // The whole banks, as a state save or hash reads them.
    for (unsigned bank = 0; bank < 2; ++bank) {
        uint32_t a = 0, b = 0;
        const uint8_t* x = gpuMemory.diagnosticData(bank, a);
        const uint8_t* y = cpuMemory.diagnosticData(bank, b);
        assert(a == b && !std::memcmp(x, y, a));
    }
    const auto& s = gpu->statistics();
    gpu->report(stdout);
    assert(s.solid && s.inlinePixels && s.copies && s.keyed && s.doubled && s.inverts && s.a4 && s.downloads && s.uploads);
    assert(cpuFills > 0 && cpuFills < fills / 2);
    gpu->detach();
    std::printf("PASS: %u fills of %u kinds, %llu on the GPU and %u on the CPU, match the CPU model\n", fills, kinds,
                (unsigned long long)s.fills, cpuFills);
}

// Differential mode: fills checked one at a time against the shadow copy, no mismatches, and CPU writes reach the
// shadow too.
static void checkDifferential() {
    static Imx1GraphicsMemory memory;
    auto gpu = makeGpu(true);
    for (uint32_t i = 0; i < 0x4000; i += 2) {
        uint32_t word = next(0x10000);
        assert(memory.accessGpu(*state, eim, 0x100000 + i, 2, true, word) == A::ok);
    }
    gpu->attach(&memory);
    for (unsigned round = 0; round < 400; ++round) {
        C::Fill f{};
        f.width = 1 + next(40);
        f.height = 1 + next(20);
        f.pitch = f.width * 2 + next(40) * 2;
        f.base = 0x100000 + 0x2000 + next(0x400) * 2;
        f.colour = next(0x10000);
        f.plain = next(2);
        if (f.plain) {
            f.sourcePitch = f.width * 2;
            f.sourceBase = 0x100000 + next(0x200) * 2;
        }
        assert(C::completeFill(*state, eim, memory, f) == A::ok);
        uint32_t word = next(0x10000);
        assert(memory.accessGpu(*state, eim, 0x100000 + next(0x1000) * 2, 2, true, word) == A::ok);
    }
    const auto& s = gpu->statistics();
    assert(s.compared == s.fills && s.fills > 300 && !s.mismatches);
    uint32_t size = 0;
    const uint8_t* shadow = gpu->shadowMemory()->rawBank(0, size);
    const uint8_t* bytes = memory.diagnosticData(0, size);
    assert(!std::memcmp(shadow, bytes, size));
    gpu->detach();
    std::printf("PASS: differential mode compared %llu fills, no mismatches\n", (unsigned long long)s.compared);
}

int main() {
    uint32_t value = 0x3e00;
    assert(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
    value = 0x11111e01;
    assert(eim.access(0x0022002c, 4, true, value) == Imx1EimAccess::ok);
    static Imx1Cs5Configuration configuration(Imx1GraphicsProfile::virtual5744Compatible8MiB, false, false);
    state = &configuration;
    configureGraphicsInterface(configuration, eim);
    configureGraphicsController(configuration, eim);
    static Imx1GraphicsMemory probe;
    assert(probe.contains(configuration, eim, 0x100000, 0x6000) && probe.contains(configuration, eim, 0x900000, 0x6000));
    checkFills();
    checkDifferential();
}
