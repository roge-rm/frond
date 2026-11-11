// The XScale D-cache is virtually tagged and still hits after the MMU goes off. With the
// cache on, VA 0x00100000 maps to PA 0x00200000; once a c1 write turns M off with C on,
// loads and stores through that VA keep going to PA 0x00200000 (looked up through the old
// page tables, cacheable mappings only) until a D-cache invalidate (c7,c6,0 or c7,c7,0),
// MMU on or reset. Runs on Exact and, where it's built in, Fast. The Fast pass runs in a
// child with every block compiled at once and the JIT's counters on, so it can check the
// window's accesses went through compiled code.
#include <unistd.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
constexpr uint32_t kL1 = 0x4000, kCoarse = 0x8000, kCode = 0x10000, kSpin = 0xeafffffe;
constexpr uint32_t kVa = 0x00100100, kStalePa = 0x00200100;
constexpr uint32_t kOld = 0x11111111, kStored = 0x33333333, kRemapped = 0x44444444;
// Each window access loop runs this many times, so Fast compiles and reruns it.
constexpr uint32_t kLoops = 100;

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
// M and C off in one write, as Cocoboot does.
const std::vector<uint32_t> kMmuOff = {
    0xe3a02070,  // mov r2, #0x70
    0xe3822a05,  // orr r2, r2, #0x5000
    0xee012f10,  // mcr p15, 0, r2, c1, c0, 0
    kNop, kNop,
};
// C off with the MMU still on.
const std::vector<uint32_t> kCacheOff = {
    0xe3a02079,  // mov r2, #0x79                  M and W
    0xe3822a05,  // orr r2, r2, #0x5000
    0xee012f10,  // mcr p15, 0, r2, c1, c0, 0
};
const uint32_t kLoad = 0xe5935100;        // ldr r5, [r3, #0x100]
const uint32_t kStoreR5 = 0xe5835104;     // str r5, [r3, #0x104]
const uint32_t kInvalD = 0xee070f16;      // mcr p15, 0, r0, c7, c6, 0
const uint32_t kInvalID = 0xee070f17;     // mcr p15, 0, r0, c7, c7, 0
const uint32_t kInvalLine = 0xee070f36;   // mcr p15, 0, r0, c7, c6, 1 (one line, keeps the rest)
const uint32_t kMmuOnAgain = 0xe3822001;  // orr r2, r2, #1 (then the c1 write below)
const uint32_t kWriteC1 = 0xee012f10;     // mcr p15, 0, r2, c1, c0, 0
// Small pages in the coarse table for VA 0x003xxxxx: 0x00300000 non-cacheable and
// 0x00380000 cacheable, both to PA 0x00200000.
const std::vector<uint32_t> kCoarseLoads = {
    0xe3a06603,  // mov r6, #0x300000
    0xe3a07838,  // mov r7, #0x380000
    0xe5968100,  // ldr r8, [r6, #0x100]
    0xe5979100,  // ldr r9, [r7, #0x100]
};

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
    // Cacheable sections, identity mapped but VA 0x001xxxxx, which goes to PA 0x002xxxxx,
    // and VA 0x003xxxxx, which goes through the coarse table.
    for (uint32_t s = 0; s < kRamSize >> 20; ++s) {
        const uint32_t entry = s == 3 ? kCoarse | 0x11 : (s == 1 ? 0x00200000 : s << 20) | 0xc0e;
        memcpy(m.ram + kL1 + 4 * s, &entry, 4);
    }
    const uint32_t uncached = 0x00200ff2, cached = 0x00200ffe;
    memcpy(m.ram + kCoarse, &uncached, 4);
    memcpy(m.ram + kCoarse + 4 * 0x80, &cached, 4);
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
    for (int i = 0; i < 100000 && cpuGetRegisters(m.cpu)[15] != spin; ++i)
        cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(m.cpu, 16);
    if (cpuGetRegisters(m.cpu)[15] != spin) {
        printf("program at %08x did not finish (pc %08x)\n", start, cpuGetRegisters(m.cpu)[15]);
        exit(1);
    }
}

// body kLoops times, counted down in r10.
void loop(Machine& m, const std::vector<uint32_t>& body) {
    std::vector<uint32_t> words = {0xe3a0a000 | kLoops};  // mov r10, #kLoops
    words.insert(words.end(), body.begin(), body.end());
    words.push_back(0xe25aa001);  // subs r10, r10, #1
    words.push_back(0x1a000000 | ((-int32_t(body.size()) - 3) & 0xffffff));  // bne to the body
    run(m, words);
}

uint32_t reg(Machine& m, int n) { return cpuGetRegisters(m.cpu)[n]; }

bool open(Machine& m) { return mmuStaleDcacheOpen(cpuGetMMU(m.cpu)); }

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

Machine reload(bool fast, Machine& from, Savestate<ChunkType>& state, const char* e) {
    Machine n = makeMachine(fast);
    memcpy(n.ram, from.ram, kRamSize);
    SavestateLoader<ChunkType> loader;
    CpuSaver target{n.cpu};
    check(loader.Load(state.GetBuffer(), state.GetSize(), target), e, "session loads");
    n.pc = from.pc;
    return n;
}

