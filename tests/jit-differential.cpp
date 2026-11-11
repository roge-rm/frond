// Differential fuzzing of the Fast engine (Dynarmic JIT) against the interpreter (Exact).
//
// Each case comes from one seed: a random machine state (registers of every mode, CPSR and
// SPSRs, flags, CP15 fault and domain registers, iWMMXt registers, data memory, page
// attributes) and a random block of ARMv5TE and Thumb segments, weighted towards LDM/STM,
// conditional execution, shifts by register, PC writes, Thumb switching, transfers that
// fault part-way and iWMMXt instructions (data processing and register transfers run in the
// JIT's cp0 and cp1 slots, loads, stores and the rest in the interpreter). Both
// engines run the case from the same state until they stop on a B . (the block's end, or
// the reset, prefetch abort, IRQ and FIQ vectors; undefined and SWI return to the next
// instruction and a data abort skips the one that aborted). Then their registers, CPSR,
// SPSRs, memory, fault status, domains, CPAR and iWMMXt state are compared. No timing is
// compared. Every Fast call is also checked against cpuMaxOvershootCycles. A failing case is
// cut down to the shortest block that still fails the same way and printed with its seed,
// which reproduces it exactly.
//
// build/test-jit-differential               directed tests and the fixed seeds 1 to 30000
// build/test-jit-differential --seed N      one case (--show prints the block first, --trace
//                                           steps both engines through it)
// build/test-jit-differential --seeds A:B [--duration S] [--no-directed]
//                                           seeds A to B-1 (A: for no end), for at most S s
// --max-unchecked L sets the JIT's unchecked limit (FROND_JIT_MAX_UNCHECKED) for the run.
// --hot N compiles a block once its start has been reached N times (FROND_JIT_HOT), with the
// code before that in the interpreter. The default is 1, every block at once.
// Fast compiles in sync mode (each block where it was asked for), unless --thread, which
// compiles in the background as in normal play, so where runs stop doesn't repeat.
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "CPU.h"
#include "MMU.h"
#include "RAM.h"
#include "mem.h"
#include "pace_patch.h"
#include "memory_buffer.h"
#include "patch_dispatch.h"
#include "jit_dynarmic.h"
#include "pxa270_WMMX.h"
#include "savestate/savestateAll.h"
#include "system_state.h"

namespace Dynarmic::A32 {
std::string DisassembleArm(std::uint32_t instruction);
std::string DisassembleThumb16(std::uint16_t instruction);
}  // namespace Dynarmic::A32

namespace {

// Physical RAM at 0, mapped one to one by small pages (one coarse table per MB).
constexpr uint32_t kRamSize = 0x40000;
constexpr uint32_t kSetup = 0x1000, kProbe = 0x1800, kTable = 0x3000;
constexpr uint32_t kL1 = 0x4000, kL2a = 0x8000, kL2b = 0x8400;
constexpr uint32_t kCode = 0x10000, kCodeEnd = 0x14000;
constexpr uint32_t kTrapWord = 0xe7fee7fe;  // ARM: undefined; Thumb: B .
constexpr uint32_t kSpinArm = 0xeafffffe;
constexpr uint16_t kSpinThumb = 0xe7fe;
constexpr uint32_t kAlias = 0x00100000;  // domain 1 alias of 0x20000-0x2ffff

enum : uint32_t { USR = 0x10, FIQ = 0x11, IRQ = 0x12, SVC = 0x13, ABT = 0x17, UND = 0x1b, SYS = 0x1f };

// ---------------------------------------------------------------- random numbers

struct Rng {
    uint64_t s[4];
    explicit Rng(uint64_t seed) {
        for (auto& x : s) {
            seed += 0x9e3779b97f4a7c15ull;
            uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
            x = z ^ (z >> 31);
        }
    }
    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    uint64_t next() {
        const uint64_t r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return r;
    }
    uint32_t u32() { return uint32_t(next() >> 32); }
    uint32_t below(uint32_t n) { return uint32_t(((next() >> 32) * uint64_t(n)) >> 32); }
    bool chance(uint32_t percent) { return below(100) < percent; }
    template <class T, size_t N>
    T pick(const T (&a)[N]) { return a[below(N)]; }
};

// ---------------------------------------------------------------- machines

struct WmmxSaver {
    Pxa270wmmx* w;
    template <class T>
    void Save(T& s) { pxa270wmmxSave(w, s); }
};

// A device page at kDevice (mapped at virtual 0x2f000, 0x30000 unmapped) that
// counts the accesses it takes, as a FIFO or doorbell register would see them. Reads return
// the count so far. Offsets 0x800-0x80f refuse every access (a bus error, no side effect).
constexpr uint32_t kDevice = 0x40000000;
struct Device {
    uint32_t reads = 0, writes = 0;
};

bool deviceAccess(void* userData, uint32_t pa, uint_fast8_t size, bool write, void* buf) {
    Device& d = *static_cast<Device*>(userData);
    const uint32_t off = pa - kDevice;
    if (off >= 0x800 && off < 0x810) return false;
    if (write) {
        d.writes++;
    } else {
        d.reads++;
        memcpy(buf, &d.reads, size < 4 ? size : 4);
        if (size > 4) memset((uint8_t*)buf + 4, 0, size - 4);
    }
    return true;
}

struct Machine {
    bool fast = false;
    ArmCpu* cpu = nullptr;
    uint8_t* ram = nullptr;
    Pxa270wmmx* wmmx = nullptr;
    ArmMem* mem = nullptr;
    Device* device = nullptr;
};

Machine makeMachine(bool fast) {
    unsetenv("FROND_JIT");
    if (!cpuSetFastEngine(fast)) {
        fprintf(stderr, "this core has no Fast engine\n");
        exit(1);
    }
    Machine m;
    m.fast = fast;
    ArmMem* mem = memInit();
    MemoryBuffer buf{};
    if (!memoryBufferAllocate(&buf, kRamSize)) abort();
    ArmRam* ram = ramInit(mem, nullptr, 0, kRamSize, &buf, true);
    ramSetNoFramebuffer(ram);
    m.ram = buf.buffer;
    m.mem = mem;
    static PacePatch pace{};
    PatchDispatch* pd = initPatchDispatch();
    m.cpu = cpuInit(kSetup, mem, ARM_MEMORY_SYSTEM_MMU, true, false, -1, 0x69054114, 0x0b16a16a, pd, &pace,
                    createSystemState());
    patchDispatchSetCpu(pd, m.cpu);
    m.wmmx = pxa270wmmxInit(m.cpu);
    m.device = new Device;
    if (!memRegionAdd(mem, kDevice, 0x1000, deviceAccess, m.device)) abort();
    return m;
}

// At a B . the current mode can fetch: a spin on a page it can't read takes the prefetch
// abort first (user code with S set), and the run goes on to the abort vector.
bool atSpin(Machine& m) {
    const uint32_t pc = cpuGetRegisters(m.cpu)[15] & ~1u;
    if (pc >= kRamSize - 4) return false;
    const bool privileged = (cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 0x1f) != USR;
    if (!MMU_TRANSLATE_RESULT_OK(mmuTranslateInspectAccess(cpuGetMMU(m.cpu), pc, privileged, false))) return false;
    if (cpuIsThumb(m.cpu)) {
        uint16_t h;
        memcpy(&h, m.ram + pc, 2);
        return h == kSpinThumb;
    }
    uint32_t w;
    memcpy(&w, m.ram + pc, 4);
    return !(pc & 3) && w == kSpinArm;
}

bool gThread = false;  // --thread: Fast compiles on the compile thread

struct RunResult {
    bool stopped = false;
    uint64_t ran = 0;        // instructions run, for Exact (which steps one at a time)
    uint32_t worstOver = 0;  // most a cpuCycle call ran past its budget
};

// Exact steps one instruction at a time, so ran counts the instructions to the stop. Fast
// runs budgets from the schedule, so runs start and end in the middle of blocks.
RunResult run(Machine& m, uint64_t scheduleSeed, uint64_t limit) {
    RunResult r;
    Rng s(scheduleSeed);
    while (r.ran < limit) {
        if (atSpin(m)) {
            r.stopped = true;
            return r;
        }
        uint32_t b = 1;
        if (m.fast) {
            const uint32_t k = s.below(100);
            b = k < 10 ? 1 : k < 30 ? 2 + s.below(19) : k < 70 ? 21 + s.below(180) : k < 95 ? 201 + s.below(1800) : 5000;
        }
        // With the compile thread, sometimes give it time so blocks install between calls.
        if (m.fast && gThread && s.below(4) == 0) std::this_thread::sleep_for(std::chrono::microseconds(s.below(200)));
        const uint32_t ran = cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(m.cpu, b);
        if (ran > b && ran - b > r.worstOver) r.worstOver = ran - b;
        r.ran += ran ? ran : 1;
    }
    r.stopped = atSpin(m);
    return r;
}

// ---------------------------------------------------------------- state

struct Bank {
    uint32_t r[7];  // r8-r14 (FIQ and user), or r13, r14 in r[5], r[6]
    uint32_t spsr;
};

struct Snapshot {
    uint32_t r[16], cpsr;
    uint32_t usr[7], fiq[7], spsrFiq;
    uint32_t banked[4][3];  // IRQ, SVC, ABT, UND: r13, r14, SPSR
    uint32_t fsr, far, dacr, cpar;
    uint32_t deviceReads, deviceWrites;
    std::vector<uint8_t> wmmx, ram;
    bool stopped;
};

Snapshot snapshot(Machine& m, bool stopped) {
    Snapshot s{};
    s.stopped = stopped;
    memcpy(s.r, cpuGetRegisters(m.cpu), sizeof(s.r));
    s.cpsr = cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR);
    s.ram.assign(m.ram, m.ram + kRamSize);
    s.deviceReads = m.device->reads;
    s.deviceWrites = m.device->writes;
    Savestate<ChunkType> st;
    WmmxSaver saver{m.wmmx};
    if (!st.Save(saver)) abort();
    s.wmmx.assign((uint8_t*)st.GetBuffer(), (uint8_t*)st.GetBuffer() + st.GetSize());
    s.cpar = cpuGetCPAR(m.cpu);
    s.dacr = mmuGetDomainCfg(cpuGetMMU(m.cpu));
    // Banks, by switching mode (this changes the CPU, so it comes last).
    auto mode = [&](uint32_t md) { cpuSetReg(m.cpu, ARM_REG_NUM_CPSR, md | 0xc0); };
    mode(SYS);
    for (int i = 0; i < 7; ++i) s.usr[i] = cpuGetRegExternal(m.cpu, 8 + i);
    mode(FIQ);
    for (int i = 0; i < 7; ++i) s.fiq[i] = cpuGetRegExternal(m.cpu, 8 + i);
    s.spsrFiq = cpuGetRegExternal(m.cpu, ARM_REG_NUM_SPSR);
    const uint32_t modes[4] = {IRQ, SVC, ABT, UND};
    for (int k = 0; k < 4; ++k) {
        mode(modes[k]);
        s.banked[k][0] = cpuGetRegExternal(m.cpu, 13);
        s.banked[k][1] = cpuGetRegExternal(m.cpu, 14);
        s.banked[k][2] = cpuGetRegExternal(m.cpu, ARM_REG_NUM_SPSR);
    }
    // FSR and FAR through the probe, in SVC with both domains open.
    mmuSetDomainCfg(cpuGetMMU(m.cpu), 0x5);
    cpuSetCPAR(m.cpu, 0);
    mode(SVC);
    cpuSetReg(m.cpu, 15, kProbe);
    for (int i = 0; i < 64 && !atSpin(m); ++i) cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(m.cpu, 1);
    if (!atSpin(m) || (cpuGetRegisters(m.cpu)[15] & ~1u) != kProbe + 8) {
        fprintf(stderr, "probe did not finish (pc %08x)\n", cpuGetRegisters(m.cpu)[15]);
        abort();
    }
    s.fsr = cpuGetRegExternal(m.cpu, 0);
    s.far = cpuGetRegExternal(m.cpu, 1);
    return s;
}

std::vector<std::string> differences(const Snapshot& a, const Snapshot& b) {
    std::vector<std::string> d;
    char line[160];
    auto cmp = [&](const char* name, uint32_t x, uint32_t y) {
        if (x == y) return;
        snprintf(line, sizeof(line), "%s: exact %08x fast %08x", name, x, y);
        d.push_back(line);
    };
    if (a.stopped != b.stopped) {
        snprintf(line, sizeof(line), "stopped: exact %d fast %d", a.stopped, b.stopped);
        d.push_back(line);
    }
    for (int i = 0; i < 16; ++i) {
        snprintf(line, sizeof(line), "r%d", i);
        cmp(std::string(line).c_str(), a.r[i], b.r[i]);
    }
    cmp("cpsr", a.cpsr, b.cpsr);
    for (int i = 0; i < 7; ++i) {
        snprintf(line, sizeof(line), "r%d_usr", 8 + i);
        cmp(std::string(line).c_str(), a.usr[i], b.usr[i]);
        snprintf(line, sizeof(line), "r%d_fiq", 8 + i);
        cmp(std::string(line).c_str(), a.fiq[i], b.fiq[i]);
    }
    cmp("spsr_fiq", a.spsrFiq, b.spsrFiq);
    const char* names[4] = {"irq", "svc", "abt", "und"};
    for (int k = 0; k < 4; ++k) {
        const char* what[3] = {"r13", "r14", "spsr"};
        for (int j = 0; j < 3; ++j) {
            snprintf(line, sizeof(line), "%s_%s", what[j], names[k]);
            cmp(std::string(line).c_str(), a.banked[k][j], b.banked[k][j]);
        }
    }
    cmp("fsr", a.fsr, b.fsr);
    cmp("far", a.far, b.far);
    cmp("dacr", a.dacr, b.dacr);
    cmp("cpar", a.cpar, b.cpar);
    cmp("device reads", a.deviceReads, b.deviceReads);
    cmp("device writes", a.deviceWrites, b.deviceWrites);
    if (a.wmmx != b.wmmx) d.push_back("iwmmxt registers differ");
    int shown = 0;
    for (uint32_t i = 0; i < kRamSize; i += 4) {
        uint32_t x, y;
        memcpy(&x, &a.ram[i], 4);
        memcpy(&y, &b.ram[i], 4);
        if (x == y) continue;
        if (shown++ < 8) {
            snprintf(line, sizeof(line), "mem %08x: exact %08x fast %08x", i, x, y);
            d.push_back(line);
        }
    }
    if (shown > 8) {
        snprintf(line, sizeof(line), "... %d memory words differ", shown);
        d.push_back(line);
    }
    return d;
}

// ---------------------------------------------------------------- encoders

uint32_t armImm(uint32_t v, bool* ok = nullptr) {
    for (uint32_t rot = 0; rot < 16; ++rot) {
        const uint32_t x = (v << (2 * rot)) | (rot ? v >> (32 - 2 * rot) : 0);
        if (x < 256) {
            if (ok) *ok = true;
            return (rot << 8) | x;
        }
    }
    if (ok) *ok = false;
    return 0;
}

constexpr uint32_t AL = 0xe0000000;
uint32_t dpImm(uint32_t cond, uint32_t op, bool s, uint32_t rn, uint32_t rd, uint32_t imm12) {
    return cond | 0x02000000 | (op << 21) | (s << 20) | (rn << 16) | (rd << 12) | imm12;
}
uint32_t movImm(uint32_t rd, uint32_t v) { return dpImm(AL, 13, false, 0, rd, armImm(v)); }
uint32_t orrImm(uint32_t rd, uint32_t v) { return dpImm(AL, 12, false, rd, rd, armImm(v)); }
uint32_t strImm(uint32_t rd, uint32_t rn, int32_t off) {
    return AL | 0x05000000 | (off >= 0 ? 1u << 23 : 0) | (rn << 16) | (rd << 12) | uint32_t(off < 0 ? -off : off);
}

// ---------------------------------------------------------------- blocks

enum Fix : uint8_t {
    NONE,
    ARM_B,        // B or BL, imm24 to the target
    ARM_BLX,      // BLX imm to a Thumb target
    ARM_ADDPC,    // ADD pc, pc, #imm to the target
    ARM_ABS_MOV,  // three instructions: MOV rT, #0x10000; ORR rT, #hi; ORR rT, #lo (+T bit, +extra)
    ARM_ABS_ORR1,
    ARM_ABS_ORR2,
    T_REL_MOV,  // four: MOV rT, #d >> 6; LSL rT, #6; ADD rT, #d & 63; ADD rT, pc
    T_REL_LSL,
    T_REL_ADD,
    T_REL_PC,
    T_BCOND,
    T_B,
    T_BL1,   // BL or BLX pair
    T_BL2,
    T_BLX2,
    ARM_SPSR_T,  // MOV rS, #psr for an exception return: T added when the target is Thumb
};

struct Ins {
    uint32_t bits;
    bool thumb;
    Fix fix = NONE;
    uint8_t extra = 0;  // ARM_ABS: added to the target (SUBS pc, rT, #4 setups)
    bool noT = false;   // ARM_ABS: no T bit in the address (exception returns take T from the SPSR)
};

struct Item {
    std::string kind;
    std::vector<Ins> ins;
    int target = -1;
    bool removable = true;
    bool removed = false;
    bool thumb = false;
};

struct Case {
    uint64_t seed = 0;
    std::vector<Item> items;
    std::vector<uint8_t> data;  // 0x20000-0x2ffff
    uint32_t table[256] = {};
    uint32_t tableWords = 0;
    uint32_t r0to7[8] = {};
    uint32_t cpsr = 0;
    bool codeCacheable = false;
    bool alignCheck = false;
    // The device page at 0x2f000 (0x30000 stays unmapped), so a transfer that crosses from
    // it into an abort shows whether the device saw its first words once, as in Exact.
    bool devicePage = true;
    uint32_t dacr = 0;
    uint32_t sr = 0x200;  // the control register's S (0x100) and R (0x200) bits
    std::vector<uint32_t> swiHandler;  // replaces the SWI vector's movs pc, lr when not empty
    // Maps the page tables at 0x8000 (privileged only), so a case can remap pages.
    bool tablesPage = false;
};

struct Assembled {
    std::vector<uint8_t> image;  // all of RAM
    uint32_t start = 0;
    bool thumb = false;
    std::vector<std::pair<uint32_t, std::pair<int, int>>> where;  // address, item, instruction
};

int resolve(const Case& c, int t) {
    while (t < int(c.items.size()) && c.items[t].removed) ++t;
    return t;
}

uint32_t ptePage(uint32_t pa, uint32_t ap, bool cacheable) {
    return pa | (ap << 10) | (ap << 8) | (ap << 6) | (ap << 4) | (cacheable ? 8 : 0) | 4 | 2;
}

