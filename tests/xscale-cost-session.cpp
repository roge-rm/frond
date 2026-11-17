// The XScale cost model's data cache state in sessions, in Exact. A machine copying memory through
// cacheable write-back pages (fills, dirty write-backs, write buffer entries) is saved mid-run with a warm
// cache, loaded into a new machine and run on: every batch then charges what the run that never saved
// charges. A session without the cache section (as older Frond versions save) loads with an empty cache
// and runs. Each batch also stays inside cpuMaxOvershootCycles, and peepholes charge their routines.
// Fast at Original (cpuSetJitMemoryCost) charges the same copy within 2% of Exact's clocks per pass;
// Fast without it (Uncapped) charges the table only.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "CPU.h"
#include "RAM.h"
#include "mem.h"
#include "memory_buffer.h"
#include "pace_patch.h"
#include "patch_dispatch.h"
#include "peephole.h"
#include "peephole_cost.h"
#include "savestate/savestateAll.h"
#include "system_state.h"
#include "dcache_cost.h"
#include "jit_dynarmic.h"
#include "xscale_cost.h"

namespace {

constexpr uint32_t kRamSize = 0x400000;
constexpr uint32_t kL1 = 0x4000, kCode = 0x10000;
constexpr uint32_t kBatch = 1000, kBatches = 4000, kSaveAt = 2500;

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++failures;
    }
}

struct Machine {
    ArmCpu* cpu = nullptr;
    uint8_t* ram = nullptr;
};

// MMU on with every section cacheable and write-back (C=1, B=1), then a copy of 48 KB from 0x200000 to
// 0x280000 eight words at a time, a byte store and a word load per step, over and over.
const std::vector<uint32_t> kProgram = {
    0xe3a00901,  // mov r0, #0x4000
    0xee020f10,  // mcr p15, 0, r0, c2, c0, 0      TTB
    0xe3e01000,  // mvn r1, #0
    0xee031f10,  // mcr p15, 0, r1, c3, c0, 0      every domain manager
    0xe3a0207d,  // mov r2, #0x7d                  M, C, W
    0xe3822a05,  // orr r2, r2, #0x5000            I, RR
    0xee012f10,  // mcr p15, 0, r2, c1, c0, 0
    0xe1a00000,  // nop
    0xe1a00000,  // nop
    // outer: (kCode + 0x24)
    0xe3a00602,  // mov r0, #0x200000             source
    0xe3a01a0a,  // mov r1, #0xa000
    0xe3811702,  // orr r1, r1, #0x80000           dest 0x28a000: overlaps no source line
    0xe3a02903,  // mov r2, #0xc000                bytes left
    // inner: (kCode + 0x34)
    0xe8b00ff0,  // ldmia r0!, {r4-r11}
    0xe8a10ff0,  // stmia r1!, {r4-r11}
    0xe5c1c000,  // strb r12, [r1]
    0xe590c100,  // ldr r12, [r0, #0x100]
    0xe2522020,  // subs r2, r2, #32
    0x1afffff9,  // bne inner
    0xeafffff4,  // b outer
};

// kProgram with r3 counting the inner loop's passes.
const std::vector<uint32_t> kCounted = [] {
    std::vector<uint32_t> p(kProgram.begin(), kProgram.begin() + 17);
    p.push_back(0xe2833001);  // add r3, r3, #1
    p.push_back(0xe2522020);  // subs r2, r2, #32
    p.push_back(0x1afffff8);  // bne inner
    p.push_back(0xeafffff3);  // b outer
    return p;
}();