void test(bool fast) {
    const char* e = fast ? "Fast" : "Exact";
    {
        Machine m = openWindow(fast);
        check(word(m, kStalePa) == kStored, e, "store with the MMU on went to the mapped PA");
        check(open(m), e, "M off with C on opens the window");
        loop(m, join({{kLoad, kStoreR5}, kCoarseLoads}));
        check(reg(m, 5) == kStored, e, "load through the old VA hits the old mapping's PA");
        check(word(m, kStalePa + 4) == kStored && word(m, kVa + 4) == 0, e,
              "store through the old VA goes to the old mapping's PA");
        check(reg(m, 9) == kStored, e, "cacheable small page in a coarse table hits");
        check(reg(m, 8) == kRemapped, e, "non-cacheable mapping reads the VA as PA");
        // A line invalidate leaves the rest of the cache.
        loop(m, {kInvalLine, kLoad});
        check(open(m) && reg(m, 5) == kStored, e, "c7,c6,1 keeps the window open");
        loop(m, {kInvalD, kLoad});
        check(!open(m), e, "c7,c6,0 closes the window");
        check(reg(m, 5) == kOld, e, "load after c7,c6,0 reads the VA as PA");
    }
    {
        Machine m = openWindow(fast);
        loop(m, {kLoad});
        check(reg(m, 5) == kStored, e, "load inside the window before c7,c7,0");
        run(m, {kInvalID, kLoad});
        check(!open(m) && reg(m, 5) == kOld, e, "c7,c7,0 closes the window");
    }
    {
        Machine m = openWindow(fast);
        // MMU on with the cache off, and the section moved to PA 0x00300000.
        const uint32_t entry = 0x00300c02;
        memcpy(m.ram + kL1 + 4, &entry, 4);
        run(m, {kLoad, kMmuOnAgain, kWriteC1, kNop, kNop, 0xee080f17 /* mcr p15,0,r0,c8,c7,0 */, kLoad});
        check(reg(m, 5) == kRemapped, e, "MMU on reads through the page tables");
        check(!open(m), e, "MMU on closes the window");
    }
    {
        Machine m = openWindow(fast);
        cpuReset(m.cpu, kCode);
        check(!open(m), e, "reset closes the window");
        run(m, {0xe3a03601, kLoad});
        check(reg(m, 5) == kOld, e, "load after reset reads the VA as PA");
    }
    {
        // C off before M: the cache is no longer looked up when the MMU goes off.
        Machine m = makeMachine(fast);
        run(m, join({kMmuOn, kStore, kCacheOff, {kNop, kNop}, kMmuOff, {kLoad}}));
        check(!open(m) && reg(m, 5) == kOld, e, "no window when C went off before M");
    }
    {
        // Saved inside the window, loaded into a new machine.
        Machine m = openWindow(fast);
        Savestate<ChunkType> state;
        CpuSaver saver{m.cpu};
        if (!state.Save(saver)) abort();
        Machine n = reload(fast, m, state, e);
        check(open(n), e, "loaded session is inside the window");
        loop(n, {kLoad});
        check(reg(n, 5) == kStored, e, "loaded session hits the old mapping's PA");
        run(n, {kInvalD, kLoad});
        check(reg(n, 5) == kOld, e, "loaded session closes on c7,c6,0");
        // The same savestate saved again after a reset carries no window.
        cpuReset(m.cpu, kCode);
        if (!state.Save(saver)) abort();
        Machine r = reload(fast, m, state, e);
        check(!open(r), e, "save after reset has no window");
        run(r, {0xe3a03601, kLoad});
        check(reg(r, 5) == kOld, e, "save after reset reads the VA as PA");
    }
    printf("%s: done\n", e);
}

// Runs the Fast pass in a child with FROND_JIT_HOT=1 and FROND_JIT_STATS=1 and checks from
// its counters that compiled code made the window's loads and stores: inside the window the
// JIT's page table stays empty, so each one is a read or write callback.
int fastInChild() {
    char exe[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return 1;
    exe[n] = 0;
    const std::string cmd = std::string("FROND_JIT_HOT=1 FROND_JIT_STATS=1 '") + exe + "' --fast 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return 1;
    unsigned long long runs = 0, ran = 0, reads = 0, writes = 0;
    bool counted = false;
    char line[4096];
    while (fgets(line, sizeof(line), p)) {
        if (!strncmp(line, "jit: runs ", 10)) {
            const char* r = strstr(line, " reads ");
            const char* w = strstr(line, " writes ");
            counted = sscanf(line, "jit: runs %llu ran %llu", &runs, &ran) == 2 && r && w &&
                      sscanf(r, " reads %llu", &reads) == 1 && sscanf(w, " writes %llu", &writes) == 1;
        } else if (strncmp(line, "jit: ", 5)) {
            fputs(line, stdout);
        }
    }
    const int status = pclose(p);
    printf("Fast: compiled runs %llu ran %llu instructions, %llu read and %llu write callbacks\n", runs, ran,
           reads, writes);
    // The window's loads: 6 * kLoops over four loops, and kLoops stores.
    const bool compiled = counted && ran > 0 && reads >= 6 * kLoops && writes >= kLoops;
    if (!compiled) printf("FAIL Fast: the window's accesses didn't run in compiled code\n");
    return status == 0 && compiled ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--fast")) {
        test(true);
        return failures ? 1 : 0;
    }
    test(false);
    if (cpuSetFastEngine(true)) failures += fastInChild();
    else printf("Fast: not built in, skipped\n");
    if (failures) return 1;
    puts("PASS: stale XScale D-cache window opens, hits and closes on both engines");
    return 0;
}