Assembled assemble(const Case& c) {
    Assembled a;
    a.image.assign(kRamSize, 0);
    auto put32 = [&](uint32_t addr, uint32_t v) { memcpy(&a.image[addr], &v, 4); };
    auto put16 = [&](uint32_t addr, uint16_t v) { memcpy(&a.image[addr], &v, 2); };
    for (uint32_t p = 0; p < kRamSize; p += 4) put32(p, kTrapWord);
    // Vectors: undefined and SWI return to the next instruction, a data abort skips the
    // instruction that aborted, the others stop the run.
    for (uint32_t v = 0; v < 0x20; v += 4) put32(v, kSpinArm);
    put32(0x04, 0xe1b0f00e);   // movs pc, lr
    put32(0x08, 0xe1b0f00e);   // movs pc, lr
    put32(0x10, 0xea00003a);   // b 0x100
    if (!c.swiHandler.empty()) {
        put32(0x08, 0xea000044);  // b 0x120
        for (size_t i = 0; i < c.swiHandler.size(); ++i) put32(0x120 + 4 * i, c.swiHandler[i]);
    }
    put32(0x100, 0xe14fd000);  // mrs sp, spsr
    put32(0x104, 0xe31d0020);  // tst sp, #0x20
    put32(0x108, 0x025ef004);  // subeqs pc, lr, #4
    put32(0x10c, 0x125ef006);  // subnes pc, lr, #6
    // Setup routine: CP15, iWMMXt, then the banks, from the table at kTable (r7).
    static const uint32_t setup[] = {
        0xe3a07a03,  // mov r7, #0x3000
        0xe8b7003f,  // ldmia r7!, {r0-r5}
        0xee0f5f11,  // mcr p15, 0, r5, c15, c1, 0 (CPAR)
        0xee023f10,  // TTB
        0xee032f10,  // DACR
        0xee080f17,  // TLB flush
        0xee014f10,  // control
        0xe1a00000, 0xe1a00000, 0xe1a00000,
        0xee050f10,  // FSR
        0xee061f10,  // FAR
    };
    uint32_t p = kSetup;
    for (uint32_t w : setup) put32(p, w), p += 4;
    for (uint32_t i = 0; i < 16; ++i) put32(p, 0xecf70102 | (i << 12)), p += 4;  // wldrd wRi, [r7], #8
    put32(p, 0xe8b7003f), p += 4;  // ldmia r7!, {r0-r5}
    const uint32_t wc[6] = {2, 3, 8, 9, 10, 11};
    for (uint32_t i = 0; i < 6; ++i) put32(p, 0xee000110 | (wc[i] << 16) | (i << 12)), p += 4;  // tmcr
    put32(p, 0xe4970004), p += 4;  // ldr r0, [r7], #4
    put32(p, 0xee0f0f11), p += 4;  // final CPAR
    const uint32_t modes[5] = {FIQ, IRQ, ABT, UND, SVC};
    for (uint32_t md : modes) {
        put32(p, 0xe321f0c0 | md), p += 4;                          // msr cpsr_c, #md|I|F
        put32(p, md == FIQ ? 0xe8b77f00 : 0xe8b76000), p += 4;      // ldmia r7!, {r8-r14} / {r13, r14}
        put32(p, 0xe4970004), p += 4;                               // ldr r0, [r7], #4
        put32(p, 0xe16ff000), p += 4;                               // msr spsr_fsxc, r0
    }
    put32(p, 0xe321f0df), p += 4;  // sys
    put32(p, 0xe8b77f00), p += 4;  // user r8-r14
    put32(p, kSpinArm);
    put32(kProbe, 0xee150f10);      // mrc p15, 0, r0, c5, c0, 0
    put32(kProbe + 4, 0xee161f10);  // mrc p15, 0, r1, c6, c0, 0
    put32(kProbe + 8, kSpinArm);
    for (uint32_t i = 0; i < c.tableWords; ++i) put32(kTable + 4 * i, c.table[i]);
    // Page tables. Section 0 by small pages, section 1 (domain 1) aliases 0x20000-0x2ffff.
    for (uint32_t i = 0; i < 0x4000; i += 4) put32(kL1 + i, 0);
    for (uint32_t i = 0; i < 0x800; i += 4) put32(kL2a + i, 0);
    put32(kL1, kL2a | 0x11);
    put32(kL1 + 4, kL2b | 0x11 | (1 << 5));
    auto page = [&](uint32_t va, uint32_t ap, bool cacheable) {
        put32(kL2a + (va >> 12) * 4, ptePage(va, ap, cacheable));
    };
    page(0x0000, 0, true);
    page(0x1000, 0, true);
    page(0x3000, 0, true);
    for (uint32_t va = kCode; va < 0x20000; va += 0x1000) page(va, 0, c.codeCacheable);
    for (uint32_t va = 0x20000; va < 0x28000; va += 0x1000) page(va, 3, true);
    page(0x28000, 1, true);  // privileged only
    page(0x29000, 2, true);  // user read-only
    page(0x2b000, 3, false);
    page(0x2c000, 0, true);  // read-only
    page(0x2d000, 3, true);
    if (c.devicePage) put32(kL2a + 0x2f * 4, ptePage(kDevice, 3, false));  // 0x30000 stays unmapped
    if (c.tablesPage) page(kL2a, 1, false);
    for (uint32_t i = 0; i < 16; ++i) put32(kL2b + i * 4, ptePage(0x20000 + i * 0x1000, 3, true));
    memcpy(&a.image[0x20000], c.data.data(), c.data.size());

    // The block: segments start word aligned.
    std::vector<uint32_t> addr(c.items.size() + 1, 0), insAddr;
    std::vector<std::vector<uint32_t>> at(c.items.size());
    uint32_t pc = kCode;
    for (size_t i = 0; i < c.items.size(); ++i) {
        const Item& it = c.items[i];
        if (i && it.thumb != c.items[i - 1].thumb) pc = (pc + 3) & ~3u;
        addr[i] = pc;
        if (it.removed) continue;
        for (const Ins& in : it.ins) {
            at[i].push_back(pc);
            pc += in.thumb ? 2 : 4;
        }
    }
    if (pc > kCodeEnd) {
        fprintf(stderr, "block too long\n");
        abort();
    }
    for (size_t i = 0; i < c.items.size(); ++i) {
        const Item& it = c.items[i];
        if (it.removed) continue;
        const int t = it.target >= 0 ? resolve(c, it.target) : -1;
        const uint32_t ta = t >= 0 ? addr[t] : 0;
        const bool tt = t >= 0 && c.items[t].thumb;
        for (size_t k = 0; k < it.ins.size(); ++k) {
            const Ins& in = it.ins[k];
            const uint32_t A = at[i][k];
            uint32_t w = in.bits;
            switch (in.fix) {
                case NONE: break;
                case ARM_B: w |= ((ta - (A + 8)) >> 2) & 0xffffff; break;
                case ARM_BLX: {
                    const uint32_t off = ta - (A + 8);
                    w = 0xfa000000 | (((off >> 1) & 1) << 24) | ((off >> 2) & 0xffffff);
                    break;
                }
                case ARM_ADDPC: {
                    bool ok;
                    const int32_t d = int32_t(ta - (A + 8));  // -4 when the target is the next word
                    if (d < 0) w = (w & ~0x01e00000u) | 0x00400000;  // SUB pc, pc, #4
                    w |= armImm(uint32_t(d < 0 ? -d : d), &ok);
                    if (!ok) {
                        fprintf(stderr, "ADD pc out of range\n");
                        abort();
                    }
                    break;
                }
                case ARM_ABS_MOV: w |= armImm(0x10000); break;
                case ARM_ABS_ORR1: w |= armImm((ta + (in.noT ? 0 : tt) + in.extra) & 0xff00); break;
                case ARM_ABS_ORR2: w |= armImm((ta + (in.noT ? 0 : tt) + in.extra) & 0xff); break;
                case ARM_SPSR_T: w |= tt ? 0x20 : 0; break;
                case T_REL_MOV:
                case T_REL_LSL:
                case T_REL_ADD: {
                    const uint32_t pcAdd = at[i][k + (T_REL_PC - in.fix)];
                    const uint32_t d = ta + tt - (pcAdd + 4);
                    if (d >= 0x4000) abort();
                    if (in.fix == T_REL_MOV) w |= (d >> 6) & 0xff;
                    if (in.fix == T_REL_ADD) w |= d & 63;
                    break;
                }
                case T_REL_PC: break;
                case T_BCOND: {
                    const int32_t off = int32_t(ta - (A + 4));
                    if (off < -256 || off > 254) {
                        fprintf(stderr, "Thumb B<cond> out of range\n");
                        abort();
                    }
                    w |= (uint32_t(off) >> 1) & 0xff;
                    break;
                }
                case T_B: {
                    const int32_t off = int32_t(ta - (A + 4));
                    if (off < -2048 || off > 2046) abort();
                    w |= (uint32_t(off) >> 1) & 0x7ff;
                    break;
                }
                case T_BL1: {
                    const uint32_t off = tt ? ta - (A + 4) : ta - ((A + 4) & ~3u);
                    w |= (off >> 12) & 0x7ff;
                    break;
                }
                case T_BL2: w |= ((ta - (A + 2)) >> 1) & 0x7ff; break;
                case T_BLX2: w |= ((ta - ((A + 2) & ~3u)) >> 1) & 0x7fe; break;
            }
            if (in.thumb) put16(A, uint16_t(w));
            else put32(A, w);
            a.where.push_back({A, {int(i), int(k)}});
        }
    }
    const int first = resolve(c, 0);
    a.start = addr[first];
    a.thumb = c.items[first].thumb;
    return a;
}

// ---------------------------------------------------------------- generator

struct Gen {
    Rng& g;
    Case& c;
    bool thumb = false;
    uint32_t modes = 0;  // modes the block may be in at this point (bit per mode & 15)
    std::vector<int> segStart, segEnd;  // flat item index ranges; segEnd is the terminator
    std::vector<bool> segThumb;
    int seg = 0, index = 0;

    static uint32_t bit(uint32_t mode) { return 1u << (mode & 15); }
    bool privileged() const { return !(modes & bit(USR)); }
    bool hasSpsr() const { return privileged() && !(modes & bit(SYS)); }

    uint32_t reg() { return g.below(15); }  // r0-r14
    uint32_t lo() { return g.below(8); }
    uint32_t cond() { return g.chance(55) ? AL : g.below(14) << 28; }

    uint32_t dataAddr(uint32_t align) {
        static const uint32_t edges[] = {0x20000, 0x28000, 0x29000, 0x2a000, 0x2b000,
                                         0x2c000, 0x2d000, 0x2e000, 0x21000, 0x24000};
        uint32_t a = g.chance(60) ? edges[g.below(10)] + g.below(97) * 4 - 192 : 0x20000 + g.below(0xe000);
        if (!g.chance(8)) a &= ~(align - 1);
        else a += g.below(4);
        return a;
    }

    // Where PC-write setups store the target: read-write for every mode, room either side.
    uint32_t safeAddr() { return 0x20100 + g.below(0x1f00) * 4; }

    void armConst(Item& it, uint32_t rd, uint32_t v) {
        it.ins.push_back({movImm(rd, v & 0xff000000), false});
        if (v & 0x00ff0000) it.ins.push_back({orrImm(rd, v & 0x00ff0000), false});
        if (v & 0x0000ff00) it.ins.push_back({orrImm(rd, v & 0x0000ff00), false});
        if (v & 0x000000ff) it.ins.push_back({orrImm(rd, v & 0x000000ff), false});
    }
    void thumbConst(Item& it, uint32_t rd, uint32_t v) {  // v in 0x20000-0x2ffff
        it.ins.push_back({0x2000 | (rd << 8) | ((v >> 10) & 0xff), true});
        it.ins.push_back({0x0000 | (10 << 6) | (rd << 3) | rd, true});  // lsl rd, rd, #10
        for (uint32_t left = v & 0x3ff; left;) {
            const uint32_t n = std::min(left, 255u);
            it.ins.push_back({0x3000 | (rd << 8) | n, true});
            left -= n;
        }
    }
    void armAbs(Item& it, uint32_t rd, uint8_t extra = 0, bool noT = false) {
        it.ins.push_back({AL | 0x03a00000 | (rd << 12), false, ARM_ABS_MOV});
        it.ins.push_back({AL | 0x03800000 | (rd << 16) | (rd << 12), false, ARM_ABS_ORR1, extra, noT});
        it.ins.push_back({AL | 0x03800000 | (rd << 16) | (rd << 12), false, ARM_ABS_ORR2, extra, noT});
    }
    void thumbRel(Item& it, uint32_t rd) {
        it.ins.push_back({0x2000u | (rd << 8), true, T_REL_MOV});
        it.ins.push_back({0x0000u | (6 << 6) | (rd << 3) | rd, true, T_REL_LSL});
        it.ins.push_back({0x3000u | (rd << 8), true, T_REL_ADD});
        it.ins.push_back({0x4478u | rd, true, T_REL_PC});
    }

    // Later items: same set only, or either; within maxAhead items when set.
    int target(bool sameSet, bool otherSet, int maxAhead = 0, bool sameSegment = false) {
        std::vector<int> ok;
        const int last = int(c.items.size());  // generated in order, so use the plan
        for (int j = index + 1; j <= segEnd.back(); ++j) {
            if (maxAhead && j - index > maxAhead) break;
            const int s = segmentOf(j);
            if (sameSegment && s != seg) break;
            const bool t = segThumb[s];
            if ((t == thumb && sameSet) || (t != thumb && otherSet)) ok.push_back(j);
        }
        (void)last;
        if (ok.empty()) return -1;
        // Favour near targets.
        const uint32_t k = g.below(uint32_t(ok.size()));
        return ok[g.chance(60) ? std::min<uint32_t>(k, g.below(4)) : k];
    }
    int segmentOf(int j) const {
        for (size_t s = 0; s < segStart.size(); ++s)
            if (j >= segStart[s] && j <= segEnd[s]) return int(s);
        return -1;
    }

    // ------------------------------------------------ ARM items

    void armDataProcessing(Item& it) {
        uint32_t op = g.below(16);
        const bool test = op >= 8 && op <= 11;
        const bool s = test || g.chance(50);
        const uint32_t rd = test ? 0 : reg();
        const uint32_t rn = (op == 13 || op == 15) ? 0 : g.chance(5) ? 15 : reg();
        const uint32_t form = g.below(10);
        uint32_t w = cond() | (op << 21) | (s << 20) | (rn << 16) | (rd << 12);
        if (form < 3) {
            it.kind = "dp imm";
            w |= 0x02000000 | (g.below(16) << 8) | g.below(256);
        } else if (form < 6 || rn == 15) {
            it.kind = "dp shift imm";
            w |= (g.below(32) << 7) | (g.below(4) << 5) | (g.chance(5) ? 15 : reg());
        } else {
            it.kind = "dp shift reg";
            const uint32_t rs = reg();
            if (g.chance(40)) it.ins.push_back({movImm(rs, g.below(g.chance(70) ? 34 : 256)), false});
            w |= (rs << 8) | (g.below(4) << 5) | 0x10 | reg();
        }
        it.ins.push_back({w, false});
    }

    void armMultiply(Item& it) {
        uint32_t rd = reg(), rn = reg(), rs = reg(), rm = reg();
        const uint32_t k = g.below(10);
        if (k < 3) {  // MUL / MLA
            while (rm == rd) rm = reg();
            it.kind = "mul";
            const bool acc = g.chance(50);
            it.ins.push_back({cond() | (acc << 21) | (g.chance(40) << 20) | (rd << 16) | ((acc ? rn : 0) << 12) |
                                  (rs << 8) | 0x90 | rm,
                              false});
        } else if (k < 6) {  // UMULL UMLAL SMULL SMLAL
            while (rn == rd) rn = reg();
            while (rm == rd || rm == rn) rm = reg();
            it.kind = "mull";
            it.ins.push_back({cond() | 0x00800000 | (g.below(4) << 21) | (g.chance(40) << 20) | (rd << 16) |
                                  (rn << 12) | (rs << 8) | 0x90 | rm,
                              false});
        } else if (k < 9) {  // v5TE DSP multiplies
            it.kind = "dsp mul";
            const uint32_t op = g.below(4), y = g.below(2), x = g.below(2);
            if (op == 2) while (rn == rd) rn = reg();
            uint32_t w = cond() | 0x01000080 | (op << 21) | (rd << 16) | (rs << 8) | (y << 6) | rm;
            if (op == 1) w = (w & ~0x20u) | (g.chance(50) ? 0x20 : 0) | ((w & 0x20) ? 0 : 0);  // SMLAW / SMULW
            if (op == 1) w |= ((w & 0x20) ? 0 : (rn << 12));
            else if (op != 3) w |= (x << 5) | (rn << 12);
            else w |= (x << 5);
            it.ins.push_back({w, false});
        } else {  // QADD QSUB QDADD QDSUB, CLZ
            if (g.chance(30)) {
                it.kind = "clz";
                it.ins.push_back({cond() | 0x016f0f10 | (rd << 12) | rm, false});
            } else {
                it.kind = "qadd";
                const uint32_t op = g.below(4);
                it.ins.push_back({cond() | 0x01000050 | (op << 21) | (rn << 16) | (rd << 12) | rm, false});
            }
        }
    }

    uint32_t armBase(Item& it, uint32_t align, uint32_t avoid = 16) {
        uint32_t rn = reg();
        while (rn == avoid) rn = reg();
        if (g.chance(65)) armConst(it, rn, g.chance(8) ? kAlias + (dataAddr(align) - 0x20000) : dataAddr(align));
        return rn;
    }

    void armLoadStore(Item& it) {
        const bool load = g.chance(55), byte = g.chance(25);
        const uint32_t rd = reg();
        const uint32_t rn = armBase(it, byte ? 1 : 4, 16);
        const bool p = g.chance(70), u = g.chance(60);
        bool w = !p ? false : g.chance(30);
        const bool wb = !p || w;
        if (wb && rn == rd) w = false;
        uint32_t ww = cond() | 0x04000000 | (p << 24) | (u << 23) | (byte << 22) | (w << 21) | (load << 20) |
                      (rn << 16) | (rd << 12);
        if ((!p || w) && rn == rd) ww &= ~0x01200000u, ww |= 0x01000000;  // no writeback
        if (g.chance(60)) {
            it.kind = load ? "ldr imm" : "str imm";
            ww |= g.chance(80) ? g.below(16) * (byte ? 1 : 4) : g.below(4096);
        } else {
            it.kind = load ? "ldr reg" : "str reg";
            uint32_t rm = reg();
            while (rm == rn) rm = reg();
            if (g.chance(60)) it.ins.push_back({movImm(rm, g.below(64)), false});
            ww |= 0x02000000 | (g.below(4) << 7) | (g.below(4) << 5) | rm;
        }
        it.ins.push_back({ww, false});
    }

    void armHalfword(Item& it) {
        const uint32_t sh = 1 + g.below(3);
        bool load = g.chance(60);
        uint32_t rd = reg();
        const bool dual = !load && sh >= 2;  // LDRD / STRD
        if (dual) rd = g.below(7) * 2;
        const uint32_t rn = armBase(it, dual ? 8 : 2, 16);
        const bool p = g.chance(70), u = g.chance(60);
        bool w = p && g.chance(30);
        if ((!p || w) && (rn == rd || (dual && rn == rd + 1))) {
            w = false;
            if (!p) return armLoadStore(it);
        }
        it.kind = dual ? (sh == 2 ? "ldrd" : "strd") : load ? "ldrh/sb/sh" : "strh";
        uint32_t ww = cond() | (p << 24) | (u << 23) | (w << 21) | (load << 20) | (rn << 16) | (rd << 12) | 0x90 |
                      (sh << 5);
        if (g.chance(65)) {
            const uint32_t off = g.below(32) * (dual ? 8 : 2) & 0xff;
            ww |= 0x00400000 | ((off >> 4) << 8) | (off & 15);
        } else {
            uint32_t rm = reg();
            while (rm == rn || (dual && (rm == rd || rm == rd + 1))) rm = reg();
            if (g.chance(60)) it.ins.push_back({movImm(rm, g.below(32) * 2), false});
            ww |= rm;
        }
        it.ins.push_back({ww, false});
    }

    void armSwap(Item& it) {
        it.kind = "swp";
        uint32_t rd = reg(), rm = reg();
        const uint32_t rn = armBase(it, 4, 16);
        while (rd == rn) rd = reg();
        while (rm == rn) rm = reg();
        it.ins.push_back({cond() | 0x01000090 | (g.chance(30) << 22) | (rn << 16) | (rd << 12) | rm, false});
    }