Machine makeMachine(bool costMemory, bool fast = false, const std::vector<uint32_t>& program = kProgram) {
    unsetenv("FROND_JIT");
    if (fast) {
        setenv("FROND_JIT_HOT", "1", 1);
        cpuSetJitSync(true);
    }
    if (!cpuSetFastEngine(fast)) {
        printf("FAIL: this core has no Fast engine\n");
        exit(1);
    }
    Machine m;
    ArmMem* mem = memInit();
    MemoryBuffer buf{};
    if (!memoryBufferAllocate(&buf, kRamSize)) abort();
    ArmRam* ram = ramInit(mem, nullptr, 0, kRamSize, &buf, true);
    ramSetNoFramebuffer(ram);
    m.ram = buf.buffer;
    static PacePatch pace{};
    PatchDispatch* pd = initPatchDispatch();
    m.cpu = cpuInit(kCode, mem, ARM_MEMORY_SYSTEM_MMU, true, false, -1, 0x69054114, 0x0b16a16a, pd, &pace,
                    createSystemState());
    patchDispatchSetCpu(pd, m.cpu);
    cpuSetCostModel(m.cpu, true, uint8_t(cost::Core::pxa27x), costMemory);
    const cost::Clocks c = cost::kClocksTx;
    cpuSetCostClocks(m.cpu, c.core, c.bus, c.memory);
    for (uint32_t s = 0; s < kRamSize >> 20; ++s) {
        const uint32_t entry = (s << 20) | 0xc0e;  // section, AP 3, C, B
        memcpy(m.ram + kL1 + 4 * s, &entry, 4);
    }
    memcpy(m.ram + kCode, program.data(), program.size() * 4);
    for (uint32_t i = 0; i < 0x10000; i += 4) {
        const uint32_t v = i * 2654435761u;
        memcpy(m.ram + 0x200000 + i, &v, 4);
    }
    return m;
}

struct CpuSaver {
    ArmCpu* cpu;
    template <class T>
    void Save(T& s) { cpuSave(cpu, s); }
    template <class T>
    void Load(T& l) { cpuLoad(cpu, l); }
};

// cpuSave plus a version 1 data cache section (the clock and 4144 bytes, an empty cache) for a CPU that
// saves none of its own.
struct CpuSaverV1 {
    ArmCpu* cpu;
    template <class T>
    void Save(T& s) {
        cpuSave(cpu, s);
        auto chunk = s.GetChunk(ChunkType::costDataCache, 1);
        if (!chunk) abort();
        std::vector<uint8_t> bytes(cost::kDataCacheSaveBytesV1, 0);
        uint64_t clock = 0;
        SaveChunkHelper helper(*chunk);
        helper.Do64(clock).DoBuffer(bytes.data(), bytes.size());
    }
    template <class T>
    void Load(T& l) { cpuLoad(cpu, l); }
};

uint32_t runBatch(Machine& m) {
    const uint32_t ran = cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(m.cpu, kBatch);
    // A batch can end early (the MMU switch), never later than the bound.
    check(ran <= kBatch || ran - kBatch <= cpuMaxOvershootCycles(m.cpu), "batch inside the overshoot bound");
    return ran;
}

