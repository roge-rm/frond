// How far one cpuCycle call runs past the cycles it was asked for, against
// cpuMaxOvershootCycles. Synthetic code: a straight ARM run longer than any cap, ARM
// conditional fallthrough chains (one block per condition change), Thumb straight runs and
// Thumb conditional branch chains, at budgets from 1 up, from fresh starts and continuing.
// Fast is checked at the built-in cap and at 64 and 256 (FROND_JIT_MAX_UNCHECKED), Exact at 19,
// compiling every block at once (FROND_JIT_HOT=1), and with the built-in interpret-first count.
// With the cap off (FROND_JIT_MAX_UNCHECKED=0) the same cases must overshoot more than any
// cap, so they really reach the unchecked paths.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "CPU.h"
#include "patch_dispatch.h"
#include "system_state.h"

static constexpr uint32_t base = 0x10000;
static std::vector<uint8_t> memory(0x10000);
static bool access(void*, uint32_t addr, uint_fast8_t size, bool write, void* buf) {
    if (addr < base || uint64_t(addr) + size > base + memory.size()) return false;
    if (write) std::memcpy(memory.data() + addr - base, buf, size);
    else std::memcpy(buf, memory.data() + addr - base, size);
    return true;
}
static void put32(uint32_t addr, uint32_t v) { std::memcpy(memory.data() + addr - base, &v, 4); }
static void put16(uint32_t addr, uint16_t v) { std::memcpy(memory.data() + addr - base, &v, 2); }

struct Program {
    const char* name;
    uint32_t start, end;  // end: the closing B . (a checked loop)
    bool thumb;
};

static std::vector<Program> build() {
    std::vector<Program> p;
    uint32_t a = base;
    // ARM: 3000 x MOV r0, r0, then B .
    p.push_back({"arm straight", a, 0, false});
    for (int i = 0; i < 3000; ++i, a += 4) put32(a, 0xe1a00000);
    p.back().end = a; put32(a, 0xeafffffe); a += 4;
    // ARM: ADDEQ r1 / ADDNE r2 alternating, every instruction its own block.
    p.push_back({"arm cond alternating", a, 0, false});
    for (int i = 0; i < 1000; ++i, a += 4) put32(a, (i & 1) ? 0x12822001 : 0x02811001);
    p.back().end = a; put32(a, 0xeafffffe); a += 4;
    // ARM: runs of 1 to 5 of one condition, then the other, with AL instructions between.
    p.push_back({"arm cond runs", a, 0, false});
    for (int i = 0, n = 0; i < 1500; ++i, a += 4) {
        const int run = 1 + (i / 7) % 5;
        if (i % 11 == 10) put32(a, 0xe2833001);  // ADD r3, r3, #1
        else put32(a, ((n++ / run) & 1) ? 0x12822001 : 0x02811001);
    }
    p.back().end = a; put32(a, 0xeafffffe); a += 4;
    // ARM: ADDEQ r1, then ADD r3 (AL), repeated: each pair is a block that falls through to
    // the next with LinkBlockFast, so with Z set the chain never checks unless the cap does.
    p.push_back({"arm cond pass chain", a, 0, false});
    for (int i = 0; i < 2000; ++i, a += 4) put32(a, (i & 1) ? 0xe2833001 : 0x02811001);
    p.back().end = a; put32(a, 0xeafffffe); a += 4;
    // Thumb: 3000 x ADDS r0, #1, then B .
    a = base + 0x8000;
    p.push_back({"thumb straight", a, 0, true});
    for (int i = 0; i < 3000; ++i, a += 2) put16(a, 0x3001);
    p.back().end = a; put16(a, 0xe7fe); a += 2;
    // Thumb: BEQ next, BNE next and MOV r8, r8 (keeps the flags) mixed.
    p.push_back({"thumb cond branches", a, 0, true});
    for (int i = 0; i < 2000; ++i, a += 2) put16(a, i % 3 == 0 ? 0xd0ff : i % 3 == 1 ? 0xd1ff : 0x46c0);
    p.back().end = a; put16(a, 0xe7fe); a += 2;
    assert(a <= base + memory.size());
    return p;
}

static const uint32_t budgets[] = {1, 2, 3, 5, 17, 19, 20, 21, 63, 64, 65, 127, 128, 129,
                                   255, 256, 257, 300, 1000, 4000};

struct Engine {
    ArmCpu* cpu;
    uint32_t bound;
};