    uint32_t regList(uint32_t maxRegs = 15) {
        uint32_t list = 0;
        const uint32_t n = 1 + (g.chance(70) ? g.below(5) : g.below(maxRegs));
        for (uint32_t i = 0; i < n; ++i) list |= 1u << g.below(maxRegs);
        return list;
    }

    void armBlock(Item& it) {
        const bool load = g.chance(55);
        const uint32_t pu = g.below(4);
        uint32_t list = regList();
        const uint32_t rn = armBase(it, 4, 16);
        bool w = g.chance(45);
        bool s = false;
        if (g.chance(6) && privileged()) {  // user bank forms
            s = true;
            w = false;
        }
        if (load && w && (list & (1u << rn)) && !g.chance(10)) w = false;  // unpredictable, rare
        if (load && !w && g.chance(25)) list |= 1u << rn;                  // base in the list
        it.kind = std::string(load ? "ldm" : "stm") + (s ? "^" : "");
        it.ins.push_back({cond() | 0x08000000 | (pu << 23) | (s << 22) | (w << 21) | (load << 20) | (rn << 16) | list,
                          false});
    }

    void armStatus(Item& it) {
        const uint32_t k = g.below(10);
        if (k < 3) {
            const bool spsr = hasSpsr() && g.chance(40);
            it.kind = spsr ? "mrs spsr" : "mrs cpsr";
            it.ins.push_back({cond() | 0x010f0000 | (spsr << 22) | (reg() << 12), false});
        } else if (k < 5) {
            it.kind = "msr cpsr_f";
            if (g.chance(50)) it.ins.push_back({cond() | 0x0328f000 | armImm(g.below(32) << 27), false});
            else it.ins.push_back({cond() | 0x0128f000 | reg(), false});
        } else if (k < 8) {
            static const uint32_t ms[] = {USR, FIQ, IRQ, SVC, ABT, UND, SYS};
            const uint32_t md = g.chance(30) ? (modes & bit(USR) ? USR : SVC) : g.pick(ms);
            const uint32_t v = md | (g.below(4) << 6);
            if (privileged()) modes |= bit(md);
            else if (!(modes & bit(USR))) modes |= bit(md);
            modes |= bit(md) & (privileged() ? ~0u : 0u);
            if (modes & ~bit(USR)) modes |= bit(md);  // any privileged path may switch
            if (g.chance(50)) {
                it.kind = "msr cpsr_c imm";
                it.ins.push_back({cond() | 0x0321f000 | v, false});
            } else {
                it.kind = "msr cpsr_fc reg";
                const uint32_t rx = reg();
                it.ins.push_back({movImm(rx, v), false});
                if (g.chance(50)) it.ins.push_back({orrImm(rx, g.below(32) << 27), false});
                it.ins.push_back({cond() | 0x0129f000 | rx, false});
            }
        } else if (hasSpsr()) {
            it.kind = "msr spsr";
            // f, s and x fields only: a random c field could leave an invalid mode to return to.
            it.ins.push_back({cond() | 0x0160f000 | (g.below(8) << 17) | reg(), false});
        } else {
            it.kind = "mrs cpsr";
            it.ins.push_back({cond() | 0x010f0000 | (reg() << 12), false});
        }
    }

    void armCp15(Item& it) {
        const uint32_t k = g.below(11), rd = reg();
        if (k < 4) {
            it.kind = "mrc p15 c3";
            it.ins.push_back({cond() | 0x0e130f10 | (rd << 12) | (g.chance(20) ? g.below(16) : 0), false});
        } else if (k < 7) {
            it.kind = "mcr p15 c3";
            // Domain 0 stays a client: as a manager the guest could write its own code
            // without cleaning the caches, where Exact runs the cached words and Fast the new.
            static const uint32_t dacr[] = {0x5, 0xd, 0x1};
            it.ins.push_back({movImm(rd, g.pick(dacr)), false});
            it.ins.push_back({cond() | 0x0e030f10 | (rd << 12), false});
        } else if (k < 9) {
            it.kind = "mrc p15";
            static const uint32_t crn[] = {0, 1, 2, 5, 6, 13};
            it.ins.push_back({cond() | 0x0e100f10 | (g.pick(crn) << 16) | (rd << 12), false});
        } else if (k == 10) {
            // S or R, never both or neither: the code and vector pages are AP 0 and have to
            // stay readable for privileged modes.
            it.kind = "mcr p15 c1 s/r";
            it.ins.push_back({0xee110f10 | (rd << 12), false});                          // mrc c1
            it.ins.push_back({dpImm(AL, 14, false, rd, rd, armImm(0x300)), false});      // bic
            it.ins.push_back({dpImm(AL, 12, false, rd, rd, armImm(g.chance(50) ? 0x100 : 0x200)), false});
            it.ins.push_back({cond() | 0x0e010f10 | (rd << 12), false});                 // mcr c1
        } else {
            it.kind = "mcr p15 c7/c8";
            static const uint32_t ops[] = {0x0e070f15, 0x0e070f35, 0x0e070f9a, 0x0e070f3a, 0x0e080f17, 0x0e070f16};
            if (g.chance(50)) armConst(it, rd, kCode + g.below(0x400) * 4);
            it.ins.push_back({cond() | g.pick(ops) | (rd << 12), false});
        }
    }

    void armWmmx(Item& it) {
        const uint32_t k = g.below(10), cp = g.below(2) << 8;
        if (k < 3) {
            it.kind = "iwmmxt cdp";
            it.ins.push_back({cond() | 0x0e000000 | (g.below(16) << 20) | (g.below(16) << 16) | (g.below(16) << 12) |
                                  cp | (g.below(8) << 5) | g.below(16),
                              false});
        } else if (k < 6) {
            it.kind = "iwmmxt mcr/mrc";
            const bool read = g.chance(50);
            uint32_t rd = g.chance(8) && read ? 15 : reg();
            it.ins.push_back({cond() | 0x0e000010 | (g.below(8) << 21) | (read << 20) | (g.below(16) << 16) |
                                  (rd << 12) | cp | (g.below(8) << 5) | (g.chance(60) ? 0 : g.below(16)),
                              false});
        } else if (k < 7) {
            it.kind = "iwmmxt mcrr/mrrc";
            uint32_t rd = reg(), rn = reg();
            while (rn == rd) rn = reg();
            it.ins.push_back({cond() | 0x0c400000 | (g.chance(50) << 20) | (rn << 16) | (rd << 12) | cp | g.below(16),
                              false});
        } else {
            it.kind = "iwmmxt ldc/stc";
            const uint32_t rn = armBase(it, 8);
            const bool p = g.chance(70);
            const bool w = !p || g.chance(30);
            it.ins.push_back({cond() | 0x0c000000 | (p << 24) | (g.chance(60) << 23) | (g.chance(50) << 22) |
                                  (w << 21) | (g.chance(55) << 20) | (rn << 16) | (g.below(16) << 12) | cp |
                                  g.below(8),
                              false});
        }
    }

    // PC writes, ARM. Returns false when there is no target for the form picked.
    bool armPcWrite(Item& it) {
        const uint32_t k = g.below(20);
        if (k < 4) {
            const int t = target(true, false);
            if (t < 0) return false;
            it.kind = "b/bl";
            it.target = t;
            it.ins.push_back({cond() | 0x0a000000 | (g.chance(30) << 24), false, ARM_B});
        } else if (k < 6) {
            const int t = target(false, true);
            if (t < 0) return false;
            it.kind = "blx imm";
            it.target = t;
            it.ins.push_back({0xfa000000, false, ARM_BLX});
        } else if (k < 9) {
            const int t = target(true, true);
            if (t < 0) return false;
            const uint32_t rt = reg();
            it.kind = "bx/blx reg";
            it.target = t;
            armAbs(it, rt);
            it.ins.push_back({cond() | 0x012fff10 | (g.chance(40) ? 0x20 : 0) | rt, false});
        } else if (k < 11) {
            const int t = target(true, false);
            if (t < 0) return false;
            const uint32_t rt = reg();
            it.kind = "mov pc";
            it.target = t;
            armAbs(it, rt);
            it.ins.push_back({cond() | 0x01a0f000 | rt, false});
        } else if (k < 12) {
            const int t = target(true, false, 8, true);
            if (t < 0) return false;
            it.kind = "add pc, pc";
            it.target = t;
            it.ins.push_back({cond() | 0x028ff000, false, ARM_ADDPC});
        } else if (k < 15) {
            const int t = target(true, true);
            if (t < 0) return false;
            const uint32_t rt = reg();
            uint32_t rb = reg();
            while (rb == rt) rb = reg();
            it.kind = "ldr pc";
            it.target = t;
            armAbs(it, rt);
            armConst(it, rb, safeAddr());
            const bool post = g.chance(30);
            const uint32_t off = post ? 0 : g.below(16) * 4;
            it.ins.push_back({strImm(rt, rb, int32_t(off)), false});
            if (post) it.ins.push_back({cond() | 0x0490f000 | (rb << 16) | 4, false});  // ldr pc, [rb], #4
            else it.ins.push_back({cond() | 0x0590f000 | (rb << 16) | off, false});
        } else if (k < 18) {
            const int t = target(true, true);
            if (t < 0) return false;
            const uint32_t rt = reg();
            uint32_t rb = reg();
            while (rb == rt) rb = reg();
            uint32_t list = regList() & ~(1u << rb);
            const uint32_t n = __builtin_popcount(list) + 1;
            const uint32_t pu = g.below(4);  // DA IA DB IB
            const int32_t slot = pu == 0 ? 0 : pu == 1 ? 4 * (n - 1) : pu == 2 ? -4 : 4 * n;
            const bool w = g.chance(45), user = hasSpsr() && g.chance(15);
            it.kind = user ? "ldm^ pc" : "ldm pc";
            it.target = t;
            if (user) {  // exception return: a valid SPSR first
                const uint32_t md = exceptionReturnMode(it, t);
                (void)md;
            }
            armAbs(it, rt, 0, user);
            armConst(it, rb, safeAddr());
            it.ins.push_back({strImm(rt, rb, slot), false});
            it.ins.push_back({cond() | 0x08108000 | (pu << 23) | (user << 22) | (w << 21) | (rb << 16) | list, false});
        } else {
            if (!hasSpsr()) return false;
            const int t = target(true, true);
            if (t < 0) return false;
            it.kind = "movs/subs pc";
            it.target = t;
            exceptionReturnMode(it, t);
            const uint32_t rt = reg();
            if (g.chance(50)) {
                armAbs(it, rt, 0, true);
                it.ins.push_back({cond() | 0x01b0f000 | rt, false});
            } else {
                armAbs(it, rt, 4, true);
                it.ins.push_back({cond() | 0x0250f004 | (rt << 16), false});
            }
        }
        return true;
    }

    // MSR SPSR_c with a mode and the target's T bit, for MOVS/SUBS pc and LDM^ pc.
    uint32_t exceptionReturnMode(Item& it, int t) {
        static const uint32_t ms[] = {USR, USR, FIQ, IRQ, SVC, SVC, ABT, UND, SYS};
        const uint32_t md = g.pick(ms);
        (void)t;
        const uint32_t rs = reg();
        it.ins.push_back({movImm(rs, md | (g.below(4) << 6)), false, ARM_SPSR_T});
        it.ins.push_back({AL | 0x0161f000 | rs, false});  // msr spsr_c, rs
        modes |= bit(md);
        return md;
    }

    void armMisc(Item& it) {
        const uint32_t k = g.below(10);
        if (k < 5) {
            it.kind = "pld";
            it.ins.push_back({0xf550f000 | (g.chance(50) << 23) | (reg() << 16) | g.below(4096), false});
        } else if (k < 7) {
            it.kind = "swi";
            it.ins.push_back({cond() | 0x0f000000 | (g.u32() & 0xffffff), false});
        } else if (k < 8) {
            it.kind = "bkpt";
            it.ins.push_back({0xe1200070 | ((g.u32() & 0xfff) << 8) | g.below(16), false});
        } else {
            it.kind = "undefined";
            it.ins.push_back({cond() | 0x07f000f0 | (g.u32() & 0x000fff0f), false});
        }
    }

    // In user mode for sure: a jump into the privileged-only page, a prefetch abort when taken.
    // An iWMMXt CDP, MCR, MRC, MCRR or MRRC the JIT runs (pxa270wmmxJitValid), condition AL.
    uint32_t wmmxValid(uint32_t pattern, uint32_t random) {
        for (int tries = 0; tries < 200; ++tries) {
            const uint32_t w = AL | pattern | (g.u32() & random);
            if (pxa270wmmxJitValid(w)) return w;
        }
        return AL | 0x0e000000;  // wor wR0, wR0, wR0
    }
    uint32_t wmmxCdp() { return wmmxValid(0x0e000000, 0x00fff1ef); }
    uint32_t wmmxMrc(uint32_t rd) { return wmmxValid(0x0e100010 | (rd << 12), 0x00ef01ef); }

    // WLDR or WSTR the JIT runs: pre-indexed with or without writeback, or post-indexed,
    // either direction, any size, base rn, cond AL unless given.
    uint32_t wmmxMem(uint32_t rn, bool cp1, bool N, bool load, uint32_t imm8, uint32_t cnd = AL) {
        const bool p = g.chance(60), w = !p || g.chance(40);
        return cnd | 0x0c000000 | (p << 24) | (g.chance(60) << 23) | (N << 22) | (w << 21) | (load << 20) |
               (rn << 16) | (g.below(16) << 12) | (cp1 ? 0x100 : 0) | imm8;
    }
    // A base for a transfer of the size cp1 and N give, often near a page edge, sometimes
    // unaligned (dataAddr).
    uint32_t wmmxBase(Item& it, bool cp1, bool N) { return armBase(it, cp1 ? (N ? 8 : 4) : (N ? 2 : 1)); }

    // Valid iWMMXt instructions for the JIT's slots, with the state around them: saturating
    // arithmetic on boundary values, the flag registers, MRC into the CPSR flags followed by
    // a conditional instruction, CPAR changes, TMIA (interpreted) mixed in, and loads and
    // stores (every addressing mode and size, page edges, copy runs).
    void armWmmxValid(Item& it) {
        static const uint32_t edges[] = {0x7fffffff, 0x80000000, 0xffffffff, 0, 0x7f7f7f7f, 0x80808080,
                                         0x7fff8000, 0xff00ff00, 0x00010001, 0x01010101};
        const uint32_t k = g.below(16), cp = g.below(2) << 8;
        const uint32_t ra = reg(), rb = reg();
        if (k >= 12) {
            armWmmxMem(it, k - 12);
            return;
        }
        if (k < 3) {
            it.kind = "iwmmxt valid cdp";
            for (uint32_t n = 1 + g.below(4); n--;) {
                const uint32_t w = wmmxCdp();
                it.ins.push_back({g.chance(20) ? (w & 0x0fffffff) | cond() : w, false});
            }
        } else if (k < 5) {
            // TMCRR boundary values into one or two registers, then a saturating add,
            // subtract or pack on them, then wCSSF or wCASF out.
            it.kind = "iwmmxt saturation";
            const uint32_t wn = g.below(16), wm = g.below(16);
            for (uint32_t wr : {wn, wm}) {
                armConst(it, ra, g.chance(70) ? g.pick(edges) : g.u32());
                armConst(it, rb, g.chance(70) ? g.pick(edges) : g.u32());
                it.ins.push_back({AL | 0x0c400000 | (rb << 16) | (ra << 12) | wr, false});  // tmcrr
            }
            if (g.chance(30)) it.ins.push_back({AL | 0x0e020110, false});  // tmcr wCSSF, r0
            static const uint32_t sat[] = {1, 3};
            uint32_t op;
            if (g.chance(70))  // WADD, WSUB (cp1 op2 4, 5): size 0-2, unsigned or signed saturation
                op = 0x0e000100 | (((g.below(3) << 2) | g.pick(sat)) << 20) | ((4 + g.below(2)) << 5);
            else  // WPACK (cp0 op2 4): from halfword, word or doubleword
                op = 0x0e000000 | ((((1 + g.below(3)) << 2) | g.pick(sat)) << 20) | (4 << 5);
            it.ins.push_back({AL | op | (wn << 16) | (g.below(16) << 12) | wm, false});
            it.ins.push_back({AL | 0x0e100110 | ((g.chance(50) ? 2 : 3) << 16) | (ra << 12), false});  // tmrc
        } else if (k < 7) {
            // An MRC into the CPSR flags (TANDC, TORC, TEXTRC, TMRC and the rest with r15),
            // then a conditional MOV that shows them.
            it.kind = "iwmmxt mrc r15";
            uint32_t w;
            if (g.chance(60)) {  // TANDC, TORC (op2 1, 2) or TEXTRC (op2 3) on wCASF
                const uint32_t op2 = 1 + g.below(3);
                w = AL | 0x0e13f110 | ((g.below(3) << 1) << 21) | (op2 << 5) | (op2 == 3 ? g.below(8) : 0);
            } else {
                w = wmmxMrc(15);
            }
            if (g.chance(50)) it.ins.push_back({wmmxCdp(), false});  // sets wCASF, most of them
            it.ins.push_back({w, false});
            it.ins.push_back({dpImm(cond(), 13, false, 0, ra, armImm(g.below(256))), false});
        } else if (k < 8) {
            // TMCR of any control register (wCID and the holes are undefined), then TMRC.
            it.kind = "iwmmxt tmcr/tmrc";
            static const uint32_t regs[] = {1, 2, 3, 8, 9, 10, 11};
            const uint32_t c = g.chance(80) ? g.pick(regs) : g.below(16);
            if (g.chance(50)) armConst(it, ra, g.chance(50) ? g.pick(edges) : g.u32());
            it.ins.push_back({cond() | 0x0e000110 | (c << 16) | (ra << 12), false});
            it.ins.push_back({cond() | 0x0e100110 | (c << 16) | (rb << 12), false});
        } else if (k < 9) {
            it.kind = "iwmmxt tmcrr/tmrrc";
            uint32_t rc = reg(), rd = reg();
            while (rd == rc) rd = reg();
            it.ins.push_back({cond() | 0x0c400000 | (rb << 16) | (ra << 12) | g.below(16), false});
            if (g.chance(50)) it.ins.push_back({wmmxCdp(), false});
            it.ins.push_back({cond() | 0x0c500000 | (rd << 16) | (rc << 12) | g.below(16), false});
        } else if (k < 10) {
            // TMIA, TMIAPH, TMIAxy: interpreted, between JIT ones.
            it.kind = "iwmmxt tmia";
            static const uint32_t kinds[] = {0, 8, 12, 13, 14, 15};
            it.ins.push_back({wmmxCdp(), false});
            it.ins.push_back({cond() | 0x0e200010 | (g.pick(kinds) << 16) | (ra << 12) | cp | (g.below(8) << 5) |
                                  g.below(15),
                              false});
            it.ins.push_back({wmmxCdp(), false});
        } else {
            // A CPAR write (privileged; undefined in user mode), then iWMMXt that may now be
            // undefined, then often CPAR back to 3 and the same again.
            it.kind = "iwmmxt cpar";
            const uint32_t a = wmmxCdp(), b = wmmxMrc(rb == 15 ? 0 : rb);
            it.ins.push_back({movImm(ra, g.below(4)), false});
            it.ins.push_back({AL | 0x0e0f0f11 | (ra << 12), false});  // mcr p15, 0, ra, c15, c1, 0
            it.ins.push_back({a, false});
            it.ins.push_back({b, false});
            if (g.chance(60)) {
                it.ins.push_back({movImm(ra, 3), false});
                it.ins.push_back({AL | 0x0e0f0f11 | (ra << 12), false});
                it.ins.push_back({a, false});
                it.ins.push_back({b, false});
            }
        }
    }