void testSaveLoad() {
    std::vector<uint32_t> straight;
    Machine a = makeMachine(true);
    for (uint32_t i = 0; i < kBatches; i++) straight.push_back(runBatch(a));
    uint64_t total = 0;
    for (uint32_t r : straight) total += r;
    // The copy misses and writes back: well over one clock per instruction.
    check(total > uint64_t(kBatches) * kBatch, "batches ran");

    Machine b = makeMachine(true);
    std::vector<uint32_t> resumed;
    for (uint32_t i = 0; i < kSaveAt; i++) resumed.push_back(runBatch(b));
    Savestate<ChunkType> state;
    CpuSaver saver{b.cpu};
    check(state.Save(saver), "session saves");
    Machine c = makeMachine(true);
    memcpy(c.ram, b.ram, kRamSize);
    SavestateLoader<ChunkType> loader;
    CpuSaver target{c.cpu};
    check(loader.Load(state.GetBuffer(), state.GetSize(), target), "session loads");
    for (uint32_t i = kSaveAt; i < kBatches; i++) resumed.push_back(runBatch(c));
    check(resumed == straight, "loaded run charges what the straight run charges");
    check(!memcmp(cpuGetRegisters(a.cpu), cpuGetRegisters(c.cpu), 16 * 4), "same registers");
    // The same load with the cache emptied afterwards charges differently, so the section is what kept
    // the counts.
    Machine f = makeMachine(true);
    memcpy(f.ram, b.ram, kRamSize);
    SavestateLoader<ChunkType> emptied;
    CpuSaver emptiedTarget{f.cpu};
    check(emptied.Load(state.GetBuffer(), state.GetSize(), emptiedTarget), "session loads again");
    cpuSetCostModel(f.cpu, true, uint8_t(cost::Core::pxa27x), true);
    cpuSetCostClocks(f.cpu, cost::kClocksTx.core, cost::kClocksTx.bus, cost::kClocksTx.memory);
    std::vector<uint32_t> emptyRun;
    for (uint32_t i = kSaveAt; i < kBatches; i++) emptyRun.push_back(runBatch(f));
    check(emptyRun != std::vector<uint32_t>(straight.begin() + kSaveAt, straight.end()), "an empty cache charges differently");

    // Without the section the cache starts empty: the session loads and runs, charging more at first.
    Machine d = makeMachine(false);
    memcpy(d.ram, b.ram, kRamSize);
    for (uint32_t i = 0; i < kSaveAt; i++) cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(d.cpu, kBatch);
    Savestate<ChunkType> old;
    CpuSaver oldSaver{d.cpu};
    check(old.Save(oldSaver), "session without the section saves");
    Machine e = makeMachine(true);
    memcpy(e.ram, d.ram, kRamSize);
    SavestateLoader<ChunkType> oldLoader;
    CpuSaver oldTarget{e.cpu};
    check(oldLoader.Load(old.GetBuffer(), old.GetSize(), oldTarget), "session without the section loads");
    uint64_t cold = 0, warm = 0;
    for (uint32_t i = 0; i < 20; i++) cold += runBatch(e);
    for (uint32_t i = 0; i < 20; i++) warm += runBatch(e);
    check(cold > 0 && warm > 0, "session without the section runs");

    // A version 1 section (before the write buffer slots) loads and runs.
    Savestate<ChunkType> v1;
    CpuSaverV1 v1Saver{d.cpu};
    check(v1.Save(v1Saver), "session with a version 1 section saves");
    Machine g = makeMachine(true);
    memcpy(g.ram, d.ram, kRamSize);
    SavestateLoader<ChunkType> v1Loader;
    CpuSaver v1Target{g.cpu};
    check(v1Loader.Load(v1.GetBuffer(), v1.GetSize(), v1Target), "session with a version 1 section loads");
    uint64_t v1Run = 0;
    for (uint32_t i = 0; i < 40; i++) v1Run += runBatch(g);
    check(v1Run > 0, "session with a version 1 section runs");
}

// Every peephole's routine, run by peephole_cost.h, gives the result the peephole computes.
void testPeepholeRoutines() {
    uint32_t seed = 12345;
    auto rnd = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed;
    };
    for (int n = 0; n < 20000; n++) {
        uint32_t num = rnd(), den = rnd() >> (rnd() % 32);
        if (!den) den = 1;
        size_t count;
        const uint32_t* words;
        for (int sign = 0; sign < 2; sign++) {
            words = peepholeRoutine(sign ? INSTR_PEEPHOLE_ADS_SDIVMOD : INSTR_PEEPHOLE_ADS_UDIVMOD, &count);
            uint32_t regs[16] = {den, num};
            uint32_t flags = 0, charged = 0;
            cost::runRoutine(words, count, regs, flags, cost::Core::pxa27x, [&](uint32_t c) { charged += c; },
                             [](uint32_t, bool) {});
            const bool ok = sign ? (int32_t(den) != -1 && regs[0] == uint32_t(int32_t(num) / int32_t(den)) &&
                                    regs[1] == uint32_t(int32_t(num) % int32_t(den)))
                                 : (regs[0] == num / den && regs[1] == num % den);
            if (!(sign && int32_t(den) == -1)) check(ok && charged > 10, sign ? "sdivmod routine" : "udivmod routine");
            if (!ok && !(sign && int32_t(den) == -1)) {
                printf("  %s %08x / %08x -> %08x %08x\n", sign ? "s" : "u", num, den, regs[0], regs[1]);
                return;
            }
        }
        for (int sign = 0; sign < 2; sign++) {
            words = peepholeRoutine(sign ? INSTR_PEEPHOLE_ADS_SDIV10 : INSTR_PEEPHOLE_ADS_UDIV10, &count);
            uint32_t regs[16] = {num};
            uint32_t flags = 0, charged = 0;
            cost::runRoutine(words, count, regs, flags, cost::Core::pxa27x, [&](uint32_t c) { charged += c; },
                             [](uint32_t, bool) {});
            const bool ok = sign ? regs[0] == uint32_t(int32_t(num) / 10) && regs[1] == uint32_t(int32_t(num) % 10)
                                 : regs[0] == num / 10 && regs[1] == num % 10;
            check(ok, sign ? "sdiv10 routine" : "udiv10 routine");
        }
    }
    // memcpy: word-aligned copies of every size up to 100 bytes make one access per word or byte.
    for (uint32_t size = 0; size <= 100; size++) {
        size_t count;
        const uint32_t* words = peepholeRoutine(INSTR_PEEPHOLE_ADS_MEMCPY, &count);
        uint32_t regs[16] = {0x1000, 0x2000, size};
        regs[13] = 0x8000;
        uint32_t flags = 0, loads = 0, stores = 0, loaded = 0, stored = 0;
        cost::runRoutine(words, count, regs, flags, cost::Core::pxa27x, [](uint32_t) {},
                         [&](uint32_t va, bool store) {
                             if (va >= 0x7000) return;  // the stacked r4 and lr
                             (store ? stores : loads)++;
                             if (store) stored = va; else loaded = va;
                         });
        const uint32_t expect = size / 4 + size % 4;
        check(loads == expect && stores == expect, "memcpy routine accesses");
        check(regs[13] == 0x8000, "memcpy routine restores sp");
        if (size) check(stored - 0x1000 == loaded - 0x2000 && stored < 0x1000 + size, "memcpy routine order");
    }
}

