// The XScale D-cache is virtually tagged and still hit after the MMU goes off. With the
// cache on, VA 0x00100000 maps to PA 0x00200000; once the MMU is off, loads and stores
// through that VA keep going to PA 0x00200000 until a D-cache invalidate (c7,c6,0 or
// c7,c7,0), MMU on or reset. Runs on Exact and, where it's built in, Fast.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "CPU.h"
#include "MMU.h"
#include "RAM.h"
#include "mem.h"
#include "memory_buffer.h"
#include "pace_patch.h"
#include "patch_dispatch.h"
#include "savestate/savestateAll.h"
#include "system_state.h"

namespace {

constexpr uint32_t kRamSize = 0x400000;
constexpr uint32_t kL1 = 0x4000, kCode = 0x10000, kSpin = 0xeafffffe;
constexpr uint32_t kVa = 0x00100100, kStalePa = 0x00200100;
constexpr uint32_t kOld = 0x11111111, kStored = 0x33333333, kRemapped = 0x44444444;

// ARM words. r3 holds 0x00100000 (the cached VA section) throughout.
constexpr uint32_t kNop = 0xe1a00000;
const std::vector<uint32_t> kMmuOn = {
    0xe3a00901,  // mov r0, #0x4000
    0xee020f10,  // mcr p15, 0, r0, c2, c0, 0      TTB
    0xe3e01000,  // mvn r1, #0
    0xee031f10,  // mcr p15, 0, r1, c3, c0, 0      every domain manager
    0xe3a0207d,  // mov r2, #0x7d                  M, C, W
    0xe3822a05,  // orr r2, r2, #0x5000            I, RR
    0xee012f10,  // mcr p15, 0, r2, c1, c0, 0
    kNop, kNop,
    0xe3a03601,  // mov r3, #0x100000
};
const std::vector<uint32_t> kStore = {
    0xe3a04033,  // mov r4, #0x33
    0xe1844404,  // orr r4, r4, r4, lsl #8
    0xe1844804,  // orr r4, r4, r4, lsl #16
    0xe5834100,  // str r4, [r3, #0x100]
};
const std::vector<uint32_t> kMmuOff = {
    0xe3a02070,  // mov r2, #0x70                  M and C off
    0xe3822a05,  // orr r2, r2, #0x5000
    0xee012f10,  // mcr p15, 0, r2, c1, c0, 0
    kNop, kNop,
};
const uint32_t kLoad = 0xe5935100;        // ldr r5, [r3, #0x100]
const uint32_t kStoreR4 = 0xe5834104;     // str r4, [r3, #0x104]
const uint32_t kInvalD = 0xee070f16;      // mcr p15, 0, r0, c7, c6, 0
const uint32_t kInvalID = 0xee070f17;     // mcr p15, 0, r0, c7, c7, 0
const uint32_t kInvalLine = 0xee070f36;   // mcr p15, 0, r0, c7, c6, 1 (one line, keeps the rest)
const uint32_t kMmuOnAgain = 0xe3822001;  // orr r2, r2, #1 (then the c1 write below)
const uint32_t kWriteC1 = 0xee012f10;     // mcr p15, 0, r2, c1, c0, 0

int failures = 0;

void check(bool ok, const char* engine, const char* what) {
    if (!ok) {
        printf("FAIL %s: %s\n", engine, what);
        ++failures;
    }
}

struct Machine {
    ArmCpu* cpu = nullptr;
    uint8_t* ram = nullptr;
    uint32_t pc = kCode;  // where the next program goes
};

Machine makeMachine(bool fast) {
    unsetenv("FROND_JIT");
    cpuSetFastEngine(fast);
    cpuSetJitSync(true);
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
    // Sections, all cacheable, every one identity mapped but VA 0x001xxxxx, which goes to
    // PA 0x002xxxxx.
    for (uint32_t s = 0; s < kRamSize >> 20; ++s) {
        const uint32_t pa = s == 1 ? 0x00200000 : s << 20;
        const uint32_t entry = pa | 0xc0e;
        memcpy(m.ram + kL1 + 4 * s, &entry, 4);
    }
    memcpy(m.ram + kVa, &kOld, 4);
    memcpy(m.ram + 0x00300100, &kRemapped, 4);
    return m;
}

uint32_t word(Machine& m, uint32_t pa) {
    uint32_t w;
    memcpy(&w, m.ram + pa, 4);
    return w;
}

// Writes the words and a B . after them, runs from their start to the B . and leaves the
// next program's start past it.
void run(Machine& m, std::vector<uint32_t> words) {
    const uint32_t start = m.pc;
    words.push_back(kSpin);
    memcpy(m.ram + start, words.data(), words.size() * 4);
    m.pc += words.size() * 4;
    cpuInvalidateCodeCache(m.cpu);
    cpuSetReg(m.cpu, 15, start);
    const uint32_t spin = m.pc - 4;
    for (int i = 0; i < 1000 && cpuGetRegisters(m.cpu)[15] != spin; ++i)
        cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(m.cpu, 1);
    if (cpuGetRegisters(m.cpu)[15] != spin) {
        printf("program at %08x did not finish (pc %08x)\n", start, cpuGetRegisters(m.cpu)[15]);
        exit(1);
    }
}

uint32_t reg(Machine& m, int n) { return cpuGetRegisters(m.cpu)[n]; }

std::vector<uint32_t> join(std::initializer_list<std::vector<uint32_t>> parts) {
    std::vector<uint32_t> all;
    for (const auto& p : parts) all.insert(all.end(), p.begin(), p.end());
    return all;
}

// MMU and D-cache on, a store through the cached VA, then MMU and cache off.
Machine openWindow(bool fast) {
    Machine m = makeMachine(fast);
    run(m, join({kMmuOn, kStore, kMmuOff}));
    return m;
}

struct CpuSaver {
    ArmCpu* cpu;
    template <class T>
    void Save(T& s) { cpuSave(cpu, s); }
    template <class T>
    void Load(T& l) { cpuLoad(cpu, l); }
};

void test(bool fast) {
    const char* e = fast ? "Fast" : "Exact";
    {
        Machine m = openWindow(fast);
        check(word(m, kStalePa) == kStored, e, "store with the MMU on went to the mapped PA");
        check(mmuStaleDcacheOpen(cpuGetMMU(m.cpu)), e, "MMU off with the cache on opens the window");
        run(m, {kLoad, kStoreR4});
        check(reg(m, 5) == kStored, e, "load through the old VA hits the recorded PA");
        check(word(m, kStalePa + 4) == kStored && word(m, kVa + 4) == 0, e, "store through the old VA goes to the recorded PA");
        // A line invalidate leaves the rest of the cache.
        run(m, {kInvalLine, kLoad});
        check(reg(m, 5) == kStored, e, "c7,c6,1 keeps the window open");
        run(m, {kInvalD, kLoad});
        check(!mmuStaleDcacheOpen(cpuGetMMU(m.cpu)), e, "c7,c6,0 closes the window");
        check(reg(m, 5) == kOld, e, "load after c7,c6,0 reads the VA as PA");
        // Nothing is recorded since the invalidate, so the MMU going off again opens nothing.
        run(m, join({kMmuOn, kMmuOff, {kLoad}}));
        check(!mmuStaleDcacheOpen(cpuGetMMU(m.cpu)) && reg(m, 5) == kOld, e, "no window without cached accesses");
    }
    {
        Machine m = openWindow(fast);
        run(m, {kInvalID, kLoad});
        check(!mmuStaleDcacheOpen(cpuGetMMU(m.cpu)) && reg(m, 5) == kOld, e, "c7,c7,0 closes the window");
    }
    {
        Machine m = openWindow(fast);
        // MMU on with the cache off, and the section moved to PA 0x00300000.
        const uint32_t entry = 0x00300c02;
        memcpy(m.ram + kL1 + 4, &entry, 4);
        run(m, {kLoad, kMmuOnAgain, kWriteC1, kNop, kNop, 0xee080f17 /* mcr p15,0,r0,c8,c7,0 */, kLoad});
        check(reg(m, 5) == kRemapped, e, "MMU on reads through the page tables");
        check(!mmuStaleDcacheOpen(cpuGetMMU(m.cpu)), e, "MMU on closes the window");
    }
    {
        Machine m = openWindow(fast);
        cpuReset(m.cpu, kCode);
        check(!mmuStaleDcacheOpen(cpuGetMMU(m.cpu)), e, "reset closes the window");
        run(m, {0xe3a03601, kLoad});
        check(reg(m, 5) == kOld, e, "load after reset reads the VA as PA");
    }
    {
        // MMU on with the cache off records nothing.
        Machine m = makeMachine(fast);
        std::vector<uint32_t> on = kMmuOn;
        on[4] = 0xe3a02079;  // mov r2, #0x79: M and W, C off
        run(m, join({on, kStore, kMmuOff, {kLoad}}));
        check(!mmuStaleDcacheOpen(cpuGetMMU(m.cpu)) && reg(m, 5) == kOld, e, "no window with the cache off");
    }
    {
        // Saved inside the window, loaded into a new machine.
        Machine m = openWindow(fast);
        Savestate<ChunkType> state;
        CpuSaver saver{m.cpu};
        if (!state.Save(saver)) abort();
        Machine n = makeMachine(fast);
        memcpy(n.ram, m.ram, kRamSize);
        SavestateLoader<ChunkType> loader;
        CpuSaver target{n.cpu};
        check(loader.Load(state.GetBuffer(), state.GetSize(), target), e, "session loads");
        check(mmuStaleDcacheOpen(cpuGetMMU(n.cpu)), e, "loaded session is inside the window");
        n.pc = m.pc;
        run(n, {kLoad});
        check(reg(n, 5) == kStored, e, "loaded session hits the recorded PA");
        run(n, {kInvalD, kLoad});
        check(reg(n, 5) == kOld, e, "loaded session closes on c7,c6,0");
    }
    printf("%s: done\n", e);
}

}  // namespace

int main() {
    test(false);
    if (cpuSetFastEngine(true)) test(true);
    else printf("Fast: not built in, skipped\n");
    if (failures) return 1;
    puts("PASS: stale XScale D-cache window opens, hits and closes on both engines");
    return 0;
}