    void armWmmxMem(Item& it, uint32_t k) {
        static const uint32_t holes[] = {0x2a000, 0x2e000, 0x2c000, 0x28000, 0x29000};
        if (k < 2) {
            // One to three transfers with CDPs between, some conditional, some moved out by
            // TMRRC so the loaded value meets ARM code.
            it.kind = "iwmmxt wldr/wstr";
            for (uint32_t n = 1 + g.below(3); n--;) {
                const bool cp1 = g.chance(60), N = g.chance(50), load = g.chance(55);
                const uint32_t rn = wmmxBase(it, cp1, N);
                const uint32_t w = wmmxMem(rn, cp1, N, load, g.chance(70) ? g.below(8) : g.below(256),
                                           g.chance(20) ? cond() : AL);
                it.ins.push_back({w, false});
                if (g.chance(30)) it.ins.push_back({wmmxCdp(), false});
                if (load && g.chance(30)) {
                    uint32_t rc = reg(), rd = reg();
                    while (rd == rc) rd = reg();
                    it.ins.push_back({AL | 0x0c500000 | (rd << 16) | (rc << 12) | ((w >> 12) & 15), false});
                }
            }
        } else if (k < 3) {
            // A WLDRD or WSTRD (or a word or halfword) that ends past a page edge: into a hole,
            // into the read-only page (stores fault), or into pages user mode can't write or
            // read (WLDR and WSTR check permissions as privileged). Writeback often.
            it.kind = "iwmmxt transfer across a page edge";
            const bool cp1 = g.chance(80), N = cp1 ? g.chance(80) : true;
            const uint32_t size = cp1 ? (N ? 8 : 4) : 2;
            const uint32_t rn = reg();
            armConst(it, rn, g.pick(holes) - size + 1 + g.below(size + 3));
            it.ins.push_back({wmmxMem(rn, cp1, N, g.chance(50), g.chance(60) ? 0 : g.below(4)), false});
            if (g.chance(40)) it.ins.push_back({dpImm(AL, 4, false, rn, rn, armImm(4)), false});  // add rn, rn, #4
        } else {
            // A copy run as Media's: two WLDRDs then two WSTRDs, post-indexed by 8.
            it.kind = "iwmmxt copy run";
            uint32_t rs = reg(), rd = reg();
            while (rd == rs) rd = reg();
            armConst(it, rs, dataAddr(8));
            armConst(it, rd, dataAddr(8));
            const uint32_t wa = g.below(16), wb = g.below(16);
            it.ins.push_back({AL | 0x0cf00102 | (rs << 16) | (wa << 12), false});  // wldrd wa, [rs], #8
            it.ins.push_back({AL | 0x0cf00102 | (rs << 16) | (wb << 12), false});
            if (g.chance(40)) it.ins.push_back({wmmxCdp(), false});
            it.ins.push_back({AL | 0x0ce00102 | (rd << 16) | (wa << 12), false});  // wstrd wa, [rd], #8
            it.ins.push_back({AL | 0x0ce00102 | (rd << 16) | (wb << 12), false});
        }
    }

    void armUserFetch(Item& it) {
        it.kind = "user fetch, privileged page";
        const uint32_t rt = reg();
        armConst(it, rt, 0x28000 + g.below(0x400) * 4 + (g.chance(30) ? 1 : 0));
        it.ins.push_back({cond() | 0x012fff10 | (g.chance(30) ? 0x20 : 0) | rt, false});
    }

    void armItem(Item& it) {
        const uint32_t k = g.below(100);
        if (k < 14) armDataProcessing(it);
        else if (k < 24) {
            armDataProcessing(it);
            it.ins.back().bits = (it.ins.back().bits & 0x0fffffff) | cond();
        } else if (k < 30) armMultiply(it);
        else if (k < 40) armLoadStore(it);
        else if (k < 45) armHalfword(it);
        else if (k < 46) armSwap(it);
        else if (k < 60) armBlock(it);
        else if (k < 66) armStatus(it);
        else if (k < 70) armCp15(it);
        else if (k < 72) armWmmx(it);
        else if (k < 77) armWmmxValid(it);
        else if (k < 79) armMisc(it);
        else if (k < 80 && modes == bit(USR)) armUserFetch(it);
        else if (!armPcWrite(it)) {
            it = Item{};
            armDataProcessing(it);
        }
    }

    // ------------------------------------------------ Thumb items

    void thumbItem(Item& it) {
        const uint32_t k = g.below(100);
        const uint32_t rd = lo(), rn = lo(), rm = lo();
        auto t16 = [&](uint32_t h) { it.ins.push_back({h & 0xffff, true}); };
        if (k < 8) {
            it.kind = "t shift imm";
            t16((g.below(3) << 11) | (g.below(32) << 6) | (rm << 3) | rd);
        } else if (k < 14) {
            it.kind = "t add/sub";
            t16(0x1800 | (g.below(4) << 9) | (g.below(8) << 6) | (rn << 3) | rd);
        } else if (k < 20) {
            it.kind = "t imm8";
            t16(0x2000 | (g.below(4) << 11) | (rd << 8) | g.below(256));
        } else if (k < 32) {
            it.kind = "t alu";
            uint32_t op = g.below(16), m = rm;
            if (op == 13) while (m == rd) m = lo();  // MUL Rd == Rm is unpredictable
            if ((op == 2 || op == 3 || op == 4 || op == 7) && g.chance(40)) t16(0x2000 | (m << 8) | g.below(40));
            t16(0x4000 | (op << 6) | (m << 3) | rd);
        } else if (k < 37) {
            it.kind = "t hi reg";
            const uint32_t op = g.below(3);  // ADD CMP MOV
            uint32_t d = g.below(15), m = g.below(op == 1 ? 15 : 16);
            while (d < 8 && m < 8) d = g.below(15), m = g.below(op == 1 ? 15 : 16);
            if (d == 13 && op != 1 && g.chance(70)) d = 12;
            t16(0x4400 | (op << 8) | ((d >> 3) << 7) | (m << 3) | (d & 7));
        } else if (k < 40) {
            it.kind = "t ldr literal";
            t16(0x4800 | (rd << 8) | g.below(256));
        } else if (k < 52) {
            const uint32_t b = lo();
            const uint32_t form = g.below(4);
            uint32_t align = form == 0 ? 4 : form == 1 ? 1 : form == 2 ? 2 : 4;
            uint32_t d = lo();
            if (g.chance(70)) thumbConst(it, b, dataAddr(align) & (g.chance(90) ? ~(align - 1) : ~0u));
            if (form == 3) {
                it.kind = "t ldr/str reg";
                uint32_t m = lo();
                while (m == b) m = lo();
                if (g.chance(70)) t16(0x2000 | (m << 8) | (g.below(16) * 2));
                t16(0x5000 | (g.below(8) << 9) | (m << 6) | (b << 3) | d);
            } else if (form == 2) {
                it.kind = "t ldrh/strh imm";
                t16(0x8000 | (g.below(2) << 11) | (g.below(32) << 6) | (b << 3) | d);
            } else {
                it.kind = "t ldr/str imm";
                t16(0x6000 | ((form == 1) << 12) | (g.below(2) << 11) | (g.below(32) << 6) | (b << 3) | d);
            }
        } else if (k < 56) {
            it.kind = "t sp ldr/str";
            t16(0x9000 | (g.below(2) << 11) | (rd << 8) | g.below(64));
        } else if (k < 59) {
            it.kind = "t add pc/sp";
            if (g.chance(50)) t16(0xa000 | (g.below(2) << 11) | (rd << 8) | g.below(256));
            else t16(0xb000 | (g.below(2) << 7) | g.below(32));
        } else if (k < 66) {
            const bool pop = g.chance(50);
            it.kind = pop ? "t pop" : "t push";
            t16(0xb400 | (pop << 11) | (!pop && g.chance(30) ? 0x100 : 0) | (g.below(255) + 1));
        } else if (k < 75) {
            const bool load = g.chance(55);
            const uint32_t b = lo();
            if (g.chance(70)) thumbConst(it, b, dataAddr(4));
            uint32_t list = g.below(255) + 1;
            // A stored base is only defined when it is the lowest register in the list.
            if (!load && (list & (1u << b)) && (list & ((1u << b) - 1))) list &= ~(1u << b);
            it.kind = load ? "t ldmia" : "t stmia";
            t16(0xc000 | (load << 11) | (b << 8) | list);
        } else if (k < 77) {
            static const uint32_t forms[] = {0xdf00, 0xbe00, 0xde00};
            const uint32_t f = g.pick(forms);
            it.kind = f == 0xdf00 ? "t swi" : f == 0xbe00 ? "t bkpt" : "t undefined";
            t16(f | g.below(256));
        } else if (!thumbPcWrite(it)) {
            it = Item{};
            it.kind = "t imm8";
            t16(0x2000 | (g.below(4) << 11) | (rd << 8) | g.below(256));
        }
    }

    bool thumbPcWrite(Item& it) {
        const uint32_t k = g.below(16);
        if (k < 4) {
            const int t = target(true, false, 6, true);
            if (t < 0) return false;
            it.kind = "t b<cond>";
            it.target = t;
            it.ins.push_back({0xd000u | (g.below(14) << 8), true, T_BCOND});
        } else if (k < 6) {
            const int t = target(true, false, 30, true);
            if (t < 0) return false;
            it.kind = "t b";
            it.target = t;
            it.ins.push_back({0xe000u, true, T_B});
        } else if (k < 8) {
            const int t = target(true, true);
            if (t < 0) return false;
            const bool arm = !segThumb[segmentOf(t)];
            it.kind = arm ? "t blx imm" : "t bl";
            it.target = t;
            it.ins.push_back({0xf000u, true, T_BL1});
            it.ins.push_back({arm ? 0xe800u : 0xf800u, true, arm ? T_BLX2 : T_BL2});
        } else if (k < 11) {
            const int t = target(true, true);
            if (t < 0) return false;
            const uint32_t rt = lo();
            it.kind = "t bx/blx";
            it.target = t;
            thumbRel(it, rt);
            it.ins.push_back({0x4700u | (g.chance(40) ? 0x80 : 0) | (rt << 3), true});
        } else if (k < 13) {
            const int t = target(true, false);
            if (t < 0) return false;
            const uint32_t rt = lo();
            it.kind = "t mov pc";
            it.target = t;
            thumbRel(it, rt);
            it.ins.push_back({0x4687u | (rt << 3), true});
        } else {
            const int t = target(true, true);
            if (t < 0) return false;
            const uint32_t rt = lo();
            const uint32_t list = g.below(256);
            it.kind = "t pop pc";
            it.target = t;
            uint32_t rs = lo();
            while (rs == rt) rs = lo();
            thumbConst(it, rs, safeAddr());
            it.ins.push_back({0x4685u | (rs << 3), true});  // mov sp, rs
            thumbRel(it, rt);
            it.ins.push_back({0x9000u | (rt << 8) | uint32_t(__builtin_popcount(list)), true});  // str rt, [sp, #4n]
            it.ins.push_back({0xbd00u | list, true});
        }
        return true;
    }

    // ------------------------------------------------ segment ends

    void terminator(Item& it, bool last) {
        it.removable = false;
        if (last) {
            it.kind = "end";
            it.ins.push_back(thumb ? Ins{kSpinThumb, true} : Ins{kSpinArm, false});
            return;
        }
        it.kind = "switch";
        it.target = segStart[seg + 1];
        if (!thumb) {
            if (g.chance(30)) {
                it.ins.push_back({0xfa000000, false, ARM_BLX});
            } else {
                const uint32_t rt = reg();
                armAbs(it, rt);
                it.ins.push_back({AL | 0x012fff10 | (g.chance(30) ? 0x20 : 0) | rt, false});
            }
        } else if (g.chance(30)) {
            it.ins.push_back({0xf000u, true, T_BL1});
            it.ins.push_back({0xe800u, true, T_BLX2});
        } else {
            const uint32_t rt = lo();
            thumbRel(it, rt);
            it.ins.push_back({0x4700u | (rt << 3), true});
        }
    }
};

uint32_t randomValue(Rng& g, Gen& gen) {
    const uint32_t k = g.below(100);
    if (k < 40) return gen.dataAddr(4);
    if (k < 60) return g.below(65);
    if (k < 68) return kAlias + gen.dataAddr(4) - 0x20000;
    return g.u32();
}

uint32_t randomPsr(Rng& g) {
    static const uint32_t ms[] = {USR, FIQ, IRQ, SVC, ABT, UND, SYS};
    return (g.u32() & 0xf8000000) | (g.below(4) << 6) | (g.chance(15) ? 0x20 : 0) | g.pick(ms);
}

Case generate(uint64_t seed) {
    Rng g(seed);
    Case c;
    c.seed = seed;
    Gen gen{g, c};
    c.data.resize(0x10000);
    for (size_t i = 0; i < c.data.size(); i += 4) {
        const uint32_t v = g.chance(30) ? randomValue(g, gen) : g.u32();
        memcpy(&c.data[i], &v, 4);
    }
    c.codeCacheable = g.chance(50);
    c.alignCheck = g.chance(30);
    // R: AP 0 is read-only for every mode. S: AP 0 is read-only for privileged modes, so
    // user code on the AP 0 code pages takes a prefetch abort.
    c.sr = g.chance(12) ? 0x100 : 0x200;
    // Domain 0 client (see armCp15), domain 1 any of no access, client and manager.
    static const uint32_t d1[] = {0, 1, 3};
    c.dacr = (g.chance(85) ? 1 : 1) | (g.pick(d1) << 2);
    // Final mode and CPSR.
    static const uint32_t ms[] = {USR, USR, USR, SVC, SVC, SYS, FIQ, IRQ, ABT, UND};
    const uint32_t mode = g.pick(ms);
    // Segments.
    const uint32_t nseg = 1 + (g.chance(60) ? g.below(4) : 0);
    bool t = g.chance(30);
    int flat = 0;
    for (uint32_t s = 0; s < nseg; ++s) {
        const int n = 2 + int(g.below(g.chance(80) ? 16 : 40));
        gen.segStart.push_back(flat);
        gen.segEnd.push_back(flat + n);
        gen.segThumb.push_back(t);
        flat += n + 1;
        t = !t;
    }
    c.cpsr = (g.u32() & 0xf8000000) | (g.below(4) << 6) | (gen.segThumb[0] ? 0x20 : 0) | mode;
    gen.modes = Gen::bit(mode);
    c.items.resize(flat);
    for (uint32_t s = 0; s < nseg; ++s) {
        gen.seg = int(s);
        gen.thumb = gen.segThumb[s];
        for (int i = gen.segStart[s]; i <= gen.segEnd[s]; ++i) {
            gen.index = i;
            Item& it = c.items[i];
            if (i == gen.segEnd[s]) gen.terminator(it, s + 1 == nseg);
            else if (gen.thumb) gen.thumbItem(it);
            else gen.armItem(it);
            it.thumb = gen.thumb;
        }
    }
    // Table for the setup routine.
    uint32_t* w = c.table;
    *w++ = g.u32() & 0x7ff;  // FSR
    *w++ = g.u32();          // FAR
    *w++ = c.dacr;
    *w++ = kL1;
    *w++ = 0x70 | 1 | (c.alignCheck ? 2 : 0) | c.sr;  // MMU on, S or R
    *w++ = 3;                                           // CPAR for the setup
    for (int i = 0; i < 32; ++i) *w++ = g.chance(30) ? g.below(256) : g.u32();
    for (int i = 0; i < 6; ++i) *w++ = g.u32();
    *w++ = g.chance(90) ? 3 : g.below(4);  // CPAR for the block
    for (int b = 0; b < 5; ++b) {
        const int n = b == 0 ? 7 : 2;
        for (int i = 0; i < n; ++i) *w++ = (i == n - 2) ? (gen.dataAddr(4) & ~3u) : randomValue(g, gen);
        *w++ = randomPsr(g);
    }
    for (int i = 0; i < 7; ++i) *w++ = i == 5 ? (gen.dataAddr(4) & ~3u) : randomValue(g, gen);
    c.tableWords = uint32_t(w - c.table);
    for (auto& r : c.r0to7) r = randomValue(g, gen);
    return c;
}

// ---------------------------------------------------------------- running a case

struct Outcome {
    bool ok = true;
    std::vector<std::string> diffs;
    uint64_t instructions = 0;
    bool unstopped = false;
};

constexpr uint64_t kLimit = 20000;

void prepare(Machine& m, const Case& c, const Assembled& a) {
    memcpy(m.ram, a.image.data(), kRamSize);
    *m.device = Device{};
    cpuInvalidateCodeCache(m.cpu);
    cpuSetReg(m.cpu, ARM_REG_NUM_CPSR, SVC | 0xc0);
    cpuSetReg(m.cpu, 15, kSetup);
    for (int i = 0; i < 400 && !atSpin(m); ++i) cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(m.cpu, 1);
    const uint32_t pc = cpuGetRegisters(m.cpu)[15];
    if (!atSpin(m) || pc < kSetup || pc >= kProbe) {
        fprintf(stderr, "seed %" PRIu64 ": setup did not finish (pc %08x cpsr %08x)\n", c.seed, pc,
                cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR));
        abort();
    }
    cpuSetReg(m.cpu, ARM_REG_NUM_CPSR, c.cpsr);
    for (int i = 0; i < 8; ++i) cpuSetReg(m.cpu, i, c.r0to7[i]);
    cpuSetReg(m.cpu, 15, a.start | (a.thumb ? 1 : 0));
}

struct Engines {
    Machine exact, fast;
    uint32_t bound = 0;
    uint32_t worstOver = 0;  // most any Fast call ran past its budget
};

Outcome runAssembled(Engines& e, const Case& c, const Assembled& a, Snapshot* exactState = nullptr) {
    Outcome o;
    prepare(e.exact, c, a);
    prepare(e.fast, c, a);
    const RunResult x = run(e.exact, c.seed * 31 + 7, kLimit);
    const RunResult f = run(e.fast, c.seed * 31 + 7, kLimit * 4);
    o.instructions = x.ran;
    e.worstOver = std::max(e.worstOver, f.worstOver);
    if (!x.stopped && !f.stopped) {
        o.unstopped = true;
        return o;
    }
    Snapshot xs = snapshot(e.exact, x.stopped);
    o.diffs = differences(xs, snapshot(e.fast, f.stopped));
    if (exactState) *exactState = std::move(xs);
    char line[120];
    if (f.worstOver > e.bound) {
        snprintf(line, sizeof(line), "fast overshoot %u past the bound %u", f.worstOver, e.bound);
        o.diffs.push_back(line);
    }
    if (x.worstOver) {
        snprintf(line, sizeof(line), "exact overshoot %u", x.worstOver);
        o.diffs.push_back(line);
    }
    o.ok = o.diffs.empty();
    return o;
}

Outcome runCase(Engines& e, const Case& c, Snapshot* exactState = nullptr) {
    return runAssembled(e, c, assemble(c), exactState);
}