// kProgram's MMU setup, then a stream of 48 KB of loads (fills that go round every way of every set) with
// a load and a store to one hot line at 0x300000 each step. The hot line's loads and stores hit, through
// the JIT's data cache hint once it holds them, until the stream evicts the line; the next load fills it
// clean and the next store dirties it again. Two stores 3840 bytes ahead of the stream miss the cache:
// the first takes a write buffer entry, the second joins it if it hasn't started draining, and the
// stream's fill of that line later turns such stores into hits. r3 counts the passes.
const std::vector<uint32_t> kHot = [] {
    std::vector<uint32_t> p(kProgram.begin(), kProgram.begin() + 9);
    const std::vector<uint32_t> loop = {
        0xe3a0d603,  // mov sp, #0x300000             the hot line
        // outer:
        0xe3a00602,  // mov r0, #0x200000
        0xe3a02903,  // mov r2, #0xc000
        // inner:
        0xe8b00ff0,  // ldmia r0!, {r4-r11}
        0xe59dc000,  // ldr r12, [sp]
        0xe28cc001,  // add r12, r12, #1
        0xe58dc004,  // str r12, [sp, #4]
        0xe58dc014,  // str r12, [sp, #20]
        0xe580cf00,  // str r12, [r0, #0xf00]       a line not in the cache: a write buffer entry
        0xe580cf04,  // str r12, [r0, #0xf04]       joins it while it hasn't started draining
        0xe2833001,  // add r3, r3, #1
        0xe2522020,  // subs r2, r2, #32
        0x1afffff5,  // bne inner
        0xeafffff2,  // b outer
    };
    p.insert(p.end(), loop.begin(), loop.end());
    return p;
}();