static Engine make(bool fast, const char* cap, const char* hot) {
    if (cap) setenv("FROND_JIT_MAX_UNCHECKED", cap, 1);
    else unsetenv("FROND_JIT_MAX_UNCHECKED");
    if (hot) setenv("FROND_JIT_HOT", hot, 1);
    else unsetenv("FROND_JIT_HOT");
    unsetenv("FROND_JIT");
    if (!cpuSetFastEngine(fast)) {
        fprintf(stderr, "this core has no Fast engine\n");
        exit(1);
    }
    auto mem = memInit();
    assert(memRegionAdd(mem, base, memory.size(), access, nullptr));
    static PacePatch pace{};
    auto pd = initPatchDispatch();
    auto cpu = cpuInit(base, mem, ARM_MEMORY_SYSTEM_MMU, true, false, -1, 0x69054114, 0x0b16a16a, pd,
                       &pace, createSystemState());
    patchDispatchSetCpu(pd, cpu);
    return {cpu, cpuMaxOvershootCycles(cpu)};
}

static void start(ArmCpu* cpu, const Program& p, uint32_t pc, bool z) {
    // SVC, IRQ and FIQ masked; Z picks which conditional instructions pass.
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, 0xd3 | (p.thumb ? 0x20 : 0) | (z ? 0x40000000 : 0));
    cpuSetReg(cpu, 15, pc | (p.thumb ? 1 : 0));
}

// regs[15] is the next instruction in both engines (curInstrPC is the interpreter's only).
static uint32_t pcNow(ArmCpu* cpu) { return cpuGetRegisters(cpu)[15] & ~1u; }

// Worst overshoot over fresh starts and over continuing runs through each program.
static uint32_t worst(Engine& e, const Program& p) {
    uint32_t w = 0;
    const uint32_t step = p.thumb ? 2 : 4;
    for (int z = 0; z < 2; ++z) {
        for (uint32_t off : {0u, 1u, 7u, 50u, 333u}) {
            for (uint32_t b : budgets) {
                start(e.cpu, p, p.start + off * step, z);
                const uint32_t ran = cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(e.cpu, b);
                if (ran > b && ran - b > w) w = ran - b;
            }
        }
        // Continuing: the budgets in turn until the closing loop, so runs start mid-block.
        start(e.cpu, p, p.start, z);
        for (size_t i = 0; pcNow(e.cpu) != p.end && i < 100000; ++i) {
            const uint32_t b = budgets[(i * 7) % (sizeof(budgets) / sizeof(budgets[0]))] % 97 + 1;
            const uint32_t ran = cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(e.cpu, b);
            if (ran > b && ran - b > w) w = ran - b;
        }
        assert(pcNow(e.cpu) == p.end);
    }
    return w;
}

int main() {
    cpuSetJitSync(true);  // Fast compiles where asked for, so runs repeat
    const auto programs = build();
    struct Setup {
        const char* label;
        bool fast;
        const char* cap;
        const char* hot;
    } setups[] = {{"exact", false, nullptr, "1"},      {"fast", true, nullptr, "1"},
                  {"fast L=64", true, "64", "1"},      {"fast L=256", true, "256", "1"},
                  {"fast uncapped", true, "0", "1"},   {"fast hot", true, nullptr, nullptr}};
    bool ok = true;
    for (const auto& s : setups) {
        Engine e = make(s.fast, s.cap, s.hot);
        uint32_t all = 0;
        for (const auto& p : programs) {
            const uint32_t w = worst(e, p);
            if (w > all) all = w;
            printf("%-14s %-22s worst %4u bound %u\n", s.label, p.name, w, e.bound);
            if (w > e.bound) ok = false;
            // Straight runs are longer than any cap: Fast must reach it, or the JIT didn't run.
            // Uncapped, they and the pass chain must go past 256, or they miss unchecked code.
            const std::string name = p.name;
            const bool straight = name.find("straight") != std::string::npos;
            if (s.fast && (straight || name == "arm cond pass chain")) {
                if (straight && e.bound != UINT32_MAX && w + 1 < e.bound) {
                    printf("  the JIT never reached its cap here\n");
                    ok = false;
                }
                if (e.bound == UINT32_MAX && w <= 256) {
                    printf("  uncapped it stayed within 256, so this case misses unchecked code\n");
                    ok = false;
                }
            }
        }
        if (!s.fast && all != 0) ok = false;  // no PACE here, so the interpreter stops on time
    }
    printf(ok ? "jit-overshoot: ok\n" : "jit-overshoot: FAILED\n");
    return ok ? 0 : 1;
}