void printCase(const Case& c) {
    const Assembled a = assemble(c);
    printf("  start %08x %s, cpsr %08x, dacr %x, %s, code %s, alignment checks %s\n", a.start,
           a.thumb ? "Thumb" : "ARM", c.cpsr, c.dacr, c.sr == 0x100 ? "S" : c.sr == 0x200 ? "R" : "S and R",
           c.codeCacheable ? "cacheable" : "uncacheable", c.alignCheck ? "on" : "off");
    printf("  r0-r7:");
    for (uint32_t r : c.r0to7) printf(" %08x", r);
    printf("\n  r8-r14 (usr):");
    for (int i = 0; i < 7; ++i) printf(" %08x", c.table[c.tableWords - 7 + i]);
    printf("\n");
    int lastItem = -1;
    for (const auto& [addr, pos] : a.where) {
        const Item& it = c.items[pos.first];
        const Ins& in = it.ins[pos.second];
        uint32_t w;
        memcpy(&w, &a.image[addr], 4);
        if (pos.first != lastItem) printf("  ; %s\n", it.kind.c_str());
        lastItem = pos.first;
        if (in.thumb)
            printf("  %08x: %04x      %s\n", addr, w & 0xffff, Dynarmic::A32::DisassembleThumb16(uint16_t(w)).c_str());
        else
            printf("  %08x: %08x  %s\n", addr, w, Dynarmic::A32::DisassembleArm(w).c_str());
    }
}

std::string field(const std::string& d) { return d.substr(0, d.find(':')); }

// Drops items while the case still fails on the first thing that differed, then prints it.
Case minimise(Engines& e, Case c, const std::string& first) {
    auto stillFails = [&](const Case& k) {
        const Outcome o = runCase(e, k);
        for (const auto& d : o.diffs)
            if (field(d) == first) return true;
        return false;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (int i = int(c.items.size()) - 1; i >= 0; --i) {
            if (!c.items[i].removable || c.items[i].removed) continue;
            c.items[i].removed = true;
            if (!stillFails(c)) c.items[i].removed = false;
            else changed = true;
        }
    }
    return c;
}

// Steps both engines one call at a time from the case's start and prints where they go.
void trace(Engines& e, const Case& c) {
    const Assembled a = assemble(c);
    prepare(e.exact, c, a);
    prepare(e.fast, c, a);
    for (int i = 0; i < 400; ++i) {
        const uint32_t* x = cpuGetRegisters(e.exact.cpu);
        const uint32_t* f = cpuGetRegisters(e.fast.cpu);
        printf("  step %2d: exact pc %08x cpsr %08x | fast pc %08x cpsr %08x\n", i, x[15],
               cpuGetRegExternal(e.exact.cpu, ARM_REG_NUM_CPSR), f[15], cpuGetRegExternal(e.fast.cpu, ARM_REG_NUM_CPSR));
        for (int r = 0; r < 15; ++r)
            if (x[r] != f[r]) printf("           r%d exact %08x fast %08x\n", r, x[r], f[r]);
        if (atSpin(e.exact) && atSpin(e.fast)) break;
        if (!atSpin(e.exact)) cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(e.exact.cpu, 1);
        if (!atSpin(e.fast)) cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(e.fast.cpu, 1);
    }
}

bool gTrace = false, gVerbose = false;

void report(Engines& e, const Case& c, const Outcome& o) {
    printf("FAIL seed %" PRIu64 "\n", c.seed);
    for (const auto& d : o.diffs) printf("  %s\n", d.c_str());
    const Case m = minimise(e, c, field(o.diffs.front()));
    const Outcome mo = runCase(e, m);
    int live = 0;
    for (const auto& it : m.items) live += !it.removed && it.removable;
    printf("  shortest block (%d items left, rerun with --seed %" PRIu64 "):\n", live, c.seed);
    for (const auto& d : mo.diffs) printf("  > %s\n", d.c_str());
    printCase(m);
    if (gTrace) trace(e, m);
    fflush(stdout);
}

// ---------------------------------------------------------------- directed tests

// What Exact must hold after a directed case, as the ARM manual says: a register of the
// final mode, the CPSR under a mask, or a word of RAM.
// LOADED: register index holds the RAM word at value (which nothing in the case writes).
struct Expect {
    enum What { REG, CPSR, MEM, FSR, FAR, LOADED, DEVICE } what;  // DEVICE: index 0 reads, 1 writes
    uint32_t index;  // register number or address
    uint32_t value;
    uint32_t mask = ~0u;
};

// A hand-written case: ARM or Thumb words at kCode, state from a seed, overrides.
struct Directed {
    const char* name;
    std::vector<uint32_t> code;  // ARM words, or Thumb halfwords when thumb
    bool thumb;
    uint32_t mode;
    std::vector<std::pair<int, uint32_t>> regs;  // r0-r14 of the mode
    std::vector<Expect> expect;
    uint32_t sr = 0x200;  // control S and R
    uint32_t dacr = 1;    // domain 0 client, domain 1 no access
    std::vector<uint32_t> swiHandler;
    bool tablesPage = false;
};

// Where the setup table holds r8-r14 of a mode: FIQ r8-r14, then r13 and r14 (and an SPSR)
// for IRQ, ABT, UND and SVC, then user r8-r14 last.
uint32_t bankIndex(const Case& c, uint32_t mode, int r) {
    const uint32_t user = c.tableWords - 7, fiq = user - 3 * 4 - 8;
    if (mode == FIQ) return fiq + (r - 8);
    if (r < 13 || mode == USR || mode == SYS) return user + (r - 8);
    const uint32_t k = mode == IRQ ? 0 : mode == ABT ? 1 : mode == UND ? 2 : 3;
    return fiq + 8 + 3 * k + (r - 13);
}

std::vector<std::string> unexpected(const Directed& d, const Snapshot& s) {
    std::vector<std::string> out;
    char line[120];
    for (const Expect& x : d.expect) {
        uint32_t v = 0;
        static const char* const names[] = {"r", "cpsr", "mem", "fsr", "far", "r", "device"};
        const char* what = names[x.what];
        uint32_t want = x.value;
        if (x.what == Expect::REG || x.what == Expect::LOADED) v = s.r[x.index];
        else if (x.what == Expect::CPSR) v = s.cpsr;
        else if (x.what == Expect::FSR) v = s.fsr;
        else if (x.what == Expect::FAR) v = s.far;
        else if (x.what == Expect::DEVICE) v = x.index ? s.deviceWrites : s.deviceReads;
        else memcpy(&v, &s.ram[x.index], 4);
        if (x.what == Expect::LOADED) memcpy(&want, &s.ram[x.value & (kRamSize - 1)], 4);
        if ((v & x.mask) == (want & x.mask)) continue;
        snprintf(line, sizeof(line), "%s %x: exact %08x, the manual %08x (mask %08x)", what, x.index, v & x.mask,
                 want & x.mask, x.mask);
        out.push_back(line);
    }
    return out;
}

Case directedCase(const Directed& d, uint64_t seed) {
    Case c = generate(seed);
    c.items.clear();
    Item it;
    it.kind = d.name;
    it.thumb = d.thumb;
    for (uint32_t w : d.code) it.ins.push_back({w, d.thumb});
    c.items.push_back(it);
    Item end;
    end.kind = "end";
    end.removable = false;
    end.thumb = d.thumb;
    end.ins.push_back(d.thumb ? Ins{kSpinThumb, true} : Ins{kSpinArm, false});
    c.items.push_back(end);
    c.cpsr = (c.cpsr & 0xf0000000) | 0xc0 | (d.thumb ? 0x20 : 0) | d.mode;
    c.dacr = d.dacr;
    c.swiHandler = d.swiHandler;
    c.tablesPage = d.tablesPage;
    c.table[2] = d.dacr;
    c.alignCheck = false;
    c.devicePage = true;
    c.sr = d.sr;
    c.table[4] = 0x70 | 1 | d.sr;
    for (auto [r, v] : d.regs) {
        if (r < 8) c.r0to7[r] = v;
        else if (r < 15) c.table[bankIndex(c, d.mode, r)] = v;
    }
    return c;
}

// Code for the BLX (immediate) cases that cross from the page at kCode to the next one.
// ARM: nops up to a BLX in the page's last word, to Thumb movs r0, #7 at 0x11002.
std::vector<uint32_t> armBlxAcrossPage() {
    std::vector<uint32_t> c(0x3ff, 0xe1a00000);  // mov r0, r0 up to 0x10ff8
    c.push_back(0xfbffffff);                     // blx 0x11002
    c.push_back(0x200746c0);                     // (nop) movs r0, #7
    c.push_back(0x0000e7fe);                     // b .
    return c;
}
// Thumb: nops up to a BLX prefix in the page's last halfword, its suffix on the next page,
// to ARM mov r0, #7 at 0x11008.
std::vector<uint32_t> thumbBlxAcrossPage() {
    std::vector<uint32_t> c(0x7ff, 0x46c0);  // mov r8, r8 up to 0x10ffc
    for (uint32_t h : {0xf000u, 0xe803u, 0x46c0u, 0x46c0u, 0x46c0u, 0x0007u, 0xe3a0u, 0xfffeu, 0xeaffu})
        c.push_back(h);
    return c;
}