// Fast at Original with the data cache hint charges exactly what it charges with every access going to
// the callbacks: every batch, the registers and the memory, straight and across a session save and load
// (the hint isn't saved), while far fewer accesses reach the callbacks. The stream evicts the hot line
// every 32 fills of its set, so a hint left behind by an eviction would let a load miss for free or a
// store skip dirtying the line, and the counts would part.
void testFastHint() {
    constexpr uint32_t kRuns = 3000, kSave = 1700;
    auto run = [](bool hint) {
        jitSetCostHint(hint);
        Machine m = makeMachine(true, true, kHot);
        cpuSetJitMemoryCost(m.cpu, true);
        return m;
    };
    Machine plain = run(false);
    std::vector<uint32_t> plainRuns;
    uint64_t from = jitMemoryCallbacks();
    for (uint32_t i = 0; i < kRuns; i++) plainRuns.push_back(runBatch(plain));
    const uint64_t plainCalls = jitMemoryCallbacks() - from;

    Machine hinted = run(true);
    std::vector<uint32_t> hintedRuns;
    from = jitMemoryCallbacks();
    for (uint32_t i = 0; i < kRuns; i++) hintedRuns.push_back(runBatch(hinted));
    const uint64_t hintedCalls = jitMemoryCallbacks() - from;
    const uint32_t passes = cpuGetRegisters(hinted.cpu)[3];
    uint64_t clocks = 0;
    for (uint32_t r : hintedRuns) clocks += r;
    printf("hint: %u passes, %.2f clocks per pass, callbacks %llu without the hint, %llu with it\n", passes,
           passes ? double(clocks) / passes : 0.0, (unsigned long long)plainCalls, (unsigned long long)hintedCalls);
    check(passes > 1536 * 2, "the hint run went round the stream more than twice");
    check(hintedRuns == plainRuns, "the hint charges what the callbacks charge, batch by batch");
    check(!memcmp(cpuGetRegisters(plain.cpu), cpuGetRegisters(hinted.cpu), 16 * 4), "the hint: same registers");
    check(!memcmp(plain.ram + 0x300000, hinted.ram + 0x300000, 32), "the hint: same hot line");
    check(!memcmp(plain.ram + 0x200000, hinted.ram + 0x200000, 0x10000), "the hint: same stream");
    check(hintedCalls * 2 < plainCalls, "hits stay in generated code");

    // Saved mid-run and loaded into a new machine: the hint starts empty and the counts carry on.
    Machine b = run(true);
    std::vector<uint32_t> resumed;
    for (uint32_t i = 0; i < kSave; i++) resumed.push_back(runBatch(b));
    Savestate<ChunkType> state;
    CpuSaver saver{b.cpu};
    check(state.Save(saver), "Fast session saves");
    Machine c = run(true);
    memcpy(c.ram, b.ram, kRamSize);
    SavestateLoader<ChunkType> loader;
    CpuSaver target{c.cpu};
    check(loader.Load(state.GetBuffer(), state.GetSize(), target), "Fast session loads");
    for (uint32_t i = kSave; i < kRuns; i++) resumed.push_back(runBatch(c));
    check(resumed == hintedRuns, "Fast loaded run with the hint charges what the straight run charges");
    check(!memcmp(cpuGetRegisters(c.cpu), cpuGetRegisters(hinted.cpu), 16 * 4), "Fast loaded run: same registers");
    jitSetCostHint(true);
    cpuSetFastEngine(false);
    unsetenv("FROND_JIT_HOT");
    cpuSetJitSync(false);
}

// Clocks per inner pass of kCounted over 3000 batches, after 500 to warm up.
double clocksPerPass(Machine& m) {
    for (uint32_t i = 0; i < 500; i++) runBatch(m);
    const uint32_t from = cpuGetRegisters(m.cpu)[3];
    uint64_t clocks = 0;
    for (uint32_t i = 0; i < 3000; i++) clocks += runBatch(m);
    const uint32_t passes = cpuGetRegisters(m.cpu)[3] - from;
    return passes ? double(clocks) / passes : 0;
}

void testFastCharges() {
    Machine exact = makeMachine(true, false, kCounted);
    const double e = clocksPerPass(exact);
    Machine original = makeMachine(true, true, kCounted);
    cpuSetJitMemoryCost(original.cpu, true);
    const double o = clocksPerPass(original);
    Machine uncapped = makeMachine(true, true, kCounted);
    const double u = clocksPerPass(uncapped);
    printf("clocks per pass: exact %.2f, fast original %.2f, fast uncapped %.2f\n", e, o, u);
    check(e > 0 && o > e * 0.98 && o < e * 1.02, "Fast at Original charges what Exact charges");
    check(u > 0 && u < e * 0.8, "Fast without memory charges is cheaper");
    check(!memcmp(exact.ram + 0x28a000, original.ram + 0x28a000, 0xc000), "Fast at Original copies the same data");
    cpuSetFastEngine(false);
    unsetenv("FROND_JIT_HOT");
    cpuSetJitSync(false);
}

}  // namespace

int main() {
    testPeepholeRoutines();
    testSaveLoad();
    testFastCharges();
    testFastHint();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("xscale cost session: ok\n");
    return 0;
}
