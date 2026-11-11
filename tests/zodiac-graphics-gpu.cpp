// The host GPU path of the Zodiac graphics engine against the CPU model: every fill kind the GPU takes, sources in
// the same bank and the other one, CPU accesses of every width between fills onto pages the GPU holds newer
// (masked writes and their patches), the differential mode and the recovery from an injected GL error. Exits 77
// where the host has no usable GPU. With --bench FRAMES [SIZE [a4]] it times a fill-heavy frame loop with the GPU and without.
#include <chrono>
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
    auto gpu = std::make_unique<Imx1GraphicsGpu>(std::move(backend), differential);
    gpu->minPixels = 0;
    return gpu;
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
                std::fflush(stdout);
                std::_Exit(1);  // The GPU thread is still running; skip the exit handlers.
            }
        }
}

// Random fills of every kind on two memories, one with the GPU attached. Destinations in bank 0 or bank 1;
// sources in either, a third of them around the destination (those stay on the CPU). Between fills, CPU writes and
// reads land on random pages, some of them ones the GPU holds newer. Every fourth fill the regions are compared.
// With minPixels, small fills on bytes the GPU hasn't drawn go to the CPU, and the rest still match.
static void checkFills(uint64_t minPixels) {
    static Imx1GraphicsMemory memories[2][2];
    Imx1GraphicsMemory& gpuMemory = memories[minPixels != 0][0];
    Imx1GraphicsMemory& cpuMemory = memories[minPixels != 0][1];
    auto gpu = makeGpu(false);
    gpu->minPixels = minPixels;
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
            // The saved dirty ranges and write counts match the CPU model's, keyed fills included.
            for (unsigned bank = 0; bank < 2; ++bank) {
                const auto a = gpuMemory.dirty(bank), b = cpuMemory.dirty(bank);
                if (a.first != b.first || a.end != b.end || a.pending != b.pending || a.generation != b.generation) {
                    std::fprintf(stderr, "FAIL: dirty state kind %u round %u bank %u: gpu %x-%x gen %llu, cpu %x-%x gen %llu\n",
                                 kind, round, bank, a.first, a.end, (unsigned long long)a.generation, b.first, b.end,
                                 (unsigned long long)b.generation);
                    std::fflush(stdout);
                    std::_Exit(1);
                }
            }
            // A few CPU writes and reads, anywhere in the regions.
            for (unsigned i = next(6); i; --i) {
                const auto& at = regions[next(2)];
                const uint32_t size = 1u << next(3);
                // Half of them next to the last fill, where the GPU most likely holds the page newer.
                const uint32_t address = next(2) ? at.base + next(at.bytes / size) * size :
                    std::min(f.base + next(64) * 4, region.base + region.bytes - 4) & ~(size - 1);
                uint32_t word = next(0x10000) | next(0x10000) << 16, a = 0, b = 0;
                if (next(3)) {
                    assert(gpuMemory.accessGpu(*state, eim, address, size, true, word) == A::ok);
                    assert(cpuMemory.accessGpu(*state, eim, address, size, true, word) == A::ok);
                } else {
                    assert(gpuMemory.accessGpu(*state, eim, address, size, false, a) == A::ok);
                    assert(cpuMemory.accessGpu(*state, eim, address, size, false, b) == A::ok);
                    if (a != b) {
                        std::fprintf(stderr, "FAIL: read kind %u round %u at %x: gpu %04x, cpu %04x\n", kind, round,
                                     address, a, b);
                        std::fflush(stdout);
                        std::_Exit(1);
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
    assert(s.maskedWrites && s.patches && s.patchPages && !s.recoveries);
    assert(minPixels ? gpu->smallFills > 50 : !gpu->smallFills);
    assert(cpuFills > 0 && cpuFills - gpu->smallFills < fills / 2);
    gpu->detach();
    std::printf("PASS: %u fills of %u kinds, %llu on the GPU and %u on the CPU (%llu small, under %llu pixels), match the "
                "CPU model\n", fills, kinds, (unsigned long long)s.fills, cpuFills, (unsigned long long)gpu->smallFills,
                (unsigned long long)minPixels);
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

// A GL error injected into a held batch of fills, patches and uploads: the commands since the last confirm run
// again on the CPU, every page comes back and later fills stay on the CPU, all matching the CPU model. Each round
// confirms a few batches first, so the replica holds read-back pages and uploads when the error comes.
static void checkRecovery() {
    for (unsigned round = 0; round < 6; ++round) {
        static Imx1GraphicsMemory gpuMemory, cpuMemory;
        auto gpu = makeGpu(false);
        const std::vector<Region> regions = {{0x100000, 0x6000}, {0x900000, 0x6000}};
        for (const auto& region : regions)
            for (uint32_t i = 0; i < region.bytes; i += 2) {
                uint32_t word = next(0x10000);
                assert(gpuMemory.accessGpu(*state, eim, region.base + i, 2, true, word) == A::ok);
                assert(cpuMemory.accessGpu(*state, eim, region.base + i, 2, true, word) == A::ok);
            }
        gpu->attach(&gpuMemory);
        auto& backend = gpu->gpuBackend();
        const unsigned before = 2 + round, held = 6 + round * 3;
        for (unsigned n = 0; n < before + held + 8; ++n) {
            if (n == before) {
                // Let the confirmed part land, then hold the rest back as one batch with an error in it.
                backend.drain();
                backend.hold(true);
            }
            const auto& region = regions[next(2)];
            C::Fill f{};
            f.width = 1 + next(60);
            f.height = 1 + next(16);
            f.pitch = f.width * 2 + next(200) * 2;
            f.base = region.base + 64 + next((region.bytes - 128 - (f.height - 1) * f.pitch - f.width * 2) / 2) * 2;
            f.colour = next(0x10000);
            // A keyed fill reads its source on the CPU, which would wait on the held commands.
            const unsigned drawn = next(4), kind = drawn == 2 && n >= before && n < before + held ? 1 : drawn;
            f.plain = kind == 1;
            f.keyed = kind == 2;
            f.invert = kind == 3;
            if (f.plain || f.keyed) {
                const auto& from = regions[f.base >= 0x900000 ? 0 : 1];
                f.sourcePitch = f.width * 2;
                f.sourceBase = from.base + next((from.bytes - f.height * f.sourcePitch - 8) / 2) * 2;
                if (f.keyed) f.colour = 0;
            }
            assert(C::completeFill(*state, eim, gpuMemory, f) == A::ok);
            assert(C::completeFillCpu(*state, eim, cpuMemory, f) == A::ok);
            for (unsigned i = next(4); i; --i) {
                const uint32_t size = 1u << next(3);
                const uint32_t address = (f.base + next(f.height * f.pitch / 4) * 4) & ~(size - 1);
                uint32_t word = next(0x10000) | next(0x10000) << 16;
                assert(gpuMemory.accessGpu(*state, eim, address, size, true, word) == A::ok);
                assert(cpuMemory.accessGpu(*state, eim, address, size, true, word) == A::ok);
            }
            if (n == before + held / 2) backend.injectError(backend.confirmed() + 1 + next(held / 2));
            if (n == before + held - 1) {
                backend.hold(false);
                backend.drain();
            }
        }
        compareRegions(gpuMemory, cpuMemory, regions, "recovery", 0, round);
        const auto& s = gpu->statistics();
        assert(s.recoveries == 1 && s.replayed >= held / 2 && !backend.healthy());
        assert(gpu->cpuFills >= 8);
        gpu->report(stdout);
        gpu->detach();
    }
    std::printf("PASS: injected GL errors recovered, replayed commands match the CPU model\n");
}

// A fill-heavy frame loop on a double-buffered 480x320 surface, as a game might draw it: clear, then 64 sprites of
// size by size pixels (keyed copies, or A4 blends with a4 set) from a sheet in the other bank, a few CPU writes
// onto the frame, and the display reading the other buffer, the frame before. Timed with the GPU attached and
// without; prints frames and fills per second and the GPU's counts.
static void bench(unsigned frames, uint32_t size, bool a4) {
    static Imx1GraphicsMemory memories[2];
    const uint32_t buffers[2] = {0x900000, 0x980000}, sheet = 0x100000, pitch = 480 * 2, sheetPitch = 1024;
    std::vector<uint8_t> shown(pitch * 320);
    for (int withGpu = 0; withGpu < 2; ++withGpu) {
        Imx1GraphicsMemory& memory = memories[withGpu];
        r = 0x9e3779b97f4a7c15ull;
        for (uint32_t i = 0; i < sheetPitch * 128; i += 2) {
            uint32_t word = next(4) ? palette[next(4)] : 0;
            assert(memory.accessGpu(*state, eim, sheet + i, 2, true, word) == A::ok);
        }
        std::unique_ptr<Imx1GraphicsGpu> gpu;
        if (withGpu) {
            gpu = makeGpu(false);
            if (const char* value = std::getenv("FROND_ZODIAC_GPU_MIN_PIXELS")) gpu->minPixels = std::strtoull(value, nullptr, 10);
            gpu->attach(&memory);
        }
        const auto start = std::chrono::steady_clock::now();
        for (unsigned n = 0; n < frames; ++n) {
            const uint32_t frame = buffers[n & 1];
            C::Fill clear{};
            clear.base = frame; clear.width = 480; clear.height = 320; clear.pitch = pitch; clear.colour = n & 0xffff;
            assert(C::completeFill(*state, eim, memory, clear) == A::ok);
            for (unsigned s = 0; s < 64; ++s) {
                C::Fill sprite{};
                sprite.width = size; sprite.height = size; sprite.pitch = pitch;
                sprite.base = frame + ((s * 37 + n * 3) % (320 - size)) * pitch + ((s * 53 + n * 5) % (480 - size)) * 2;
                sprite.sourceBase = sheet + (s % 4) * size * 2;
                sprite.sourcePitch = sheetPitch;
                if (a4) { sprite.a4 = true; sprite.colour = 1 + s % 14; }
                else { sprite.keyed = true; sprite.colour = 0; }
                assert(C::completeFill(*state, eim, memory, sprite) == A::ok);
            }
            for (unsigned w = 0; w < 16; ++w) {
                uint32_t word = w * 0x1111;
                assert(memory.accessGpu(*state, eim, frame + 8 * pitch + w * 4, 4, true, word) == A::ok);
            }
            if (n && !memory.copyGpuRange(*state, eim, buffers[~n & 1], pitch * 320, shown.data())) std::abort();
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("bench %s %ux%u %s: %u frames in %.3f s, %.1f frames/s, %.0f fills/s\n", withGpu ? "gpu" : "cpu",
                    size, size, a4 ? "a4" : "keyed", frames, seconds, frames / seconds, frames * 65 / seconds);
        if (gpu) {
            gpu->report(stdout);
            gpu->detach();
        }
    }
    uint32_t a = 0, b = 0;
    const uint8_t* x = memories[0].diagnosticData(1, a);
    const uint8_t* y = memories[1].diagnosticData(1, b);
    std::printf("bench end states %s\n", a == b && !std::memcmp(x, y, a) ? "equal" : "DIFFER");
}

int main(int argc, char** argv) {
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
    if (argc >= 3 && !std::strcmp(argv[1], "--bench")) {
        bench(unsigned(std::atoi(argv[2])), argc > 3 ? uint32_t(std::atoi(argv[3])) : 32, argc > 4 && !std::strcmp(argv[4], "a4"));
        return 0;
    }
    checkFills(0);
    checkFills(400);
    checkDifferential();
    checkRecovery();
}