bool directed(Engines& e) {
    bool ok = true;
    // 0x29000 is user read-only and 0x2a000 unmapped, so a transfer from 0x29ff8 up faults
    // on its third word.
    const Directed tests[] = {
        // Base in the list, loaded before the fault (the 0188 note).
        {"ldmia r0, {r0, r1, r2} faulting after r0", {0xe8900007}, false, USR, {{0, 0x29ff8}}},
        {"ldmib r1, {r0, r1, r2} faulting after r1", {0xe9910007}, false, USR, {{1, 0x29ff4}}},
        {"ldmdb r2, {r1, r2, r3, r4} from the top", {0xe912001e}, false, SVC, {{2, 0x2a008}}},
        {"ldmia r3, {r3, pc} faulting on pc", {0xe8938008}, false, USR, {{3, 0x29ffc}}},
        {"ldmia r1!, {r2, pc} writeback, faulting on pc", {0xe8b18004}, false, USR, {{1, 0x29ffc}}},
        {"ldr r0, [r1], #4 faulting", {0xe4910004}, false, USR, {{1, 0x2a000}}},
        {"ldr r0, [r1, #4]! faulting", {0xe5b10004}, false, USR, {{1, 0x29ffc}}},
        {"str r0, [r1], #-4 user store to read-only", {0xe4010004}, false, USR, {{1, 0x29010}}},
        {"stmia r1!, {r2, pc} faulting on pc", {0xe8a18004}, false, USR, {{1, 0x29ffc}}},
        {"stmdb r1!, {r2-r5} crossing into a hole", {0xe921003c}, false, SVC, {{1, 0x2b008}}},
        {"thumb ldmia r0!, {r0, r1, r2}", {0xc807}, true, USR, {{0, 0x29ff8}}},
        {"thumb pop {r0, pc} faulting on pc", {0xbd01}, true, USR, {{13, 0x29ffc}}},
        {"sys ldmia r4, {r4-r6, r9, r12, sp} domain fault", {0xe8943270}, false, SYS, {{4, 0x101044}}},
        {"sys ldmia r4, {r4-r6} domain fault", {0xe8940070}, false, SYS, {{4, 0x101044}}},
        {"sys ldr r5, [r4] domain fault", {0xe5945000}, false, SYS, {{4, 0x101044}}},
        {"usr ldr r5, [r4] domain fault", {0xe5945000}, false, USR, {{4, 0x101044}}},
        // MRC/MCR p15 c3 in user mode: undefined through the interpreter (the 2e note).
        {"user mrc p15, 0, r0, c3, c0, 0", {0xe3a01005, 0xee130f10}, false, USR, {{0, 0x1234}}},
        {"user mcr p15, 0, r0, c3, c0, 0", {0xe3a01005, 0xee030f10}, false, USR, {{0, 0x0}}},
        {"user mcr p15, 0, r0, c7, c5, 1", {0xe3a01005, 0xee070f35}, false, USR, {{0, 0x10000}}},
        {"svc mcr p15, 0, r0, c7, c5, 1", {0xe3a01005, 0xee070f35, 0xe2811001}, false, SVC, {{0, 0x10000}},
         {{Expect::REG, 1, 6}}},
        {"svc mrc/mcr p15 c3", {0xe3a00007, 0xee030f10, 0xee131f10}, false, SVC, {}},
        // The other p15 slot reads (c0 ID and cache type, c1 control, c2 TTB) and the c7 cache
        // operations that do nothing, in a privileged mode, then in user mode, where each is
        // undefined through the interpreter (the undefined vector returns to the next one).
        {"svc mrc p15 c0, c1, c2 and mcr p15 c7 no-ops",
         {0xe3a01005, 0xee101f10, 0xee104f30, 0xee112f10, 0xee123f10, 0xee070fb2, 0xee070f3a, 0xee070f5a,
          0xee070f9a, 0xee070f16, 0xee070f36, 0xee070f56, 0xee070f1a, 0xee070f5e, 0xee070fd5, 0xe2855001},
         false, SVC, {{5, 1}}, {{Expect::REG, 3, kL1, 0xffffc000}, {Expect::REG, 5, 2}}},
        {"sys mrc p15 c2 in a loop", {0xee123f10, 0xe2555001, 0x1afffffc}, false, SYS, {{5, 3}},
         {{Expect::REG, 3, kL1, 0xffffc000}, {Expect::REG, 5, 0}}},
        {"user mrc p15, 0, r3, c2, c0, 0", {0xe3a01005, 0xee123f10, 0xe2811001}, false, USR, {{3, 0x1234}},
         {{Expect::REG, 3, 0x1234}, {Expect::REG, 1, 6}, {Expect::CPSR, 0, USR, 0x1f}}},
        {"user mrc p15, 0, r1, c0, c0, 0", {0xe3a02005, 0xee101f10}, false, USR, {{1, 0x1234}},
         {{Expect::REG, 1, 0x1234}}},
        {"user mrc p15, 0, r2, c1, c0, 0", {0xe3a01005, 0xee112f10}, false, USR, {{2, 0x1234}},
         {{Expect::REG, 2, 0x1234}}},
        {"user mcr p15, 0, r0, c7, c2, 5", {0xe3a01005, 0xee070fb2, 0xe2811001}, false, USR, {},
         {{Expect::REG, 1, 6}}},
        {"user mcr p15, 0, r0, c7, c10, 4", {0xee070f9a}, false, USR, {}},
        // SWI in the JIT: the vector's movs pc, lr returns to the next instruction. From user
        // mode (a privilege change), from SVC (none), conditional, from Thumb, and the
        // semihosting SWI, which stays with the interpreter.
        {"user swi", {0xe3a01005, 0xef000042, 0xe2811001}, false, USR, {},
         {{Expect::REG, 1, 6}, {Expect::CPSR, 0, USR, 0x3f}}},
        {"svc swi", {0xe3a01005, 0xef000042, 0xe2811001}, false, SVC, {{14, 0x1234}},
         {{Expect::REG, 1, 6}, {Expect::REG, 14, 0x10008}, {Expect::CPSR, 0, SVC, 0x3f}}},
        {"user swi loop", {0xef000001, 0xe2555001, 0x1afffffc}, false, USR, {{5, 5}},
         {{Expect::REG, 5, 0}, {Expect::CPSR, 0, USR, 0x3f}}},
        {"user swine not taken, swieq taken", {0xe3a01005, 0xe3510005, 0x1f000042, 0x0f000043, 0xe2811001}, false,
         USR, {}, {{Expect::REG, 1, 6}, {Expect::CPSR, 0, USR | 0x40000000, 0x4000003f}}},
        {"irq swi", {0xe3a01005, 0xef000042, 0xe2811001}, false, IRQ, {}, {{Expect::REG, 1, 6}}},
        {"thumb user swi", {0x2105, 0xdf42, 0x3101}, true, USR, {},
         {{Expect::REG, 1, 6}, {Expect::CPSR, 0, USR | 0x20, 0x3f}}},
        {"thumb svc swi", {0x2105, 0xdf42, 0x3101}, true, SVC, {{14, 0x1234}},
         {{Expect::REG, 1, 6}, {Expect::REG, 14, 0x10004}, {Expect::CPSR, 0, SVC | 0x20, 0x3f}}},
        {"thumb user swi loop", {0xdf07, 0x3d01, 0xd1fc}, true, USR, {{5, 4}},
         {{Expect::REG, 5, 0}, {Expect::CPSR, 0, USR | 0x20, 0x3f}}},
        {"semihosting swi", {0xe3a00000, 0xe3a01005, 0xef123456, 0xe2811001}, false, USR, {},
         {{Expect::REG, 1, 6}}},
        {"thumb semihosting swi", {0x2000, 0x2105, 0xdfab, 0x3101}, true, USR, {}, {{Expect::REG, 1, 6}}},
        // BLX (immediate) from ARM and from Thumb, within a page and across into the next.
        {"blx to thumb", {0xe3a00001, 0xfaffffff, 0xe7fe2007}, false, USR, {},
         {{Expect::REG, 0, 7}, {Expect::REG, 14, 0x10008}, {Expect::CPSR, 0, USR | 0x20, 0x3f}}},
        {"blx across a page to thumb", armBlxAcrossPage(), false, USR, {},
         {{Expect::REG, 0, 7}, {Expect::REG, 14, 0x11000}, {Expect::CPSR, 0, USR | 0x20, 0x3f}}},
        {"thumb blx to arm", {0x2001, 0xf000, 0xe802, 0x46c0, 0x0007, 0xe3a0, 0xfffe, 0xeaff}, true, USR, {},
         {{Expect::REG, 0, 7}, {Expect::REG, 14, 0x10007}, {Expect::CPSR, 0, USR, 0x3f}}},
        {"thumb blx across a page to arm", thumbBlxAcrossPage(), true, USR, {},
         {{Expect::REG, 0, 7}, {Expect::REG, 14, 0x11003}, {Expect::CPSR, 0, USR, 0x3f}}},
        // DACR changes in a loop: the JIT drops the page table entries of pages in changed
        // domains and keeps the rest. r10: both client, r11: domain 0 manager, r12: domain 1
        // no access, r9: as r12 with domain 5 client. The domain 1 page filled by the first
        // load faults after r12 (r8 stays 0), the AP 0 page takes a store under manager only,
        // and the domain 0 page filled writable keeps taking stores.
        {"svc dacr loop",
         {0xee03af10, 0xe5945000, 0xe5817000, 0xe5926000, 0xee03bf10, 0xe5827004, 0xe5817004, 0xee03cf10,
          0xe5817008, 0xe5948000, 0xee039f10, 0xe5948004, 0xe581700c, 0xe5827008, 0xe2877001, 0xe2533001,
          0x1affffee},
         false, SVC,
         {{1, 0x21000}, {2, 0x2c000}, {3, 8}, {4, 0x101044}, {7, 0x77}, {8, 0}, {9, 0x401}, {10, 5}, {11, 7},
          {12, 1}},
         {{Expect::REG, 3, 0}, {Expect::REG, 8, 0}, {Expect::MEM, 0x2c004, 0x7e}, {Expect::MEM, 0x2100c, 0x7e},
          {Expect::FSR, 0, 0x0f, 0xff}, {Expect::FAR, 0, 0x2c008}},
         0x200, 5},
        // The user Env keeps its own page table: a user read fills the privileged-only page
        // while domain 0 is manager, a SWI handler sets domain 0 back to client, and the
        // user read of the page then takes a permission fault (FSR 0xf, r8 stays 0x77). The
        // first load touches the vector page, as a first translation of a page empties the
        // page tables (a full flush) and the exception entry would otherwise do that.
        {"usr dacr manager then client, read of AP 1",
         {0xe5997000, 0xe3a06003, 0xee036f10, 0xe321f0d0, 0xe5945000, 0xef000000, 0xe5948000}, false, SVC,
         {{4, 0x28010}, {5, 0x5555}, {8, 0x77}, {9, 0}},
         {{Expect::REG, 8, 0x77}, {Expect::FSR, 0, 0x0f, 0xff}, {Expect::FAR, 0, 0x28010}, {Expect::CPSR, 0, USR, 0x1f}},
         0x200, 1, {0xe3a06001, 0xee036f10, 0xe1b0f00e}},
        // Domain changes and remaps interleaved, on the domain 1 alias of 0x21000 (r4). Each
        // pass: a store with domain 1 manager (r10), one as a client (r11), the page remapped
        // read-only (AP 0, r6) and the TLB flushed (r12), a read, a store as manager again,
        // then one as a client, which takes a permission fault (FSR 0x1f) as the manager's
        // writable entry must have gone read-only, then the page remapped writable (r8).
        {"svc dacr changes and remaps interleaved",
         {0xee03af10, 0xe5847000, 0xee03bf10, 0xe5847004, 0xe5856000, 0xee08cf17, 0xe5949000, 0xee03af10,
          0xe5847008, 0xee03bf10, 0xe584700c, 0xe5858000, 0xee08cf17, 0xe2877001, 0xe2533001, 0x1affffef},
         false, SVC,
         {{3, 8}, {4, 0x101040}, {5, kL2b + 4}, {6, 0x2100e}, {7, 0x77}, {8, 0x21ffe}, {10, 0xd}, {11, 5},
          {12, 0}},
         {{Expect::REG, 3, 0}, {Expect::REG, 9, 0x7e}, {Expect::MEM, 0x21040, 0x7e}, {Expect::MEM, 0x21044, 0x7e},
          {Expect::MEM, 0x21048, 0x7e}, {Expect::FSR, 0, 0x1f, 0xff}, {Expect::FAR, 0, 0x10104c}},
         0x200, 5, {}, true},
        // ADC with S: V is signed overflow of the sum with the carry in.
        {"adcs r2, r0, r1: -1 + 1 + 0", {0xe328f000, 0xe0b02001}, false, USR, {{0, 0xffffffff}, {1, 1}},
         {{Expect::REG, 2, 0}, {Expect::CPSR, 0, 0x60000000, 0xf8000000}}},
        {"adcs r2, r0, r1: 0x7fffffff + 0 + 1", {0xe328f202, 0xe0b02001}, false, USR, {{0, 0x7fffffff}, {1, 0}},
         {{Expect::REG, 2, 0x80000000}, {Expect::CPSR, 0, 0x90000000, 0xf8000000}}},
        {"thumb adds r3, r4, #0; adc r0, r1: -1 + 1 + 0", {0x1c23, 0x4148}, true, USR, {{0, 0xffffffff}, {1, 1}},
         {{Expect::REG, 0, 0}, {Expect::CPSR, 0, 0x60000000, 0xf0000000}}},
        // QSUB and QDSUB saturate on a signed subtraction overflow.
        {"qsub r2, r0, r1: no overflow", {0xe328f000, 0xe1212050}, false, USR, {{0, 0x40000000}, {1, 0x40000000}},
         {{Expect::REG, 2, 0}, {Expect::CPSR, 0, 0, 0x08000000}}},
        {"qsub r2, r0, r1: saturates low", {0xe328f000, 0xe1212050}, false, USR, {{0, 0x80000000}, {1, 1}},
         {{Expect::REG, 2, 0x80000000}, {Expect::CPSR, 0, 0x08000000, 0x08000000}}},
        {"qsub r2, r0, r1: saturates high", {0xe328f000, 0xe1212050}, false, USR, {{0, 0x7fffffff}, {1, 0xffffffff}},
         {{Expect::REG, 2, 0x7fffffff}, {Expect::CPSR, 0, 0x08000000, 0x08000000}}},
        {"qdsub r2, r0, r1: no overflow", {0xe328f000, 0xe1612050}, false, USR, {{0, 0x40000000}, {1, 0x20000000}},
         {{Expect::REG, 2, 0}, {Expect::CPSR, 0, 0, 0x08000000}}},
        {"qdsub r2, r0, r1: saturates low", {0xe328f000, 0xe1612050}, false, USR, {{0, 0x80000000}, {1, 1}},
         {{Expect::REG, 2, 0x80000000}, {Expect::CPSR, 0, 0x08000000, 0x08000000}}},
        // Thumb STMIA always writes back; with the base lowest in the list its old value is stored.
        {"thumb stmia r1!, {r1, r2}", {0xc106}, true, USR, {{1, 0x20100}, {2, 0x12345678}},
         {{Expect::REG, 1, 0x20108}, {Expect::MEM, 0x20100, 0x20100}, {Expect::MEM, 0x20104, 0x12345678}}},
        {"thumb stmia r0!, {r0, r1, r2}", {0xc007}, true, SVC, {{0, 0x20200}, {1, 0x11111111}, {2, 0x22222222}},
         {{Expect::REG, 0, 0x2020c}, {Expect::MEM, 0x20200, 0x20200}, {Expect::MEM, 0x20208, 0x22222222}}},
        {"thumb stmia r3!, {r2}", {0xc304}, true, USR, {{3, 0x20300}, {2, 0x33333333}},
         {{Expect::REG, 3, 0x20304}, {Expect::MEM, 0x20300, 0x33333333}}},
        {"thumb stmia r0!, {r0, r1, r2} faulting on r2", {0xc007}, true, USR, {{0, 0x2bff8}, {1, 0x11111111}},
         {{Expect::REG, 0, 0x2bff8}, {Expect::MEM, 0x2bff8, 0x2bff8}, {Expect::MEM, 0x2bffc, 0x11111111}}},
        // A faulting LDM or STM with a banked base leaves the base of the mode it ran in.
        {"svc ldmia sp, {r0, sp, lr} faulting on lr", {0xe89d6001}, false, SVC, {{13, 0x29ff8}, {14, 0x12340000}},
         {{Expect::REG, 13, 0x29ff8}, {Expect::REG, 14, 0x12340000}}},
        {"usr ldmda sp, {r0, sp} faulting on r0", {0xe81d2001}, false, USR, {{13, 0x2b000}},
         {{Expect::REG, 13, 0x2b000}, {Expect::CPSR, 0, USR, 0x1f}}},
        {"usr ldmda lr, {r0, lr} faulting on r0", {0xe81e4001}, false, USR, {{14, 0x2b000}},
         {{Expect::REG, 14, 0x2b000}, {Expect::CPSR, 0, USR, 0x1f}}},
        {"irq ldmdb lr, {r1, lr} faulting on r1", {0xe91e4002}, false, IRQ, {{14, 0x2b004}},
         {{Expect::REG, 14, 0x2b004}}},
        {"svc stmia lr, {r0, lr} faulting on lr", {0xe88e4001}, false, SVC, {{0, 0x0a0b0c0d}, {14, 0x2bffc}},
         {{Expect::REG, 14, 0x2bffc}, {Expect::MEM, 0x2bffc, 0x0a0b0c0d}}},
        {"svc stmia sp, {r0, sp} faulting on sp", {0xe88d2001}, false, SVC, {{0, 0x0a0b0c0d}, {13, 0x2bffc}},
         {{Expect::REG, 13, 0x2bffc}, {Expect::MEM, 0x2bffc, 0x0a0b0c0d}}},
        // Read permissions: AP with the control register's S and R, domains, and fetches. A
        // faulting read leaves its register, and FSR has the domain and the page permission
        // (0xf) or page domain (0xb) fault.
        {"usr ldr r5, [r4] from AP 1", {0xe5945000}, false, USR, {{4, 0x28010}, {5, 0x5555}},
         {{Expect::REG, 5, 0x5555}, {Expect::FSR, 0, 0x0f, 0xff}, {Expect::FAR, 0, 0x28010}, {Expect::CPSR, 0, USR, 0x1f}}},
        {"usr ldrb r5, [r4] from AP 1", {0xe5d45000}, false, USR, {{4, 0x28013}, {5, 0x5555}},
         {{Expect::REG, 5, 0x5555}, {Expect::FSR, 0, 0x0f, 0xff}, {Expect::FAR, 0, 0x28013}}},
        {"usr ldrh r5, [r4] from AP 1", {0xe1d450b0}, false, USR, {{4, 0x28ffe}, {5, 0x5555}},
         {{Expect::REG, 5, 0x5555}, {Expect::FSR, 0, 0x0f, 0xff}, {Expect::FAR, 0, 0x28ffe}}},
        {"usr ldmia r4, {r5, r6} from AP 1", {0xe8940060}, false, USR, {{4, 0x28010}},
         {{Expect::REG, 4, 0x28010}, {Expect::FSR, 0, 0x0f, 0xff}, {Expect::FAR, 0, 0x28010}}},
        {"usr ldmia r4, {r5, r6} into AP 1", {0xe8940060}, false, USR, {{4, 0x27ffc}},
         {{Expect::REG, 4, 0x27ffc}, {Expect::FSR, 0, 0x0f, 0xff}, {Expect::FAR, 0, 0x28000}}},
        {"thumb usr ldr r5, [r4] from AP 1", {0x6825}, true, USR, {{4, 0x28010}, {5, 0x5555}},
         {{Expect::REG, 5, 0x5555}, {Expect::FSR, 0, 0x0f, 0xff}, {Expect::FAR, 0, 0x28010}}},
        {"svc str and ldr at AP 1", {0xe5846000, 0xe5945000}, false, SVC, {{4, 0x28010}, {6, 0x12345678}},
         {{Expect::REG, 5, 0x12345678}, {Expect::MEM, 0x28010, 0x12345678}}},
        {"usr ldr r5, [r4] from AP 2", {0xe5945000}, false, USR, {{4, 0x29010}, {5, 0x5555}},
         {{Expect::LOADED, 5, 0x29010}}},
        {"usr ldr r5, [r4] from AP 3", {0xe5945000}, false, USR, {{4, 0x20010}, {5, 0x5555}},
         {{Expect::LOADED, 5, 0x20010}}},
        {"usr ldr r5, [r4] from AP 0 with R", {0xe5945000}, false, USR, {{4, 0x2c010}, {5, 0x5555}},
         {{Expect::LOADED, 5, 0x2c010}}},
        {"svc ldr r5, [r4] from AP 0 with S", {0xe5945000}, false, SVC, {{4, 0x2c010}, {5, 0x5555}},
         {{Expect::LOADED, 5, 0x2c010}}, 0x100},
        {"svc ldrt r5, [r4] from AP 0 with S", {0xe4b45000}, false, SVC, {{4, 0x2c010}, {5, 0x5555}},
         {{Expect::REG, 5, 0x5555}, {Expect::FSR, 0, 0x0f, 0xff}, {Expect::FAR, 0, 0x2c010}}, 0x100},
        {"svc ldrt r5, [r4] from AP 1", {0xe4b45000}, false, SVC, {{4, 0x28010}, {5, 0x5555}},
         {{Expect::REG, 5, 0x5555}, {Expect::FSR, 0, 0x0f, 0xff}, {Expect::FAR, 0, 0x28010}}},
        {"svc ldrt r5, [r4] from AP 2", {0xe4b45000}, false, SVC, {{4, 0x29010}, {5, 0x5555}},
         {{Expect::LOADED, 5, 0x29010}}},
        {"sys ldr r5, [r4] domain 1 no access", {0xe5945000}, false, SYS, {{4, 0x101044}, {5, 0x5555}},
         {{Expect::REG, 5, 0x5555}, {Expect::FSR, 0, 0x1b, 0xff}, {Expect::FAR, 0, 0x101044}}},
        {"usr ldr r5, [r4] domain 1 client", {0xe5945000}, false, USR, {{4, 0x101044}, {5, 0x5555}},
         {{Expect::LOADED, 5, 0x21044}}, 0x200, 0x5},
        {"usr ldr r5, [r4] domain 1 manager", {0xe5945000}, false, USR, {{4, 0x101044}, {5, 0x5555}},
         {{Expect::LOADED, 5, 0x21044}}, 0x200, 0xd},
        {"usr ldr r5, [r4] from AP 1, domain 0 manager", {0xe5945000}, false, USR, {{4, 0x28010}, {5, 0x5555}},
         {{Expect::LOADED, 5, 0x28010}}, 0x200, 0x7},
        // Fetches: the prefetch abort vector stops the run, in ABT with r14 the address + 4.
        {"usr bx into AP 1", {0xe12fff10}, false, USR, {{0, 0x28000}},
         {{Expect::CPSR, 0, ABT, 0x3f}, {Expect::REG, 14, 0x28004}}},
        {"thumb usr bx into AP 1", {0x4700}, true, USR, {{0, 0x28001}},
         {{Expect::CPSR, 0, ABT, 0x3f}, {Expect::REG, 14, 0x28004}}},
        {"usr fetch from AP 0 with S", {0xe1a00000}, false, USR, {},
         {{Expect::CPSR, 0, ABT, 0x3f}, {Expect::REG, 14, 0x10004}}, 0x100},
        {"sys bx into domain 1 no access", {0xe12fff10}, false, SYS, {{0, 0x101000}},
         {{Expect::CPSR, 0, ABT, 0x3f}, {Expect::REG, 14, 0x101004}}},
        // SVC sets S (AP 0 privileged only), then drops to user: the next fetch aborts.
        {"svc sets S, msr to usr, fetch from AP 0",
         {0xee110f10, 0xe3c00c03, 0xe3800c01, 0xee010f10, 0xe321f0d0, 0xe1a00000}, false, SVC, {},
         {{Expect::CPSR, 0, ABT, 0x3f}, {Expect::REG, 14, 0x10018}}},
        // A Thumb BL or BLX half without its other half runs in the interpreter. Dynarmic
        // would join it with the next halfword: f801 4f70 is strb.w r4, [r1, #0x70]!. Between
        // the halves the interpreter's LR is 2 less than the manual's (the prefix adds the
        // next halfword's address, the suffix adds 2 back), so these expect its values.
        {"thumb lone bl suffix at the start", {0xf801, 0x4f70}, true, USR, {{1, 0x20100}, {14, 0x10000}},
         {{Expect::REG, 1, 0x20100}, {Expect::REG, 14, 0x10003}}},
        {"thumb lone bl suffix in a block", {0x2201, 0xf801, 0x4f70}, true, USR, {{1, 0x20100}, {14, 0x10002}},
         {{Expect::REG, 1, 0x20100}, {Expect::REG, 2, 1}, {Expect::REG, 14, 0x10005}}},
        // e880 0003 is stm.w r0, {r0, r1}; as a BLX suffix it goes to ARM at 0x10008.
        {"thumb lone blx suffix at the start", {0xe880, 0x0003, 0xe7fe, 0x0000, 0xfffe, 0xeaff}, true, USR,
         {{0, 0x20100}, {14, 0xff08}}, {{Expect::REG, 14, 0x10003}, {Expect::CPSR, 0, USR, 0x3f}}},
        {"thumb bl prefix without a suffix", {0xf000, 0x2005}, true, USR, {{0, 0xffffffff}},
         {{Expect::REG, 0, 5}, {Expect::REG, 14, 0x10002}}},
        {"thumb bl prefix without a suffix in a block", {0x2201, 0xf000, 0x2005}, true, USR, {{0, 0xffffffff}},
         {{Expect::REG, 0, 5}, {Expect::REG, 2, 1}, {Expect::REG, 14, 0x10004}}},
        // An IRQ handler returns with ldm^ (interpreter) onto a BL suffix at 0x1000c.
        {"irq ldm^ return onto a bl suffix",
         {0xe16ff002, 0xe58d3000, 0xe8dd8000, 0x4f70f800, 0xe7fee7fe}, false, IRQ,
         {{0, 0x20100}, {2, 0xf2}, {3, 0x1000c}, {13, 0x20200}, {14, 0x1000e}},
         {{Expect::REG, 0, 0x20100}, {Expect::REG, 14, 0x1000f}, {Expect::CPSR, 0, 0x32, 0x3f}}},
        // iWMMXt in the JIT's slots. WADDHSS on 0x7fff and 0xffff lanes plus 1: lanes 1 and 3
        // saturate (wCSSF bits 2 and 6), lanes 0 and 2 wrap to 0.
        {"iwmmxt waddhss saturating, wcssf",
         {0xe3a00000, 0xee020110, 0xe3e00102, 0xe1a01000, 0xec410002, 0xe3a02801, 0xe3822001, 0xe1a03002,
          0xec432003, 0xee721183, 0xee126110, 0xec598001},
         false, USR, {},
         {{Expect::REG, 6, 0x44}, {Expect::REG, 8, 0x7fff0000}, {Expect::REG, 9, 0x7fff0000}}},
        // TANDCW r15 with every wCASF nibble set: NZCV all set, then MI and EQ both pass.
        {"iwmmxt tandcw r15, conditional after",
         {0xe3e00000, 0xee030110, 0xe3a01000, 0xe3a02000, 0xee93f130, 0x43a01001, 0x03a02001}, false, USR, {},
         {{Expect::REG, 1, 1}, {Expect::REG, 2, 1}, {Expect::CPSR, 0, 0xf0000000, 0xf0000000}}},
        // The same block three times, CPAR 3, 2, 3: the second time WOR (cp0) at 0x1000c is
        // undefined (r14_und 0x10010, read into r7 at the end) and TMRC (cp1) still runs.
        {"iwmmxt cpar off and on in a loop",
         {0xe3a04003, 0xe3a05003, 0xee0f4f11, 0xee021003, 0xee136110, 0xe2244001, 0xee0f4f11, 0xe2555001,
          0x1afffff8, 0xe3a07000, 0xe321f0db, 0xe1a0700e, 0xe321f0d3},
         false, SVC, {},
         {{Expect::REG, 5, 0}, {Expect::REG, 4, 2}, {Expect::REG, 7, 0x10010}}},
        // iWMMXt loads and stores in the JIT's slots. A failed WLDR or WSTR takes the data
        // abort as an ordinary load or store does (FSR and FAR set, the handler returns to the
        // next instruction) and writes no base back.
        {"iwmmxt wldrd [r1, #8]! crossing into a hole",
         {0xedf10102, 0xe1a02001}, false, SVC, {{1, 0x29ff4}},
         {{Expect::REG, 1, 0x29ff4}, {Expect::REG, 2, 0x29ff4}, {Expect::FSR, 0, 0x07, 0xff},
          {Expect::FAR, 0, 0x2a000}}},
        {"iwmmxt wstrd [r1], #8 crossing into the read-only page",
         {0xece10102, 0xe1a02001}, false, USR, {{1, 0x2bffc}},
         {{Expect::REG, 1, 0x2bffc}, {Expect::REG, 2, 0x2bffc}, {Expect::FSR, 0, 0x0f, 0xff},
          {Expect::FAR, 0, 0x2c000}}},
        {"iwmmxt wldrw [r1, #-4]! from a hole", {0xed310101}, false, SVC, {{1, 0x2a004}},
         {{Expect::REG, 1, 0x2a004}, {Expect::FSR, 0, 0x07, 0xff}, {Expect::FAR, 0, 0x2a000}}},
        // Checked for the current mode: user mode can't read the privileged-only page, SVC can.
        {"iwmmxt user wldrd from the privileged-only page",
         {0xedd10100, 0xec532000}, false, USR, {{1, 0x28008}},
         {{Expect::FSR, 0, 0x0f, 0xff}, {Expect::FAR, 0, 0x28008}, {Expect::CPSR, 0, USR, 0x1f}}},
        {"iwmmxt svc wldrd from the privileged-only page",
         {0xedd10100, 0xec532000}, false, SVC, {{1, 0x28008}},
         {{Expect::LOADED, 2, 0x28008}, {Expect::LOADED, 3, 0x2800c}}},
        {"iwmmxt user wstrb to the user read-only page",
         {0xed810004, 0xe1a02001}, false, USR, {{1, 0x29010}},
         {{Expect::REG, 2, 0x29010}, {Expect::FSR, 0, 0x0f, 0xff}, {Expect::FAR, 0, 0x29014}}},
        // cp0 offsets are bytes, cp1 offsets words: wldrb [r1, #5]!, wstrh [r2], #-6,
        // wldrw [r3, #-4]!, wstrw [r4, #-8]! (offset 2 words).
        {"iwmmxt offsets and writeback",
         {0xedb10005, 0xec620006, 0xed331101, 0xed242102}, false, SVC,
         {{1, 0x20100}, {2, 0x20200}, {3, 0x20300}, {4, 0x20400}},
         {{Expect::REG, 1, 0x20105}, {Expect::REG, 2, 0x201fa}, {Expect::REG, 3, 0x202fc},
          {Expect::REG, 4, 0x203f8}}},
        // Unaligned: halfword at an odd address, doubleword at 4 mod 8, word at 2 mod 4.
        {"iwmmxt unaligned", {0xedd10000, 0xedd21100, 0xed932100, 0xedc41100}, false, SVC,
         {{1, 0x20101}, {2, 0x20204}, {3, 0x20302}, {4, 0x20404}}, {}},
        // Media's copy loop, 8 times 16 bytes, then the same from 0x29f80 into the hole at
        // 0x2a000: every load from there aborts, r1 stays, the stores go on.
        {"iwmmxt copy loop",
         {0xecf10102, 0xecf11102, 0xece20102, 0xece21102, 0xe2533001, 0x1afffff9}, false, SVC,
         {{1, 0x20100}, {2, 0x20800}, {3, 8}},
         {{Expect::REG, 1, 0x20180}, {Expect::REG, 2, 0x20880}, {Expect::REG, 3, 0}}},
        {"iwmmxt copy loop into a hole",
         {0xecf10102, 0xecf11102, 0xece20102, 0xece21102, 0xe2533001, 0x1afffff9}, false, SVC,
         {{1, 0x29f80}, {2, 0x20800}, {3, 32}},
         {{Expect::REG, 1, 0x2a000}, {Expect::REG, 2, 0x20a00}, {Expect::REG, 3, 0}}},
        // A WLDRD or WSTRD whose second word fails has done the first word's access once,
        // as Exact does (the JIT doesn't run it again): a device page at 0x2f000 counts them,
        // then a translation fault at 0x30000 or a refused register at device offset 0x800.
        {"iwmmxt wstrd from a device into a hole",
         {0xedc10100, 0xe1a02001}, false, SVC, {{1, 0x2fffc}},
         {{Expect::REG, 2, 0x2fffc}, {Expect::DEVICE, 1, 1}, {Expect::DEVICE, 0, 0}, {Expect::FAR, 0, 0x30000}}},
        {"iwmmxt wldrd from a device into a hole",
         {0xedd10100, 0xec532000}, false, SVC, {{1, 0x2fffc}},
         {{Expect::DEVICE, 0, 1}, {Expect::DEVICE, 1, 0}}},
        {"iwmmxt user wstrd [r1], #8 into a refused device register",
         {0xece10102, 0xe1a02001}, false, USR, {{1, 0x2f7fc}},
         {{Expect::REG, 1, 0x2f7fc}, {Expect::REG, 2, 0x2f7fc}, {Expect::DEVICE, 1, 1}, {Expect::FSR, 0, 0x0a, 0x0f},
          {Expect::FAR, 0, 0x2f800}}},
        {"iwmmxt wldrd and wstrd on a device",
         {0xedd10100, 0xec532000, 0xedc10100, 0xedd10100}, false, SVC, {{1, 0x2f100}},
         {{Expect::REG, 2, 1}, {Expect::REG, 3, 2}, {Expect::DEVICE, 0, 4}, {Expect::DEVICE, 1, 2}}},
        // LDM and STM that fault part-way after device words: each word before the fault is
        // read or written once, as in Exact (the JIT takes the abort without a re-run).
        {"ldmia r1, {r2-r5} from a device into a hole", {0xe891003c}, false, SVC, {{1, 0x2fff8}},
         {{Expect::REG, 1, 0x2fff8}, {Expect::DEVICE, 0, 2}, {Expect::FSR, 0, 0x07, 0xff}, {Expect::FAR, 0, 0x30000}}},
        {"stmia r1!, {r2-r5} from a device into a hole", {0xe8a1003c}, false, SVC, {{1, 0x2fff8}},
         {{Expect::REG, 1, 0x2fff8}, {Expect::DEVICE, 1, 2}, {Expect::FAR, 0, 0x30000}}},
        {"user ldmia r1, {r2-r5} into a refused device register", {0xe891003c}, false, USR, {{1, 0x2f7f8}},
         {{Expect::DEVICE, 0, 2}, {Expect::FSR, 0, 0x0a, 0x0f}, {Expect::FAR, 0, 0x2f800}}},
        {"stmdb r1!, {r2-r5} from a device down", {0xe921003c}, false, SVC, {{1, 0x30008}}, {{Expect::REG, 1, 0x30008}}},
        {"thumb ldmia r0!, {r1-r3} from a device into a hole", {0xc80e}, true, USR, {{0, 0x2fff8}},
         {{Expect::REG, 0, 0x2fff8}, {Expect::DEVICE, 0, 2}}},
        {"thumb push {r0-r3} from a device into a hole", {0xb40f}, true, SVC, {{13, 0x30008}},
         {{Expect::REG, 13, 0x30008}}},
        {"ldr r0, [r1] from a refused device register", {0xe5910000}, false, SVC, {{1, 0x2f804}},
         {{Expect::DEVICE, 0, 0}, {Expect::FSR, 0, 0x0a, 0x0f}, {Expect::FAR, 0, 0x2f804}}},
        // CPAR bit 1 off: WLDRD (cp1) at 0x10008 is undefined, wldrb (cp0) still runs.
        {"iwmmxt cpar off for a load",
         {0xe3a04001, 0xee0f4f11, 0xedd10100, 0xedd20000, 0xe3a07000, 0xe321f0db, 0xe1a0700e, 0xe321f0d3},
         false, SVC, {{1, 0x20100}, {2, 0x20200}},
         {{Expect::REG, 7, 0x1000c}}},
    };
    for (const auto& d : tests) {
        for (uint64_t seed = 1; seed <= 4; ++seed) {
            const Case c = directedCase(d, seed);
            Snapshot xs;
            Outcome o = runCase(e, c, &xs);
            if (o.ok && !o.unstopped) {
                o.diffs = unexpected(d, xs);
                if (o.diffs.empty() && xs.stopped) continue;
                if (!xs.stopped) o.diffs.push_back("exact did not stop");
            }
            printf("FAIL directed: %s (seed %" PRIu64 ")%s\n", d.name, seed, o.unstopped ? " did not stop" : "");
            for (const auto& s : o.diffs) printf("  %s\n", s.c_str());
            printCase(c);
            ok = false;
            break;
        }
    }
    return ok;
}

// The XScale fetch buffers when the guest rewrites code without invalidating. The
// interpreter fills a buffer at A (an MCR the JIT hands back), the code at Y then rewrites
// A+8, and the guest later jumps back to A+8 with no invalidation. An XScale (Exact) runs
// the old word while a buffer still holds A's line and the new one once its buffers have
// moved on. Fast drops a buffer when a store hits its line, so it always runs the new word:
// the JIT doesn't move the buffers on, so without that it kept the old line long after
// Exact had refetched. The state is also saved in Fast and loaded in a fresh Exact CPU, as
// a session would be, which then has no old line either.
// A block compiled on the compile thread with a link to B, whose code is rewritten before
// the block installs: the installed block has to run the new B. Fast only, thread on.
// 0: as expected, -1: not.
int staleLink() {
    const char* name = "link to a block rewritten before install";
    cpuSetJitSync(false);
    static Machine m = makeMachine(true);
    cpuSetJitSync(!gThread);
    Case c = generate(99);
    c.items.clear();
    c.codeCacheable = false;
    Item end;
    end.kind = "end";
    end.removable = false;
    end.ins.push_back({kSpinArm, false});
    c.items.push_back(end);
    Assembled a = assemble(c);
    const uint32_t A = kCode, B = kCode + 0x1000, S = kCode + 0x2000;  // separate pages
    auto put = [&](uint32_t addr, uint32_t v) { memcpy(&a.image[addr], &v, 4); };
    put(A, 0xe3a05000);      // mov r5, #0
    put(A + 4, 0xea0003fd);  // b B
    put(B, 0xe3a05001);      // mov r5, #1 (rewritten to mov r5, #2)
    put(B + 4, 0xea0003fd);  // b S
    put(S, kSpinArm);
    a.start = B;
    a.thumb = false;
    c.cpsr = SVC | 0xc0;
    prepare(m, c, a);
    auto runFrom = [&](uint32_t pc) {
        cpuSetReg(m.cpu, 15, pc);
        cpuSetReg(m.cpu, 5, 9);
        for (int i = 0; i < 100 && !(atSpin(m) && cpuGetRegisters(m.cpu)[15] == S); ++i)
            cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(m.cpu, 50);
        return cpuGetRegisters(m.cpu)[15] == S ? cpuGetRegExternal(m.cpu, 5) : 99u;
    };
    // B compiled and installed.
    for (int i = 0; i < 20; ++i) {
        runFrom(B);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // A queued, and emitted linked to B while the run waits.
    const uint32_t before = runFrom(A);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // B rewritten from outside the run, so its block is dropped before A installs.
    const uint32_t word = 0xe3a05002;
    memAccess(m.mem, B, 4, true, (void*)&word);
    const uint32_t after = runFrom(A), again = runFrom(A);
    printf("%s: r5 %u before, %u and %u after the rewrite\n", name, before, after, again);
    if (before != 1 || after != 2 || again != 2) {
        printf("FAIL %s: expected 1, then 2\n", name);
        return -1;
    }
    return 0;
}

// An indirect jump (BX) from A to B, which the JIT takes through its fast dispatch table,
// with B rewritten from outside twice: each time the table has to drop B, so A's next BX runs
// the new code. With --thread the second rewrite hits a B installed from the compile thread.
// Fast only. 0: as expected, -1: not.
int staleDispatch() {
    const char* name = "indirect jump to a block rewritten while dispatch holds it";
    cpuSetJitSync(false);
    static Machine m = makeMachine(true);
    cpuSetJitSync(!gThread);
    Case c = generate(97);
    c.items.clear();
    c.codeCacheable = false;
    Item end;
    end.kind = "end";
    end.removable = false;
    end.ins.push_back({kSpinArm, false});
    c.items.push_back(end);
    Assembled a = assemble(c);
    const uint32_t A = kCode, B = kCode + 0x1000, S = kCode + 0x2000;  // separate pages
    auto put = [&](uint32_t addr, uint32_t v) { memcpy(&a.image[addr], &v, 4); };
    put(A, 0xe3a05000);      // mov r5, #0
    put(A + 4, 0xe12fff17);  // bx r7
    put(B, 0xe3a05001);      // mov r5, #1 (rewritten to #2, then #3)
    put(B + 4, 0xea0003fd);  // b S
    put(S, kSpinArm);
    a.start = A;
    a.thumb = false;
    c.cpsr = SVC | 0xc0;
    prepare(m, c, a);
    auto runFrom = [&](uint32_t pc) {
        cpuSetReg(m.cpu, 15, pc);
        cpuSetReg(m.cpu, 5, 9);
        cpuSetReg(m.cpu, 7, B);
        for (int i = 0; i < 100 && !(atSpin(m) && cpuGetRegisters(m.cpu)[15] == S); ++i)
            cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(m.cpu, 50);
        return cpuGetRegisters(m.cpu)[15] == S ? cpuGetRegExternal(m.cpu, 5) : 99u;
    };
    // Runs from A a while, so A and B are compiled (and installed) and the BX is in the table.
    auto settle = [&](uint32_t want) {
        uint32_t bad = 0;
        for (int i = 0; i < 30; ++i) {
            if (runFrom(A) != want) bad++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return bad;
    };
    const uint32_t bad1 = settle(1);
    const uint32_t word2 = 0xe3a05002, word3 = 0xe3a05003;
    memAccess(m.mem, B, 4, true, (void*)&word2);
    const uint32_t bad2 = settle(2);
    memAccess(m.mem, B, 4, true, (void*)&word3);
    const uint32_t bad3 = settle(3);
    printf("%s: runs of 30 not as expected: %u before, %u after the first rewrite, %u after the second\n", name,
           bad1, bad2, bad3);
    if (bad1 || bad2 || bad3) {
        printf("FAIL %s: expected 1, then 2, then 3\n", name);
        return -1;
    }
    return 0;
}

// The code cache filling mid-run, so dynarmic clears it itself (AddressSpace::Emit with the
// compile thread off, the compile thread's own checks otherwise) while A's BX to B is in the
// fast dispatch table. The clear has to empty the table: the code emitted after it lands where
// A and B were, so a stale entry would run filler code. The cache is made small, and a filler
// of 2048 two-word blocks is rewritten and run until a clear, then for a quarter as many passes
// again without one, so the code after the last clear covers A and B (the compile thread
// emits only part of the filler each pass). inPlace: FROND_JIT_THREAD=0, else as --thread
// says. Fast only. 0: as expected, -1: not.
int cacheFullDispatch(bool inPlace) {
    const char* name = inPlace ? "code cache filled mid-run, compiling in place" : "code cache filled mid-run";
    // Dynarmic clears with under 1 MB left; x64 also takes a 2 MB constant pool and a bigger
    // prelude. A filler pass emits well under what is left.
#if defined(__aarch64__)
    constexpr size_t kCache = (5u << 19);
#else
    constexpr size_t kCache = (8u << 20);
#endif
    if (inPlace) setenv("FROND_JIT_THREAD", "0", 1);
    cpuSetJitCodeCacheSize(kCache);
    Machine* made;
    if (inPlace) {
        static Machine one = makeMachine(true);
        made = &one;
    } else {
        static Machine one = makeMachine(true);
        made = &one;
    }
    Machine& m = *made;
    cpuSetJitCodeCacheSize(0);
    unsetenv("FROND_JIT_THREAD");
    Case c = generate(96);
    c.items.clear();
    c.codeCacheable = false;
    Item end;
    end.kind = "end";
    end.removable = false;
    end.ins.push_back({kSpinArm, false});
    c.items.push_back(end);
    Assembled a = assemble(c);
    const uint32_t A = kCode, B = kCode + 0x1000, S = kCode + 0x2000, F = kCode + 0x3000;
    constexpr uint32_t kFill = 2048;
    auto put = [&](uint32_t addr, uint32_t v) { memcpy(&a.image[addr], &v, 4); };
    auto branch = [](uint32_t from, uint32_t to) { return 0xea000000 | (((to - from - 8) >> 2) & 0xffffff); };
    put(A, 0xe3a05000);      // mov r5, #0
    put(A + 4, 0xe12fff17);  // bx r7
    put(B, 0xe3a05001);      // mov r5, #1
    put(B + 4, branch(B + 4, S));
    put(S, kSpinArm);
    for (uint32_t i = 0; i < kFill; ++i) {
        const uint32_t at = F + 8 * i;
        put(at, 0xe3a05c01);  // mov r5, #0x100
        put(at + 4, branch(at + 4, i + 1 < kFill ? at + 8 : S));
    }
    a.start = A;
    a.thumb = false;
    c.cpsr = SVC | 0xc0;
    prepare(m, c, a);
    auto runTo = [&](uint32_t pc) {
        cpuSetReg(m.cpu, 15, pc);
        cpuSetReg(m.cpu, 5, 9);
        cpuSetReg(m.cpu, 7, B);
        for (int i = 0; i < 2000 && !(atSpin(m) && cpuGetRegisters(m.cpu)[15] == S); ++i)
            cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(m.cpu, 50);
        return cpuGetRegisters(m.cpu)[15] == S ? cpuGetRegExternal(m.cpu, 5) : 99u;
    };
    auto settle = [&]() {
        uint32_t bad = 0;
        for (int i = 0; i < 30; ++i) {
            if (runTo(A) != 1) bad++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return bad;
    };
    // Each pass gives the filler new immediates, so its blocks are dropped and emitted again.
    uint32_t pass = 0, fillBad = 0;
    auto fill = [&]() {
        ++pass;
        for (uint32_t i = 0; i < kFill; ++i) {
            const uint32_t word = 0xe3a05c00 | (pass & 0xff);  // mov r5, #pass << 8
            memAccess(m.mem, F + 8 * i, 4, true, (void*)&word);
        }
        for (int i = 0; i < 3; ++i) {
            if (runTo(F) != ((pass & 0xff) << 8)) fillBad++;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        uint64_t clears = 0;
        cpuJitCacheClears(m.cpu, &clears);
        return clears;
    };
    const uint32_t bad1 = settle();
    uint64_t clears0 = 0, clears = 0;
    if (!cpuJitCacheClears(m.cpu, &clears0)) {
        printf("FAIL %s: no clear count\n", name);
        return -1;
    }
    clears = clears0;
    while (pass < 1000 && clears == clears0) clears = fill();
    const uint32_t fillPasses = pass, want = std::max<uint32_t>(1, fillPasses / 4);
    uint32_t quiet = 0;
    for (uint32_t i = 0; i < 4 * fillPasses + 20 && quiet < want && clears != clears0; ++i) {
        const uint64_t before = clears;
        clears = fill();
        quiet = clears == before ? quiet + 1 : 0;
    }
    const uint32_t bad2 = settle();
    printf("%s: %llu clears in %u filler passes, runs of 30 not as expected: %u before, %u after; filler runs wrong %u\n",
           name, (unsigned long long)(clears - clears0), pass, bad1, bad2, fillBad);
    if (clears == clears0 || quiet < want) {
        printf("FAIL %s: the cache never filled, or never stopped clearing\n", name);
        return -1;
    }
    if (bad1 || bad2 || fillBad) {
        printf("FAIL %s: expected r5 1 from A to B every run\n", name);
        return -1;
    }
    return 0;
}

// A block compiled ahead (B, a branch target of A that hasn't run) whose code is rewritten
// from outside before the guest first goes there: the new B has to run. Fast, thread on,
// compiling ahead on (it's off by default).
int staleAhead() {
    const char* name = "block compiled ahead, rewritten before use";
    cpuSetJitSync(false);
    setenv("FROND_JIT_AHEAD", "1", 1);
    static Machine m = makeMachine(true);
    unsetenv("FROND_JIT_AHEAD");
    cpuSetJitSync(!gThread);
    Case c = generate(98);
    c.items.clear();
    c.codeCacheable = false;
    Item end;
    end.kind = "end";
    end.removable = false;
    end.ins.push_back({kSpinArm, false});
    c.items.push_back(end);
    Assembled a = assemble(c);
    const uint32_t A = kCode, B = kCode + 0x1000, S = kCode + 0x2000;  // separate pages
    auto put = [&](uint32_t addr, uint32_t v) { memcpy(&a.image[addr], &v, 4); };
    put(A, 0xe3a05000);       // mov r5, #0
    put(A + 4, 0xe3560001);   // cmp r6, #1
    put(A + 8, 0x0a0003fc);   // beq B
    put(A + 12, 0xea0007fb);  // b S
    put(B, 0xe3a05001);       // mov r5, #1 (rewritten to mov r5, #2)
    put(B + 4, 0xea0003fd);   // b S
    put(S, kSpinArm);
    a.start = A;
    a.thumb = false;
    c.cpsr = SVC | 0xc0;
    prepare(m, c, a);
    auto runFrom = [&](uint32_t pc, uint32_t r6) {
        cpuSetReg(m.cpu, 15, pc);
        cpuSetReg(m.cpu, 5, 9);
        cpuSetReg(m.cpu, 6, r6);
        for (int i = 0; i < 100 && !(atSpin(m) && cpuGetRegisters(m.cpu)[15] == S); ++i)
            cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(m.cpu, 50);
        return cpuGetRegisters(m.cpu)[15] == S ? cpuGetRegExternal(m.cpu, 5) : 99u;
    };
    uint64_t stale0, compiled0, used0;
    if (!cpuJitCompileCounts(m.cpu, &stale0, &compiled0, &used0)) {
        printf("FAIL %s: no compile counts\n", name);
        return -1;
    }
    // A compiled and installed without B running; B compiled ahead meanwhile. A's other
    // target (A + 12) runs, so once one more block is compiled ahead than used, it is B,
    // kept ready.
    bool ready = false;
    uint64_t stale = 0, compiled = 0, used = 0;
    for (int i = 0; i < 2000 && !ready; ++i) {
        runFrom(A, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        cpuJitCompileCounts(m.cpu, &stale, &compiled, &used);
        ready = i >= 20 && (compiled - compiled0) > (used - used0);
    }
    if (!ready) {
        printf("FAIL %s: B was never compiled ahead (compiled %llu, used %llu)\n", name,
               (unsigned long long)(compiled - compiled0), (unsigned long long)(used - used0));
        return -1;
    }
    const uint64_t staleBefore = stale;
    const uint32_t word = 0xe3a05002;
    memAccess(m.mem, B, 4, true, (void*)&word);
    uint32_t bad = 0, last = 0;
    for (int i = 0; i < 20; ++i) {
        last = runFrom(A, 1);
        if (last != 2) bad++;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    cpuJitCompileCounts(m.cpu, &stale, &compiled, &used);
    printf("%s: r5 %u after the rewrite, %u runs of 20 not 2, %llu dropped as stale\n", name, last, bad,
           (unsigned long long)(stale - staleBefore));
    if (bad || stale == staleBefore) {
        printf("FAIL %s: expected 2, with the ready B dropped\n", name);
        return -1;
    }
    return 0;
}

// Privilege switches with code kept per privilege level, as on the TG50, which runs
// applications in user mode. S is set, so the vectors and kCode are privileged only. User
// code at U loops 8 times: a load from the privileged-only page (data abort, skipped), a
// SWI into a handler at 0x200 that reads that page, stores to Y and, on its 4th entry,
// rewrites the first word of X (a user function on a page user code ran) and invalidates
// its I-cache line, then a call to X, then the same MCR in user mode (undefined). The run
// gets to user mode by MOVS PC from SVC, or by MSR CPSR_c from code on a user page. X adds
// 1 to r0 until it's rewritten to add 16: r0 is 3 + 5 * 16 at the end. The privileged store
// to X goes through the page table that had Y's page writable before user code ran X, so
// it only reaches the user level's code if X's code flag write-protected both levels.
bool privilegeSwitch(Engines& e) {
    constexpr uint32_t U = 0x24000, X = 0x25000, Y = X + 0x100, M = 0x26000;
    bool ok = true;
    for (int variant = 0; variant < 2; ++variant) {
        const char* name = variant ? "privilege switch, msr cpsr_c" : "privilege switch, movs pc";
        const Directed d{name,
                         {0xe3a09000, 0xe361f010, 0xe1a0e005, 0xe1b0f00e},  // mov r9, #0; msr spsr_c, #usr;
                         false,                                             // mov lr, r5; movs pc, lr
                         SVC,
                         {{0, 0}, {1, 0x28000}, {2, 0xe2800010}, {4, X}, {5, U}, {7, 0x5a5a5a5a}},
                         {{Expect::REG, 0, 83}, {Expect::REG, 3, 0}, {Expect::REG, 9, 8},
                          {Expect::LOADED, 6, 0x28000}, {Expect::CPSR, 0, USR, 0x3f},
                          {Expect::MEM, Y, 0x5a5a5a5a}, {Expect::MEM, X, 0xe2800010}},
                         0x100};
        for (uint64_t seed = 1; seed <= 8; ++seed) {
            const Case c = directedCase(d, seed);
            Assembled a = assemble(c);
            auto put = [&](uint32_t addr, uint32_t v) { memcpy(&a.image[addr], &v, 4); };
            put(0x08, 0xea00007c);  // b 0x200
            const uint32_t handler[] = {
                0xe5916000,  // ldr r6, [r1]
                0xe5847100,  // str r7, [r4, #0x100]
                0xe2899001,  // add r9, r9, #1
                0xe3590004,  // cmp r9, #4
                0x05842000,  // streq r2, [r4]
                0xee074f35,  // mcr p15, 0, r4, c7, c5, 1
                0xe1b0f00e,  // movs pc, lr
            };
            const uint32_t user[] = {
                0xe3a03008,  // mov r3, #8
                0xe5918000,  // ldr r8, [r1] (aborts)
                0xef000000,  // swi 0
                0xeb0003fb,  // bl X
                0xee074f35,  // mcr p15, 0, r4, c7, c5, 1 (undefined)
                0xe2533001,  // subs r3, r3, #1
                0x1afffff9,  // bne U + 4
                kSpinArm,
            };
            for (uint32_t i = 0; i < 7; ++i) put(0x200 + 4 * i, handler[i]);
            for (uint32_t i = 0; i < 8; ++i) put(U + 4 * i, user[i]);
            put(X, 0xe2800001);      // add r0, r0, #1
            put(X + 4, 0xe12fff1e);  // bx lr
            put(M, 0xe3a09000);      // mov r9, #0
            put(M + 4, 0xe321f010);  // msr cpsr_c, #usr
            put(M + 8, 0xe1a0f005);  // mov pc, r5
            if (variant) a.start = M;
            Snapshot xs;
            Outcome o = runAssembled(e, c, a, &xs);
            if (o.ok && !o.unstopped) {
                o.diffs = unexpected(d, xs);
                if (o.diffs.empty() && xs.stopped) continue;
                if (!xs.stopped) o.diffs.push_back("exact did not stop");
            }
            printf("FAIL %s (seed %" PRIu64 ")%s\n", name, seed, o.unstopped ? " did not stop" : "");
            for (const auto& s : o.diffs) printf("  %s\n", s.c_str());
            ok = false;
            break;
        }
    }
    return ok;
}

struct CpuSaver {
    ArmCpu* cpu;
    template <class T>
    void Save(T& s) { cpuSave(cpu, s); }
    void Load(SavestateLoader<ChunkType>& l) { cpuLoad(cpu, l); }
};

// moveOn: after the store the guest runs through another line (Z) before stopping, so
// Exact's buffers no longer hold A's line; otherwise it stops in Y's line and Exact runs
// the old word, the expected difference. wmmx: the rewrite is a WSTRW (the JIT's cp1 slot)
// instead of an STR. 0: as expected, -1: not.
int fetchBuffer(Engines& e, bool moveOn, bool wmmx = false) {
    const char* name = wmmx ? (moveOn ? "fetch buffer, moved on, wstrw" : "fetch buffer, still held, wstrw")
                            : (moveOn ? "fetch buffer, moved on" : "fetch buffer, still held");
    Case c = generate(99);
    c.items.clear();
    c.codeCacheable = false;  // the fetch buffers are used for uncacheable code
    c.dacr = 1;
    c.table[2] = 1;
    if (wmmx) c.table[6 + 32 + 6] = 3;  // CPAR for the block (generate's table)
    const uint32_t A = kCode, Y = kCode + 0x100, Z = kCode + 0x180, S = kCode + 0x200;
    std::vector<std::pair<uint32_t, uint32_t>> words;
    if (moveOn)
        words = {
            {A + 0, 0xee070f9a},   // mcr p15, 0, r0, c7, c10, 4 (drain): interpreted
            {A + 4, 0xea00003d},   // b Y
            {A + 8, 0xe3a05001},   // mov r5, #1 (rewritten to mov r5, #2)
            {A + 12, 0xea00007b},  // b S
            {Y + 0, 0xe59f1008},   // ldr r1, [pc, #8] (the new word)
            {Y + 4, 0xe59f2008},   // ldr r2, [pc, #8] (A + 8)
            {Y + 8, 0xe5821000},   // str r1, [r2]
            {Y + 12, 0xea00001b},  // b Z
            {Z, 0xea00001e},       // b S (to the spin, where the host saves); Z's line evicts A's
            {Y + 16, 0xe3a05002},  // the new word
            {Y + 20, A + 8},
            {S, kSpinArm},
        };
    else
        words = {
            {A + 0, 0xee070f9a},   // mcr p15, 0, r0, c7, c10, 4 (drain): interpreted
            {A + 4, 0xea00003d},   // b Y
            {A + 8, 0xe3a05001},   // mov r5, #1 (rewritten to mov r5, #2)
            {A + 12, 0xea00003e},  // b Y + 12
            {Y + 0, 0xe59f1008},   // ldr r1, [pc, #8] (the new word)
            {Y + 4, 0xe59f2008},   // ldr r2, [pc, #8] (A + 8)
            {Y + 8, 0xe5821000},   // str r1, [r2]
            {Y + 12, kSpinArm},    // the spin, where the host saves: A's line is still buffered
            {Y + 16, 0xe3a05002},  // the new word
            {Y + 20, A + 8},
        };
    if (wmmx) {
        // tmcrr wR0, r1, r3 and wstrw wR0, [r2] in place of the STR: Y's words from Y + 8
        // move down one, and the loads reach one further.
        for (auto& [addr, v] : words) {
            if (addr == Y + 0) v = 0xe59f100c;
            if (addr == Y + 4) v = 0xe59f200c;
            if (addr == Y + 8) v = 0xec431000;
            else if (addr >= Y + 12 && addr < Y + 0x20) {
                addr += 4;
                if (v == 0xea00001b) v = 0xea00001a;  // b Z
            }
            if (addr == A + 12 && v == 0xea00003e) v = 0xea00003f;  // b Y + 16
        }
        words.push_back({Y + 12, 0xed820100});
    }
    const uint32_t spin = moveOn ? S : wmmx ? Y + 16 : Y + 12;
    // The code page has to be writable for the rewrite: patch the code into data page
    // 0x2b000 (read-write, uncacheable) instead, on the generator's image.
    Item end;
    end.kind = "end";
    end.removable = false;
    end.ins.push_back({kSpinArm, false});
    c.items.push_back(end);
    Assembled a = assemble(c);
    const uint32_t base = 0x2b000;
    auto put = [&](uint32_t addr, uint32_t v) { memcpy(&a.image[addr - kCode + base], &v, 4); };
    for (uint32_t p = base; p < base + 0x1000; p += 4) memcpy(&a.image[p], &kTrapWord, 4);
    for (auto [addr, v] : words) put(addr, v == A + 8 ? base + 8 : v);
    a.start = base;
    a.thumb = false;
    c.cpsr = SVC | 0xc0;
    const uint32_t spinAt = spin - kCode + base;

    struct Result {
        uint32_t r5;
        bool ok;
    };
    auto finish = [&](Machine& m) -> Result {
        // Back to A + 8 without an invalidation, then to the spin.
        cpuSetReg(m.cpu, 15, base + 8);
        for (int i = 0; i < 100 && !(atSpin(m) && (cpuGetRegisters(m.cpu)[15] != base + 8)); ++i)
            cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(m.cpu, m.fast ? 7 : 1);
        return {cpuGetRegExternal(m.cpu, 5), atSpin(m) && cpuGetRegisters(m.cpu)[15] == spinAt};
    };
    auto toSpin = [&](Machine& m) {
        prepare(m, c, a);
        cpuSetReg(m.cpu, 5, 0);
        for (int i = 0; i < 1000 && !atSpin(m); ++i) cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(m.cpu, m.fast ? 50 : 1);
        return atSpin(m) && cpuGetRegisters(m.cpu)[15] == spinAt;
    };
    if (!toSpin(e.exact) || !toSpin(e.fast)) {
        printf("FAIL %s: setup did not reach the save point\n", name);
        return -1;
    }
    // Fast save, loaded into a fresh Exact CPU with Fast's RAM.
    Savestate<ChunkType> saved;
    CpuSaver fs{e.fast.cpu};
    if (!saved.Save(fs)) abort();
    std::vector<uint8_t> blob((uint8_t*)saved.GetBuffer(), (uint8_t*)saved.GetBuffer() + saved.GetSize());
    static Machine loaded = makeMachine(false);
    memcpy(loaded.ram, e.fast.ram, kRamSize);
    cpuInvalidateCodeCache(loaded.cpu);
    SavestateLoader<ChunkType> loader;
    CpuSaver ls{loaded.cpu};
    if (!loader.Load(blob.data(), blob.size(), ls)) abort();

    const Result x = finish(e.exact), f = finish(e.fast), l = finish(loaded);
    printf("%s: r5 after the rewrite: exact %u, fast %u, fast save loaded in exact %u\n", name, x.r5, f.r5, l.r5);
    if (!x.ok || !f.ok || !l.ok) {
        printf("FAIL %s: did not get back to the spin\n", name);
        return -1;
    }
    // Exact runs the old word only while A's line is still buffered; Fast and the Fast save
    // always run the new one.
    const uint32_t exactWant = moveOn ? 2 : 1;
    if (x.r5 != exactWant || f.r5 != 2 || l.r5 != 2) {
        printf("FAIL %s: expected exact %u, fast 2, fast save loaded in exact 2\n", name, exactWant);
        return -1;
    }
    printf(moveOn ? "%s: same in both engines\n" : "%s: exact runs the old word, fast the new one, as expected\n",
           name);
    return 0;
}

}  // namespace

// Every iWMMXt word pxa270wmmxJitValid accepts (all CDP, MCR, MRC, MCRR, MRRC, LDC and STC
// encodings on cp0 and cp1, condition AL) gets a callback from the JIT's slot as dynarmic
// would ask for it: dynarmic asserts on a refusal.
bool wmmxCompiles(Engines& e) {
    uint64_t valid[3] = {}, refused = 0;
    auto check = [&](uint32_t w, int k) {
        if (!pxa270wmmxJitValid(w)) return;
        ++valid[k];
        if (cpuJitWmmxCompiles(e.fast.cpu, w)) return;
        if (++refused <= 10) printf("FAIL iwmmxt %08x is valid for the JIT but its slot refuses it\n", w);
    };
    for (uint32_t x = 0; x < (1u << 24); ++x)
        if (!(x & 0xe00)) check(AL | 0x0e000000 | x, 0);  // CDP, MCR, MRC
    for (uint32_t x = 0; x < (1u << 21); ++x)
        if (!(x & 0xe00)) check(AL | 0x0c400000 | x, 1);  // MCRR, MRRC
    for (uint32_t x = 0; x < (1u << 25); ++x)
        if (!(x & 0xe00) && (x & 0x01e00000) != 0x00400000) check(AL | 0x0c000000 | x, 2);  // LDC, STC
    printf("iwmmxt slots: %" PRIu64 " CDP/MCR/MRC, %" PRIu64 " MCRR/MRRC and %" PRIu64
           " LDC/STC words valid, %" PRIu64 " refused\n",
           valid[0], valid[1], valid[2], refused);
    return !refused && valid[0] && valid[1] && valid[2];
}

int main(int argc, char** argv) {
    uint64_t first = 1, last = 30001, one = 0;
    bool single = false, show = false, runDirected = true, open = false;
    double duration = 0;
    setenv("FROND_JIT_HOT", "1", 1);
    cpuSetJitSync(true);
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seed" && i + 1 < argc) {
            one = strtoull(argv[++i], nullptr, 0);
            single = true;
        } else if (a == "--seeds" && i + 1 < argc) {
            const std::string r = argv[++i];
            const size_t colon = r.find(':');
            first = strtoull(r.substr(0, colon).c_str(), nullptr, 0);
            open = colon == std::string::npos || colon + 1 == r.size();
            last = open ? UINT64_MAX : strtoull(r.substr(colon + 1).c_str(), nullptr, 0);
        } else if (a == "--duration" && i + 1 < argc) {
            duration = atof(argv[++i]);
        } else if (a == "--max-unchecked" && i + 1 < argc) {
            setenv("FROND_JIT_MAX_UNCHECKED", argv[++i], 1);
        } else if (a == "--hot" && i + 1 < argc) {
            setenv("FROND_JIT_HOT", argv[++i], 1);
        } else if (a == "--thread") {
            cpuSetJitSync(false);
            gThread = true;
        } else if (a == "--verbose") {
            gVerbose = true;
        } else if (a == "--trace") {
            gTrace = true;
        } else if (a == "--show") {
            show = true;
        } else if (a == "--no-directed") {
            runDirected = false;
        } else {
            fprintf(stderr,
                    "usage: %s [--seed N [--show] [--trace]] [--seeds A:B] [--duration S] [--no-directed]\n"
                    "       [--max-unchecked L] [--hot N] [--thread] [--verbose]\n",
                    argv[0]);
            return 2;
        }
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Engines e{makeMachine(false), makeMachine(true)};
    e.bound = cpuMaxOvershootCycles(e.fast.cpu);
    if (cpuMaxOvershootCycles(e.exact.cpu) != 19 || e.bound <= 19 || e.bound == UINT32_MAX) {
        fprintf(stderr, "unexpected bounds: exact %u fast %u\n", cpuMaxOvershootCycles(e.exact.cpu), e.bound);
        return 1;
    }
    if (single) {
        const Case c = generate(one);
        if (show) printCase(c);
        if (gTrace && show) trace(e, c);
        const Outcome o = runCase(e, c);
        if (!show && !o.ok) printCase(c);
        if (o.unstopped) printf("seed %" PRIu64 ": neither engine stopped\n", one);
        if (o.ok) {
            printf("seed %" PRIu64 ": same in both engines, %" PRIu64 " instructions\n", one, o.instructions);
            return 0;
        }
        report(e, c, o);
        return 1;
    }
    bool ok = true;
    if (runDirected && !single) {
        ok = directed(e) && ok;
        ok = privilegeSwitch(e) && ok;
        ok = wmmxCompiles(e) && ok;
        ok = fetchBuffer(e, true) == 0 && ok;
        ok = fetchBuffer(e, false) == 0 && ok;
        ok = fetchBuffer(e, true, true) == 0 && ok;
        ok = fetchBuffer(e, false, true) == 0 && ok;
        ok = staleLink() == 0 && ok;
        ok = staleDispatch() == 0 && ok;
        ok = cacheFullDispatch(false) == 0 && ok;
        ok = cacheFullDispatch(true) == 0 && ok;
        ok = staleAhead() == 0 && ok;
    }
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    uint64_t cases = 0, failures = 0, unstopped = 0, instructions = 0;
    double nextReport = 60;
    for (uint64_t seed = first; seed < last; ++seed) {
        if (duration > 0 && elapsed() >= duration) break;
        if (gVerbose) printf("seed %" PRIu64 "\n", seed);
        const Case c = generate(seed);
        const Outcome o = runCase(e, c);
        ++cases;
        instructions += o.instructions;
        unstopped += o.unstopped;
        if (!o.ok) {
            ++failures;
            ok = false;
            if (failures <= 20) report(e, c, o);
        }
        if (open || duration > 0) {
            if (elapsed() >= nextReport) {
                printf("progress: %.0f s, seeds %" PRIu64 "-%" PRIu64 ", %" PRIu64 " cases, %" PRIu64
                       " instructions, %" PRIu64 " failures\n",
                       elapsed(), first, seed, cases, instructions, failures);
                fflush(stdout);
                nextReport += 60;
            }
        }
    }
    printf("jit-differential: %" PRIu64 " cases (seeds %" PRIu64 " to %" PRIu64 "), %" PRIu64
           " instructions, %" PRIu64 " did not stop, %" PRIu64 " failures, %.1f s, bound %u, worst overshoot %u\n",
           cases, first, first + cases - 1, instructions, unstopped, failures, elapsed(), e.bound, e.worstOver);
    printf(ok ? "jit-differential: ok\n" : "jit-differential: FAILED\n");
    return ok ? 0 : 1;
}
