//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include <utility>
#include "CPU.h"
#include "arm920_state.h"
#include "cpu_diagnostics.h"
#ifdef FROND_DYNARMIC
#include "jit_dynarmic.h"
#include "idle_loops.h"
#include "delay_loops.h"
#include "poll_loops.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <iostream>
#include <unordered_map>

#include "CPEndian.h"
#include "MMU.h"
#ifndef __EMSCRIPTEN__
#include "MMU_internal.h"
#endif
#include "MPU.h"
#include "cp15mmu.h"
#include "cp15mpu.h"
#include "cputil.h"
#include "gdbstub.h"
#include "icache.h"
#ifndef __EMSCRIPTEN__
#include "icache_internal.h"
#include "mem_internal.h"
#endif
#include "mem.h"
#include "ROM.h"
#include "memcpy.h"
#include "pace.h"
#include "patch68k.h"
#include "patch_dispatch.h"
#include "peephole.h"
#include <algorithm>
#include "xscale_cost.h"
#include "dcache_cost.h"
#include "peephole_cost.h"
#include "pxa270_WMMX.h"
#include "savestate/savestateAll.h"
#include "system_state.h"

#pragma GCC diagnostic ignored "-Wunused-but-set-parameter"

#define xstr(s) str(s)
#define str(s) #s

#define unlikely(x) __builtin_expect((x), 0)
#define likely(x) __builtin_expect((x), 1)

#define NO_SUPPORT_FCSE
#define NO_STRICT_CPU
#define NO_TRACE_PACE

#define ARM_MODE_2_REG 0x0F
#define ARM_MODE_2_WORD 0x10
#define ARM_MODE_2_LOAD 0x20
#define ARM_MODE_2_T 0x40
#define ARM_MODE_2_INV 0x80
#define ARM_MODE_2_SYSCALL 0x01

#define ARM_MODE_3_TYPE 0x30  // flag for the below 4 types
#define ARM_MODE_3_H 0x00
#define ARM_MODE_3_SH 0x10
#define ARM_MODE_3_SB 0x20
#define ARM_MODE_3_D 0x30
#define ARM_MODE_3_LOAD 0x40
#define ARM_MODE_3_INV 0x80
#define ARM_MDOE_3_PCREL 0x01

#define ARM_MODE_4_REG 0x0F
#define ARM_MODE_4_INC 0x10  // incr or decr
#define ARM_MODE_4_BFR 0x20  // before or after
#define ARM_MODE_4_WBK 0x40  // writeback?
#define ARM_MODE_4_S 0x80    // S bit set?

#define ARM_MODE_5_REG 0x0F
#define ARM_MODE_5_IS_OPTION 0x10  // is value option (as opposed to offset)
#define ARM_MODE_5_RR 0x20         // MCRR or MRCC instrs

#define REG_NO_SP 13
#define REG_NO_LR 14
#define REG_NO_PC 15

#define INJECTED_CALL_LR_MAGIC 0xfffffffc

#define EXEC_FN_PREFIX_VALUE 0x53ae0000

#define SAVESTATE_VERSION 3

#ifdef __EMSCRIPTEN__
    #define PREFIX_EXEC_FN(...) (ExecFn((uint32_t)__VA_ARGS__ + EXEC_FN_PREFIX_VALUE))
    #define ATTR_EMCC_NOINLINE __attribute__((noinline))
#else
    #define PREFIX_EXEC_FN(...) (__VA_ARGS__)
    #define ATTR_EMCC_NOINLINE
#endif

#define cpuPrvGetRegNotPC(cpu, reg) (cpu->regs[reg])

typedef void (*ExecFn)(struct ArmCpu *cpu, uint32_t instr);

/*

        coprocessors:

                                0    - DSP (pxa only)
                                0, 1 - WMMX (pxa only)
                                11   - VFP (arm standard)
                                15   - system control (arm standard)
*/

struct ArmBankedRegs {
    uint32_t R13, R14;
    uint32_t SPSR;  // usr mode doesn't have an SPSR

    template <typename T>
    void DoSaveLoad(T &chunkHelper) {
        chunkHelper.Do32(R13).Do32(R14).Do32(SPSR);
    }
};

using M68kTrapHandlerMap = std::unordered_map<uint32_t, M68kTrapHandler>;

// ARM920T Fast: the RAM pages code was decoded or translated from (a bit per 4 KiB page over the 4 GiB
// physical map), and in each page the 64-byte lines it came from (decoded code) or the 4-byte words
// (translated code). A store drops code only when it lands on a marked line or word, so data sharing a
// page with code leaves the code alone; translated code loses only the blocks the store overlaps. A
// marked page has no write entries in the host TLB, so every store to it comes through
// cpuPrvArm920Stored. Line and word masks are kept per group of 64 pages and allocated when a group is
// first used.
struct Arm920CodeMap {
    static constexpr uint32_t kGroups = 1u << 14;
    uint64_t pages[kGroups] = {};
    uint64_t *lines[kGroups] = {};
    // Fast blocks: a bit per 4-byte word of each page instead (16 per page), with stores checked by word.
    uint64_t *words[kGroups] = {};
    std::vector<uint16_t> groups;  // groups with page bits set since the last clear, each once
    uint64_t listed[kGroups / 64] = {};

    ~Arm920CodeMap() {
        for (uint64_t *l : lines) delete[] l;
        for (uint64_t *w : words) delete[] w;
    }
    bool page(uint32_t pa) const { return (pages[pa >> 18] >> ((pa >> 12) & 63)) & 1; }
    // Marks the line holding pa in a marked page.
    void noteLine(uint32_t pa) { lines[pa >> 18][(pa >> 12) & 63] |= uint64_t(1) << ((pa >> 6) & 63); }
    uint64_t *pageWords(uint32_t pa) {
        uint64_t *&w = words[pa >> 18];
        if (!w) w = new uint64_t[64 * 16]();
        return w + ((pa >> 12) & 63) * 16;
    }
    // Marks the word holding pa in a marked page.
    void noteWord(uint32_t pa) { pageWords(pa)[(pa >> 8) & 15] |= uint64_t(1) << ((pa >> 2) & 63); }
    void setWords(uint32_t pa, const uint64_t *mask) { std::memcpy(pageWords(pa), mask, 16 * sizeof(uint64_t)); }
    // Marks the line holding pa, or only its page; true if the page wasn't marked before.
    bool note(uint32_t pa, bool line = true) {
        const uint32_t g = pa >> 18;
        const uint64_t bit = uint64_t(1) << ((pa >> 12) & 63);
        if (!lines[g]) lines[g] = new uint64_t[64]();
        if (line) noteLine(pa);
        if (pages[g] & bit) return false;
        pages[g] |= bit;
        if (!(listed[g >> 6] & (uint64_t(1) << (g & 63)))) {
            listed[g >> 6] |= uint64_t(1) << (g & 63);
            groups.push_back(uint16_t(g));
        }
        return true;
    }
    // Whether [pa, pa + size), inside one page, touches a marked line. The page must be marked.
    bool hit(uint32_t pa, uint32_t size) const {
        const uint32_t first = (pa >> 6) & 63, last = ((pa + size - 1) >> 6) & 63;
        const uint64_t upTo = last == 63 ? ~uint64_t(0) : (uint64_t(1) << (last + 1)) - 1;
        return lines[pa >> 18][(pa >> 12) & 63] & upTo & ~((uint64_t(1) << first) - 1);
    }
    // The same by word. The page must be marked.
    bool hitWords(uint32_t pa, uint32_t size) const {
        const uint64_t *w = words[pa >> 18];
        if (!w) return false;
        w += ((pa >> 12) & 63) * 16;
        for (uint32_t i = (pa >> 2) & 1023, last = ((pa + size - 1) >> 2) & 1023; i <= last; i++)
            if ((w[i >> 6] >> (i & 63)) & 1) return true;
        return false;
    }
    void clearPage(uint32_t pa) {
        pages[pa >> 18] &= ~(uint64_t(1) << ((pa >> 12) & 63));
        lines[pa >> 18][(pa >> 12) & 63] = 0;
        if (uint64_t *w = words[pa >> 18]) std::memset(w + ((pa >> 12) & 63) * 16, 0, 16 * sizeof(uint64_t));
    }
    void clear() {
        for (uint16_t g : groups) {
            pages[g] = 0;
            std::memset(lines[g], 0, 64 * sizeof(uint64_t));
            if (words[g]) std::memset(words[g], 0, 64 * 16 * sizeof(uint64_t));
            listed[g >> 6] &= ~(uint64_t(1) << (g & 63));
        }
        groups.clear();
    }
};

// Fast's data cache hint, in the layout generated code reads (A32::UserConfig::frond_cost_hint).
// join[VA[14:4]] is va & ~15 when a store there goes to the write buffer (a buffered or write-through
// page, or a write-back one without the line) and a write buffer slot holds its block; joinStart is the
// latest such slot's drain start. The store joins it, at no charge and with no change, when that start
// is at or after the store's clock, runBase - ticks left + pending (as cpuPrvCostAccess's now, with
// runBase the run's base clock plus its budget), so generated code checks that too, with the ticks left
// at most budget. halted is set once a callback has ended the run for its charges (at the next link):
// from then on hits needn't go to the callbacks for that.
struct CostJitHint {
    uint32_t load[1024];
    uint32_t store[2048];
    uint32_t pending;
    uint32_t pad;  // 0: generated code reads pending as a 64-bit word
    uint32_t halted;
    uint32_t pad2;
    int64_t runBase;
    int64_t budget;
    uint32_t join[2048];
    uint64_t joinStart[2048];
};
static_assert(offsetof(CostJitHint, store) == 1024 * 4 && offsetof(CostJitHint, pending) == 3072 * 4 &&
              offsetof(CostJitHint, halted) == 12296 && offsetof(CostJitHint, runBase) == 12304 &&
              offsetof(CostJitHint, budget) == 12312 && offsetof(CostJitHint, join) == 12320 &&
              offsetof(CostJitHint, joinStart) == 20512);

struct ArmCpu {
    uint64_t exceptionSequence;
    ArmCpuModel model;
    M68kSyscallHandler m68kSyscallHandler;
    void* m68kSyscallContext;
    uint32_t regs[16];  // current active regs as per current mode
    uint32_t SPSR;
    uint32_t flags;
    bool Q, T, I, F;
    uint8_t M;
    bool privileged;

    uint32_t curInstrPC;
#ifndef __EMSCRIPTEN__
    const uint32_t* cyclesInProgress;
#endif

    // Cost model (xscale_cost.h, dcache_cost.h). The I-cache charges costCore's table per instruction when
    // its costOn is set (1 per instruction otherwise); costMemory adds the region and data cache charges
    // of loads and stores (XScale). jitHint.pending holds the running instruction's memory charges until its
    // loop adds them to the count; costClock is every finished batch's clocks, the base of the data cache
    // model's bus time. regionCharge and cacheCosts are in core clocks at costClocks. paceCost is a 68k op.
    bool costMemory;
    bool costMemoryAsked;  // cpuSetCostModel's memory
    bool costJitMemory;    // Fast charges memory too (Original speed, cpuSetJitMemoryCost)
    bool costInJitRun;     // a JIT run is in progress: the data cache model's time adds jitRanNow
    uint8_t costCore;
    uint32_t paceCost;
    uint64_t costClock;
    cost::Clocks costClocks;
    bool costSdram16;  // the SDRAM bus is 16 bits wide (MDCNFG DWID0), from the memory controller; not saved
    uint32_t regionCharge[cost::regionCount][2];
    cost::CacheCosts cacheCosts[3];
    cost::DataCacheState dcache;
    uint8_t dcacheHint[32];  // the way each set hit last, looked at first; not saved
    // Fast's data cache hint, checked by generated loads and stores before the page table
    // (A32::UserConfig::frond_cost_hint), always a subset of dcache: load[VA[14:5]] is va & ~31 when that
    // line is in the model and its page is cached, store[VA[14:4]] is va & ~15 when the line is in the
    // model, its page write-back and that half dirty, so those accesses cost nothing and change nothing.
    // Kept only while costJitHint is set; not saved (a load or reset empties it). pending is the running
    // instruction's memory charges (pendingCharge), which generated code compares with the cycles left.
    CostJitHint jitHint;
    bool costJitHint;  // Fast with memory charges: jitHint is kept
#ifndef __EMSCRIPTEN__
    // Per host TLB entry, written with it: the page's C (bit 0), B (1) and X (2) bits and its region (4-6).
    uint8_t hostTlbInfo[2][HOST_TLB_SIZE];
#endif

    struct ArmBankedRegs bank_usr;  // usr regs when in another mode
    struct ArmBankedRegs bank_svc;  // svc regs when in another mode
    struct ArmBankedRegs bank_abt;  // abt regs when in another mode
    struct ArmBankedRegs bank_und;  // und regs when in another mode
    struct ArmBankedRegs bank_irq;  // irq regs when in another mode
    struct ArmBankedRegs bank_fiq;  // fiq regs when in another mode
    uint32_t extra_regs[5];  // fiq regs when not in fiq mode, usr regs when in fiq mode. R8-12

    uint16_t waitingIrqs;
    uint16_t waitingFiqs;
    uint16_t waitingEventsTotal;
    uint16_t CPAR;

    struct ArmCoprocessor coproc[16];  // coprocessors
    struct Pxa270wmmx *wmmx;            // iWMMXt on coprocessors 0 and 1, or null

    // various other cpu config options
    uint32_t vectorBase;  // address of vector base

    uint32_t pid;  // for fcse

    // Undefined instruction exceptions taken, and the last one's pc and instruction, for machines that
    // stop on them (cpuUndefinedTaken).
    uint64_t undefinedCount;
    uint32_t undefinedPc, undefinedInstr;
    // Called on each one as it's taken, or null (cpuSetUndefinedHook).
    void (*undefinedHook)(void *context, uint32_t pc, uint32_t instr);
    void *undefinedHookContext;
    // Failed data accesses are logged only for the first 16 and then every power of two (cpuSetAbortLogLimit).
    bool abortLogLimited;

    uint8_t memorySystemKind;
    union {
        struct ArmMmu *mmu;
        struct ArmMpu *mpu;
    } memorySystem;

    struct icache *ic;
    struct ArmMem *mem;

    union {
        struct ArmCP15MMU *cp15mmu;
        struct ArmCP15MPU *cp15mpu;
    } cp15;

    struct PacePatch *pacePatch;
    uint32_t paceOffset;
    bool modePace;
    // Idle loop heads cpuJitPlain left to the interpreter (Fast idle skip), 0 for none.
    uint32_t idleHeads[2];
    // The idle loops matched at those heads and their pass costs (cpuPrvIdleLoopAt). Host only, never saved.
    struct IdleMatch {
        uint32_t pc;  // 0: empty
        const struct IdleLoop *loop;
        uint8_t *pages[4];  // RAM pages holding the loop and window, their lines flagged as code
        uint32_t pageCount;
        uint32_t mapGen;
        uint64_t permContext;
        bool privileged;
        uint32_t costKey;  // costOn and costCore the costs are for, plus 1; 0 for none
        uint32_t cost[3];  // per head form (idleHeadVariant), 0 until worked out
        const struct DelayLoop *delay;  // delayMatch only: the delay loop, in place of loop
        const struct PollLoop *poll;    // pollMatch only: the poll loop, in place of loop
    } idleMatch[2];
    // The same for the OSCR delay loops (delay_loops.h, cpuPrvDelaySkip). cost[0] and cost[1] are the pass
    // and load costs for costKey.
    uint32_t delayHeads[2];
    IdleMatch delayMatch[2];
    // The OS timer for the delay skip (cpuSetDelayTimer): OSCR's physical address, and what a load of it
    // would give with that many cycles in progress, without changing anything. False when such a load
    // would reach a match. Host binding, never saved.
    uint32_t delayOscrPa;
    bool (*delayPeek)(void *, uint32_t, uint32_t *);
    void *delayPeekContext;
    // The same for the GPIO level polls (poll_loops.h, cpuPrvPollSkip), and the level peek (cpuSetPollPeek).
    uint32_t pollHeads[2];
    IdleMatch pollMatch[2];
    bool (*pollPeek)(void *, uint32_t, uint32_t *);
    void *pollPeekContext;
    bool sleeping;
    bool arm920WaitEnabled, arm920Waiting;

    struct stub *debugStub;
    struct PatchDispatch *patchDispatch;
    struct SystemState *systemState;

    uint32_t slowPath;
    void (*earlyAudioHook)(ArmCpu*,void*,uint32_t,uint32_t) = nullptr;
    void* earlyAudioContext = nullptr;
    uint32_t earlyAudioFrom[2]{};
    uint16_t breakPaceSyscall;
    #ifndef __EMSCRIPTEN__
    uint32_t nativePc[2];
    struct { uint32_t address; NativeCallback callback; void* context; } nativeCallbacks[8];
    uint8_t nativeCallDepth;
    bool nativeSessionActive;
    uint32_t nativeStackLower, nativeStackUpper;
    uint32_t nativeStackOwnerAddress, nativeStackOwnerValue;
    uint32_t nativeScratchBase, nativeScratchSize;
    uint8_t* nativeScratchData;
    #endif


    M68kTrapHandlerMap *m68kTrap0Handlers;
    // ARM920T Fast engine, not saved. leanRun is set only inside cpuArm920RunLean; leanExit asks it to stop
    // before the current instruction.
    bool leanRun, leanExit;
    uint32_t leanBlNext;
    Arm920LeanPage leanPage;
    Arm920LeanGuard leanGuard;
    void* leanContext;
    // The RAM pages and lines the decode cache holds code from, and the data accesses this lean run made
    // that the step would count as bus accesses.
    Arm920CodeMap* leanCode;
    uint64_t leanBus, leanCodeDrops;
    // Cost model clocks of the instructions this lean run has finished (leanRetired the instructions), the
    // running one's charge, and the probe's answer for reads of pages that aren't plain.
    uint32_t leanDone, leanRetired, leanCharge;
    Arm920LeanRead leanRead;
    // The probe's answer for single stores to pages that aren't plain, the instruction the lean loop is running
    // and the run's budget, which such a store can shrink.
    Arm920LeanWrite leanWrite;
    uint32_t leanInstr, leanBudget;
    // Fast blocks: virtual pages whose stores went to leanWrite (not plain, not cacheable or bufferable), keyed as
    // the host TLB is (cpuJitLeanDeviceWrite). pa is the page's physical address with bit 0 set when in use.
    struct { uint64_t key; uint32_t pa; } leanDeviceWrite[16];
    // Fast blocks: the pc of the last access-pc instruction generated code ran (cpuArm920SetJitAccessPcs), taken by
    // the lean read callback for the access it makes, then cleared.
    uint32_t jitAccessPc;
    // Fast blocks: cycles of this run the board has charged already (cpuArm920JitCharge), taken off the hooks' ran.
    uint32_t jitCharged;
    const uint32_t *jitAccessPcs;
    uint32_t jitAccessPcCount;
    // Fast: code hooks (cpuArm920SetCodeHooks). The lean path and translated code stop before these pcs and leave
    // them to the step. Host only, never saved.
    uint32_t codeHooks[ARM920_MAX_CODE_HOOKS];
    uint32_t codeHookCount;
    // Most instructions this lean run may retire (cpuArm920RunLeanFor), and whether the JIT's callbacks are
    // using the lean memory path (no read or write answers from the probe there).
    uint32_t leanCount;
    bool leanJit;
#ifdef FROND_DYNARMIC
    struct FrondJit *jit;
    // cpuJitMemoryOnly: code fetches outside RAM and ROM fail, and are noted.
    bool jitMemoryOnly, jitRefused;
    // ARM920T Fast blocks (cpuArm920SetJit): the JIT is run only by cpuArm920RunJit, never by cpuCycle
    // (jitLoopOnly), and while jitBlocks is on it reads code and data from the lean pages. jitCode has the
    // RAM pages and lines it has translated code from, as leanCode.
    bool jitLoopOnly, jitBlocks;
    Arm920CodeMap *jitCode;
#endif

    template <typename T>
    void DoSaveLoad(T &chunkHelper, uint32_t version);
};

// Drops the idle, delay and poll loop matches (cpuPrvIdleLoopAt, cpuPrvDelayLoopAt, cpuPrvPollLoopAt): those with a line in the RAM
// page at host, or all of them for null.
static void cpuPrvIdleForget(ArmCpu *cpu, const uint8_t *host = nullptr) {
    auto forget = [&](ArmCpu::IdleMatch &m) {
        if (!m.pc) return;
        bool drop = !host;
        for (uint32_t i = 0; i < m.pageCount && !drop; i++) drop = m.pages[i] == host;
        if (drop) m.pc = 0;
    };
    for (auto &m : cpu->idleMatch) forget(m);
    for (auto &m : cpu->delayMatch) forget(m);
    for (auto &m : cpu->pollMatch) forget(m);
}

enum ImmShiftType {
    shiftTypeNoop,
    shiftTypeZero,
    shiftTypeLSL,
    shiftTypeLSR,
    shiftTypeASR,
    shiftTypeROR,
    shiftTypeRRX,
};

struct ImmShift {
    ImmShiftType type;
    uint32_t coBit;
    uint8_t shift;
};

static uint32_t *table_thumb2arm = NULL;
static bool table_conditions[256];
static ImmShift table_immShiftReg[1024];
static ImmShift table_immShiftImm[128];

// AL (0xE) always passes, so it skips the flags and table lookup.
static FORCE_INLINE bool cpuPrvConditionFails(const struct ArmCpu *cpu, uint32_t instr) {
    if ((instr >> 28) == 0x0e) return false;
    return table_conditions[((cpu->flags & 0xf0000000UL) >> 24) | (instr >> 28)];
}

// What an instruction costs from its charge slot (icacheCostSlot): its cost, or 1 when its condition fails.
static FORCE_INLINE uint32_t cpuPrvSlotCharge(const struct ArmCpu *cpu, uint32_t slot) {
    const uint32_t cond = slot >> 8;
    if (likely(cond == 14)) return slot & 0xff;
    return table_conditions[((cpu->flags & 0xf0000000UL) >> 24) | cond] ? 1 : slot & 0xff;
}

// Adds the memory charges of the instruction that just ran to the loop's count and to what peripherals see.
static FORCE_INLINE void cpuPrvTakeCharge(struct ArmCpu *cpu, uint32_t &acc, uint32_t &shown) {
    if (unlikely(cpu->jitHint.pending)) {
        acc += cpu->jitHint.pending;
        cpu->jitHint.pending = 0;
        shown = acc;
    }
}

static FORCE_INLINE uint8_t cpuPrvCostInfo(bool c, bool b, bool x, cost::Region region) {
    return uint8_t((c ? 1 : 0) | (b ? 2 : 0) | (x ? 4 : 0) | (unsigned(region) << 4));
}

// The hint's empty entries: each one holds a block of the next index, which no address of its own index
// matches.
static void cpuPrvJoinClear(struct ArmCpu *cpu) {
    for (uint32_t i = 0; i < 2048; i++) cpu->jitHint.join[i] = ((i + 1) & 2047) << 4;
}

static void cpuPrvHintClear(struct ArmCpu *cpu) {
    for (uint32_t i = 0; i < 1024; i++) cpu->jitHint.load[i] = ((i + 1) & 1023) << 5;
    for (uint32_t i = 0; i < 2048; i++) cpu->jitHint.store[i] = ((i + 1) & 2047) << 4;
    cpuPrvJoinClear(cpu);
    cpu->jitHint.pad = 0;
    cpu->jitHint.halted = 0;
    cpu->jitHint.runBase = INT64_MAX / 2;
    cpu->jitHint.budget = 0;
}

// Drops a 16-byte block (its address) from the join table.
static FORCE_INLINE void cpuPrvJoinDrop(struct ArmCpu *cpu, uint32_t block) {
    const uint32_t i = (block >> 4) & 2047;
    if (cpu->jitHint.join[i] == block) cpu->jitHint.join[i] = ((i + 1) & 2047) << 4;
}

// After a store to block went to the write buffer: sets block's join entry. A store that took an entry
// (queued) has it in the slot written last, whose drain start is the latest (starts never go down);
// one that joined left the slots as they were, so an entry already there is still right.
static void cpuPrvJoinNote(struct ArmCpu *cpu, cost::Core core, uint32_t block, bool queued) {
    const uint32_t i = (block >> 4) & 2047;
    const unsigned depth = cost::writeBufferDepth(core);
    if (queued) {
        cpu->jitHint.join[i] = block;
        cpu->jitHint.joinStart[i] = cpu->dcache.bufferStart[(cpu->dcache.bufferNext + depth - 1) % depth];
        return;
    }
    if (cpu->jitHint.join[i] == block) return;
    const uint32_t tag = ((block >> 4) & 0x0fffffffu) | (1u << 31);
    bool found = false;
    uint64_t start = 0;
    for (unsigned k = 0; k < depth; k++) {
        if (cpu->dcache.bufferBlock[k] != tag) continue;
        if (!found || cpu->dcache.bufferStart[k] > start) start = cpu->dcache.bufferStart[k];
        found = true;
    }
    if (!found) return;
    cpu->jitHint.join[i] = block;
    cpu->jitHint.joinStart[i] = start;
}

// cacheStall, dropping the join entry of a block whose slot it reuses when that slot was the block's
// latest (the slots go round in order, so its older ones went before).
static uint32_t cpuPrvCacheStall(struct ArmCpu *cpu, cost::Core core, const cost::CacheEvents &e,
                                 const cost::CacheCosts &k, uint64_t now) {
    if (cpu->costJitHint && (e.writebacks | e.buffered)) {
        const unsigned depth = cost::writeBufferDepth(core);
        const unsigned n = std::min<unsigned>(e.writebacks + e.buffered, depth);
        for (unsigned q = 0; q < n; q++) {
            const unsigned slot = (cpu->dcache.bufferNext + q) % depth;
            const uint32_t b = cpu->dcache.bufferBlock[slot];
            if (!b) continue;
            const uint32_t block = b << 4, i = (block >> 4) & 2047;
            if (cpu->jitHint.join[i] == block && cpu->jitHint.joinStart[i] == cpu->dcache.bufferStart[slot])
                cpuPrvJoinDrop(cpu, block);
        }
    }
    return cost::cacheStall(cpu->dcache, core, e, k, now);
}

// Drops a line (its address, va & ~31) from the hint: its loads, or only its two store halves.
static FORCE_INLINE void cpuPrvHintDrop(struct ArmCpu *cpu, uint32_t line, bool loads) {
    CostJitHint &h = cpu->jitHint;
    if (loads) {
        const uint32_t i = (line >> 5) & 1023;
        if (h.load[i] == line) h.load[i] = ((i + 1) & 1023) << 5;
    }
    for (uint32_t half = line; half < line + 32; half += 16) {
        const uint32_t i = (half >> 4) & 2047;
        if (h.store[i] == half) h.store[i] = ((i + 1) & 2047) << 4;
    }
}

// After an access at va under a cached policy p: notes va's line in the hint if the model now holds it,
// and its dirty halves for stores if the page is write-back. Reads the way dcacheHint points at, where
// dataCacheAccessHinted leaves a line it found or filled.
static FORCE_INLINE void cpuPrvHintNote(struct ArmCpu *cpu, cost::Core core, uint32_t va, cost::Policy p) {
    const cost::CacheGeometry &g = cost::cacheGeometry(core);
    const unsigned set = (va >> 5) & ((1u << g.setBits) - 1);
    const uint32_t l = cpu->dcache.line[set * g.ways + cpu->dcacheHint[set] % g.ways];
    if ((l & (cost::kLineValid | cost::kLineAddressMask)) != (((va >> 5) & cost::kLineAddressMask) | cost::kLineValid))
        return;
    const uint32_t line = va & ~31u;
    cpu->jitHint.load[(line >> 5) & 1023] = line;
    if (p < cost::Policy::writeBack) return;
    if (l & cost::kLineDirtyLow) cpu->jitHint.store[(line >> 4) & 2047] = line;
    if (l & cost::kLineDirtyHigh) cpu->jitHint.store[((line + 16) >> 4) & 2047] = line + 16;
}

static FORCE_INLINE uint32_t cpuPrvEvictedLine(uint32_t l) { return (l & cost::kLineAddressMask) << 5; }

void cpuCostHintClear(struct ArmCpu *cpu) {
    if (cpu->costJitHint) cpuPrvHintClear(cpu);
}

const uint32_t *cpuJitCostHint(struct ArmCpu *cpu) { return cpu->jitHint.load; }

void cpuJitCostRunStart(struct ArmCpu *cpu, uint32_t budget, bool joins) {
    cpu->jitHint.halted = 0;
    // Without a batch the callbacks leave the model alone, and outside cpuPrvCycleJit their clock leaves
    // out jitRanNow; an impossible base sends every such store to them, as does a run counting in
    // fractions of a clock (!joins).
    cpu->jitHint.runBase = joins && cpu->cyclesInProgress && cpu->costInJitRun
                               ? int64_t(cpu->costClock + *cpu->cyclesInProgress + budget)
                               : INT64_MAX / 2;
    cpu->jitHint.budget = budget;
}

void cpuJitCostHalted(struct ArmCpu *cpu) { cpu->jitHint.halted = 1; }

#ifndef __EMSCRIPTEN__
// One load or store's charge: the data cache model's for SDRAM, SRAM and flash, the region charge for the
// rest. info is the page's (cpuPrvCostInfo). Only inside a batch: accesses from outside one (host reads,
// injected call setup) neither change the model nor charge. PACE's accesses change it without a charge,
// as its op cost covers them.
static __attribute__((noinline)) void cpuPrvCostAccess(struct ArmCpu *cpu, uint32_t va, uint8_t info, bool write) {
    if (!cpu->cyclesInProgress) return;
    const cost::Region region = cost::Region(info >> 4);
    const cost::Core core = cost::Core(cpu->costCore);
    uint32_t charge;
    if (!cost::regionCached(region)) {
        charge = cpu->regionCharge[region][write];
    } else {
        const struct ArmMmu *mmu = cpu->memorySystem.mmu;
        const cost::Policy p = cost::policyOf(core, mmu->transTablPA != MMU_DISABLED_TTP, mmu->dcOn, info & 1,
                                              info & 2, info & 4);
        uint64_t now = cpu->costClock + *cpu->cyclesInProgress + cpu->jitHint.pending;
#ifdef FROND_DYNARMIC
        if (cpu->costInJitRun) now += jitRanNow(cpu->jit);
#endif
        const cost::CacheEvents e = cost::dataCacheAccessHinted(cpu->dcache, core, va, p, write, now, cpu->dcacheHint);
        if (cpu->costJitHint) {
            if (e.evicted) cpuPrvHintDrop(cpu, cpuPrvEvictedLine(e.evicted), true);
            if (cost::policyCached(p)) cpuPrvHintNote(cpu, core, va, p);
            // A filled line turns its blocks' stores into hits.
            if (e.fills) {
                cpuPrvJoinDrop(cpu, va & ~31u);
                cpuPrvJoinDrop(cpu, (va & ~31u) + 16);
            }
        }
        // Stores that go to the write buffer: buffered and write-through pages, and write-back ones that
        // missed without allocating.
        const bool buffer = cpu->costJitHint && write &&
                            (p == cost::Policy::buffered || p == cost::Policy::writeThrough ||
                             (p == cost::Policy::writeBack && !e.hits));
        if (!(e.fills | e.writebacks | e.buffered | e.uncachedLoads | e.uncachedStores)) {
            if (buffer) cpuPrvJoinNote(cpu, core, va & ~15u, false);
            return;
        }
        const unsigned k = region == cost::sram ? 1 : region == cost::flash ? 2 : 0;
        charge = cpuPrvCacheStall(cpu, core, e, cpu->cacheCosts[k], now);
        if (buffer) cpuPrvJoinNote(cpu, core, va & ~15u, e.buffered);
    }
    if (!cpu->modePace) cpu->jitHint.pending += charge;
}

// A host TLB hit's charge, from the page's info stored when the entry was filled.
template <int size, bool write>
static FORCE_INLINE void cpuPrvCostHostHit(struct ArmCpu *cpu, uint32_t va) {
    const uint8_t info = cpu->hostTlbInfo[write][(va >> 12) & (HOST_TLB_SIZE - 1)];
    cpuPrvCostAccess(cpu, va, info, write);
    if constexpr (size == 8) cpuPrvCostAccess(cpu, va + 4, info, write);
}
#endif

static FORCE_INLINE uint32_t cpuPrvClz(uint32_t val) {
    if (!val) return 32;

    if (sizeof(int) == sizeof(uint32_t)) return __builtin_clz(val);

    if (sizeof(long) == sizeof(uint32_t)) return __builtin_clzl(val);

    if (sizeof(long long) == sizeof(uint32_t)) return __builtin_clzll(val);

    ERR("CLZ undefined");
}

static FORCE_INLINE uint32_t cpuPrvROR(uint32_t val, uint_fast8_t ror) {
    return (val >> ror) | (val << (32 - ror));
}

// Whether cpu is an ARM920T: fixed by an interpreter copy's kind, looked up for ARM_KIND_ANY, which the
// cold paths outside the copies use.
#define ARM_KIND_ANY -1
template <int kind>
static FORCE_INLINE bool cpuPrvArm920(const struct ArmCpu *cpu) {
    if constexpr (kind == ARM_KIND_ANY)
        return cpu->model == ArmCpuModel::arm920t;
    else
        return armKindIsArm920(kind);
}

template <int kind = ARM_KIND_ANY>
static FORCE_INLINE void cpuPrvSetPC(struct ArmCpu *cpu, uint32_t pc)  // with interworking
{
    if (cpuPrvArm920<kind>(cpu)) icacheArm920BranchInline(cpu->ic);
    const bool wasT = cpu->T;

    cpu->regs[REG_NO_PC] = pc & ~1UL;
    cpu->T = (pc & 1);

    if (cpu->T != wasT) cpuSetSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE);
}

template <bool wasT>
static FORCE_INLINE uint32_t cpuPrvGetReg(struct ArmCpu *cpu, uint_fast8_t reg) {
    uint32_t ret;

    ret = cpu->regs[reg];
    if (reg == REG_NO_PC) ret += wasT ? 2 : 4;

    return ret;
}

template <bool wasT, bool pc>
static FORCE_INLINE uint32_t cpuPrvGetReg(struct ArmCpu *cpu, uint_fast8_t reg) {
    if constexpr (pc)
        return cpu->regs[REG_NO_PC] + (wasT ? 2 : 4);
    else
        return cpu->regs[reg];
}

static FORCE_INLINE void cpuPrvSetRegNotPC(struct ArmCpu *cpu, uint_fast8_t reg, uint32_t val) {
    cpu->regs[reg] = val;
}

template <int kind>
static FORCE_INLINE void cpuPrvSetRegK(struct ArmCpu *cpu, uint_fast8_t reg, uint32_t val) {
    if (reg == REG_NO_PC) {
        if (cpuPrvArm920<kind>(cpu)) val = (val & (cpu->T ? ~1u : ~3u)) | uint32_t(cpu->T);
        cpuPrvSetPC<kind>(cpu, val);
    } else
        cpuPrvSetRegNotPC(cpu, reg, val);
}

static FORCE_INLINE void cpuPrvSetReg(struct ArmCpu *cpu, uint_fast8_t reg, uint32_t val) {
    cpuPrvSetRegK<ARM_KIND_ANY>(cpu, reg, val);
}

template <bool pc, int kind = ARM_KIND_ANY>
static FORCE_INLINE void cpuPrvSetReg(struct ArmCpu *cpu, uint_fast8_t reg, uint32_t val) {
    if constexpr (pc) {
        if (cpuPrvArm920<kind>(cpu)) val = (val & (cpu->T ? ~1u : ~3u)) | uint32_t(cpu->T);
        cpuPrvSetPC<kind>(cpu, val);
    } else
        cpuPrvSetRegNotPC(cpu, reg, val);
}

static FORCE_INLINE struct ArmBankedRegs *cpuPrvModeToBankedRegsPtr(struct ArmCpu *cpu,
                                                                    uint_fast8_t mode) {
    switch (mode) {
        case ARM_SR_MODE_USR:
        case ARM_SR_MODE_SYS:
            return &cpu->bank_usr;

        case ARM_SR_MODE_FIQ:
            return &cpu->bank_fiq;

        case ARM_SR_MODE_IRQ:
            return &cpu->bank_irq;

        case ARM_SR_MODE_SVC:
            return &cpu->bank_svc;

        case ARM_SR_MODE_ABT:
            return &cpu->bank_abt;

        case ARM_SR_MODE_UND:
            return &cpu->bank_und;

        default:
            ERR("Invalid mode passed to cpuPrvModeToBankedRegsPtr()");
            return NULL;
    }
}

template <int kind = ARM_KIND_ANY>
static void cpuUpdateSlowPath(struct ArmCpu *cpu) {
    // The ARM920T stops for an interrupt it can take; the other cores for any event while F
    // is clear, as they always have, so their timing stays.
    if (cpuPrvArm920<kind>(cpu) ? (cpu->waitingFiqs && !cpu->F) || (cpu->waitingIrqs && !cpu->I)
                                           : cpu->waitingEventsTotal && !cpu->F) {
        cpuSetSlowPath(cpu, SLOW_PATH_REASON_EVENTS);
    } else {
        cpuClearSlowPath(cpu, SLOW_PATH_REASON_EVENTS);
    }

    if (cpu->sleeping) {
        cpuSetSlowPath(cpu, SLOW_PATH_REASON_SLEEP);
    } else {
        cpuClearSlowPath(cpu, SLOW_PATH_REASON_SLEEP);
    }
}

static FORCE_INLINE void cpuPrvSwitchToMode(struct ArmCpu *cpu, uint_fast8_t newMode) {
    struct ArmBankedRegs *saveTo, *getFrom;
    uint_fast8_t i, curMode;
    uint32_t tmp;

    curMode = cpu->M;
    if (curMode == newMode) return;

    if (curMode == ARM_SR_MODE_FIQ || newMode == ARM_SR_MODE_FIQ) {  // bank/unbank the fiq regs

        for (i = 0; i < 5; i++) {
            tmp = cpu->extra_regs[i];
            cpu->extra_regs[i] = cpu->regs[i + 8];
            cpu->regs[i + 8] = tmp;
        }
    }

    saveTo = cpuPrvModeToBankedRegsPtr(cpu, curMode);
    getFrom = cpuPrvModeToBankedRegsPtr(cpu, newMode);

    if (saveTo != getFrom) {
        saveTo->R13 = cpu->regs[REG_NO_SP];
        saveTo->R14 = cpu->regs[REG_NO_LR];
        saveTo->SPSR = cpu->SPSR;

        cpu->regs[REG_NO_SP] = getFrom->R13;
        cpu->regs[REG_NO_LR] = getFrom->R14;
        cpu->SPSR = getFrom->SPSR;
    }
    // User and system share a bank, but still change mode and privilege.

    cpu->M = newMode;
    const bool privileged = cpu->M != ARM_SR_MODE_USR;
    // Fetches are checked for the mode: the I-cache fast paths check their line again.
    if (privileged != cpu->privileged && cpu->ic) cpu->ic->epoch++;
    cpu->privileged = privileged;
}

template <int kind = ARM_KIND_ANY>
static FORCE_INLINE void cpuPrvSetPSRlo8(struct ArmCpu *cpu, uint_fast8_t val) {
    const bool wasT = cpu->T;

    cpuPrvSwitchToMode(cpu, val & ARM_SR_M);
    cpu->T = !!(val & ARM_SR_T);
    cpu->F = !!(val & ARM_SR_F);
    cpu->I = !!(val & ARM_SR_I);

    cpuUpdateSlowPath<kind>(cpu);
    if (cpu->T != wasT) cpuSetSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE);
}

static FORCE_INLINE void cpuPrvSetPSRhi8(struct ArmCpu *cpu, uint32_t val) {
    cpu->flags = val & 0xf0000000UL;
    cpu->Q = !!(val & ARM_SR_Q);
}

static FORCE_INLINE uint32_t cpuPrvMaterializeCPSR(struct ArmCpu *cpu) {
    uint32_t ret = cpu->flags;

    if (cpu->Q) ret |= ARM_SR_Q;
    if (cpu->T) ret |= ARM_SR_T;
    if (cpu->I) ret |= ARM_SR_I;
    if (cpu->F) ret |= ARM_SR_F;
    ret |= cpu->M;

    return ret;
}

uint32_t cpuGetRegExternal(struct ArmCpu *cpu, uint_fast8_t reg) {
    if (reg < 16)  // real reg
        return (reg == REG_NO_PC) ? (cpu->curInstrPC + (cpu->T ? 1 : 0)) : cpu->regs[reg];
    else if (reg == ARM_REG_NUM_CPSR)
        return cpuPrvMaterializeCPSR(cpu);
    else if (reg == ARM_REG_NUM_SPSR)
        return cpu->SPSR;
    else
        return 0;
}

void cpuSetReg(struct ArmCpu *cpu, uint_fast8_t reg, uint32_t val) {
    if (reg == ARM_REG_NUM_CPSR) {
        cpuPrvSetPSRlo8(cpu, val);
        cpuPrvSetPSRhi8(cpu, val);
    } else if (reg < 16)
        cpuPrvSetReg(cpu, reg, val);
}

#define cpuSetReg _DO_NOT_USE_cpuSetReg_IN_CPU_C_

template <int kind = ARM_KIND_ANY>
static void cpuPrvException(struct ArmCpu *cpu, uint32_t vector_pc, uint32_t lr,
                            uint_fast8_t newLowBits)  // enters arm mode
{
    ++cpu->exceptionSequence;
    if (cpuPrvArm920<kind>(cpu)) icacheArm920BranchInline(cpu->ic);
#ifndef __EMSCRIPTEN__
    if (cpu->nativeCallDepth && ((newLowBits & 0x1f) == ARM_SR_MODE_ABT ||
                                  (newLowBits & 0x1f) == ARM_SR_MODE_UND))
        cpuSetSlowPath(cpu, SLOW_PATH_REASON_NATIVE_FAULT);
#endif
    if (cpu->modePace) {
#ifdef TRACE_PACE
        fprintf(stderr, "exception in PACE %#010x %#010x\n", lr,
                cpu->paceOffset + cpu->pacePatch->enterPace);
#endif

        if (!paceSave68kState()) {
            uint32_t addr;
            bool wasWrite;
            uint_fast8_t fsr;

            paceGetMemeryFault(&addr, &wasWrite, &fsr);

            fprintf(stderr,
                    "ignoreing memory fault in PACE during save68kState: %s, addr=%#010x, "
                    "fsr=%#04x\n",
                    wasWrite ? "write" : "read", addr, fsr);
        }

        cpu->modePace = false;
        cpuSetSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE);
    }

    uint32_t spsr = cpuPrvMaterializeCPSR(cpu);

    // ARM DDI0100I A2.6: only Reset and FIQ force F; other exceptions preserve it. Only on the
    // ARM920T, so the other cores' timing and results stay as they were.
    cpuPrvSetPSRlo8<kind>(cpu, cpuPrvArm920<kind>(cpu) ? newLowBits | (spsr & ARM_SR_F) : newLowBits);
    cpu->SPSR = spsr;
    cpu->regs[REG_NO_LR] = lr;
    cpu->regs[REG_NO_PC] = vector_pc;
}

// The ARM920T's FCSE: a VA below 32 MB is moved to VA | PID, the MVA its caches, TLB and FAR use.
static FORCE_INLINE uint32_t cpuPrvMva(const struct ArmCpu *cpu, uint32_t va) {
    return va < 0x02000000u ? va | cpu->pid : va;
}

// input addr is VA not MVA
template <int kind = ARM_KIND_ANY>
static void cpuPrvHandleMemErr(struct ArmCpu *cpu, uint32_t addr, bool write, bool instrFetch,
                               uint_fast8_t fsr) {
    // A lean access that stops the lean run takes no abort: the guarded step runs the instruction again.
    if (unlikely(cpu->leanExit)) return;
    if (cpuPrvArm920<kind>(cpu) &&
        cp15MMUArm920DataCache(cpu->cp15.cp15mmu)->guard.active) {
        // An unmodelled cache/buffer target is a diagnostic trap, with FSR/FAR retained.
        ++cpu->undefinedCount;
        cpu->undefinedPc = cpu->curInstrPC;
        cpu->undefinedInstr = 0;
        if (cpu->undefinedHook) cpu->undefinedHook(cpu->undefinedHookContext, cpu->curInstrPC, 0);
        cpuPrvException<kind>(cpu, cpu->vectorBase + ARM_VECTOR_OFFT_UND, cpu->curInstrPC + (cpu->T ? 2 : 4),
                        ARM_SR_MODE_UND | ARM_SR_I);
        return;
    }
    // FAR gets the MVA. Only the ARM920T path has the FCSE.
    if (cpuPrvArm920<kind>(cpu)) addr = cpuPrvMva(cpu, addr);

    if (cpu->memorySystemKind == ARM_MEMORY_SYSTEM_MMU &&
        !(instrFetch && cpuPrvArm920<kind>(cpu))) {
        cp15MMUSetFaultStatus(cpu->cp15.cp15mmu, addr, fsr);
    } else if (instrFetch && cpuPrvArm920<kind>(cpu)) {
        // The ARM920T keeps a prefetch abort's status in its own FSR and leaves FSR and FAR as they were.
        cp15MMUSetInstructionFaultStatus(cpu->cp15.cp15mmu, fsr);
    }

    if (instrFetch) {
        // handle prefetch abort (LR is addr of aborted instr + 4)
        cpuPrvException<kind>(cpu, cpu->vectorBase + ARM_VECTOR_OFFT_P_ABT, cpu->curInstrPC + 4,
                        ARM_SR_MODE_ABT | ARM_SR_I);
    } else {
        // handle data abort (LR is addr of aborted instr + 8)
        cpuPrvException<kind>(cpu, cpu->vectorBase + ARM_VECTOR_OFFT_D_ABT, cpu->curInstrPC + 8,
                        ARM_SR_MODE_ABT | ARM_SR_I);
    }
}

template <bool wasT>
static FORCE_INLINE uint32_t cpuPrvArmAdrMode_1(struct ArmCpu *cpu, uint32_t instr,
                                                bool *carryOutP) {
    uint_fast8_t v, a;
    bool co = cpu->flags & ARM_SR_C;  // be default carry out = C flag
    uint32_t ret;
    struct ImmShift *shift;

    if (instr & 0x02000000UL) {  // immed

        v = (instr >> 7) & 0x1E;
        ret = cpuPrvROR(instr & 0xFF, v);
        if (v) co = !!(ret & 0x80000000UL);
    } else if ((instr & 0x00000FF0UL) == 0) {  // Rm, LSL #0: Rm as it is, carry unchanged
        ret = cpuPrvGetReg<wasT>(cpu, instr & 0x0F);
    } else {
        v = (instr >> 5) & 3;                         // get shift type
        ret = cpuPrvGetReg<wasT>(cpu, instr & 0x0F);  // get Rm

        if (instr & 0x00000010UL) {
            a = cpuPrvGetRegNotPC(cpu, (instr >> 8) & 0x0F);
            shift = table_immShiftReg + ((v << 8) | (a & 0xff));

        } else {
            a = (instr >> 7) & 0x1F;
            shift = table_immShiftImm + ((v << 5) | a);
        }

        switch (shift->type) {  // perform shifts
            case shiftTypeNoop:
                break;

            case shiftTypeLSL:  // LSL
                co = ret & shift->coBit;
                ret <<= shift->shift;
                break;

            case shiftTypeLSR:  // LSR
                co = ret & shift->coBit;
                ret >>= shift->shift;
                break;

            case shiftTypeASR:  // ASR
                co = ret & shift->coBit;
                ret = (int32_t)ret >> shift->shift;
                break;

            case shiftTypeROR:  // ROR
                co = ret & shift->coBit;
                ret = cpuPrvROR(ret, shift->shift);
                break;

            case shiftTypeZero:
                co = ret & shift->coBit;
                ret = 0;
                break;

            case shiftTypeRRX:
                a = co;
                co = ret & 1;
                ret = ret >> 1;
                if (a) ret |= 0x80000000UL;
                break;
        }
    }

    *carryOutP = co;
    return ret;
}

/*
idea:

addbefore is what to add to add to base reg before addressing, addafter is what to add after. we
ALWAYS do writeback, but if not requested by instr, it will be zero

for [Rx, 5]   baseReg = x addbefore = 5 addafter = -5
for [Rx, 5]!  baseReg = x addBefore = 0 addafter = 0
for [Rx], 5   baseReg = x addBefore = 0 addAfter = 5

t = T bit (LDR vs LDRT)

baseReg is returned in return val along with flags:


ARM_MODE_2_REG	is mask for reg
ARM_MODE_2_WORD	is flag for word access
ARM_MODE_2_LOAD	is flag for load
ARM_MODE_2_INV	is flag for invalid instructions
ARM_MODE_2_T	is flag for T


*/
static uint_fast8_t cpuPrvArmAdrModeDecode_2(uint32_t instr) {
    uint_fast8_t mode = 0;

    if (!(instr & 0x00400000UL)) mode |= ARM_MODE_2_WORD;
    if (instr & 0x00100000UL) mode |= ARM_MODE_2_LOAD;
    if (!(instr & 0x01000000UL)) {
        if (instr & 0x00200000UL) mode |= ARM_MODE_2_T;
    }

    return mode;
}

/*
same comments as for addr mode 2 apply

#define ARM_MODE_3_REG	0x0F	//flag for actual reg number used
#define ARM_MODE_3_TYPE	0x30	//flag for the below 4 types
#define ARM_MODE_3_H	0x00
#define ARM_MODE_3_SH	0x10
#define ARM_MODE_3_SB	0x20
#define ARM_MODE_3_D	0x30
#define ARM_MODE_3_LOAD	0x40
#define ARM_MODE_3_INV	0x80
*/
static uint_fast8_t cpuPrvArmAdrModeDecode_3(uint32_t instr) {
    uint_fast8_t reg = 0;

    reg = (instr >> 16) & 0x0F;

    if (!(instr & 0x00400000UL) && (instr & 0x00000F00UL))
        reg |= ARM_MODE_3_INV;  // bits 8-11 must be 1 always

    switch (((instr >> 5) & 0x3) | ((instr >> 18) & 0x04)) {
        case 0x7:  // S && H && L
            reg |= ARM_MODE_3_SH;
            reg |= ARM_MODE_3_LOAD;
            break;

        case 0x3:  // S && H && !L
            reg |= ARM_MODE_3_D;
            break;

        case 0x6:  // S && !H && L
            reg |= ARM_MODE_3_SB;
            reg |= ARM_MODE_3_LOAD;
            break;

        case 0x2:  // S && !H && !L
            reg |= ARM_MODE_3_D;
            reg |= ARM_MODE_3_LOAD;
            break;

        case 0x5:  // !S && H && L
            reg |= ARM_MODE_3_LOAD;
            break;

        case 0x1:  // !S && H && !L
            break;

        case 0x0:  // !S && !H && !L
            reg |= ARM_MODE_3_INV;
            break;
    }

#ifdef STRICT_CPU
    if ((instr & 0x00000090UL) != 0x00000090UL)
        reg |= ARM_MODE_3_INV;  // bits 4 and 7 must be 1 always

    if (!(instr & 0x01000000UL) && (instr & 0x00200000UL)) {
        if (instr & 0x00200000UL)
            reg |= ARM_MODE_3_INV;  // W must be 0 in this case, else unpredictable (in this case -
                                    // invalid instr)
    }
#endif
    return reg;
}

/*
        #define ARM_MODE_4_REG	0x0F
        #define ARM_MODE_4_INC	0x10	//incr or decr
        #define ARM_MODE_4_BFR	0x20	//after or before
        #define ARM_MODE_4_WBK	0x40	//writeback?
        #define ARM_MODE_4_S	0x80	//S bit set?
*/

static uint_fast8_t cpuPrvArmAdrModeDecode_4(uint32_t instr) {
    uint_fast8_t mode = 0;

    if (instr & 0x00200000UL) mode |= ARM_MODE_4_WBK;
    if (instr & 0x00800000UL) mode |= ARM_MODE_4_INC;
    if (instr & 0x01000000UL) mode |= ARM_MODE_4_BFR;

    return mode;
}

/*
#define ARM_MODE_5_REG		0x0F
#define ARM_MODE_5_IS_OPTION	0x10	//is value option (as opposed to offset)
#define ARM_MODE_5_RR		0x20	//MCRR or MRCC instrs
*/
static FORCE_INLINE uint_fast8_t cpuPrvArmAdrMode_5(struct ArmCpu *cpu, uint32_t instr,
                                                    uint32_t *addBeforeP, uint32_t *addAfterP,
                                                    uint8_t *optionValP) {
    uint_fast8_t reg;
    uint32_t val;

    val = instr & 0xFF;
    reg = (instr >> 16) & 0x0F;

    *addBeforeP = 0;
    *addAfterP = 0;
    *optionValP = 0;

    if (!(instr & 0x01000000UL)) {  // unindexed or postindexed

        if (instr & 0x00200000UL)  // postindexed
            *addAfterP = val;
        else {  // unindexed
            if (!(instr & 0x00800000UL))
                reg |= ARM_MODE_5_RR;  // U must be 1 for unindexed, else it is MCRR/MRCC
            *optionValP = val;
        }
    } else {  // offset or preindexed

        *addBeforeP = val;

        if (instr & 0x00200000UL)  // preindexed
            *addAfterP = val;
    }

    if (!(reg & ARM_MODE_5_IS_OPTION)) {
        if (!(instr & 0x00800000UL)) {
            *addBeforeP = -*addBeforeP;
            *addAfterP = -*addAfterP;
        }
    }
    return reg;
}

template <bool spsr, int kind = ARM_KIND_ANY>
static FORCE_INLINE void cpuPrvSetPSR(struct ArmCpu *cpu, uint_fast8_t mask, uint32_t val) {
    if constexpr (spsr)  // setting SPSR in sys or usr mode is no harm since they arent used, so
                         // just do it without any checks
        cpu->SPSR = val;
    else {
        if (cpu->privileged && (mask & 1)) cpuPrvSetPSRlo8<kind>(cpu, val);

        if (mask & 8) cpuPrvSetPSRhi8(cpu, val);
    }
}

static FORCE_INLINE bool cpuPrvSignedAdditionOverflows(int32_t a, int32_t b, int32_t sum) {
    int32_t c;

    return __builtin_add_overflow_i32(a, b, &c);
}

static FORCE_INLINE bool cpuPrvSignedSubtractionOverflows(int32_t a, int32_t b,
                                                          int32_t diff)  // diff = a - b
{
    int32_t c;

    return __builtin_sub_overflow_i32(a, b, &c);
}

static FORCE_INLINE int32_t cpuPrvMedia_signedSaturate32(int32_t sign) {
    return (sign < 0) ? -0x80000000L : 0x7fffffffl;
}

#ifndef __EMSCRIPTEN__
// The read half of ramAccessInline, for RAM and ROM (which reads the same way).
template <int size>
static FORCE_INLINE void cpuPrvHostRead(const uint8_t *addr, void *bufP) {
    switch (size) {
        case 1:
            *(uint8_t *)bufP = *addr;
            break;
        case 2:
            *(uint16_t *)bufP = le16toh(*((const uint16_t *)addr));
            break;
        case 4:
            *(uint32_t *)bufP = le32toh(*((const uint32_t *)addr));
            break;
        case 8:
            ((uint64_t *)bufP)[0] = le64toh(*((const uint64_t *)addr));
            break;
    }
}

// After an access that went through the MMU to primary RAM or ROM, cache the page's
// host address. Only for pages the MMU TLB holds now, so a hit gives what
// mmuTranslateCached would. Writes are RAM only. Both are keyed by privilege, since the
// permission check passed for that privilege and the current domains, S and R.
template <bool write>
static __attribute__((noinline)) void cpuPrvHostTlbFill(struct ArmCpu *cpu, uint32_t va, uint32_t pa,
                                            bool privileged, uint8_t info) {
    const struct ArmMmu *mmu = cpu->memorySystem.mmu;
    if (mmu->transTablPA != MMU_DISABLED_TTP) {
        if (mmu->tlb[va >> 12].revision != mmu->revision) return;
    } else if (mmu->staleOn) {
        return;  // the stale D-cache window: every access goes through mmuStaleTranslate
    }
    struct ArmMem *mem = cpu->mem;
    struct HostTlb &hostTlb = mem->hostTlb;
    const uint32_t vpage = va & ~0xfffu, ppage = pa & ~0xfffu;
    const auto holds = [&](const struct ArmMemRegion &r) {
        return r.ub && r.pa <= ppage && uint64_t(ppage) + 0x1000 <= r.ub;
    };
    const struct ArmMemRegion &ram = mem->regions[REGION_RAM];
    const struct ArmMemRegion &rom = mem->regions[REGION_ROM];
    if constexpr (write) {
        if (!holds(ram)) return;
        hostTlb.ram = (struct ArmRam *)ram.uD;
        hostTlb.write[(va >> 12) & (HOST_TLB_SIZE - 1)] = {
            hostTlbKey(hostTlb.gen, va, privileged), uintptr_t(uint32_t(ppage - vpage))};
        cpu->hostTlbInfo[1][(va >> 12) & (HOST_TLB_SIZE - 1)] = info;
    } else {
        uint8_t *host;
        if (holds(ram)) {
            const struct ArmRam *r = (const struct ArmRam *)ram.uD;
            host = (uint8_t *)r->buf.buffer + (ppage - r->adr);
        } else if (holds(rom)) {
            host = (uint8_t *)romResolveAddress((struct ArmRom *)rom.uD, ppage, 0x1000);
            if (!host) return;
        } else {
            return;
        }
        hostTlb.read[(va >> 12) & (HOST_TLB_SIZE - 1)] = {hostTlbKey(hostTlb.gen, va, privileged),
                                                          uintptr_t(host) - vpage};
        cpu->hostTlbInfo[0][(va >> 12) & (HOST_TLB_SIZE - 1)] = info;
    }
}

#ifdef FROND_DYNARMIC
// The Fast engine's fill after an access through the MMU: RAM and ROM pages go to the host
// TLB as above, and a page of a device region to the device page table, under the same
// rules (the MMU TLB holds the page now, keyed by gen and privilege), with the region the
// access went to. Regions don't overlap, so an address inside it reaches that handler.
template <bool write>
static __attribute__((noinline)) void cpuPrvJitFill(struct ArmCpu *cpu, uint32_t va, uint32_t pa,
                                                   bool privileged, uint8_t info) {
    const struct ArmMmu *mmu = cpu->memorySystem.mmu;
    if (mmu->transTablPA != MMU_DISABLED_TTP) {
        if (mmu->tlb[va >> 12].revision != mmu->revision) return;
    } else if (mmu->staleOn) {
        return;
    }
    struct ArmMem *mem = cpu->mem;
    const uint32_t ppage = pa & ~0xfffu;
    const auto meets = [&](const struct ArmMemRegion &r) {
        return r.ub && r.pa < uint64_t(ppage) + 0x1000 && ppage < r.ub;
    };
    if (meets(mem->regions[REGION_RAM]) || meets(mem->regions[REGION_ROM])) {
        cpuPrvHostTlbFill<write>(cpu, va, pa, privileged, info);
        return;
    }
    for (unsigned i = REGION_BASE; i < NUM_MEM_REGIONS; i++) {
        const struct ArmMemRegion &r = mem->regions[i];
        if (!r.ub || r.pa > pa || r.ub <= pa) continue;
        struct HostTlb &t = mem->hostTlb;
        struct HostTlbIoEntry &e = (write ? t.ioWrite : t.ioRead)[hostTlbIoIndex(va)];
        e.key = hostTlbKey(t.gen, va, privileged);
        e.delta = pa - va;
        e.pa = r.pa;
        e.ub = r.ub;
        e.aF = r.aF;
        e.uD = r.uD;
        return;
    }
}
#endif
#endif

#ifndef __EMSCRIPTEN__
// ARM920T Fast data accesses, only inside cpuArm920RunLean. The host TLB arrays hold host page addresses
// (host - va) for reads and writes alike, filled only for pages the lean page callback calls plain and the
// MMU TLB holds now, keyed by privilege; mem->hostTlb.gen moves on every TLB, domain, S, R, TTB and
// control change. Anything else sets leanExit, so the guarded step runs the instruction again.
// The lean loop's current instruction has a single data access, a store: ARM STR, STRB, STRH, Thumb STR,
// STRB, STRH in all their forms. In Thumb, leanInstr holds the table_thumb2arm translation where there is one
// (always above 0xffff, as its condition is AL) and the Thumb halfword where there isn't.
static bool cpuPrvLeanSingleStore(const struct ArmCpu *cpu) {
    const uint32_t i = cpu->leanInstr;
    if (cpu->T && i <= 0xffff) {
        const uint32_t top = i & 0xf800;
        return top == 0x6000 || top == 0x7000 || top == 0x8000 || top == 0x9000 ||
               (i & 0xfe00) == 0x5000 || (i & 0xfe00) == 0x5200 || (i & 0xfe00) == 0x5400;
    }
    return (i & 0x0c100000) == 0x04000000 || (i & 0x0e1000f0) == 0x000000b0;
}

// The lean loop's current instruction has a single data access, a load: ARM LDR, LDRB, LDRH, LDRSB, LDRSH,
// Thumb LDR, LDRB, LDRH, LDRSB, LDRSH in all their forms, translated or not.
static bool cpuPrvLeanSingleLoad(const struct ArmCpu *cpu) {
    const uint32_t i = cpu->leanInstr;
    if (cpu->T && i <= 0xffff) {
        const uint32_t top = i & 0xf800;
        const uint32_t reg = i & 0xfe00;
        return top == 0x6800 || top == 0x4800 || top == 0x9800 || top == 0x7800 || top == 0x8800 ||
               reg == 0x5800 || reg == 0x5600 || reg == 0x5a00 || reg == 0x5c00 || reg == 0x5e00;
    }
    return (i & 0x0c100000) == 0x04100000 || ((i & 0x0e100090) == 0x00100090 && (i & 0x60));
}

// A store to [pa, pa + size), inside one page. Translated code is dropped by block: those whose code the
// store overlaps, in the virtual pages translated from pa. Decoded code isn't tied to the physical pages
// it came from, so a store to any of its lines drops it all. A store to other lines of those pages drops
// nothing.
static void cpuPrvArm920Stored(ArmCpu *cpu, uint32_t pa, uint32_t size) {
#ifdef FROND_DYNARMIC
    // The word marks stay until jitRun works them out again from the blocks left.
    if (cpu->jitCode && unlikely(cpu->jitCode->page(pa)) && cpu->jitCode->hitWords(pa, size))
        jitCodeWrittenPa(cpu->jit, pa, size);
#endif
    if (!cpu->leanCode || likely(!cpu->leanCode->page(pa)) || !cpu->leanCode->hit(pa, size)) return;
    cpu->leanCode->clear();
    icacheArm920DropDecoded(cpu->ic);
    ++cpu->leanCodeDrops;
}

// Whether a store to the page at pa has to come through cpuPrvArm920Stored (it holds code).
static bool cpuPrvArm920CodePage(const ArmCpu *cpu, uint32_t pa) {
#ifdef FROND_DYNARMIC
    if (cpu->jitCode && cpu->jitCode->page(pa)) return true;
#endif
    return cpu->leanCode && cpu->leanCode->page(pa);
}

#ifdef FROND_DYNARMIC
// The cycles of the blocks this JIT run has finished that the board hasn't charged yet.
static uint32_t cpuPrvJitRanUncharged(const ArmCpu *cpu) {
    const uint32_t ran = jitRanNow(cpu->jit);
    return ran > cpu->jitCharged ? ran - cpu->jitCharged : 0;
}
#endif

template <int size, bool write>
static __attribute__((noinline)) bool cpuPrvLeanMemOpSlow(struct ArmCpu *cpu, void *buf, uint32_t va,
                                                         bool privileged) {
    if (va & (size - 1)) {
        cpu->leanExit = true;
        return false;
    }
    struct ArmMmu *mmu = cpu->memorySystem.mmu;
    // The FCSE: the MMU sees the MVA. The host TLB stays keyed by VA, as cpuSetPid flushes it on a PID change.
    const uint32_t mva = cpuPrvMva(cpu, va);
    const MMUTranslateResult r = mmuTranslateCached<write>(mmu, mva, privileged);
    bool counted = false;
    uint8_t *page = MMU_TRANSLATE_RESULT_OK(r) && cpu->leanPage
                        ? cpu->leanPage(cpu->leanContext, MMU_TRANSLATE_RESULT_PA(r) & ~0xfffu, write, &counted)
                        : nullptr;
    if (!page) {
        // Only where the step's D-cache model would bypass: a cacheable or bufferable access stops there.
        const bool cached = ((cp15MMUArm920Control(cpu->cp15.cp15mmu) & 4) && MMU_TRANSLATE_RESULT_CACHEABLE(r)) ||
                            MMU_TRANSLATE_RESULT_BUFFERABLE(r);
        if constexpr (!write) {
            uint32_t value, cost = 0;
            // Fast blocks answer reads the same way, with ran the cycles of the blocks the run has finished (its
            // time is charged at its end). An access there that a later one in the same instruction leaves to the
            // step is taken again by the step, which changes nothing for the reads and writes these callbacks answer.
            uint32_t ran = cpu->leanDone, charge = cpu->leanCharge;
#ifdef FROND_DYNARMIC
            if (cpu->leanJit) ran = cpuPrvJitRanUncharged(cpu), charge = 1;
#endif
            const uint32_t left = ran + charge < cpu->leanBudget ? cpu->leanBudget - ran - charge : 0;
            if (size <= 4 && MMU_TRANSLATE_RESULT_OK(r) && !cached && cpu->leanRead &&
                cpu->leanRead(cpu->leanContext, MMU_TRANSLATE_RESULT_PA(r), size,
                              cpu->leanJit ? std::exchange(cpu->jitAccessPc, 0) : cpu->curInstrPC, ran, left,
                              cpu->leanJit || cpuPrvLeanSingleLoad(cpu), &value, &cost)) {
                memcpy(buf, &value, size);
                cpu->leanBudget -= cost;
                ++cpu->leanBus;
                return true;
            }
        } else if (size <= 4 && MMU_TRANSLATE_RESULT_OK(r) && !cached && cpu->leanWrite &&
                   (cpu->leanJit || cpuPrvLeanSingleStore(cpu))) {
            uint32_t value = 0, cost = 0;
            memcpy(&value, buf, size);
            // ran as for reads: in Fast blocks the cycles of the blocks the run has finished.
            uint32_t ran = cpu->leanDone, charge = cpu->leanCharge;
#ifdef FROND_DYNARMIC
            if (cpu->leanJit) ran = cpuPrvJitRanUncharged(cpu), charge = 1;
#endif
            const uint32_t left = ran + charge < cpu->leanBudget ? cpu->leanBudget - ran - charge : 0;
            if (cpu->leanWrite(cpu->leanContext, MMU_TRANSLATE_RESULT_PA(r), size, value, ran, left, &cost)) {
                cpu->leanBudget -= cost;
                ++cpu->leanBus;
#ifdef FROND_DYNARMIC
                // The next stores to this page from generated code skip the translation (cpuJitLeanDeviceWrite),
                // while the MMU TLB entry it came from holds.
                if (cpu->leanJit &&
                    (mmu->transTablPA == MMU_DISABLED_TTP || mmu->tlb[mva >> 12].revision == mmu->revision)) {
                    auto &e = cpu->leanDeviceWrite[(va >> 12) & 15];
                    e.key = hostTlbKey(cpu->mem->hostTlb.gen, va, privileged);
                    e.pa = (MMU_TRANSLATE_RESULT_PA(r) & ~0xfffu) | 1;
                }
#endif
                return true;
            }
        }
        cpu->leanExit = true;
        return false;
    }
    // A page holding code keeps no write entry, so each store to it is checked against the code's lines.
    bool codePage = false;
    if constexpr (write) {
        cpuPrvArm920Stored(cpu, MMU_TRANSLATE_RESULT_PA(r), size);
        codePage = cpuPrvArm920CodePage(cpu, MMU_TRANSLATE_RESULT_PA(r));
    }
    uint8_t *host = page + (MMU_TRANSLATE_RESULT_PA(r) & 0xfffu);
    if constexpr (write) memcpy(host, buf, size); else memcpy(buf, host, size);
    cpu->leanBus += counted;
    // Bit 0 of the addend marks a counted page; host pages are at least 2-byte aligned.
    if ((mmu->transTablPA == MMU_DISABLED_TTP || mmu->tlb[mva >> 12].revision == mmu->revision) &&
        !(uintptr_t(page) & 1) && !codePage) {
        struct HostTlb &t = cpu->mem->hostTlb;
        struct HostTlbEntry &e = (write ? t.write : t.read)[(va >> 12) & (HOST_TLB_SIZE - 1)];
        e.key = hostTlbKey(t.gen, va, privileged);
        e.addend = (uintptr_t(page) - (va & ~0xfffu)) | uintptr_t(counted);
    }
    return true;
}

template <int size, bool write>
static FORCE_INLINE bool cpuPrvLeanMemOp(struct ArmCpu *cpu, void *buf, uint32_t va, bool privileged) {
    const struct HostTlb &t = cpu->mem->hostTlb;
    const struct HostTlbEntry &e = (write ? t.write : t.read)[(va >> 12) & (HOST_TLB_SIZE - 1)];
    if (likely(!(va & (size - 1)) && e.key == hostTlbKey(t.gen, va, privileged))) {
        uint8_t *host = (uint8_t *)((e.addend & ~uintptr_t(1)) + va);
        if constexpr (write) memcpy(host, buf, size); else memcpy(buf, host, size);
        cpu->leanBus += e.addend & 1;
        return true;
    }
    return cpuPrvLeanMemOpSlow<size, write>(cpu, buf, va, privileged);
}
#endif

// arm920 is the interpreter kind's model, from cpuPrvMemOpEx. jit: the Fast engine's callbacks,
// which also note device pages (cpuPrvJitFill).
template <int memorySystemKind, int size, bool write, bool arm920, bool jit = false>
static FORCE_INLINE bool cpuPrvMemOpExModel(struct ArmCpu *cpu, void *buf, uint32_t vaddr,
                                            bool priviledged, uint_fast8_t *fsrP) {
#ifndef __EMSCRIPTEN__
    if (unlikely(cpu->nativeCallDepth && cpu->nativeScratchData &&
                 vaddr >= cpu->nativeScratchBase &&
                 uint64_t(vaddr) + size <= uint64_t(cpu->nativeScratchBase) + cpu->nativeScratchSize)) {
        auto* scratch = cpu->nativeScratchData + vaddr - cpu->nativeScratchBase;
        if constexpr (write) memcpy(scratch, buf, size); else memcpy(buf, scratch, size);
        return true;
    }
    if constexpr (arm920 && armMemorySystemOf(memorySystemKind) == ARM_MEMORY_SYSTEM_MMU && size <= 4) {
        if (unlikely(cpu->leanRun)) return cpuPrvLeanMemOp<size, write>(cpu, buf, vaddr, priviledged);
    }
#endif

    if constexpr (size > 1) {
        if (vaddr & (size - 1)) {
            if constexpr (armMemorySystemOf(memorySystemKind) == ARM_MEMORY_SYSTEM_MMU && size == 4) {
                if (!cp15MMUAlignmentEnabled(cpu->cp15.cp15mmu)) {
                    vaddr &= ~3u;
                } else {
                    if (fsrP) *fsrP = 1;
                    return false;
                }
            } else {
                if (fsrP) *fsrP = 1;  // alignment fault;
                return false;
            }
        }
    }

    // The ARM920T's FCSE: the TLB and the D-cache model below are keyed by MVA.
    if constexpr (arm920) vaddr = cpuPrvMva(cpu, vaddr);

#ifndef __EMSCRIPTEN__
    if constexpr (armMemorySystemOf(memorySystemKind) == ARM_MEMORY_SYSTEM_MMU && size <= 8) {
      if constexpr (!arm920) {
        struct HostTlb &hostTlb = cpu->mem->hostTlb;
        if constexpr (write) {
            const struct HostTlbEntry &e = hostTlb.write[(vaddr >> 12) & (HOST_TLB_SIZE - 1)];
            if (likely(e.key == hostTlbKey(hostTlb.gen, vaddr, priviledged))) {
                if (cpu->costMemory) cpuPrvCostHostHit<size, true>(cpu, vaddr);
                return ramAccessInline<size, true>(hostTlb.ram, vaddr + uint32_t(e.addend), buf);
            }
        } else {
            const struct HostTlbEntry &e = hostTlb.read[(vaddr >> 12) & (HOST_TLB_SIZE - 1)];
            if (likely(e.key == hostTlbKey(hostTlb.gen, vaddr, priviledged))) {
                if (cpu->costMemory) cpuPrvCostHostHit<size, false>(cpu, vaddr);
                cpuPrvHostRead<size>((const uint8_t *)(e.addend + vaddr), buf);
                return true;
            }
        }
      }
    }
#endif

    uint32_t pa;
    uint8_t costInfo = 0;
    if constexpr (armMemorySystemOf(memorySystemKind) == ARM_MEMORY_SYSTEM_MMU) {
        MMUTranslateResult translateResult =
#ifndef __EMSCRIPTEN__
            mmuTranslateCached<write, true>(cpu->memorySystem.mmu, vaddr, priviledged);
#else
            mmuTranslate(cpu->memorySystem.mmu, vaddr, priviledged, write);
#endif

        if (!MMU_TRANSLATE_RESULT_OK(translateResult)) {
            if (fsrP) *fsrP = MMU_TRANSLATE_RESULT_FSR(translateResult);
            return false;
        }

        pa = MMU_TRANSLATE_RESULT_PA(translateResult);
        costInfo = cpuPrvCostInfo(MMU_TRANSLATE_RESULT_CACHEABLE(translateResult),
                                  MMU_TRANSLATE_RESULT_BUFFERABLE(translateResult),
                                  MMU_TRANSLATE_RESULT_XSCALE_X(translateResult), cost::pxaRegion(pa));
#ifndef __EMSCRIPTEN__
        // Before the access, so a device read sees its own charge.
        if constexpr (!arm920) {
            if (cpu->costMemory) {
                cpuPrvCostAccess(cpu, vaddr, costInfo, write);
                if constexpr (size == 8) cpuPrvCostAccess(cpu, vaddr + 4, costInfo, write);
            }
        }
#endif
        if constexpr (arm920) {
            auto* dc = cp15MMUArm920DataCache(cpu->cp15.cp15mmu);
            dc->mem = cpu->mem;
            const auto ctl = cp15MMUArm920Control(cpu->cp15.cp15mmu);
            const auto result = dc->access(vaddr, translateResult, size, write, buf, ctl & 4, ctl & 0x4000);
            if (result == Arm920DataResult::ok) return true;
            if (result == Arm920DataResult::unsupported) { if (fsrP) *fsrP = 0; return false; }
        }
    } else {
        if (!MPU_TEST_RESULT_OK(mpuTestAddress(cpu->memorySystem.mpu, vaddr, priviledged, write))) {
            if (fsrP) *fsrP = 1;
            return false;
        }

        pa = vaddr;
    }

#ifndef __EMSCRIPTEN__
    bool ok = memAccessRamInline<size, write>(cpu->mem, pa, buf);
#else
    bool ok = memAccess<size, write>(cpu->mem, pa, buf);
#endif

    if (!ok) {
        if (fsrP) *fsrP = 10;  // external abort on non-linefetch

        return false;
    }

#ifndef __EMSCRIPTEN__
    if constexpr (armMemorySystemOf(memorySystemKind) == ARM_MEMORY_SYSTEM_MMU && size <= 8 && !arm920) {
#ifdef FROND_DYNARMIC
        if constexpr (jit) {
            cpuPrvJitFill<write>(cpu, vaddr, pa, priviledged, costInfo);
            return true;
        }
#endif
        cpuPrvHostTlbFill<write>(cpu, vaddr, pa, priviledged, costInfo);
    }
#endif

    return true;
}

// The ARM920T's lean run, data cache model and missing host TLB are only in its own kind's copy.
template <int memorySystemKind, int size, bool write, bool jit = false>
static FORCE_INLINE bool cpuPrvMemOpEx(struct ArmCpu *cpu, void *buf, uint32_t vaddr,
                                       bool priviledged, uint_fast8_t *fsrP) {
    return cpuPrvMemOpExModel<armMemorySystemOf(memorySystemKind), size, write, armKindIsArm920(memorySystemKind),
                              jit>(cpu, buf, vaddr, priviledged, fsrP);
}

template <int size, bool write>
static FORCE_INLINE bool cpuPrvMemOpExGeneric(struct ArmCpu *cpu, void *buf, uint32_t vaddr,
                                              bool priviledged, uint_fast8_t *fsrP) {
    if (cpu->model == ArmCpuModel::arm920t)
        return cpuPrvMemOpEx<ARM_MEMORY_SYSTEM_ARM920T, size, write>(cpu, buf, vaddr, priviledged, fsrP);
    if (cpu->memorySystemKind == ARM_MEMORY_SYSTEM_MMU) {
        return cpuPrvMemOpEx<ARM_MEMORY_SYSTEM_MMU, size, write>(cpu, buf, vaddr, priviledged,
                                                                 fsrP);
    } else {
        return cpuPrvMemOpEx<ARM_MEMORY_SYSTEM_MPU, size, write>(cpu, buf, vaddr, priviledged,
                                                                 fsrP);
    }
}

#ifndef __EMSCRIPTEN__
// Logs a failed data access, for every one or, with abortLogLimited, the first 16 and then every power of two.
static void cpuPrvLogAbort(struct ArmCpu *cpu, bool write, unsigned size, uint32_t vaddr, unsigned fsr) {
    if (cpu->abortLogLimited) {
        static std::atomic<uint64_t> logged{0};
        const uint64_t n = logged.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n > 16 && (n & (n - 1))) return;
    }
    fprintf(stderr, "%c of %u bytes to 0x%08lx failed (pc=0x%08x, fsr=%u)!\n", (int)(write ? 'W' : 'R'), size,
            (unsigned long)vaddr, cpu->curInstrPC, fsr);
}
#endif

// for internal use
template <int memorySystemKind, int size, bool write>
static FORCE_INLINE bool cpuPrvMemOp(struct ArmCpu *cpu, void *buf, uint32_t vaddr,
                                     bool priviledged, uint_fast8_t *fsrP) {
#ifdef __EMSCRIPTEN__
    return cpuPrvMemOpEx<memorySystemKind, size, write>(cpu, buf, vaddr, priviledged, fsrP);
#else
    if (cpuPrvMemOpEx<memorySystemKind, size, write>(cpu, buf, vaddr, priviledged, fsrP))
        return true;
    if (cpu->leanExit) return false;

    cpuPrvLogAbort(cpu, write, unsigned(size), vaddr, fsrP ? unsigned(*fsrP) : 0);

    gdbStubDebugBreakRequested(cpu->debugStub);

    return false;
#endif
}

// for internal use
template <int size, bool write>
static FORCE_INLINE bool cpuPrvMemOpGeneric(struct ArmCpu *cpu, void *buf, uint32_t vaddr,
                                            bool priviledged, uint_fast8_t *fsrP) {
#ifdef __EMSCRIPTEN__
    return cpuPrvMemOpExGeneric<size, write>(cpu, buf, vaddr, priviledged, fsrP);
#else
    if (cpuPrvMemOpExGeneric<size, write>(cpu, buf, vaddr, priviledged, fsrP)) return true;

    cpuPrvLogAbort(cpu, write, unsigned(size), vaddr, fsrP ? unsigned(*fsrP) : 0);

    gdbStubDebugBreakRequested(cpu->debugStub);

    return false;
#endif
}

bool cpuFetchInstruction(ArmCpu *cpu, uint32_t va, uint32_t* word, uint_fast8_t* fsr) {
    if (cpu->model != ArmCpuModel::arm920t) return false;
    return icacheArm920Fetch(cpu->ic, va, cpu->T ? 2 : 4, cpu->privileged, fsr, word);
}

void cpuInvalidateCodeCache(struct ArmCpu *cpu) { icacheInval(cpu->ic); }

void cpuDropTranslatedCode(struct ArmCpu *cpu) {
#ifdef FROND_DYNARMIC
    // Fast blocks turned off hold nothing that runs; turning them on again drops it all.
    if (cpu->jit && (cpu->jitBlocks || !cpu->jitLoopOnly)) jitClear(cpu->jit);
#else
    (void)cpu;
#endif
}

#ifdef FROND_DYNARMIC
static bool cpuPrvFastEngine;
bool cpuSetFastEngine(bool fast) {
    cpuPrvFastEngine = fast;
    return true;
}
void cpuSetJitSync(bool sync) { jitSetSync(sync); }
bool cpuJitExecutionCounts(const ArmCpu *cpu, uint64_t *blocks, uint64_t *instructions) {
    // The ARM920T Fast blocks JIT is the lean path's, outside the saved engine (stateFlagFast).
    if (!cpu || !cpu->jit || cpu->jitLoopOnly) return false;
    jitExecutionCounts(cpu->jit, blocks, instructions);
    return true;
}
bool cpuJitCompileCounts(const struct ArmCpu *cpu, uint64_t *stale, uint64_t *aheadCompiled, uint64_t *aheadUsed) {
    if (!cpu || !cpu->jit) return false;
    jitCompileCounts(cpu->jit, stale, aheadCompiled, aheadUsed);
    return true;
}
void cpuSetJitCodeCacheSize(size_t bytes) { jitSetCodeCacheSize(bytes); }
bool cpuJitCacheClears(const struct ArmCpu *cpu, uint64_t *clears) {
    if (!cpu || !cpu->jit) return false;
    *clears = jitCacheClears(cpu->jit);
    return true;
}
#else
bool cpuSetFastEngine(bool fast) { return !fast; }
void cpuSetJitSync(bool) {}
bool cpuJitExecutionCounts(const ArmCpu *, uint64_t *, uint64_t *) { return false; }
bool cpuJitCompileCounts(const struct ArmCpu *, uint64_t *, uint64_t *, uint64_t *) { return false; }
void cpuSetJitCodeCacheSize(size_t) {}
bool cpuJitCacheClears(const struct ArmCpu *, uint64_t *) { return false; }
#endif

// The most one interpreted instruction can charge, memory included.
static uint32_t cpuPrvMaxInstructionCharge(const struct ArmCpu *cpu) {
    if (!cpu || !cpu->ic->costOn) return 1;
    const cost::Core core = cost::Core(cpu->costCore);
    uint32_t most = cost::maxInstructionCost(core);
    if (cpu->costMemory) {
        // One access books at most a fill behind two dirty halves and a buffered store, or one uncached
        // access, or one region charge; the first can also wait for a full write buffer. An LDM or STM
        // makes at most 16 accesses. A peephole charges its whole routine, which this doesn't bound: the
        // memcpy peephole's charge grows with its size.
        uint32_t access = 0, drain = 0;
        for (const cost::CacheCosts &k : cpu->cacheCosts) {
            const uint32_t cached = k.fill + 2 * k.writeback + k.buffered;
            const uint32_t uncached = k.uncachedLoad > k.uncachedStore ? k.uncachedLoad : k.uncachedStore;
            access = std::max({access, cached, uncached});
            drain = std::max({drain, k.writeback, k.buffered});
        }
        for (const auto &r : cpu->regionCharge) access = std::max({access, r[0], r[1]});
        most += cost::writeBufferDepth(core) * drain + 16 * access;
    }
    return most;
}

// A batch stops once its count reaches the budget, so it passes it by at most the largest single charge
// - 1: an interpreted instruction or a PACE op.
uint32_t cpuMaxOvershootCycles(const struct ArmCpu *cpu) {
    const uint32_t instruction = cpuPrvMaxInstructionCharge(cpu);
    const uint32_t pace = cpu ? cpu->paceCost : 20;
    const uint32_t interpreter = std::max(instruction, pace) - 1;
#ifdef FROND_DYNARMIC
    if (cpu && cpu->jit) {
        // cpuPrvCycleJit: a run passes its budget by at most L x the largest cost - 1 (every link
        // checks, no block is longer than L instructions, a Thumb pair counting one and costing
        // less than the largest), then a run that stopped for the interpreter runs one more
        // instruction before the budget check. With memory charges (Fast at Original) a block's
        // accesses are charged as they happen and end the run at the next link once they pass what
        // was left, so the largest cost takes the memory charge too.
        const uint32_t l = jitMaxUnchecked(cpu->jit);
        if (!l) return UINT32_MAX;
        const uint32_t most = cpu->costMemory ? instruction
                              : cpu->ic->costOn ? cost::maxInstructionCost(cost::Core(cpu->costCore)) : 1;
        return std::max(l * most - 1 + instruction, interpreter);
    }
#endif
    return interpreter;
}

bool cpuJitCostCore(const struct ArmCpu *cpu, uint8_t *core) {
    *core = cpu->costCore;
    return cpu->ic->costOn;
}

void cpuSetCostClocks(struct ArmCpu *cpu, uint32_t coreKHz, uint32_t busKHz, uint32_t memoryKHz) {
    const cost::Clocks c = {coreKHz, busKHz, memoryKHz};
    const cost::Core core = cost::Core(cpu->costCore);
    cpu->costClocks = c;
    for (unsigned r = 0; r < cost::regionCount; r++)
        for (unsigned w = 0; w < 2; w++) cpu->regionCharge[r][w] = cost::regionCost(core, cost::Region(r), w, c);
    cpu->cacheCosts[0] = cost::cacheCosts(core, cost::sdram, c, cpu->costSdram16);
    cpu->cacheCosts[1] = cost::cacheCosts(core, cost::sram, c);
    cpu->cacheCosts[2] = cost::cacheCosts(core, cost::flash, c);
}

void cpuSetCostSdramWidth(struct ArmCpu *cpu, bool sdram16) {
    if (cpu->costSdram16 == sdram16) return;
    cpu->costSdram16 = sdram16;
    cpuSetCostClocks(cpu, cpu->costClocks.core, cpu->costClocks.bus, cpu->costClocks.memory);
}

// Exact always charges the data cache and region model. Fast charges it only at Original
// (cpuSetJitMemoryCost), from its memory callbacks and in the instructions it interprets alike;
// Uncapped Fast charges the table only.
static bool cpuPrvCostMemory(const struct ArmCpu *cpu) {
    bool fast = false;
#ifdef FROND_DYNARMIC
    fast = cpu->jit && !cpu->jitLoopOnly;
#endif
    return cpu->ic->costOn && cpu->costMemoryAsked && (!fast || cpu->costJitMemory) &&
           cpu->memorySystemKind == ARM_MEMORY_SYSTEM_MMU && cpu->model != ArmCpuModel::arm920t;
}

// Works out costMemory and costJitHint; the hint starts empty whenever it starts being kept.
static void cpuPrvSetCostMemory(struct ArmCpu *cpu) {
    cpu->costMemory = cpuPrvCostMemory(cpu);
    bool fast = false;
#ifdef FROND_DYNARMIC
    fast = cpu->jit && !cpu->jitLoopOnly;
#endif
    const bool hint = cpu->costMemory && fast;
    if (hint && !cpu->costJitHint) cpuPrvHintClear(cpu);
    cpu->costJitHint = hint;
}

void cpuSetJitMemoryCost(struct ArmCpu *cpu, bool on) {
    cpu->costJitMemory = on;
    const bool was = cpu->costMemory;
    cpuPrvSetCostMemory(cpu);
    if (cpu->costMemory && !was) {
        // The model wasn't kept while off: it starts empty.
        cost::dataCacheReset(cpu->dcache, cost::Core(cpu->costCore));
        std::memset(cpu->dcacheHint, 0, sizeof(cpu->dcacheHint));
        cpuCostHintClear(cpu);
    }
}

bool cpuJitMemoryCost(const struct ArmCpu *cpu) { return cpu->costMemory; }

uint32_t cpuJitPendingCharge(const struct ArmCpu *cpu) { return cpu->jitHint.pending; }

void cpuSetCostModel(struct ArmCpu *cpu, bool on, uint8_t core, bool memory) {
    cpu->costCore = core;
    cpu->costMemoryAsked = memory;
    cpu->paceCost = on ? cost::paceOpCost(cost::Core(core)) : 20;
    icacheSetCost(cpu->ic, on, core);
    cpuPrvSetCostMemory(cpu);
    cost::dataCacheReset(cpu->dcache, cost::Core(core));
    std::memset(cpu->dcacheHint, 0, sizeof(cpu->dcacheHint));
    cpuCostHintClear(cpu);
    cpu->costClock = 0;
    cpu->jitHint.pending = 0;
    cpuSetCostClocks(cpu, cpu->costClocks.core, cpu->costClocks.bus, cpu->costClocks.memory);
}

void cpuCostCacheOp(struct ArmCpu *cpu, uint32_t crm, uint32_t op2, uint32_t va) {
    if (!cpu->costMemory) return;
    const cost::Core core = cost::Core(cpu->costCore);
    cost::CacheEvents e;
    if ((crm == 7 || crm == 6) && op2 == 0) cost::dataCacheInvalidateAll(cpu->dcache);
    else if (crm == 6 && op2 == 1) e = cost::dataCacheLineOp(cpu->dcache, core, va, false, true);
    else if (crm == 10 && op2 == 1) e = cost::dataCacheLineOp(cpu->dcache, core, va, true, false);
    else if (crm == 14 && op2 == 1) e = cost::dataCacheLineOp(cpu->dcache, core, va, true, true);
    else if (crm == 2 && op2 == 5) e = cost::dataCacheAllocateLine(cpu->dcache, core, va);
    else if (crm == 10 && op2 == 4) cost::dataCacheDrain(cpu->dcache);
    else return;
    // The hint drops what the model lost: every line, the line, its dirty halves or the replaced line.
    // Join entries go with a drain (every slot emptied) and with the line an allocate takes.
    if (cpu->costJitHint) {
        if (op2 == 0) cpuPrvHintClear(cpu);
        else if (crm == 6 || crm == 14) cpuPrvHintDrop(cpu, va & ~31u, true);
        else if (crm == 10 && op2 == 1) cpuPrvHintDrop(cpu, va & ~31u, false);
        else if (crm == 10 && op2 == 4) cpuPrvJoinClear(cpu);
        else if (crm == 2 && op2 == 5) {
            if (e.evicted) cpuPrvHintDrop(cpu, cpuPrvEvictedLine(e.evicted), true);
            cpuPrvJoinDrop(cpu, va & ~31u);
            cpuPrvJoinDrop(cpu, (va & ~31u) + 16);
        }
    }
#ifndef __EMSCRIPTEN__
    // A clean's write-backs go on the bus and are charged to the MCR, inside a batch.
    if (e.writebacks && cpu->cyclesInProgress) {
        const uint64_t now = cpu->costClock + *cpu->cyclesInProgress + cpu->jitHint.pending;
        cpu->jitHint.pending += cpuPrvCacheStall(cpu, core, e, cpu->cacheCosts[0], now);
    }
#endif
}

template <typename T>
void cpuCostSave(const struct ArmCpu *cpu, T &savestate) {
    if (!cpu->costMemory) return;
    // Version 2 adds the write buffer slots.
    auto chunk = savestate.GetChunk(ChunkType::costDataCache, 2);
    if (!chunk) ERR("unable to allocate chunk");
    uint8_t bytes[cost::kDataCacheSaveBytes];
    cost::dataCacheSaveBytes(cpu->dcache, bytes);
    SaveChunkHelper helper(*chunk);
    helper.Do64(cpu->costClock).DoBuffer(bytes, sizeof(bytes));
}

// A session without the section starts with an empty cache: the first accesses miss.
template <typename T>
void cpuCostLoad(struct ArmCpu *cpu, T &loader) {
    cost::dataCacheReset(cpu->dcache, cost::Core(cpu->costCore));
    std::memset(cpu->dcacheHint, 0, sizeof(cpu->dcacheHint));
    cpuCostHintClear(cpu);  // refilled from the loaded model by the accesses that follow
    cpu->costClock = 0;
    cpu->jitHint.pending = 0;
    if (!cpu->costMemory || !loader.HasChunk(ChunkType::costDataCache)) return;
    uint32_t version = 0;
    auto chunk = loader.GetChunkOrFail(ChunkType::costDataCache, 2, "data cache cost model", version);
    if (!chunk) return;
    uint8_t bytes[cost::kDataCacheSaveBytes];
    uint64_t clock = 0;
    LoadChunkHelper helper(*chunk);
    helper.Do64(clock).DoBuffer(bytes, version >= 2 ? cost::kDataCacheSaveBytes : cost::kDataCacheSaveBytesV1);
    if (chunk->HasError()) {
        loader.NotifyError();
        return;
    }
    cost::dataCacheLoadBytes(cpu->dcache, bytes, version);
    cpu->costClock = clock;
}

void cpuWriteProtectHost(struct ArmCpu *cpu, const uint8_t *host, uint32_t size) {
#ifdef FROND_DYNARMIC
    if (cpu && cpu->jit) jitWriteProtect(cpu->jit, host, size);
#else
    (void)cpu;
    (void)host;
    (void)size;
#endif
}

// for external use
bool cpuMemOpExternal(struct ArmCpu *cpu, void *buf, uint32_t vaddr, uint_fast8_t size,
                      bool write)  // for external use
{
    if (write) {
        switch (size) {
            case 1:
                return cpuPrvMemOpExGeneric<1, true>(cpu, buf, vaddr, true, NULL);

            case 2:
                return cpuPrvMemOpExGeneric<2, true>(cpu, buf, vaddr, true, NULL);

            case 4:
                return cpuPrvMemOpExGeneric<4, true>(cpu, buf, vaddr, true, NULL);

            case 8:
                return cpuPrvMemOpExGeneric<8, true>(cpu, buf, vaddr, true, NULL);

            default:
                ERR("invalid size %i\n", size);
        }
    } else {
        switch (size) {
            case 1:
                return cpuPrvMemOpExGeneric<1, false>(cpu, buf, vaddr, true, NULL);

            case 2:
                return cpuPrvMemOpExGeneric<2, false>(cpu, buf, vaddr, true, NULL);

            case 4:
                return cpuPrvMemOpExGeneric<4, false>(cpu, buf, vaddr, true, NULL);

            case 8:
                return cpuPrvMemOpExGeneric<8, false>(cpu, buf, vaddr, true, NULL);

            default:
                ERR("invalid size %i\n", size);
        }
    }
}

// A coprocessor's data access for the guest, checked for the current mode. False with *fsr
// set on a fault, which cpuDataAbortExternal then logs and takes.
bool cpuMemOpGuest(struct ArmCpu *cpu, void *buf, uint32_t vaddr, uint_fast8_t size, bool write,
                   uint_fast8_t *fsr) {
    const bool p = cpu->privileged;
    switch (size) {
        case 1:
            return write ? cpuPrvMemOpExGeneric<1, true>(cpu, buf, vaddr, p, fsr)
                         : cpuPrvMemOpExGeneric<1, false>(cpu, buf, vaddr, p, fsr);
        case 2:
            return write ? cpuPrvMemOpExGeneric<2, true>(cpu, buf, vaddr, p, fsr)
                         : cpuPrvMemOpExGeneric<2, false>(cpu, buf, vaddr, p, fsr);
        case 4:
            return write ? cpuPrvMemOpExGeneric<4, true>(cpu, buf, vaddr, p, fsr)
                         : cpuPrvMemOpExGeneric<4, false>(cpu, buf, vaddr, p, fsr);
        default:
            ERR("invalid size %i\n", size);
    }
    return false;
}

// A failed data access of the instruction at curInstrPC: logged as cpuPrvMemOp logs the
// interpreter's own, then the data abort.
static void cpuPrvDataAbort(struct ArmCpu *cpu, uint32_t vaddr, unsigned size, bool write, uint_fast8_t fsr) {
#ifndef __EMSCRIPTEN__
    cpuPrvLogAbort(cpu, write, size, vaddr, unsigned(fsr));
    gdbStubDebugBreakRequested(cpu->debugStub);
#endif
    cpuPrvHandleMemErr(cpu, vaddr, write, false, fsr);
}

void cpuDataAbortExternal(struct ArmCpu *cpu, uint32_t vaddr, uint_fast8_t size, bool write, uint_fast8_t fsr) {
    cpuPrvDataAbort(cpu, vaddr, size, write, fsr);
}

static FORCE_INLINE uint64_t cpuPrv64FromHalves(uint64_t hi, uint32_t lo) {
    // better than shifting in almost all compilers
    union {
        struct {
#if __BYTE_ORDER == __LITTLE_ENDIAN
            uint32_t lo;
            uint32_t hi;
#elif __BYTE_ORDER == __BIG_ENDIAN
            uint32_t hi;
            uint32_t lo;
#else
    #error "endianness undefined"
#endif
        };
        uint64_t val;
    } t;

    t.hi = hi;
    t.lo = lo;

    return t.val;
}

static FORCE_INLINE bool cpuPrvSignedSubtractionWithPossibleCarryOverflows(
    uint32_t a, uint32_t b,
    uint32_t diff)  // diff = a - b
{
    return ((a ^ b) & (a ^ diff)) >> 31;
}

static FORCE_INLINE bool cpuPrvSignedAdditionWithPossibleCarryOverflows(uint32_t a, uint32_t b,
                                                                        uint32_t sum) {
    return ((a ^ b ^ 0x80000000UL) & (a ^ sum)) >> 31;
}

static bool cpuPrvPaceFaultDiagnostics() {
#ifndef __EMSCRIPTEN__
    static const bool enabled = std::getenv("FROND_PACE_FAULT_DIAGNOSTICS") != nullptr;
    return enabled;
#else
    return false;
#endif
}

static void cpuPrvHandlePaceMemoryFault(struct ArmCpu *cpu, const char* origin,
                                      uint32_t instructionPc = 0,
                                      const PaceDebugState* before = nullptr) {
    uint32_t addr;
    bool wasWrite;
    uint_fast8_t fsr;

    paceGetMemeryFault(&addr, &wasWrite, &fsr);
#ifndef __EMSCRIPTEN__
    if (cpuPrvPaceFaultDiagnostics()) {
        const auto after = paceDebugState();
        fprintf(stderr, "PACE-FAULT origin=%s addr=%08x write=%u fsr=%02x arm_pc=%08x cpsr=%08x modePace=%u internal_state=%08x instruction_pc_available=%u instruction_pc=%08x retained_opcode_unvalidated=%04x\n",
                origin, addr, unsigned(wasWrite), unsigned(fsr), cpu->curInstrPC,
                cpuPrvMaterializeCPSR(cpu), unsigned(cpu->modePace), after.statePtr,
                unsigned(before != nullptr), instructionPc,
                before ? unsigned(paceGetLastOpcode()) : 0);
        for (unsigned i = 0; i < 16; ++i)
            fprintf(stderr, "PACE-FAULT arm r%u=%08x\n", i, cpu->regs[i]);
        for (const auto* state : {before, &after}) {
            if (!state) continue;
            fprintf(stderr, "PACE-FAULT %s pc=%08x sr=%04x state=%08x\n",
                    state == before ? "before" : "after", state->pc, unsigned(state->sr), state->statePtr);
            for (unsigned i = 0; i < 8; ++i)
                fprintf(stderr, "PACE-FAULT %s d%u=%08x a%u=%08x\n",
                        state == before ? "before" : "after", i, state->d[i], i, state->a[i]);
        }
        fflush(stderr);
    }
#endif
    cpuPrvHandleMemErr(cpu, addr, wasWrite, false, fsr);

#ifndef __EMSCRIPTEN__
    fprintf(stderr, "memory fault in PACE %s at 0x%08x\n", wasWrite ? "writing" : "reading", addr);
#endif
}

template <int memorySystemKind, bool wasT>
static void execFn_invalid(struct ArmCpu *cpu, uint32_t instr);

// On the ARM920T the NV condition forms are undefined, except PLD, which never executes there: the HP
// iPAQ rx1950's bootloader has PLD in its copy loops and runs on the real ARM920T.
template <int memorySystemKind, bool wasT>
static void execFn_noop(struct ArmCpu *cpu, uint32_t instr) {
    if (armKindIsArm920(memorySystemKind) && (instr >> 28) == 15 && (instr & 0x0D70F000UL) != 0x0550F000UL)
        execFn_invalid<memorySystemKind, wasT>(cpu, instr);
}

// PACE entrypoint was called from ARM: regular invocation
template <int memorySystemKind>
static void execFn_paceEnter(struct ArmCpu *cpu, uint32_t instr) {
    uint_fast8_t fsr = 0;

    for (uint8_t reg : {REG_NO_LR, 11, 10, 9, 8, 7, 6, 5, 4, 0}) {
        cpu->regs[REG_NO_SP] -= 4;

        if (!cpuPrvMemOp<memorySystemKind, 4, true>(cpu, &cpu->regs[reg], cpu->regs[REG_NO_SP],
                                                    cpu->privileged, &fsr))
            return cpuPrvHandleMemErr<memorySystemKind>(cpu, cpu->regs[REG_NO_SP], true, false, fsr);
    }

    paceSetPriviledged(cpu->privileged);
    paceSetStatePtr(cpu->regs[0]);

    if (!paceLoad68kState()) return cpuPrvHandlePaceMemoryFault(cpu, __func__);

    cpu->modePace = true;
    cpu->paceOffset = cpu->curInstrPC - cpu->pacePatch->enterPace;

    cpuSetSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE);

#ifdef TRACE_PACE
    fprintf(stderr, "enter PACE\n");
#endif

    return;
}

// PACE execution was resumed from ARM: resume after interrupt / exception
static void execFn_paceResume(struct ArmCpu *cpu, uint32_t instr) {
    paceSetPriviledged(cpu->privileged);
    paceSetStatePtr(cpu->regs[0]);

    if (!paceLoad68kState()) return cpuPrvHandlePaceMemoryFault(cpu, __func__);

    cpu->modePace = true;
    cpu->paceOffset = cpu->curInstrPC - 4 - cpu->pacePatch->enterPace;
    cpu->regs[REG_NO_PC] = cpu->paceOffset + cpu->pacePatch->resumePace;

    cpuSetSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE);

#ifdef TRACE_PACE
    fprintf(stderr, "resume PACE\n");
#endif
}

// PACE was reentered after a callout
template <int memorySystemKind>
static void execFn_paceReturnFromCallout(struct ArmCpu *cpu, uint32_t instr) {
    uint_fast8_t fsr = 0;

    // restore r0 / state pointer from stack
    if (!cpuPrvMemOp<memorySystemKind, 4, false>(cpu, &cpu->regs[0], cpu->regs[REG_NO_SP],
                                                 cpu->privileged, &fsr))
        return cpuPrvHandleMemErr<memorySystemKind>(cpu, cpu->regs[REG_NO_SP], false, false, fsr);

    // restore r9 / dispatch table pointer from stack
    if (!cpuPrvMemOp<memorySystemKind, 4, false>(cpu, &cpu->regs[9], cpu->regs[REG_NO_SP] + 24,
                                                 cpu->privileged, &fsr))
        return cpuPrvHandleMemErr<memorySystemKind>(cpu, cpu->regs[REG_NO_SP] + 24, false, false, fsr);

    paceSetPriviledged(cpu->privileged);
    paceSetStatePtr(cpu->regs[0]);

    if (!paceLoad68kState()) return cpuPrvHandlePaceMemoryFault(cpu, __func__);

    cpu->modePace = true;
    cpu->paceOffset = cpu->curInstrPC - 8 - cpu->pacePatch->enterPace;
    cpu->regs[REG_NO_PC] = cpu->paceOffset + cpu->pacePatch->resumePace;

    cpuSetSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE);

#ifdef TRACE_PACE
    fprintf(stderr, "PACE return from callout\n");
#endif
}

// The ROM PACE has finished the instruction native PACE faulted on (cpuPrvPaceRomFallback)
// and is about to dispatch the next one: r10 is its address + 4, r4 the SR, r8 its opcode
// and [sp] the state. Stores them as the ROM PACE's own exit does, then resumes native PACE.
template <int memorySystemKind>
static void execFn_paceDispatch(struct ArmCpu *cpu, uint32_t instr) {
    uint_fast8_t fsr = 0;
    uint32_t state = 0;

    if (!cpuPrvMemOp<memorySystemKind, 4, false>(cpu, &state, cpu->regs[REG_NO_SP], cpu->privileged,
                                                 &fsr))
        return cpuPrvHandleMemErr<memorySystemKind>(cpu, cpu->regs[REG_NO_SP], false, false, fsr);

    const struct {
        uint32_t offset, value;
    } stores[] = {{0x44, cpu->regs[10] - 4}, {0x48, cpu->regs[4]}, {0, cpu->regs[8]}};
    for (auto store : stores) {
        if (!cpuPrvMemOp<memorySystemKind, 4, true>(cpu, &store.value, state + store.offset,
                                                    cpu->privileged, &fsr))
            return cpuPrvHandleMemErr<memorySystemKind>(cpu, state + store.offset, true, false, fsr);
    }

    // r9 is the caller's again, as native PACE callouts expect.
    if (!cpuPrvMemOp<memorySystemKind, 4, false>(cpu, &cpu->regs[9], cpu->regs[REG_NO_SP] + 24,
                                                 cpu->privileged, &fsr))
        return cpuPrvHandleMemErr<memorySystemKind>(cpu, cpu->regs[REG_NO_SP] + 24, false, false, fsr);

    cpu->regs[0] = state;
    paceSetPriviledged(cpu->privileged);
    paceSetStatePtr(state);

    if (!paceLoad68kState()) return cpuPrvHandlePaceMemoryFault(cpu, __func__);

    cpu->modePace = true;
    cpu->regs[REG_NO_PC] = cpu->paceOffset + cpu->pacePatch->resumePace;

    cpuSetSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE);
}

// TG50 automatic library Sleep/Wake, in place of "mov r3, #0" before "bl 200ec798".
// r7 is the 68k state, r1 the library table index. The refnum goes in the free stack word
// below A7; 200de128 then reserves argBytes + 4 under A7, so the library sees it at 8(A6)
// after LINK, and A7 is back where it was when the call returns. If A7 is odd or the write
// fails, the call keeps the firmware's zero-argument form.
template <int memorySystemKind>
static void execFn_tg50LibraryRefnum(struct ArmCpu *cpu, uint32_t instr) {
    uint_fast8_t fsr = 0;
    uint32_t a7 = 0;
    cpu->regs[3] = 0;
    if (!cpuPrvMemOpEx<memorySystemKind, 4, false>(cpu, &a7, cpu->regs[7] + 0x40,
                                                   cpu->privileged, &fsr) ||
        (a7 & 1) || a7 < 2)
        return;
    // 68k memory is big-endian.
    uint16_t refnum = __builtin_bswap16(uint16_t(cpu->regs[1]));
    if (cpuPrvMemOpEx<memorySystemKind, 2, true>(cpu, &refnum, a7 - 2, cpu->privileged, &fsr))
        cpu->regs[3] = 2;
}

// Charges a peephole the clocks of the ADS routine it replaces, less the 1 its own slot charged, and
// runs that routine's loads and stores through the data cache model in order. before: the registers as
// the peephole found them.
template <int memorySystemKind>
static void cpuPrvPeepholeCharge(struct ArmCpu *cpu, uint32_t instr, const uint32_t *before) {
#ifndef __EMSCRIPTEN__
    if (!cpu->ic->costOn) return;
    size_t count = 0;
    const uint32_t *words = peepholeRoutine(instr, &count);
    uint32_t regs[16];
    std::memcpy(regs, before, sizeof(regs));
    uint32_t flags = cpu->flags & 0xf0000000UL;
    uint32_t owed = 1;
    cost::runRoutine(
        words, count, regs, flags, cost::Core(cpu->costCore),
        [cpu, &owed](uint32_t c) {
            const uint32_t taken = c < owed ? c : owed;
            owed -= taken;
            cpu->jitHint.pending += c - taken;
        },
        [cpu](uint32_t va, bool store) {
            if constexpr (armMemorySystemOf(memorySystemKind) == ARM_MEMORY_SYSTEM_MMU) {
                if (!cpu->costMemory) return;
                const MMUTranslateResult r =
                    store ? mmuTranslateCached<true, true>(cpu->memorySystem.mmu, va, cpu->privileged)
                          : mmuTranslateCached<false, true>(cpu->memorySystem.mmu, va, cpu->privileged);
                if (!MMU_TRANSLATE_RESULT_OK(r)) return;
                cpuPrvCostAccess(cpu, va,
                                 cpuPrvCostInfo(MMU_TRANSLATE_RESULT_CACHEABLE(r), MMU_TRANSLATE_RESULT_BUFFERABLE(r),
                                                MMU_TRANSLATE_RESULT_XSCALE_X(r),
                                                cost::pxaRegion(MMU_TRANSLATE_RESULT_PA(r))),
                                 store);
            } else {
                (void)cpu, (void)va, (void)store;
            }
        });
#else
    (void)cpu, (void)instr, (void)before;
#endif
}

static void execFn_peephole_ADC_udiv10(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t before[16];
    std::memcpy(before, cpu->regs, sizeof(before));
    const uint32_t num = cpuPrvGetRegNotPC(cpu, 0);

    cpuPrvSetRegNotPC(cpu, 0, num / 10ul);
    cpuPrvSetRegNotPC(cpu, 1, num % 10ul);

    cpuPrvSetPC(cpu, cpuPrvGetRegNotPC(cpu, REG_NO_LR));
    cpuPrvPeepholeCharge<ARM_MEMORY_SYSTEM_MMU>(cpu, instr, before);
}

static void execFn_peephole_ADC_sdiv10(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t before[16];
    std::memcpy(before, cpu->regs, sizeof(before));
    const int32_t num = cpuPrvGetRegNotPC(cpu, 0);

    cpuPrvSetRegNotPC(cpu, 0, num / 10l);
    cpuPrvSetRegNotPC(cpu, 1, num % 10l);

    cpuPrvSetPC(cpu, cpuPrvGetRegNotPC(cpu, REG_NO_LR));
    cpuPrvPeepholeCharge<ARM_MEMORY_SYSTEM_MMU>(cpu, instr, before);
}

template <int memorySystemKind>
static void execFn_peephole_ADC_memcpy(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t before[16];
    std::memcpy(before, cpu->regs, sizeof(before));
    uint32_t src = cpuPrvGetRegNotPC(cpu, 1);
    uint32_t dest = cpuPrvGetRegNotPC(cpu, 0);
    uint32_t size = cpuPrvGetRegNotPC(cpu, 2);

    // This should not be necessary, but it works around a bug where UDMH fails to
    // invalidate the cache...
    icacheInvalRange(cpu->ic, dest, size);

    MemcpyResult result;
    if constexpr (armMemorySystemOf(memorySystemKind) == ARM_MEMORY_SYSTEM_MMU) {
        memcpy_armToArm(dest, src, size, cpu->privileged, cpu->mem, cpu->memorySystem.mmu, &result);
    } else {
        memcpy_armToArm(dest, src, size, cpu->privileged, cpu->mem, cpu->memorySystem.mpu, &result);
    }

    if (!result.ok) {
        cpuPrvHandleMemErr<memorySystemKind>(cpu, result.faultAddr, result.wasWrite, false, result.fsr);
    } else {
        cpuPrvSetPC<memorySystemKind>(cpu, cpuPrvGetRegNotPC(cpu, REG_NO_LR));
        cpuPrvPeepholeCharge<memorySystemKind>(cpu, instr, before);
    }
}

template <int memorySystemKind, bool wasT>
static void execFn_invalid(struct ArmCpu *cpu, uint32_t instr) {
    ++cpu->undefinedCount;
    cpu->undefinedPc = cpu->curInstrPC;
    cpu->undefinedInstr = instr;
    if (cpu->undefinedHook) cpu->undefinedHook(cpu->undefinedHookContext, cpu->curInstrPC, instr);
    // A guest whose own handler takes the exception can loop on it, so the log gets the first 16 and then
    // every power of two, with the count.
    static std::atomic<uint64_t> logged{0};
    const uint64_t n = logged.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16 || !(n & (n - 1)))
        fprintf(stderr, "Invalid instr 0x%08lx seen at 0x%08lx with CPSR 0x%08lx (undefined %llu)\n",
                (unsigned long)instr, (unsigned long)cpu->curInstrPC,
                (unsigned long)cpuPrvMaterializeCPSR(cpu), (unsigned long long)n);

    cpuPrvException<memorySystemKind>(cpu, cpu->vectorBase + ARM_VECTOR_OFFT_UND, cpu->curInstrPC + (wasT ? 2 : 4),
                    ARM_SR_MODE_UND | ARM_SR_I);
}

template <int memorySystemKind, bool wasT, bool align2>
static void execFn_b2thumb(struct ArmCpu *cpu, uint32_t instr) {
    if (armKindIsArm920(memorySystemKind)) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

    uint32_t ea;

    ea = instr << 8;
    ea = ((int32_t)ea) >> 7;
    ea += 4;
    if (!wasT) ea <<= 1;
    ea += cpu->curInstrPC;

    if (align2) ea += 2;
    cpu->regs[REG_NO_LR] = cpu->curInstrPC + (wasT ? 2 : 4);
    if (!cpu->T) ea |= 1UL;  // set T flag if needed

    cpuPrvSetPC<memorySystemKind>(cpu, ea);
}

template <int memorySystemKind, bool wasT, bool two>
static void execFn_cp_mem2reg(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t addBefore, addAfter;
    uint_fast8_t mode, cpNo;
    uint8_t memVal8;

    if (two && armKindIsArm920(memorySystemKind)) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);
    if (cpuPrvConditionFails(cpu, instr)) return;

    cpNo = (instr >> 8) & 0x0F;
    mode = cpuPrvArmAdrMode_5(cpu, instr, &addBefore, &addAfter, &memVal8);

    if (cpNo >= 14) {  // cp14 and cp15 are for priviledged users only
        if (!cpu->privileged) goto invalid_instr;
    } else if (!(cpu->CPAR & (1UL << cpNo)))  // others are access-controlled by CPAR
        goto invalid_instr;

    if (mode & ARM_MODE_5_RR) {  // handle MCRR, MRCC
        if (armKindIsArm920(memorySystemKind)) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

        if (!cpu->coproc[cpNo].twoRegF ||
            !cpu->coproc[cpNo].twoRegF(cpu, cpu->coproc[cpNo].userData, !!(instr & 0x00100000UL),
                                       (instr >> 4) & 0x0F, (instr >> 12) & 0x0F,
                                       (instr >> 16) & 0x0F, instr & 0x0F))
            goto invalid_instr;
    } else {  // handle LDC/STC

        if (!cpu->coproc[cpNo].memAccess ||
            !cpu->coproc[cpNo].memAccess(cpu, cpu->coproc[cpNo].userData, two,
                                         !!(instr & 0x00400000UL), !(instr & 0x00100000UL),
                                         (instr >> 12) & 0x0F, mode & ARM_MODE_5_REG, addBefore,
                                         addAfter, (mode & ARM_MODE_5_IS_OPTION) ? &memVal8 : NULL))
            goto invalid_instr;
    }

    return;

invalid_instr:
    execFn_invalid<memorySystemKind, wasT>(cpu, instr);
}

template <int memorySystemKind, bool wasT, bool two>
static void execFn_cp_dp(struct ArmCpu *cpu, uint32_t instr) {
    uint_fast8_t cpNo;

    if (two && armKindIsArm920(memorySystemKind)) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);
    if (cpuPrvConditionFails(cpu, instr)) return;

    cpNo = (instr >> 8) & 0x0F;

    if (cpNo >= 14) {  // cp14 and cp15 are for priviledged users only
        if (!cpu->privileged) goto invalid_instr;
    } else if (!(cpu->CPAR & (1UL << cpNo)))  // others are access-controlled by CPAR
        goto invalid_instr;

    if (instr & 0x00000010UL) {  // MCR[2]/MRC[2]

        if (!cpu->coproc[cpNo].regXfer ||
            !cpu->coproc[cpNo].regXfer(cpu, cpu->coproc[cpNo].userData, two,
                                       !!(instr & 0x00100000UL), (instr >> 21) & 0x07,
                                       (instr >> 12) & 0x0F, (instr >> 16) & 0x0F, instr & 0x0F,
                                       (instr >> 5) & 0x07))
            goto invalid_instr;
    } else {  // CDP

        if (!cpu->coproc[cpNo].dataProcessing ||
            !cpu->coproc[cpNo].dataProcessing(
                cpu, cpu->coproc[cpNo].userData, two, (instr >> 20) & 0x0F, (instr >> 12) & 0x0F,
                (instr >> 16) & 0x0F, instr & 0x0F, (instr >> 5) & 0x07))
            goto invalid_instr;
    }

    return;

invalid_instr:
    execFn_invalid<memorySystemKind, wasT>(cpu, instr);
}

template <int memorySystemKind, bool wasT, int tag>
static void execFn_swb(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t op1, ea, memVal32;
    bool ok;
    uint_fast8_t fsr;
    uint8_t memVal8;

    if (cpuPrvConditionFails(cpu, instr)) return;

    if constexpr (armMemorySystemOf(memorySystemKind) == ARM_MEMORY_SYSTEM_MMU) {
        if (armKindIsArm920(memorySystemKind) && (tag == 0 || tag == 4)) {
            ea = cpuPrvGetRegNotPC(cpu, (instr >> 16) & 0x0f);
            const unsigned size = tag == 0 ? 4 : 1;
            if (size == 4 && (ea & 3)) {
                if (cp15MMUAlignmentEnabled(cpu->cp15.cp15mmu))
                    return cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, false, false, 1);
                ea &= ~3u;
            }
            for (bool writing : {false, true}) {
                const auto translation = mmuTranslate(cpu->memorySystem.mmu, cpuPrvMva(cpu, ea), cpu->privileged, writing);
                if (!MMU_TRANSLATE_RESULT_OK(translation))
                    return cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, writing, false, MMU_TRANSLATE_RESULT_FSR(translation));
                auto* dc = cp15MMUArm920DataCache(cpu->cp15.cp15mmu);
                dc->mem = cpu->mem;
                if (!dc->supported(MMU_TRANSLATE_RESULT_PA(translation), size, writing))
                    return cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, writing, false, 0);
            }
        }
    }

    switch (tag) {
        case 0:  // SWP

            ea = cpuPrvGetRegNotPC(cpu, (instr >> 16) & 0x0F);
            ok = cpuPrvMemOp<memorySystemKind, 4, false>(cpu, &memVal32, ea, cpu->privileged, &fsr);
            if (!ok) {
                cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, false, false, fsr);
                return;
            }
            op1 = (ea & 3) ? cpuPrvROR(memVal32, (ea & 3) * 8) : memVal32;
            memVal32 = cpuPrvGetRegNotPC(cpu, instr & 0x0F);
            ok = cpuPrvMemOp<memorySystemKind, 4, true>(cpu, &memVal32, ea, cpu->privileged, &fsr);
            if (!ok) {
                cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, true, false, fsr);
                return;
            }
            cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, op1);
            break;

        case 4:  // SWPB

            ea = cpuPrvGetRegNotPC(cpu, (instr >> 16) & 0x0F);
            ok = cpuPrvMemOp<memorySystemKind, 1, false>(cpu, &memVal8, ea, cpu->privileged, &fsr);
            if (!ok) {
                cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, false, false, fsr);
                return;
            }
            op1 = memVal8;
            memVal8 = cpuPrvGetRegNotPC(cpu, instr & 0x0F);
            ok = cpuPrvMemOp<memorySystemKind, 1, true>(cpu, &memVal8, ea, cpu->privileged, &fsr);
            if (!ok) {
                cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, true, false, fsr);
                return;
            }
            cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, op1);
            break;

        default:
            return execFn_invalid<memorySystemKind, wasT>(cpu, instr);
    }
}

template <int memorySystemKind, bool wasT, int tag, bool flags>
static void execFn_mult(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t op1, op2, res;
    uint64_t res64;

    if (cpuPrvConditionFails(cpu, instr)) return;

    switch (tag) {  // multiplies

        case 0:  // MUL
        case 1:

            res = 0;
#ifdef STRICT_CPU
            if (instr & 0x0000F000UL) goto invalid_instr;
#endif
            goto mul32;

        case 2:  // MLA
        case 3:

            res = cpuPrvGetRegNotPC(cpu, (instr >> 12) & 0x0F);
        mul32:
            res +=
                cpuPrvGetRegNotPC(cpu, (instr >> 8) & 0x0F) * cpuPrvGetRegNotPC(cpu, instr & 0x0F);
            cpuPrvSetRegNotPC(cpu, (instr >> 16) & 0x0F, res);
            if (flags) {  // S

                cpu->flags &= ~(ARM_SR_Z | ARM_SR_N);
                if (!res) cpu->flags |= ARM_SR_Z;
                cpu->flags |= (res & 0x80000000UL);
            }
            return;

        case 8:  // UMULL
        case 9:
        case 12:  // SMULL
        case 13:

            res64 = 0;
            goto mul64;

        case 10:  // UMLAL
        case 11:
        case 14:  // SMLAL
        case 15:

            res64 = cpuPrv64FromHalves(cpuPrvGetRegNotPC(cpu, (instr >> 16) & 0x0F),
                                       cpuPrvGetRegNotPC(cpu, (instr >> 12) & 0x0F));

        mul64:
            op1 = cpuPrvGetRegNotPC(cpu, (instr >> 8) & 0x0F);
            op2 = cpuPrvGetRegNotPC(cpu, instr & 0x0F);

            if (instr & 0x00400000UL)
                res64 += (int64_t)(int32_t)op1 * (int64_t)(int32_t)op2;
            else
                res64 += (uint64_t)(uint32_t)op1 * (uint64_t)(uint32_t)op2;

            cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, res64);
            cpuPrvSetRegNotPC(cpu, (instr >> 16) & 0x0F, res64 >> 32);

            if (flags) {  // S

                cpu->flags &= ~(ARM_SR_Z | ARM_SR_N);
                if (!res64) cpu->flags |= ARM_SR_Z;
                cpu->flags |= (res64 & 0x8000000000000000ULL) >> 32;
            }
            break;

        default:
            return execFn_invalid<memorySystemKind, wasT>(cpu, instr);
    }
}

template <int memorySystemKind, bool wasT, int mode, bool pc, bool addBefore, bool addAfter,
          bool immediate, bool negate>
static void execFn_load_store_1(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t ea, increment;
    bool ok;
    uint_fast8_t fsr, reg;
    uint16_t memVal16;
    uint8_t memVal8;
    uint32_t doubleMem[2];

    if (cpuPrvConditionFails(cpu, instr)) return;
    if ((mode & ARM_MODE_3_TYPE) == ARM_MODE_3_D && armKindIsArm920(memorySystemKind))
        return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

    reg = (instr >> 16) & 0x0F;

    if constexpr (addBefore || addAfter) {
        increment = immediate ? ((instr >> 4) & 0xF0) | (instr & 0x0F)
                              : cpuPrvGetRegNotPC(cpu, instr & 0x0F);

        if constexpr (negate) increment = -increment;
    }

    ea = cpuPrvGetReg<wasT, pc>(cpu, reg);
    if constexpr (addBefore) ea += increment;

    if (mode & ARM_MODE_3_LOAD) {
        switch (mode & ARM_MODE_3_TYPE) {
            case ARM_MODE_3_H:

                ok = cpuPrvMemOp<memorySystemKind, 2, false>(cpu, &memVal16, ea, cpu->privileged,
                                                             &fsr);
                if (!ok) {
                    cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, false, false, fsr);
                    return;
                }
                cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, memVal16);
                break;

            case ARM_MODE_3_SH:

                ok = cpuPrvMemOp<memorySystemKind, 2, false>(cpu, &memVal16, ea, cpu->privileged,
                                                             &fsr);
                if (!ok) {
                    cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, false, false, fsr);
                    return;
                }
                cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, (int32_t)(int16_t)memVal16);
                break;

            case ARM_MODE_3_SB:

                ok = cpuPrvMemOp<memorySystemKind, 1, false>(cpu, &memVal8, ea, cpu->privileged,
                                                             &fsr);
                if (!ok) {
                    cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, false, false, fsr);
                    return;
                }
                cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, (int32_t)(int8_t)memVal8);
                break;

            case ARM_MODE_3_D:

                ok = cpuPrvMemOp<memorySystemKind, 8, false>(cpu, doubleMem, ea, cpu->privileged,
                                                             &fsr);
                if (!ok) {
                    cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, false, false, fsr);
                    return;
                }

                cpuPrvSetRegNotPC(cpu, ((instr >> 12) & 0x0F) + 0, doubleMem[0]);
                cpuPrvSetRegNotPC(cpu, ((instr >> 12) & 0x0F) + 1, doubleMem[1]);
                break;
        }
    } else {
        switch (mode & ARM_MODE_3_TYPE) {
            case ARM_MODE_3_H:
                memVal16 = mode & ARM_MDOE_3_PCREL
                               ? cpuPrvGetReg<wasT, true>(cpu, (instr >> 12) & 0x0F)
                               : cpuPrvGetRegNotPC(cpu, (instr >> 12) & 0x0F);
                ok = cpuPrvMemOp<memorySystemKind, 2, true>(cpu, &memVal16, ea, cpu->privileged,
                                                            &fsr);
                if (!ok) {
                    cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, true, false, fsr);
                    return;
                }
                break;

            case ARM_MODE_3_SH:
            case ARM_MODE_3_SB:
                return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

            case ARM_MODE_3_D:

                doubleMem[0] = cpuPrvGetRegNotPC(cpu, ((instr >> 12) & 0x0F) + 0);
                doubleMem[1] = cpuPrvGetRegNotPC(cpu, ((instr >> 12) & 0x0F) + 1);
                ok = cpuPrvMemOp<memorySystemKind, 8, true>(cpu, doubleMem, ea, cpu->privileged,
                                                            &fsr);
                if (!ok) {
                    cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, true, false, fsr);
                    return;
                }
                break;
        }
    }

    if constexpr (addAfter) {
        if constexpr (addBefore)
            cpuPrvSetRegNotPC(cpu, reg, ea);
        else
            cpuPrvSetRegNotPC(cpu, reg, ea + increment);
    }
}

template <int memorySystemKind, bool wasT>
static void execFn_clz(struct ArmCpu *cpu, uint32_t instr) {
    if (cpuPrvConditionFails(cpu, instr)) return;
    if (armKindIsArm920(memorySystemKind)) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

    cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, cpuPrvClz(cpuPrvGetRegNotPC(cpu, instr & 0xF)));
}

template <int memorySystemKind, bool wasT, bool link>
static void execFn_bl_reg(struct ArmCpu *cpu, uint32_t instr) {
    if (cpuPrvConditionFails(cpu, instr)) return;
    if (link && armKindIsArm920(memorySystemKind)) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

    cpuPrvSetPC<memorySystemKind>(cpu, cpuPrvGetReg<wasT>(cpu, instr & 0x0F));

    if (link)
        cpuPrvSetRegNotPC(cpu, REG_NO_LR,
                          cpu->curInstrPC + (wasT ? 3 : 4));  // save return value for BLX
}

template <int memorySystemKind, bool wasT, bool spsr>
static void execFn_psr2reg(struct ArmCpu *cpu, uint32_t instr) {
    if (cpuPrvConditionFails(cpu, instr)) return;

    cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, spsr ? cpu->SPSR : cpuPrvMaterializeCPSR(cpu));
}

template <int memorySystemKind, bool wasT, bool spsr, bool pc>
static void execFn_reg2psr(struct ArmCpu *cpu, uint32_t instr) {
    if (cpuPrvConditionFails(cpu, instr)) return;

    cpuPrvSetPSR<spsr, memorySystemKind>(cpu, (instr >> 16) & 0x0F, cpuPrvGetReg<wasT, pc>(cpu, instr & 0x0F));
}

template <int memorySystemKind, bool wasT, int tag>
static void execFn_dspadd(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t op1, op2, res;

    if (cpuPrvConditionFails(cpu, instr)) return;
    if (armKindIsArm920(memorySystemKind)) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

    op1 = cpuPrvGetRegNotPC(cpu, instr & 0x0F);          // Rm
    op2 = cpuPrvGetRegNotPC(cpu, (instr >> 16) & 0x0F);  // Rn
    switch (tag) {                                       // what op?

        case 0:  // QADD

            res = op1 + op2;
            if (cpuPrvSignedAdditionOverflows(op1, op2, res)) {
                cpu->Q = 1;
                res = cpuPrvMedia_signedSaturate32(op1);
            }
            break;

        case 1:  // QSUB

            res = op1 - op2;
            if (cpuPrvSignedSubtractionOverflows(op1, op2, res)) {
                cpu->Q = 1;
                res = cpuPrvMedia_signedSaturate32(op1);
            }
            break;
        case 2:  // QDADD

            res = op2 + op2;
            if (cpuPrvSignedAdditionOverflows(op2, op2, res)) {
                cpu->Q = 1;
                res = cpuPrvMedia_signedSaturate32(op2);
            }
            op2 = res;

            res = op1 + op2;
            if (cpuPrvSignedAdditionOverflows(op1, op2, res)) {
                cpu->Q = 1;
                res = cpuPrvMedia_signedSaturate32(op1);
            }
            break;

        case 3:  // QDSUB

            res = op2 + op2;
            if (cpuPrvSignedAdditionOverflows(op2, op2, res)) {
                cpu->Q = 1;
                res = cpuPrvMedia_signedSaturate32(op2);
            }
            op2 = res;

            res = op1 - op2;
            if (cpuPrvSignedSubtractionOverflows(op1, op2, res)) {
                cpu->Q = 1;
                res = cpuPrvMedia_signedSaturate32(op1);
            }
            break;

        default:
            __builtin_unreachable();
    }

    cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, res);
}

template <int memorySystemKind, bool wasT>
static void execFn_softbreak(struct ArmCpu *cpu, uint32_t instr) {
    if (cpuPrvConditionFails(cpu, instr)) return;
    if (armKindIsArm920(memorySystemKind)) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

    cpuPrvException<memorySystemKind>(cpu, cpu->vectorBase + ARM_VECTOR_OFFT_P_ABT, cpu->curInstrPC + 4,
                    ARM_SR_MODE_ABT | ARM_SR_I);
}

template <int memorySystemKind, bool wasT, int tag>
static void execFn_dspmul(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t op1, op2, res;
    uint64_t res64;

    if (cpuPrvConditionFails(cpu, instr)) return;
    if (armKindIsArm920(memorySystemKind)) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

    op1 = cpuPrvGetRegNotPC(cpu, instr & 0x0F);         // Rm
    op2 = cpuPrvGetRegNotPC(cpu, (instr >> 8) & 0x0F);  // Rs
    switch (tag) {                                      // what op?
        case 0:                                         // SMLAxy
            if (instr & 0x00000020UL)
                op1 >>= 16;
            else
                op1 = (uint16_t)op1;

            if (instr & 0x00000040UL)
                op2 >>= 16;
            else
                op2 = (uint16_t)op2;

            res = (int32_t)(int16_t)op1 * (int32_t)(int16_t)op2;
            op1 = res;
            op2 = cpuPrvGetRegNotPC(cpu, (instr >> 12) & 0x0F);  // Rn
            res = op1 + op2;
            if (cpuPrvSignedAdditionOverflows(op1, op2, res)) cpu->Q = 1;
            cpuPrvSetRegNotPC(cpu, (instr >> 16) & 0x0F, res);
            break;

        case 1:  // SMLAWy/SMULWy

            if (instr & 0x00000040UL)
                op2 >>= 16;
            else
                op2 = (uint16_t)op2;

            res = (((int64_t)(int32_t)op1 * (int64_t)(int16_t)op2) >> 16);

            if ((instr & 0x00000020UL) == 0) {
                op1 = res;
                op2 = cpuPrvGetRegNotPC(cpu, (instr >> 12) & 0x0F);  // Rn
                res = op1 + op2;
                if (cpuPrvSignedAdditionOverflows(op1, op2, res)) cpu->Q = 1;
            }

            cpuPrvSetRegNotPC(cpu, (instr >> 16) & 0x0F, res);
            break;

        case 2:  // SMLALxy

            if (instr & 0x00000020UL)
                op1 >>= 16;
            else
                op1 = (uint16_t)op1;

            if (instr & 0x00000040UL)
                op2 >>= 16;
            else
                op2 = (uint16_t)op2;

            res64 = cpuPrv64FromHalves(cpuPrvGetRegNotPC(cpu, (instr >> 16) & 0x0F),
                                       cpuPrvGetRegNotPC(cpu, (instr >> 12) & 0x0F));
            res64 += (int32_t)(int16_t)op1 * (int32_t)(int16_t)op2;

            cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, res64);
            cpuPrvSetRegNotPC(cpu, (instr >> 16) & 0x0F, res64 >> 32);
            break;

        case 3:  // SMULxy
            if (instr & 0x00000020UL)
                op1 >>= 16;
            else
                op1 = (uint16_t)op1;

            if (instr & 0x00000040UL)
                op2 >>= 16;
            else
                op2 = (uint16_t)op2;

            res = (int32_t)(int16_t)op1 * (int32_t)(int16_t)op2;
            cpuPrvSetRegNotPC(cpu, (instr >> 16) & 0x0F, res);
            break;
    }
}

template <int memorySystemKind, bool wasT, int op, bool setFlags, bool srcPc, bool destPc,
          int tier = 0>
static void execFn_dproc(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t op1, op2, res, sr;
    uint64_t res64;
    bool cOut;

    if (cpuPrvConditionFails(cpu, instr)) return;

    op2 = cpuPrvArmAdrMode_1<wasT>(cpu, instr, &cOut);

    switch (op) {
        case 0:  // AND
            op1 = cpuPrvGetReg<wasT, srcPc>(cpu, (instr >> 16) & 0x0F);
            res = op1 & op2;
            break;

        case 1:  // EOR
            op1 = cpuPrvGetReg<wasT, srcPc>(cpu, (instr >> 16) & 0x0F);
            res = op1 ^ op2;
            break;

        case 2:  // SUB
            op1 = cpuPrvGetReg<wasT, srcPc>(cpu, (instr >> 16) & 0x0F);
            res = op1 - op2;
            if (setFlags) {
                cpu->flags &= ~ARM_SR_V;
                if (cpuPrvSignedSubtractionOverflows(op1, op2, res)) cpu->flags |= ARM_SR_V;
                cOut = !__builtin_sub_overflow_u32(op1, op2, &res);
            }
            break;

        case 3:  // RSB
            op1 = cpuPrvGetReg<wasT, srcPc>(cpu, (instr >> 16) & 0x0F);
            res = op2 - op1;
            if (setFlags) {
                cpu->flags &= ~ARM_SR_V;
                if (cpuPrvSignedSubtractionOverflows(op2, op1, res)) cpu->flags |= ARM_SR_V;
                cOut = !__builtin_sub_overflow_u32(op2, op1, &res);
            }
            break;

        case 4:  // ADD
            op1 = cpuPrvGetReg<wasT, srcPc>(cpu, (instr >> 16) & 0x0F);
            res = op1 + op2;
            if (setFlags) {
                cpu->flags &= ~ARM_SR_V;
                if (cpuPrvSignedAdditionOverflows(op1, op2, res)) cpu->flags |= ARM_SR_V;
                cOut = __builtin_add_overflow_u32(op1, op2, &res);
            }
            break;

        case 5:  // ADC
            op1 = cpuPrvGetReg<wasT, srcPc>(cpu, (instr >> 16) & 0x0F);
            res = res64 = (uint64_t)op1 + op2 + ((cpu->flags & ARM_SR_C) >> 29);
            if (setFlags) {  // hard to get this right in C in 32 bits so go to 64...
                cOut = res64 >> 32;
                cpu->flags &= ~ARM_SR_V;
                if (cpuPrvSignedAdditionWithPossibleCarryOverflows(op1, op2, res))
                    cpu->flags |= ARM_SR_V;
            }
            break;

        case 6:  // SBC
            op1 = cpuPrvGetReg<wasT, srcPc>(cpu, (instr >> 16) & 0x0F);
            res = res64 = (uint64_t)op1 - op2 - ((~cpu->flags & ARM_SR_C) >> 29);
            if (setFlags) {  // hard to get this right in C in 32 bits so go to 64...
                cOut = !(res64 >> 32);
                cpu->flags &= ~ARM_SR_V;
                if (cpuPrvSignedSubtractionWithPossibleCarryOverflows(op1, op2, res))
                    cpu->flags |= ARM_SR_V;
            }
            break;

        case 7:  // RSC
            op1 = cpuPrvGetReg<wasT, srcPc>(cpu, (instr >> 16) & 0x0F);
            res = res64 = (uint64_t)op2 - op1 - ((~cpu->flags & ARM_SR_C) >> 29);
            if (setFlags) {  // hard to get this right in C in 32 bits so go to 64...
                cOut = !(res64 >> 32);
                cpu->flags &= ~ARM_SR_V;
                if (cpuPrvSignedSubtractionWithPossibleCarryOverflows(op2, op1, res))
                    cpu->flags |= ARM_SR_V;
            }
            break;

        case 8:  // TST
            cpu->flags &= ~(ARM_SR_Z | ARM_SR_N | ARM_SR_C);
            op1 = cpuPrvGetReg<wasT, srcPc>(cpu, (instr >> 16) & 0x0F);
            res = op1 & op2;
            goto dp_flag_set;

        case 9:               // TEQ
            if (!setFlags) {  // MSR CPSR, imm

                cpuPrvSetPSR<false, memorySystemKind>(cpu, (instr >> 16) & 0x0F,
                                    cpuPrvROR(instr & 0xFF, ((instr >> 8) & 0x0F) * 2));
                return;
            }
            cpu->flags &= ~(ARM_SR_Z | ARM_SR_N | ARM_SR_C);
            op1 = cpuPrvGetReg<wasT, srcPc>(cpu, (instr >> 16) & 0x0F);
            res = op1 ^ op2;
            goto dp_flag_set;

        case 10:  // CMP
            cpu->flags &= ~(ARM_SR_Z | ARM_SR_N | ARM_SR_C | ARM_SR_V);
            op1 = cpuPrvGetReg<wasT, srcPc>(cpu, (instr >> 16) & 0x0F);
            res = op1 - op2;
            if (cpuPrvSignedSubtractionOverflows(op1, op2, res)) cpu->flags |= ARM_SR_V;
            cOut = !__builtin_sub_overflow_u32(op1, op2, &res);
            goto dp_flag_set;

        case 11:              // CMN
            if (!setFlags) {  // MSR SPSR, imm

                cpuPrvSetPSR<true, memorySystemKind>(cpu, (instr >> 16) & 0x0F,
                                   cpuPrvROR(instr & 0xFF, ((instr >> 8) & 0x0F) * 2));
                return;
            }
            cpu->flags &= ~(ARM_SR_Z | ARM_SR_N | ARM_SR_C | ARM_SR_V);
            op1 = cpuPrvGetReg<wasT, srcPc>(cpu, (instr >> 16) & 0x0F);
            res = op1 + op2;
            cpu->flags &= ~ARM_SR_V;
            if (cpuPrvSignedAdditionOverflows(op1, op2, res)) cpu->flags |= ARM_SR_V;
            cOut = __builtin_add_overflow_u32(op1, op2, &res);
            goto dp_flag_set;

        case 12:  // ORR
            op1 = cpuPrvGetReg<wasT, srcPc>(cpu, (instr >> 16) & 0x0F);
            res = op1 | op2;
            break;

        case 13:  // MOV
            res = op2;
            break;

        case 14:  // BIC
            op1 = cpuPrvGetReg<wasT, srcPc>(cpu, (instr >> 16) & 0x0F);
            res = op1 & ~op2;
            break;

        case 15:  // MVN
            res = ~op2;
            break;
    }

    if (!setFlags)  // simple store
        cpuPrvSetReg<destPc, memorySystemKind>(cpu, (instr >> 12) & 0x0F, res);
    else if (destPc) {  // copy SPSR to CPSR. we allow in user
                        // mode too - allowed and faster

        if (armKindIsArm920(memorySystemKind)) icacheArm920BranchInline(cpu->ic);
        sr = cpu->SPSR;
        cpuPrvSetPSRlo8<memorySystemKind>(cpu, sr);
        cpuPrvSetPSRhi8(cpu, sr);
        cpu->regs[REG_NO_PC] = res;  // do it right here - if we let it use cpuPrvSetReg, it
                                     // will check lower bit...
    } else {                         // store and set flags

        cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, res);
        cpu->flags &= ~(ARM_SR_Z | ARM_SR_N | ARM_SR_C);

    dp_flag_set:
        if (cOut) cpu->flags |= ARM_SR_C;
        if (!res) cpu->flags |= ARM_SR_Z;
        cpu->flags |= (res & 0x80000000UL);
    }
}

template <int memorySystemKind, bool wasT, int mode, bool srcPc, bool destPc, bool addBefore,
          bool addAfter, bool immediate, bool negate>
static void execFn_memop_load_store_2(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t op1, ea, memVal32, increment;
    bool ok;
    uint_fast8_t fsr, sourceReg, destReg;
    uint8_t memVal8;
    bool privileged = cpu->privileged;

    if (cpuPrvConditionFails(cpu, instr)) return;

    sourceReg = (instr >> 16) & 0x0F;
    if constexpr (mode & ARM_MODE_2_T) privileged = false;

    if constexpr (addBefore || addAfter) {
        if (immediate) {
            increment = instr & 0xFFFUL;
        } else {
            increment = cpuPrvGetRegNotPC(cpu, instr & 0x0F);
            uint8_t shift = (instr >> 7) & 0x1F;

            switch ((instr >> 5) & 3) {
                case 0:  // LSL
                    increment <<= shift;
                    break;

                case 1:  // LSR
                    increment = shift ? (increment >> shift) : 0;
                    break;

                case 2:  // ASR
                    if (!shift) shift = 31;
                    increment = (((int32_t)increment) >> shift);
                    break;

                case 3:  // ROR/RRX
                    if (shift)
                        increment = cpuPrvROR(increment, shift);
                    else {  // RRX
                        increment = increment >> 1;
                        increment |= ((cpu->flags & ARM_SR_C) << 2);
                    }
            }
        }

        if (negate) increment = -increment;
    }

    ea = cpuPrvGetReg<wasT, srcPc>(cpu, sourceReg);
    if constexpr (addBefore) ea += increment;

    if (mode & ARM_MODE_2_LOAD) {
        if (mode & ARM_MODE_2_WORD) {
            ok = cpuPrvMemOp<memorySystemKind, 4, false>(cpu, &memVal32, ea, privileged, &fsr);
            if (!ok) {
                cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, false, false, fsr);
                return;
            }

            if (ea & 3) memVal32 = cpuPrvROR(memVal32, (ea & 3) * 8);
            destReg = (instr >> 12) & 0x0F;

            // from RePalm:
            //
            // #define CALL_OSCALL(tab,num)	"LDR R12,[R9, #-" #tab "] \nLDR PC,[R12, #"
            // #num "]"
            if constexpr (mode & ARM_MODE_2_SYSCALL) {
                if constexpr (destPc)
                // syscall && destPc -> sourceReg == 9 && destReg == 12
                {
                    cpuPrvSetReg<destPc, memorySystemKind>(cpu, destReg, memVal32);
                    patchDispatchOnLoadPcFromR12(cpu->patchDispatch, addBefore ? increment : 0,
                                                 cpu->regs);
                } else {
                    // syscall && !destPc -> sourceReg == 12 && destReg == 15
                    cpuPrvSetReg<destPc, memorySystemKind>(cpu, destReg, memVal32);
                    patchDispatchOnLoadR12FromR9(cpu->patchDispatch, addBefore ? increment : 0);
                }
            } else {
                cpuPrvSetReg<destPc, memorySystemKind>(cpu, destReg, memVal32);
            }
        } else {
            ok = cpuPrvMemOp<memorySystemKind, 1, false>(cpu, &memVal8, ea, privileged, &fsr);
            if (!ok) {
                cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, false, false, fsr);
                return;
            }
            cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, memVal8);
        }
    } else {
        op1 = cpuPrvGetReg<wasT, destPc>(cpu, (instr >> 12) & 0x0F);
        if constexpr (!wasT && destPc && (mode & ARM_MODE_2_WORD))
            if (armKindIsArm920(memorySystemKind)) op1 = cpu->curInstrPC + 12;

        if (mode & ARM_MODE_2_WORD) {
            memVal32 = op1;
            ok = cpuPrvMemOp<memorySystemKind, 4, true>(cpu, &memVal32, ea, privileged, &fsr);
            if (!ok) {
                cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, true, false, fsr);
                return;
            }
        } else {
            memVal8 = op1;
            ok = cpuPrvMemOp<memorySystemKind, 1, true>(cpu, &memVal8, ea, privileged, &fsr);
            if (!ok) {
                cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, true, false, fsr);
                return;
            }
        }
    }

    if constexpr (addAfter) {
        if constexpr (addBefore)
            cpuPrvSetRegNotPC(cpu, sourceReg, ea);
        else
            cpuPrvSetRegNotPC(cpu, sourceReg, ea + increment);
    }
}

template <int memorySystemKind, bool wasT, int mode, bool isLoad, bool sBit>
static void execFn_memop_load_store_multi(struct ArmCpu *cpu, uint32_t instr) {
    if (cpuPrvConditionFails(cpu, instr)) return;

    bool userModeRegs = false, copySPSR = false, ok;
    uint32_t loadedPc = 0xfffffffful;
    uint_fast8_t fsr;
    uint_fast16_t regsList = instr & 0xffff;
    uint_fast8_t reg = (instr >> 16) & 0x0f;
    uint32_t memVal32, sr, ea;
    uint32_t origBaseRegVal = ea = cpuPrvGetRegNotPC(cpu, reg);
    // Every mode goes from the lowest register at the lowest address up, as the ARM920T and XScale do,
    // so DA and DB start at the bottom of the block; it matters for which words a transfer across a page
    // edge takes before the abort.
    constexpr bool up = mode & ARM_MODE_4_INC;
    constexpr bool before = up ? (mode & ARM_MODE_4_BFR) : !(mode & ARM_MODE_4_BFR);
    const uint32_t bottom = origBaseRegVal - 4 * uint32_t(__builtin_popcount(regsList));
    if (!up) ea = bottom;

    if (sBit &&
        !((cpu->M == ARM_SR_MODE_USR) || (cpu->M == ARM_SR_MODE_SYS))) {  // sort out what "S" means
        if (isLoad && (regsList & (1 << REG_NO_PC)))
            copySPSR = true;
        else
            userModeRegs = true;
    }

#define LOAD_STORE_MULTI_ITER(idx)                                                                 \
    do {                                                                                           \
        constexpr uint8_t regNo = idx;                                                             \
        if (!(regsList & (1 << regNo))) continue;                                                  \
                                                                                                   \
        if (!isLoad) {                                                                             \
            memVal32 = cpuPrvGetReg<wasT>(cpu, regNo);                                             \
            if (!wasT && regNo == REG_NO_PC && armKindIsArm920(memorySystemKind))                  \
                memVal32 = cpu->curInstrPC + 12;                                                   \
            if (unlikely(userModeRegs)) {                                                          \
                if (regNo >= 8 && regNo <= 12 && (cpu->M == ARM_SR_MODE_FIQ))                      \
                    memVal32 = cpu->extra_regs[regNo - 8];                                         \
                else if (regNo == REG_NO_SP)                                                       \
                    memVal32 = cpu->bank_usr.R13;                                                  \
                else if (regNo == REG_NO_LR)                                                       \
                    memVal32 = cpu->bank_usr.R14;                                                  \
            }                                                                                      \
        }                                                                                          \
                                                                                                   \
        if (before) ea += 4;                                                                       \
        ok = cpuPrvMemOp<memorySystemKind, 4, !isLoad>(cpu, &memVal32, ea, cpu->privileged, &fsr); \
        if (!ok) {                                                                                 \
            /* Put a loaded base back before the abort switches mode and bank. */                 \
            if (regsList & (1 << reg)) cpuPrvSetRegK<memorySystemKind>(cpu, reg, origBaseRegVal);                     \
            cpuPrvHandleMemErr<memorySystemKind>(cpu, ea, !isLoad, false, fsr);                                      \
                                                                                                   \
            return;                                                                                \
        }                                                                                          \
        if (!before) ea += 4;                                                                      \
                                                                                                   \
        if (isLoad) {                                                                              \
            if (unlikely(userModeRegs)) {                                                          \
                if (regNo >= 8 && regNo <= 12 && (cpu->M == ARM_SR_MODE_FIQ)) {                    \
                    cpu->extra_regs[regNo - 8] = memVal32;                                         \
                    continue;                                                                      \
                } else if (regNo == REG_NO_SP) {                                                   \
                    cpu->bank_usr.R13 = memVal32;                                                  \
                    continue;                                                                      \
                } else if (regNo == REG_NO_LR) {                                                   \
                    cpu->bank_usr.R14 = memVal32;                                                  \
                    continue;                                                                      \
                }                                                                                  \
            }                                                                                      \
                                                                                                   \
            if (unlikely(regNo == REG_NO_PC && copySPSR))                                          \
                loadedPc = memVal32;                                                               \
            else                                                                                   \
                cpuPrvSetRegK<memorySystemKind>(cpu, regNo, memVal32);                                                \
        }                                                                                          \
    } while (0)

    LOAD_STORE_MULTI_ITER(0);
    LOAD_STORE_MULTI_ITER(1);
    LOAD_STORE_MULTI_ITER(2);
    LOAD_STORE_MULTI_ITER(3);
    LOAD_STORE_MULTI_ITER(4);
    LOAD_STORE_MULTI_ITER(5);
    LOAD_STORE_MULTI_ITER(6);
    LOAD_STORE_MULTI_ITER(7);
    LOAD_STORE_MULTI_ITER(8);
    LOAD_STORE_MULTI_ITER(9);
    LOAD_STORE_MULTI_ITER(10);
    LOAD_STORE_MULTI_ITER(11);
    LOAD_STORE_MULTI_ITER(12);
    LOAD_STORE_MULTI_ITER(13);
    LOAD_STORE_MULTI_ITER(14);
    LOAD_STORE_MULTI_ITER(15);

#undef LOAD_STORE_MULTI_ITER

    if (mode & ARM_MODE_4_WBK) cpuPrvSetRegNotPC(cpu, reg, up ? ea : bottom);
    if (copySPSR) {
        if (armKindIsArm920(memorySystemKind)) icacheArm920BranchInline(cpu->ic);
        sr = cpu->SPSR;
        cpuPrvSetPSRlo8<memorySystemKind>(cpu, sr);
        cpuPrvSetPSRhi8(cpu, sr);
        cpu->regs[REG_NO_PC] = loadedPc;  // direct write - yes
        if (cpu->T)
            cpu->regs[REG_NO_PC] &= ~1;
        else
            cpu->regs[REG_NO_PC] &= ~3;
    }
}

template <int memorySystemKind, bool wasT, bool link>
static void execFn_bl(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t ea;

    if (cpuPrvConditionFails(cpu, instr)) return;

    ea = instr << 8;
    ea = ((int32_t)ea) >> 7;
    ea += 4;
    if (!wasT) ea <<= 1;
    ea += cpu->curInstrPC;

    if (instr & 0x01000000UL) cpu->regs[REG_NO_LR] = cpu->curInstrPC + (wasT ? 2 : 4);
    if (cpu->T) ea |= 1UL;  // keep T flag as needed

    cpuPrvSetPC<memorySystemKind>(cpu, ea);
}

template <int memorySystemKind, bool wasT>
static void execFn_swi(struct ArmCpu *cpu, uint32_t instr) {
    uint_fast8_t fsr;

    if (cpuPrvConditionFails(cpu, instr)) return;

    // ARM920T always enters the guest SWI vector, for every immediate and operation.
    if (!armKindIsArm920(memorySystemKind) &&
        ((wasT && (instr & 0x00fffffful) == 0xab) ||
         (!wasT && (instr & 0x00fffffful) == 0x123456ul))) {
        if (cpu->regs[0] == 4) {
            uint32_t addr = cpu->regs[1];
            uint8_t ch;

            while (cpuPrvMemOp<memorySystemKind, 1, false>(cpu, &ch, addr++, true, &fsr) && ch)
                fprintf(stderr, "%c", ch);

            return;
        } else if (cpu->regs[0] == 3) {
            uint8_t ch;

            if (cpuPrvMemOp<memorySystemKind, 1, false>(cpu, &ch, cpu->regs[1], true, &fsr) && ch)
                fprintf(stderr, "%c", ch);

            return;
        } else if (cpu->regs[0] == 0x132 && gdbStubEnabled(cpu->debugStub)) {
#ifndef __EMSCRIPTEN__
            fprintf(stderr, "debug break requested\n");
#endif
            gdbStubDebugBreakRequested(cpu->debugStub);

            return;
        }
    }

    cpuPrvException<memorySystemKind>(cpu, cpu->vectorBase + ARM_VECTOR_OFFT_SWI, cpu->curInstrPC + (wasT ? 2 : 4),
                    ARM_SR_MODE_SVC | ARM_SR_I);
}

template <int memorySystemKind>
static void execFn_peephole_ADC_udivmod(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t before[16];
    std::memcpy(before, cpu->regs, sizeof(before));
    const uint32_t num = cpuPrvGetRegNotPC(cpu, 1);
    const uint32_t den = cpuPrvGetRegNotPC(cpu, 0);

    if (den == 0) {
        // mov r2, #0
        execFn_dproc<memorySystemKind, false, 13, false, false, false, 1>(
            cpu, FIRST_INSTRUCTION_ADS_UDIVMOD);
        return;
    }

    cpuPrvSetRegNotPC(cpu, 0, num / den);
    cpuPrvSetRegNotPC(cpu, 1, num % den);

    cpuPrvSetPC<memorySystemKind>(cpu, cpuPrvGetRegNotPC(cpu, REG_NO_LR));
    cpuPrvPeepholeCharge<memorySystemKind>(cpu, instr, before);
}

template <int memorySystemKind>
static void execFn_peephole_ADC_sdivmod(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t before[16];
    std::memcpy(before, cpu->regs, sizeof(before));
    const int32_t num = cpuPrvGetRegNotPC(cpu, 1);
    const int32_t den = cpuPrvGetRegNotPC(cpu, 0);

    if (den == 0) {
        // ands r2, r0, #128, #8
        execFn_dproc<memorySystemKind, false, 0, true, false, false, 1>(
            cpu, FIRST_INSTRUCTION_ADS_SDIVMOD);
        return;
    }

    cpuPrvSetRegNotPC(cpu, 0, num / den);
    cpuPrvSetRegNotPC(cpu, 1, num % den);

    cpuPrvSetPC<memorySystemKind>(cpu, cpuPrvGetRegNotPC(cpu, REG_NO_LR));
    cpuPrvPeepholeCharge<memorySystemKind>(cpu, instr, before);
}

template <int memorySystemKind, bool wasT>
static ExecFn ATTR_EMCC_NOINLINE cpuPrvDecoderArm(uint32_t instr) {
    if ((instr >> 28) == 0x0f) {
        switch ((instr >> 24) & 0x0f) {
            case 5:
            case 7:
                return (instr & 0x0D70F000UL) == 0x0550F000UL
                           ? PREFIX_EXEC_FN(execFn_noop<memorySystemKind, wasT>)
                           : PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);

            case 10:
            case 11:
                return (instr & 0x01000000UL)
                           ? PREFIX_EXEC_FN(execFn_b2thumb<memorySystemKind, wasT, true>)
                           : PREFIX_EXEC_FN(execFn_b2thumb<memorySystemKind, wasT, false>);

            case 12:
            case 13:
                return PREFIX_EXEC_FN(execFn_cp_mem2reg<memorySystemKind, wasT, true>);

            case 14:
                return PREFIX_EXEC_FN(execFn_cp_dp<memorySystemKind, wasT, true>);
        }

        if (!wasT) {
            switch (instr) {
                case INSTR_PACE_ENTER:
                    return PREFIX_EXEC_FN(execFn_paceEnter<memorySystemKind>);

                case INSTR_PACE_RESUME:
                    return PREFIX_EXEC_FN(execFn_paceResume);

                case INSTR_PACE_RETURN_FROM_CALLOUT:
                    return PREFIX_EXEC_FN(execFn_paceReturnFromCallout<memorySystemKind>);

                case INSTR_TG50_LIBRARY_REFNUM:
                    return PREFIX_EXEC_FN(execFn_tg50LibraryRefnum<memorySystemKind>);

                case INSTR_PACE_DISPATCH:
                    return PREFIX_EXEC_FN(execFn_paceDispatch<memorySystemKind>);

                case INSTR_PEEPHOLE_ADS_UDIVMOD:
                    return PREFIX_EXEC_FN(execFn_peephole_ADC_udivmod<memorySystemKind>);

                case INSTR_PEEPHOLE_ADS_SDIVMOD:
                    return PREFIX_EXEC_FN(execFn_peephole_ADC_sdivmod<memorySystemKind>);

                case INSTR_PEEPHOLE_ADS_UDIV10:
                    return PREFIX_EXEC_FN(execFn_peephole_ADC_udiv10);

                case INSTR_PEEPHOLE_ADS_SDIV10:
                    return PREFIX_EXEC_FN(execFn_peephole_ADC_sdiv10);

                case INSTR_PEEPHOLE_ADS_MEMCPY:
                    return PREFIX_EXEC_FN(execFn_peephole_ADC_memcpy<memorySystemKind>);
            }
        }

        return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);
    }

    switch ((instr >> 24) & 0x0F) {
        case 0:
        case 1:
            if ((instr & 0x00000090UL) == 0x00000090) {  // multiplies, extra load/stores
                                                         // (table 3.2)

                if ((instr & 0x00000060UL) == 0x00000000) {  // swp[b], mult(acc), mult(acc) long

                    if (instr & 0x01000000UL) {  // SWB/SWPB

                        switch ((instr >> 20) & 0x0F) {
                            case 0:  // SWP
                                return PREFIX_EXEC_FN(execFn_swb<memorySystemKind, wasT, 0>);

                            case 4:  // SWPB
                                return PREFIX_EXEC_FN(execFn_swb<memorySystemKind, wasT, 4>);

                            default:
                                return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);
                        }

                    } else {
                        const bool flags = instr & 0x00100000UL;

#define EXEC_MULT(tag)                                                            \
    return flags ? PREFIX_EXEC_FN(execFn_mult<memorySystemKind, wasT, tag, true>) \
                 : PREFIX_EXEC_FN(execFn_mult<memorySystemKind, wasT, tag, false>);

                        switch ((instr >> 20) & 0x0F) {  // multiplies

                            case 0:  // MUL
                            case 1:
                                EXEC_MULT(0);

                            case 2:  // MLA
                            case 3:
                                EXEC_MULT(2);

                            case 8:  // UMULL
                            case 9:
                            case 12:  // SMULL
                            case 13:
                                EXEC_MULT(8);

                            case 10:  // UMLAL
                            case 11:
                            case 14:  // SMLAL
                            case 15:
                                EXEC_MULT(10);

                            default:
                                return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);
                        }

#undef EXEC_MULT
                    }
                } else {  // load/store signed/unsigned byte/halfword/two_words
                    uint_fast8_t mode = cpuPrvArmAdrModeDecode_3(instr);
                    const bool pc = ((instr >> 16) & 0x0f) == REG_NO_PC;
                    const bool addAfter = !(instr & 0x01000000UL) || (instr & 0x00200000UL);
                    const bool addBefore = (instr & 0x01000000UL);
                    const bool negate = !(instr & 0x00800000UL);
                    const bool immediate = instr & 0x00400000UL;

                    if (mode & ARM_MODE_2_INV)
                        return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);

#define EXEC_LOAD_STORE_1_IMMEDIATE(mode, pc, addBefore, addAfter, immediate)                 \
    (negate ? PREFIX_EXEC_FN(execFn_load_store_1<memorySystemKind, wasT, mode, pc, addBefore, \
                                                 addAfter, immediate, true>)                  \
            : PREFIX_EXEC_FN(execFn_load_store_1<memorySystemKind, wasT, mode, pc, addBefore, \
                                                 addAfter, immediate, false>))

#define EXEC_LOAD_STORE_1_ADD_AFTER(mode, pc, addBefore, addAfter)                         \
    ((addBefore || addAfter)                                                               \
         ? (immediate ? EXEC_LOAD_STORE_1_IMMEDIATE(mode, pc, addBefore, addAfter, true)   \
                      : EXEC_LOAD_STORE_1_IMMEDIATE(mode, pc, addBefore, addAfter, false)) \
         : PREFIX_EXEC_FN(execFn_load_store_1<memorySystemKind, wasT, mode, pc, addBefore, \
                                              addAfter, false, false>))

#define EXEC_LOAD_STORE_1_ADD_BEFORE(mode, pc, addBefore)              \
    (addAfter ? EXEC_LOAD_STORE_1_ADD_AFTER(mode, pc, addBefore, true) \
              : EXEC_LOAD_STORE_1_ADD_AFTER(mode, pc, addBefore, false))

#define EXEC_LOAD_STORE_1_PC(mode, pc)                        \
    (addBefore ? EXEC_LOAD_STORE_1_ADD_BEFORE(mode, pc, true) \
               : EXEC_LOAD_STORE_1_ADD_BEFORE(mode, pc, false))

#define EXEC_LOAD_STORE_1(mode) \
    return (pc ? EXEC_LOAD_STORE_1_PC(mode, true) : EXEC_LOAD_STORE_1_PC(mode, false))

                    if (mode & ARM_MODE_3_LOAD) {
                        switch (mode & ARM_MODE_3_TYPE) {
                            case ARM_MODE_3_H:
                                EXEC_LOAD_STORE_1(ARM_MODE_3_LOAD | ARM_MODE_3_H);

                            case ARM_MODE_3_SH:
                                EXEC_LOAD_STORE_1(ARM_MODE_3_LOAD | ARM_MODE_3_SH);

                            case ARM_MODE_3_SB:
                                EXEC_LOAD_STORE_1(ARM_MODE_3_LOAD | ARM_MODE_3_SB);

                            case ARM_MODE_3_D:
                                EXEC_LOAD_STORE_1(ARM_MODE_3_LOAD | ARM_MODE_3_D);
                        }
                    } else {
                        switch (mode & ARM_MODE_3_TYPE) {
                            case ARM_MODE_3_H:
                                if (((instr >> 12) & 0x0f) == REG_NO_PC)
                                    EXEC_LOAD_STORE_1(ARM_MODE_3_H | ARM_MDOE_3_PCREL);
                                else
                                    EXEC_LOAD_STORE_1(ARM_MODE_3_H);

                            case ARM_MODE_3_SH:
                            case ARM_MODE_3_SB:
                                return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);

                            case ARM_MODE_3_D:
                                EXEC_LOAD_STORE_1(ARM_MODE_3_D);
                        }
                    }
#undef EXEC_LOAD_STORE_1_ADD_BEFORE
#undef EXEC_LOAD_STORE_1_ADD_AFTER
#undef EXEC_LOAD_STORE_1_IMMEDIATE
#undef EXEC_LOAD_STORE_1_PC
#undef EXEC_LOAD_STORE_1
                }
            } else if ((instr & 0x01900000UL) == 0x01000000UL) {  // misc instrs (table 3.3)
                switch ((instr >> 4) & 0x0F) {
                    case 0:  // move reg to PSR or move PSR to reg

                        if ((instr & 0x00BF0FFFUL) == 0x000F0000UL) {  // move PSR to reg
                            return (instr & 0x00400000UL)
                                       ? PREFIX_EXEC_FN(
                                             execFn_psr2reg<memorySystemKind, wasT, true>)
                                       : PREFIX_EXEC_FN(
                                             execFn_psr2reg<memorySystemKind, wasT, false>);
                        } else if ((instr & 0x00B0FFF0UL) == 0x0020F000UL) {  // move reg to PSR
                            if ((instr & 0x0f) == 0x0f)
                                return (instr & 0x00400000UL)
                                           ? PREFIX_EXEC_FN(
                                                 execFn_reg2psr<memorySystemKind, wasT, true, true>)
                                           : PREFIX_EXEC_FN(execFn_reg2psr<memorySystemKind, wasT,
                                                                           false, true>);
                            else
                                return (instr & 0x00400000UL)
                                           ? PREFIX_EXEC_FN(execFn_reg2psr<memorySystemKind, wasT,
                                                                           true, false>)
                                           : PREFIX_EXEC_FN(execFn_reg2psr<memorySystemKind, wasT,
                                                                           false, false>);

                        } else
                            return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);

                    case 5:  // enhanced DSP adds/subtracts
                        if (instr & 0x00000F00UL)
                            return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);

                        switch ((instr >> 21) & 3) {  // what op?
                            case 0:                   // QADD
                                return PREFIX_EXEC_FN(execFn_dspadd<memorySystemKind, wasT, 0>);

                            case 1:  // QSUB
                                return PREFIX_EXEC_FN(execFn_dspadd<memorySystemKind, wasT, 1>);

                            case 2:  // QDADD
                                return PREFIX_EXEC_FN(execFn_dspadd<memorySystemKind, wasT, 2>);

                            case 3:  // QDSUB
                                return PREFIX_EXEC_FN(execFn_dspadd<memorySystemKind, wasT, 3>);

                            default:
                                __builtin_unreachable();
                        }

                    case 7:  // soft breakpoint
                        return PREFIX_EXEC_FN(execFn_softbreak<memorySystemKind, wasT>);

                    case 8:
                    case 9:
                    case 10:
                    case 11:
                    case 12:
                    case 13:
                    case 14:
                    case 15:
                        switch ((instr >> 21) & 3) {  // what op?
                            case 0:                   // SMLAxy
                                return PREFIX_EXEC_FN(execFn_dspmul<memorySystemKind, wasT, 0>);

                            case 1:  // SMLAWy/SMULWy
                                if ((instr & 0x00000020UL) && (instr & 0x0000F000UL))
                                    return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);

                                return PREFIX_EXEC_FN(execFn_dspmul<memorySystemKind, wasT, 1>);

                            case 2:  // SMLALxy
                                return PREFIX_EXEC_FN(execFn_dspmul<memorySystemKind, wasT, 2>);

                            case 3:  // SMULxy
                                if (instr & 0x0000F000UL)
                                    return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);
                                return PREFIX_EXEC_FN(execFn_dspmul<memorySystemKind, wasT, 3>);

                            default:
                                __builtin_unreachable();
                        }

                    case 1:  // BLX/BX/BXJ or CLZ
                    case 3:
                        if (instr & 0x00400000UL) {  // CLZ
                            return PREFIX_EXEC_FN(execFn_clz<memorySystemKind, wasT>);
                        } else {
                            if ((instr & 0x0FFFFF00UL) != 0x012FFF00UL)
                                return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);

                            return (instr & 0x00000030UL) == 0x00000030UL
                                       ? PREFIX_EXEC_FN(execFn_bl_reg<memorySystemKind, wasT, true>)
                                       : PREFIX_EXEC_FN(
                                             execFn_bl_reg<memorySystemKind, wasT, false>);
                        }

                    default:
                        return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);
                }

                // fall through to data processing;
            }

            [[fallthrough]];

        case 2:
        case 3:  // data process immediate val, move imm to SR
        {
            const bool setFlags = !!(instr & 0x00100000UL);
            const bool srcPc = ((instr >> 16) & 0x0F) == 0x0f;
            const bool destPc = ((instr >> 12) & 0x0F) == 0x0f;

#define EXEC_DPROC(op)                                                                          \
    if (setFlags) {                                                                             \
        if (srcPc)                                                                              \
            return destPc ? PREFIX_EXEC_FN(                                                     \
                                execFn_dproc<memorySystemKind, wasT, op, true, true, true>)     \
                          : PREFIX_EXEC_FN(                                                     \
                                execFn_dproc<memorySystemKind, wasT, op, true, true, false>);   \
        else                                                                                    \
            return destPc ? PREFIX_EXEC_FN(                                                     \
                                execFn_dproc<memorySystemKind, wasT, op, true, false, true>)    \
                          : PREFIX_EXEC_FN(                                                     \
                                execFn_dproc<memorySystemKind, wasT, op, true, false, false>);  \
    } else {                                                                                    \
        if (srcPc)                                                                              \
            return destPc ? PREFIX_EXEC_FN(                                                     \
                                execFn_dproc<memorySystemKind, wasT, op, false, true, true>)    \
                          : PREFIX_EXEC_FN(                                                     \
                                execFn_dproc<memorySystemKind, wasT, op, false, true, false>);  \
        else                                                                                    \
            return destPc ? PREFIX_EXEC_FN(                                                     \
                                execFn_dproc<memorySystemKind, wasT, op, false, false, true>)   \
                          : PREFIX_EXEC_FN(                                                     \
                                execFn_dproc<memorySystemKind, wasT, op, false, false, false>); \
    }

            switch ((instr >> 21) & 0x0F) {
                case 0:  // AND
                    EXEC_DPROC(0);

                case 1:  // EOR
                    EXEC_DPROC(1);

                case 2:  // SUB
                    EXEC_DPROC(2);

                case 3:  // RSB
                    EXEC_DPROC(3);

                case 4:  // ADD
                    EXEC_DPROC(4);

                case 5:  // ADC
                    EXEC_DPROC(5);

                case 6:  // SBC
                    EXEC_DPROC(6);

                case 7:  // RSC
                    EXEC_DPROC(7);

                case 8:  // TST
                    if (!setFlags) return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);
                    EXEC_DPROC(8);

                case 9:  // TEQ
                    EXEC_DPROC(9);

                case 10:  // CMP
                    if (!setFlags) return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);
                    EXEC_DPROC(10);

                case 11:  // CMN
                    EXEC_DPROC(11);

                case 12:  // ORR
                    EXEC_DPROC(12);

                case 13:  // MOV
                    EXEC_DPROC(13);

                case 14:  // BIC
                    EXEC_DPROC(14);

                case 15:  // MVN
                    EXEC_DPROC(15);

                default:
                    __builtin_unreachable();
            }
#undef EXEC_DPROC
        } break;

        case 4:
        case 5:  // load/store imm offset

            goto load_store_mode_2;
            break;

        case 6:
        case 7:                        // load/store reg offset
            if (instr & 0x00000010UL)  // media and undefined instrs
                return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);

        load_store_mode_2: {
            const uint_fast8_t mode = cpuPrvArmAdrModeDecode_2(instr);
            const uint8_t regSrc = (instr >> 16) & 0x0f;
            const uint8_t regDst = (instr >> 12) & 0x0f;
            const bool srcPc = regSrc == 15;
            const bool destPc = regDst == 15;
            const bool addAfter = !(instr & 0x01000000UL) || (instr & 0x00200000UL);
            const bool addBefore = (instr & 0x01000000UL);
            const bool negate = !(instr & 0x00800000UL);
            const bool immediate = !(instr & 0x02000000UL);
            const bool syscall = (regSrc == 9 && regDst == 12) || (regSrc == 12 && destPc);

#define EXEC_LOAD_STORE_2_IMMEDIATE(mode, srcPc, destPc, addBefore, addAfter, immediate)         \
    (negate                                                                                      \
         ? PREFIX_EXEC_FN(execFn_memop_load_store_2<memorySystemKind, wasT, mode, srcPc, destPc, \
                                                    addBefore, addAfter, immediate, true>)       \
         : PREFIX_EXEC_FN(execFn_memop_load_store_2<memorySystemKind, wasT, mode, srcPc, destPc, \
                                                    addBefore, addAfter, immediate, false>))

#define EXEC_LOAD_STORE_2_ADD_AFTER(mode, srcPc, destPc, addBefore, addAfter)                     \
    ((addBefore || addAfter)                                                                      \
         ? (immediate                                                                             \
                ? EXEC_LOAD_STORE_2_IMMEDIATE(mode, srcPc, destPc, addBefore, addAfter, true)     \
                : EXEC_LOAD_STORE_2_IMMEDIATE(mode, srcPc, destPc, addBefore, addAfter, false))   \
         : (PREFIX_EXEC_FN(execFn_memop_load_store_2<memorySystemKind, wasT, mode, srcPc, destPc, \
                                                     addBefore, addAfter, false, false>)))

#define EXEC_LOAD_STORE_2_ADD_BEFORE(mode, srcPc, destPc, addBefore)              \
    (addAfter ? EXEC_LOAD_STORE_2_ADD_AFTER(mode, srcPc, destPc, addBefore, true) \
              : EXEC_LOAD_STORE_2_ADD_AFTER(mode, srcPc, destPc, addBefore, false))

#define EXEC_LOAD_STORE_2_DEST_PC(mode, srcPc, destPc)                   \
    (addBefore ? EXEC_LOAD_STORE_2_ADD_BEFORE(mode, srcPc, destPc, true) \
               : EXEC_LOAD_STORE_2_ADD_BEFORE(mode, srcPc, destPc, false))

#define EXEC_LOAD_STORE_2_SRC_PC(mode, srcPc)              \
    (destPc ? EXEC_LOAD_STORE_2_DEST_PC(mode, srcPc, true) \
            : EXEC_LOAD_STORE_2_DEST_PC(mode, srcPc, false))

#define EXEC_LOAD_STORE_2(mode) \
    return (srcPc ? EXEC_LOAD_STORE_2_SRC_PC(mode, true) : EXEC_LOAD_STORE_2_SRC_PC(mode, false))

            if (mode & ARM_MODE_2_INV)
                return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);

            if (mode & ARM_MODE_2_LOAD) {
                if (mode & ARM_MODE_2_WORD) {
                    if (mode & ARM_MODE_2_T) {
                        if (syscall)
                            EXEC_LOAD_STORE_2(ARM_MODE_2_LOAD | ARM_MODE_2_WORD | ARM_MODE_2_T |
                                              ARM_MODE_2_SYSCALL);
                        else
                            EXEC_LOAD_STORE_2(ARM_MODE_2_LOAD | ARM_MODE_2_WORD | ARM_MODE_2_T);
                    } else {
                        if (syscall)
                            EXEC_LOAD_STORE_2(ARM_MODE_2_LOAD | ARM_MODE_2_WORD |
                                              ARM_MODE_2_SYSCALL);
                        else
                            EXEC_LOAD_STORE_2(ARM_MODE_2_LOAD | ARM_MODE_2_WORD);
                    }
                } else {
                    if (mode & ARM_MODE_2_T) {
                        EXEC_LOAD_STORE_2(ARM_MODE_2_LOAD | ARM_MODE_2_T);
                    } else {
                        EXEC_LOAD_STORE_2(ARM_MODE_2_LOAD);
                    }
                }
            } else {
                if (mode & ARM_MODE_2_WORD) {
                    if (mode & ARM_MODE_2_T) {
                        EXEC_LOAD_STORE_2(ARM_MODE_2_WORD | ARM_MODE_2_T);
                    } else {
                        EXEC_LOAD_STORE_2(ARM_MODE_2_WORD);
                    }
                } else {
                    if (mode & ARM_MODE_2_T) {
                        EXEC_LOAD_STORE_2(ARM_MODE_2_T);
                    } else {
                        EXEC_LOAD_STORE_2(0);
                    }
                }
            }
#undef EXEC_LOAD_STORE_2_IMMEDIATE
#undef EXEC_LOAD_STORE_2_SHIFT_MODE
#undef EXEC_LOAD_STORE_2_ADD_AFTER
#undef EXEC_LOAD_STORE_2_ADD_BEFORE
#undef EXEC_LOAD_STORE_2_DEST_PC
#undef EXEC_LOAD_STORE_2_SRC_PC
#undef EXEC_LOAD_STORE_2
        }

#define EXEC_LOAD_STORE_MULTI(wasT, mode, isLoad)                                         \
    ((instr & 0x00400000UL)                                                               \
         ? PREFIX_EXEC_FN(                                                                \
               execFn_memop_load_store_multi<memorySystemKind, wasT, mode, isLoad, true>) \
         : PREFIX_EXEC_FN(                                                                \
               execFn_memop_load_store_multi<memorySystemKind, wasT, mode, isLoad, false>))

        case 8:
        case 9:  // load/store multiple
        {
            const uint_fast8_t mode = cpuPrvArmAdrModeDecode_4(instr);
            const bool isLoad = !!(instr & 0x00100000UL);

            if (mode & ARM_MODE_4_WBK) {
                if (mode & ARM_MODE_4_INC) {
                    if (mode & ARM_MODE_4_BFR)
                        return isLoad ? EXEC_LOAD_STORE_MULTI(
                                            wasT, ARM_MODE_4_WBK | ARM_MODE_4_INC | ARM_MODE_4_BFR,
                                            true)
                                      : EXEC_LOAD_STORE_MULTI(
                                            wasT, ARM_MODE_4_WBK | ARM_MODE_4_INC | ARM_MODE_4_BFR,
                                            false);
                    else
                        return isLoad ? EXEC_LOAD_STORE_MULTI(wasT, ARM_MODE_4_WBK | ARM_MODE_4_INC,
                                                              true)
                                      : EXEC_LOAD_STORE_MULTI(wasT, ARM_MODE_4_WBK | ARM_MODE_4_INC,
                                                              false);
                } else {
                    if (mode & ARM_MODE_4_BFR)
                        return isLoad ? EXEC_LOAD_STORE_MULTI(wasT, ARM_MODE_4_WBK | ARM_MODE_4_BFR,
                                                              true)
                                      : EXEC_LOAD_STORE_MULTI(wasT, ARM_MODE_4_WBK | ARM_MODE_4_BFR,
                                                              false);
                    else
                        return isLoad ? EXEC_LOAD_STORE_MULTI(wasT, ARM_MODE_4_WBK, true)
                                      : EXEC_LOAD_STORE_MULTI(wasT, ARM_MODE_4_WBK, false);
                }
            } else {
                if (mode & ARM_MODE_4_INC) {
                    if (mode & ARM_MODE_4_BFR)
                        return isLoad ? EXEC_LOAD_STORE_MULTI(wasT, ARM_MODE_4_INC | ARM_MODE_4_BFR,
                                                              true)
                                      : EXEC_LOAD_STORE_MULTI(wasT, ARM_MODE_4_INC | ARM_MODE_4_BFR,
                                                              false);
                    else
                        return isLoad ? EXEC_LOAD_STORE_MULTI(wasT, ARM_MODE_4_INC, true)
                                      : EXEC_LOAD_STORE_MULTI(wasT, ARM_MODE_4_INC, false);
                } else {
                    if (mode & ARM_MODE_4_BFR)
                        return isLoad ? EXEC_LOAD_STORE_MULTI(wasT, ARM_MODE_4_BFR, true)
                                      : EXEC_LOAD_STORE_MULTI(wasT, ARM_MODE_4_BFR, false);
                    else
                        return isLoad ? EXEC_LOAD_STORE_MULTI(wasT, 0, true)
                                      : EXEC_LOAD_STORE_MULTI(wasT, 0, false);
                }
            }
        }
#undef EXEC_LOAD_STORE_MULTI

        case 10:
        case 11:  // B/BL/BLX(if cond=0b1111)
            return (instr & 0x01000000UL)
                       ? PREFIX_EXEC_FN(execFn_bl<memorySystemKind, wasT, true>)
                       : PREFIX_EXEC_FN(execFn_bl<memorySystemKind, wasT, false>);

        case 12:
        case 13:  // coprocessor load/store and double register transfers
                  // coproc_mem_2reg:
            return PREFIX_EXEC_FN(execFn_cp_mem2reg<memorySystemKind, wasT, false>);

        case 14:  // coprocessor data processing and register transfers
            // coproc_dp:
            return PREFIX_EXEC_FN(execFn_cp_dp<memorySystemKind, wasT, false>);

        case 15:  // SWI
            return PREFIX_EXEC_FN(execFn_swi<memorySystemKind, wasT>);
    }

    return PREFIX_EXEC_FN(execFn_invalid<memorySystemKind, wasT>);
}

template <int memorySystemKind>
static void execFn_thumb_3_ldr(struct ArmCpu *cpu, uint32_t instr) {
    const uint32_t pc = cpuPrvGetReg<true>(cpu, REG_NO_PC) & ~0x03UL;
    const uint32_t addr = pc + ((instr & 0xff) << 2);
    uint32_t memVal32;
    uint_fast8_t fsr;

    bool ok = cpuPrvMemOp<memorySystemKind, 4, false>(cpu, &memVal32, addr, cpu->privileged, &fsr);
    if (!ok)
        cpuPrvHandleMemErr<memorySystemKind>(cpu, addr, false, false, fsr);
    else
        cpuPrvSetRegNotPC(cpu, (instr >> 8) & 0x07, memVal32);
}

template <int memorySystemKind, bool pcD, bool pcS>
static void execFn_thumb_4_add(struct ArmCpu *cpu, uint32_t instr) {
    uint_fast8_t vD = (instr & 7) | ((instr >> 4) & 0x08);
    uint_fast8_t v8 = (instr >> 3) & 0xF;

    uint32_t v32 = cpuPrvGetReg<true, pcD>(cpu, vD) + cpuPrvGetReg<true, pcS>(cpu, v8);
    if constexpr (pcD) v32 |= 1;

    cpuPrvSetReg<pcD, memorySystemKind>(cpu, vD, v32);
}

template <int memorySystemKind, bool pcD, bool pcS>
static void execFn_thumb_3_mov(struct ArmCpu *cpu, uint32_t instr) {
    uint_fast8_t vD = (instr & 7) | ((instr >> 4) & 0x08);
    uint_fast8_t v8 = (instr >> 3) & 0xF;

    uint32_t v32 = cpuPrvGetReg<true, pcS>(cpu, v8);
    if constexpr (pcD) v32 |= 1;

    cpuPrvSetReg<pcD, memorySystemKind>(cpu, vD, v32);
}

template <int memorySystemKind, bool blx, bool pcS>
static void execFn_thumb_bl_x(struct ArmCpu *cpu, uint32_t instr) {
    if (blx && armKindIsArm920(memorySystemKind)) return execFn_invalid<memorySystemKind, true>(cpu, instr);

    if constexpr (blx) cpu->regs[REG_NO_LR] = cpu->regs[REG_NO_PC] + 1;

    if constexpr (pcS)
        cpuPrvSetPC<memorySystemKind>(cpu, (cpu->regs[REG_NO_PC] + 2) & ~3UL);
    else
        cpuPrvSetPC<memorySystemKind>(cpu, cpu->regs[(instr >> 3) & 0xF]);
}

template <int memorySystemKind, bool sp>
static void execFn_thumb_load_addr(struct ArmCpu *cpu, uint32_t instr) {
    cpuPrvSetReg<false, memorySystemKind>(
        cpu, (instr >> 8) & 0x07,
        (sp ? cpuPrvGetRegNotPC(cpu, REG_NO_SP) : (cpuPrvGetReg<true>(cpu, REG_NO_PC) & ~0x03UL)) +
            ((instr & 0xff) << 2));
}

template <int memorySystemKind>
static void execFn_thumb_bl_x_prefix(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t offsetHi = instr & 0x7FF;

    if (instr & 0x0400) offsetHi |= 0x000FF800UL;
    cpu->regs[REG_NO_LR] = cpu->regs[REG_NO_PC] + 2 + (offsetHi << 12);
}

template <int memorySystemKind>
static void execFn_thumb_bl_suffix(struct ArmCpu *cpu, uint32_t instr) {
    const uint32_t lr = cpu->regs[REG_NO_PC] | 1ul;
    const uint32_t offsetLo = instr & 0x7FF;

    cpu->regs[REG_NO_PC] = cpu->regs[REG_NO_LR] + (offsetLo << 1);
    cpu->regs[REG_NO_LR] = lr;
    if (unlikely(cpu->earlyAudioHook!=nullptr) &&
        (cpu->curInstrPC==cpu->earlyAudioFrom[0] || cpu->curInstrPC==cpu->earlyAudioFrom[1]))
        cpu->earlyAudioHook(cpu,cpu->earlyAudioContext,cpu->regs[REG_NO_PC]|1u,cpu->curInstrPC);
}

template <int memorySystemKind>
static void execFn_thumb_blx_suffix(struct ArmCpu *cpu, uint32_t instr) {
    if (armKindIsArm920(memorySystemKind)) return execFn_invalid<memorySystemKind, true>(cpu, instr);

    const uint32_t lr = cpu->regs[REG_NO_PC] | 1ul;
    const uint32_t offsetLo = instr & 0x7FF;

    if (cpu->T) cpuSetSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE);

    cpu->regs[REG_NO_PC] = (cpu->regs[REG_NO_LR] + (offsetLo << 1)) & ~3ul;
    cpu->regs[REG_NO_LR] = lr;
    cpu->T = 0;
}

template <int memorySystemKind>
static ExecFn ATTR_EMCC_NOINLINE cpuPrvDecoderThumb(uint32_t instr) {
    switch (instr >> 12) {
        case 4:  // LDR(3) ADD(4) CMP(3) MOV(3) BX MVN CMP(2) CMN TST ADC SBC NEG MUL LSL(2)
                 // LSR(2) ASR(2) ROR AND EOR ORR BIC

            if (instr & 0x0800) {  // LDR(3)
                return PREFIX_EXEC_FN(execFn_thumb_3_ldr<memorySystemKind>);
            } else if (instr & 0x0400) {
                const bool pcD = ((instr & 7) | ((instr >> 4) & 0x08)) == 15;
                const bool pcS = ((instr >> 3) & 0xF) == 15;
                const bool blx = instr & 0x80;

                switch ((instr >> 8) & 0x03) {
                    case 0:
                        return (
                            pcS ? (pcD ? PREFIX_EXEC_FN(
                                             execFn_thumb_4_add<memorySystemKind, true, true>)
                                       : PREFIX_EXEC_FN(
                                             execFn_thumb_4_add<memorySystemKind, false, true>))
                                : (pcD ? PREFIX_EXEC_FN(
                                             execFn_thumb_4_add<memorySystemKind, true, false>)
                                       : PREFIX_EXEC_FN(
                                             execFn_thumb_4_add<memorySystemKind, false, false>)));

                    case 2:
                        return (
                            pcS ? (pcD ? PREFIX_EXEC_FN(
                                             execFn_thumb_3_mov<memorySystemKind, true, true>)
                                       : PREFIX_EXEC_FN(
                                             execFn_thumb_3_mov<memorySystemKind, false, true>))
                                : (pcD ? PREFIX_EXEC_FN(
                                             execFn_thumb_3_mov<memorySystemKind, true, false>)
                                       : PREFIX_EXEC_FN(
                                             execFn_thumb_3_mov<memorySystemKind, false, false>)));

                    case 3:
                        return blx ? (pcS ? PREFIX_EXEC_FN(
                                                execFn_thumb_bl_x<memorySystemKind, true, true>)
                                          : PREFIX_EXEC_FN(
                                                execFn_thumb_bl_x<memorySystemKind, true, false>))
                                   : (pcS ? PREFIX_EXEC_FN(
                                                execFn_thumb_bl_x<memorySystemKind, false, true>)
                                          : PREFIX_EXEC_FN(
                                                execFn_thumb_bl_x<memorySystemKind, false, false>));
                }
            }

            break;

        case 10:
            return (instr & 0x0800)
                       ? PREFIX_EXEC_FN(execFn_thumb_load_addr<memorySystemKind, true>)
                       : PREFIX_EXEC_FN(execFn_thumb_load_addr<memorySystemKind, false>);

        case 14:
        case 15:
            switch ((instr >> 11) & 3) {
                case 1:
                    return PREFIX_EXEC_FN(execFn_thumb_blx_suffix<memorySystemKind>);

                case 2:
                    return PREFIX_EXEC_FN(execFn_thumb_bl_x_prefix<memorySystemKind>);

                case 3:
                    return PREFIX_EXEC_FN(execFn_thumb_bl_suffix<memorySystemKind>);
            }
    }

    ERR("thumb opcode should be transcoded:" __FILE__ ":" str(__LINE__));
    __builtin_unreachable();
}

static uint32_t cpuPrvCompressExecFn(ExecFn execFn) {
#ifdef __EMSCRIPTEN__
    return (uint32_t)execFn;
#else
    return (int32_t)((uint8_t *)execFn - (uint8_t *)execFn_noop<ARM_MEMORY_SYSTEM_MMU, true>);
#endif
}

static ExecFn cpuPrvDecompressExecFn(uint32_t compressed) {
#ifdef __EMSCRIPTEN__
    return (ExecFn)compressed;
#else
    return (ExecFn)((uint8_t *)execFn_noop<ARM_MEMORY_SYSTEM_MMU, true> + (int32_t)compressed);
#endif
}

template <int memorySystemKind>
uint32_t cpuDecodeArm(uint32_t instr) {
    return cpuPrvCompressExecFn(cpuPrvDecoderArm<memorySystemKind, false>(instr));
}

template uint32_t cpuDecodeArm<ARM_MEMORY_SYSTEM_MMU>(uint32_t instr);
template uint32_t cpuDecodeArm<ARM_MEMORY_SYSTEM_MPU>(uint32_t instr);
template uint32_t cpuDecodeArm<ARM_MEMORY_SYSTEM_ARM920T>(uint32_t instr);

template <int memorySystemKind>
uint32_t cpuDecodeThumb(uint16_t instr, uint32_t &translatedInstr) {
    translatedInstr = table_thumb2arm[instr];

    if (translatedInstr) {
        return cpuPrvCompressExecFn(cpuPrvDecoderArm<memorySystemKind, true>(translatedInstr));
    } else {
        translatedInstr = instr;
        return cpuPrvCompressExecFn(cpuPrvDecoderThumb<memorySystemKind>(instr));
    }
}

template uint32_t cpuDecodeThumb<ARM_MEMORY_SYSTEM_MMU>(uint16_t instr, uint32_t &translatedInstr);
template uint32_t cpuDecodeThumb<ARM_MEMORY_SYSTEM_MPU>(uint16_t instr, uint32_t &translatedInstr);
template uint32_t cpuDecodeThumb<ARM_MEMORY_SYSTEM_ARM920T>(uint16_t instr, uint32_t &translatedInstr);

#ifdef __EMSCRIPTEN__

// The content of this function will be replaced with a jump table later.
template <int memorySystemKind, int tier>
static void __attribute__((noinline)) cpuPrvDispatchExecFnArm(ExecFn execFn, struct ArmCpu *cpu,
                                                              uint32_t instr) {
    ((ExecFn)((uint32_t)execFn & 0xffff))(cpu, instr);
}

// The content of this function will be replaced with a jump table later.
template <int memorySystemKind, int tier>
static void __attribute__((noinline)) cpuPrvDispatchExecFnThumb(ExecFn execFn, struct ArmCpu *cpu,
                                                                uint32_t instr) {
    ((ExecFn)((uint32_t)execFn & 0xffff))(cpu, instr);
}

#endif

static uint32_t translateThumb(uint16_t instrT) {
    bool vB;
    uint32_t instr = 0xE0000000UL /*most likely thing*/;
    uint16_t v16;
    uint_fast8_t v8;

    switch (instrT >> 12) {
        case 0:  // LSL(1) LSR(1) ASR(1) ADD(1) SUB(1) ADD(3) SUB(3)
        case 1:
            if ((instrT & 0x1800) != 0x1800) {  // LSL(1) LSR(1) ASR(1)

                instr |= 0x01B00000UL | ((instrT & 0x7) << 12) | ((instrT >> 3) & 7) |
                         ((instrT >> 6) & 0x60) | ((instrT << 1) & 0xF80);
            } else {
                vB = !!(instrT & 0x0200);  // SUB or ADD ?
                instr |= ((vB ? 5UL : 9UL) << 20) | (((uint32_t)(instrT & 0x38)) << 13) |
                         ((instrT & 0x07) << 12) | ((instrT >> 6) & 0x07);

                if (instrT & 0x0400) {  // ADD(1) SUB(1)

                    instr |= 0x02000000UL;
                } else {  // ADD(3) SUB(3)

                    // nothing to do here
                }
            }
            break;

        case 2:  // MOV(1) CMP(1) ADD(2) SUB(2)
        case 3:
            instr |= instrT & 0x00FF;
            switch ((instrT >> 11) & 3) {
                case 0:  // MOV(1)
                    instr |= 0x03B00000UL | ((instrT & 0x0700) << 4);
                    break;

                case 1:  // CMP(1)
                    instr |= 0x03500000UL | (((uint32_t)(instrT & 0x0700)) << 8);
                    break;

                case 2:  // ADD(2)
                    instr |= 0x02900000UL | ((instrT & 0x0700) << 4) |
                             (((uint32_t)(instrT & 0x0700)) << 8);
                    break;

                case 3:  // SUB(2)
                    instr |= 0x02500000UL | ((instrT & 0x0700) << 4) |
                             (((uint32_t)(instrT & 0x0700)) << 8);
                    break;
            }
            break;

        case 4:  // LDR(3) ADD(4) CMP(3) MOV(3) BX MVN CMP(2) CMN TST ADC SBC NEG MUL LSL(2)
                 // LSR(2) ASR(2) ROR AND EOR ORR BIC

            if (instrT & 0x0800) {  // LDR(3)
                return 0;
            } else if (instrT & 0x0400) {  // ADD(4) CMP(3) MOV(3) BX

                uint8_t vD;

                vD = (instrT & 7) | ((instrT >> 4) & 0x08);
                v8 = (instrT >> 3) & 0xF;

                switch ((instrT >> 8) & 3) {
                    case 0:  // ADD(4)
                        return 0;

                    case 1:  // CMP(3)

                        instr |= 0x01500000UL | (((uint32_t)vD) << 16) | v8;
                        break;

                    case 2:  // MOV(3)
                        return 0;

                    case 3:  // BX
                        return 0;

                    default:
                        goto undefined;
                }
            } else {  // AND EOR LSL(2) LSR(2) ASR(2) ADC SBC ROR TST NEG CMP(2) CMN ORR MUL BIC
                      // MVN (in val_tabl order)
                static const uint32_t val_tabl[16] = {
                    0x00100000UL, 0x00300000UL, 0x01B00010UL, 0x01B00030UL,
                    0x01B00050UL, 0x00B00000UL, 0x00D00000UL, 0x01B00070UL,
                    0x01100000UL, 0x02700000UL, 0x01500000UL, 0x01700000UL,
                    0x01900000UL, 0x00100090UL, 0x01D00000UL, 0x01F00000UL};

                // 00 = none
                // 10 = bit0 val
                // 11 = bit3 val
                // MVN BIC MUL ORR CMN CMP(2) NEG TST ROR SBC ADC ASR(2) LSR(2) LSL(2) EOR AND

                const uint32_t use16 = 0x2AAE280AUL;  // 0010 1010 1010 1110 0010 1000 0000 1010
                const uint32_t use12 = 0xA208AAAAUL;  // 1010 0010 0000 1000 1010 1010 1010 1010
                const uint32_t use8 = 0x0800C3F0UL;   // 0000 1000 0000 0000 1100 0011 1111 0000
                const uint32_t use0 = 0xFFF3BEAFUL;   // 1111 1111 1111 0011 1011 1110 1010 1111
                uint8_t vals[4] = {0};

                vals[2] = (instrT & 7);
                vals[3] = (instrT >> 3) & 7;
                v8 = (instrT >> 6) & 15;
                instr |= val_tabl[v8];
                v8 <<= 1;
                instr |= ((uint32_t)(vals[(use16 >> v8) & 3UL])) << 16;
                instr |= ((uint32_t)(vals[(use12 >> v8) & 3UL])) << 12;
                instr |= ((uint32_t)(vals[(use8 >> v8) & 3UL])) << 8;
                instr |= ((uint32_t)(vals[(use0 >> v8) & 3UL])) << 0;
            }
            break;

        case 5:  // STR(2)  STRH(2) STRB(2) LDRSB LDR(2) LDRH(2) LDRB(2) LDRSH		(in
                 // val_tbl orver)
        {
            static const uint32_t val_tabl[8] = {0x07800000UL, 0x018000B0UL, 0x07C00000UL,
                                                 0x019000D0UL, 0x07900000UL, 0x019000B0UL,
                                                 0x07D00000UL, 0x019000F0UL};
            instr |= ((instrT >> 6) & 7) | ((instrT & 7) << 12) |
                     (((uint32_t)(instrT & 0x38)) << 13) | val_tabl[(instrT >> 9) & 7];
        } break;

        case 6:  // LDR(1) STR(1)	(bit11 set = ldr)

            instr |= ((instrT & 7) << 12) | (((uint32_t)(instrT & 0x38)) << 13) |
                     ((instrT >> 4) & 0x7C) | 0x05800000UL;
            if (instrT & 0x0800) instr |= 0x00100000UL;
            break;

        case 7:  // LDRB(1) STRB(1)	(bit11 set = ldrb)

            instr |= ((instrT & 7) << 12) | (((uint32_t)(instrT & 0x38)) << 13) |
                     ((instrT >> 6) & 0x1F) | 0x05C00000UL;
            if (instrT & 0x0800) instr |= 0x00100000UL;
            break;

        case 8:  // LDRH(1) STRH(1)	(bit11 set = ldrh)

            instr |= ((instrT & 7) << 12) | (((uint32_t)(instrT & 0x38)) << 13) |
                     ((instrT >> 5) & 0x0E) | ((instrT >> 1) & 0x300) | 0x01C000B0UL;
            if (instrT & 0x0800) instr |= 0x00100000UL;
            break;

        case 9:  // LDR(4) STR(3)	(bit11 set = ldr)

            instr |= ((instrT & 0x700) << 4) | ((instrT & 0xFF) << 2) | 0x058D0000UL;
            if (instrT & 0x0800) instr |= 0x00100000UL;
            break;

        case 10:  // ADD(5) ADD(6)	(bit11 set = add(6))
            return 0;

        case 11:  // ADD(7) SUB(4) PUSH POP BKPT

            if ((instrT & 0x0600) == 0x0400) {  // PUSH POP

                instr |= (instrT & 0xFF) | 0x000D0000UL;

                if (instrT & 0x0800) {  // POP

                    if (instrT & 0x0100) instr |= 0x00008000UL;
                    instr |= 0x08B00000UL;
                } else {  // PUSH

                    if (instrT & 0x0100) instr |= 0x00004000UL;
                    instr |= 0x09200000UL;
                }
            } else if (instrT & 0x0100) {
                goto undefined;
            } else
                switch ((instrT >> 9) & 7) {
                    case 0:  // ADD(7) SUB(4)

                        instr |= 0x020DDF00UL | (instrT & 0x7F) |
                                 ((instrT & 0x0080) ? 0x00400000UL : 0x00800000UL);
                        break;

                    case 7:  // BKPT

                        instr |= 0x01200070UL | (instrT & 0x0F) | ((instrT & 0xF0) << 4);
                        break;

                    default:

                        goto undefined;
                }
            break;

        case 12:  // LDMIA STMIA		(bit11 set = ldmia)
            instr |= 0x08800000UL | (((uint32_t)(instrT & 0x700)) << 8) | (instrT & 0xFF);
            // STMIA always writes back; LDMIA only when the base isn't loaded.
            if (instrT & 0x0800) instr |= 0x00100000UL;
            if (!(instrT & 0x0800) || !((1UL << ((instrT >> 8) & 0x07)) & instrT))
                instr |= 0x00200000UL;
            break;

        case 13:  // B(1), SWI, undefined instr space
            v8 = ((instrT >> 8) & 0x0F);
            if (v8 == 14) {  // undefined instr
                goto undefined;
            } else if (v8 == 15) {  // SWI
                instr |= 0x0F000000UL | (instrT & 0xFF);
            } else {  // B(1)
                instr = (((uint32_t)v8) << 28) | 0x0A000000UL | (instrT & 0xFF);
                if (instrT & 0x80) instr |= 0x00FFFF00UL;
            }
            break;

        case 14:  // B(2) BL BLX(1) undefined instr space
        case 15:
            v16 = (instrT & 0x7FF);
            switch ((instrT >> 11) & 3) {
                case 0:  // B(2)

                    instr |= 0x0A000000UL | v16;
                    if (instrT & 0x0400) instr |= 0x00FFF800UL;
                    break;

                case 1:  // BLX(1)_suffix
                case 2:  // BLX(1)_prefix BL_prefix
                case 3:  // BL_suffix
                    return 0;
            }

            if (instrT & 0x0800)
                goto undefined;  // avoid BLX_suffix and undefined instr space in there
            instr |= 0x0A000000UL | (instrT & 0x7FF);
            if (instrT & 0x0400) instr |= 0x00FFF800UL;
            break;
    }

    return instr;

undefined:

    return 0xE7F000F0UL | (instrT & 0x0F) |
           ((instrT & 0xFFF0) << 4);  // guranteed undefined instr, inside it we store the
                                      // original thumb instr :)=-)
}

static bool cpuPrvConditionTableEntry(uint8_t key) {
    const bool N = key & 0x80, Z = key & 0x40, C = key & 0x20, V = key & 0x10;

    switch (key & 0x0f) {
        case 0:  // EQ
            return Z;

        case 1:  // NE
            return !Z;

        case 2:  // CS
            return C;

        case 3:  // CC
            return !C;

        case 4:  // MI
            return N;

        case 5:  // PL
            return !N;

        case 6:  // VS
            return V;

        case 7:  // VC
            return !V;

        case 8:  // HI
            return C && !Z;

        case 9:  // LS
            return !C || Z;

        case 10:  // GE
            return N == V;

        case 11:  // LT
            return N != V;

        case 12:  // GT
            return !Z && N == V;

        case 13:  // LE
            return Z || N != V;

        default:
            return true;
    }
}

static ImmShift cpuPrvImmShiftRegTableEntry(uint32_t key) {
    const uint8_t a = key;
    const uint8_t v = (key >> 8) & 0x03;

    ImmShift shift;
    shift.type = shiftTypeNoop;

    if (a == 0) return shift;

    switch (v) {  // perform shifts

        case 0:  // LSL
            if (a < 32) {
                shift.type = shiftTypeLSL;
                shift.coBit = 1 << (32 - a);
                shift.shift = a;
            } else {
                shift.type = shiftTypeZero;
                shift.coBit = a == 32 ? 1 : 0;
                shift.shift = 0;
            }
            break;

        case 1:  // LSR
            if (a < 32) {
                shift.type = shiftTypeLSR;
                shift.coBit = 1 << (a - 1);
                shift.shift = a;
            } else {
                shift.type = shiftTypeZero;
                shift.coBit = a == 32 ? 0x80000000 : 0;
                shift.shift = 0;
            }
            break;

        case 2:  // ASR
            shift.type = shiftTypeASR;

            if (a < 32) {
                shift.coBit = 1 << (a - 1);
                shift.shift = a;
            } else {
                shift.coBit = 0x80000000;
                shift.shift = 31;
            }
            break;

        case 3:  // ROR
            shift.type = shiftTypeROR;
            shift.coBit = 1 << ((a - 1) & 0x1f);
            shift.shift = a & 0x1f;

            break;
    }

    return shift;
}

static ImmShift cpuPrvImmShiftImmTableEntry(uint32_t key) {
    const uint8_t a = key & 0x1f;
    const uint8_t v = (key >> 5) & 0x03;

    ImmShift shift;
    shift.type = shiftTypeNoop;

    switch (v) {  // perform shifts

        case 0:  // LSL
            if (a != 0) {
                shift.type = shiftTypeLSL;
                shift.coBit = 1 << (32 - a);
                shift.shift = a;
            }
            break;

        case 1:  // LSR
            if (a == 0) {
                shift.type = shiftTypeZero;
                shift.coBit = 0x80000000;
                shift.shift = 32;
            } else {
                shift.type = shiftTypeLSR;
                shift.coBit = 1 << (a - 1);
                shift.shift = a;
            }
            break;

        case 2:  // ASR
            shift.type = shiftTypeASR;

            if (a == 0) {
                shift.coBit = 1 << 31;
                shift.shift = 31;
            } else {
                shift.coBit = 1 << (a - 1);
                shift.shift = a;
            }
            break;

        case 3:  // ROR
            if (a == 0) {
                shift.type = shiftTypeRRX;
            } else {
                shift.type = shiftTypeROR;
                shift.coBit = 1 << (a - 1);
                shift.shift = a == 32 ? 0 : a;
            }
            break;
    }

    return shift;
}

void cpuReset(struct ArmCpu *cpu, uint32_t pc) {
    cpu->arm920Waiting = false;
    cpu->exceptionSequence = 0;
#ifndef __EMSCRIPTEN__
    cpuClearNativeRuntime(cpu);
#endif
    gdbStubResetPace(cpu->debugStub);
    cpu->modePace = false; // Reset starts at the native vector, never stale 68k state.
    cpu->I = true;  // start w/o interrupts in supervisor mode
    cpu->F = true;
    cpu->M = ARM_SR_MODE_SVC;
    cpu->privileged = true;

    cpuPrvSetPC(cpu, pc);
    cpuPrvIdleForget(cpu);
    icacheInval(cpu->ic);
    systemStateSetUiInitialized(cpu->systemState, false);

    switch (cpu->memorySystemKind) {
        case ARM_MEMORY_SYSTEM_MMU:
            mmuReset(cpu->memorySystem.mmu);
            if (cpu->model == ArmCpuModel::arm920t) cp15MMUConfigureArm920Bootstrap(cpu->cp15.cp15mmu);
            break;

        case ARM_MEMORY_SYSTEM_MPU:
            mpuReset(cpu->memorySystem.mpu);
            break;
    }

    cpuUpdateSlowPath(cpu);
}

static void initStatic() {
    static bool initialized = false;
    if (initialized) return;

    table_thumb2arm = (uint32_t *)malloc(0x10000 * sizeof(uint32_t));

    for (uint32_t instr = 0; instr < 0x10000; instr++)
        table_thumb2arm[instr] = translateThumb(instr);

    for (int i = 0; i < 256; i++) table_conditions[i] = !cpuPrvConditionTableEntry(i);
    for (int i = 0; i < 1024; i++) table_immShiftReg[i] = cpuPrvImmShiftRegTableEntry(i);
    for (int i = 0; i < 128; i++) table_immShiftImm[i] = cpuPrvImmShiftImmTableEntry(i);
}

#ifdef FROND_DYNARMIC
static bool cpuPrvJitWanted(ArmCpuModel model);
static void cpuPrvJitInval(void *ctx, uint32_t va, uint32_t size, bool guest);
static const uint8_t *cpuPrvJitWatchFetch(void *ctx, uint32_t pa);
#endif

struct ArmCpu *cpuInit(uint32_t pc, struct ArmMem *mem, uint8_t memorySystemKind, bool xscale,
                       bool omap, int debugPort, uint32_t cpuid, uint32_t cacheId,
                       struct PatchDispatch *patchDispatch, struct PacePatch *pacePatch,
                       struct SystemState *systemState, ArmCpuModel model) {
    return cpuInit(pc, mem, memorySystemKind, xscale, omap, debugPort, cpuid, cacheId,
                   patchDispatch, pacePatch, systemState, model, false);
}

struct ArmCpu *cpuInit(uint32_t pc, struct ArmMem *mem, uint8_t memorySystemKind, bool xscale,
                       bool omap, int debugPort, uint32_t cpuid, uint32_t cacheId,
                       struct PatchDispatch *patchDispatch, struct PacePatch *pacePatch,
                       struct SystemState *systemState, ArmCpuModel model, bool jitSingleStep) {
    if (model == ArmCpuModel::arm920t && (xscale || omap || memorySystemKind != ARM_MEMORY_SYSTEM_MMU))
        ERR("ARM920T bootstrap requires the non-XScale MMU");
    initStatic();

    struct ArmCpu *cpu = (struct ArmCpu *)malloc(sizeof(*cpu));

    if (!cpu) ERR("cannot alloc CPU");

    memset(cpu, 0, sizeof(*cpu));
    cpu->model = model;
    cpu->paceCost = 20;  // cost model off: 20 per PACE op (cpuSetCostModel)

    cpu->debugStub = gdbStubInit(cpu, debugPort);

    cpu->mem = mem;

    cpu->memorySystemKind = memorySystemKind;
    if (memorySystemKind == ARM_MEMORY_SYSTEM_MMU) {
        cpu->memorySystem.mmu = mmuInit(mem, xscale);
        cpu->ic = icacheInit(mem, cpu->memorySystem.mmu);
        icacheArm920SetFcsePid(cpu->ic, &cpu->pid);
        cpu->cp15.cp15mmu =
            cp15MMUInit(cpu, cpu->memorySystem.mmu, cpu->ic, cpuid, cacheId, xscale, omap);

        paceInit(cpu->mem, cpu->memorySystem.mmu);
    } else {
        cpu->memorySystem.mpu = mpuCreate();
        cpu->ic = icacheInit(mem, cpu->memorySystem.mpu);
        cpu->cp15.cp15mpu = cp15MPUInit(cpu, cpu->memorySystem.mpu, cpu->ic, cpuid, cacheId);

        paceInit(cpu->mem, cpu->memorySystem.mpu);
    }

    if (!cpu->ic) ERR("Cannot init icache");

    cpu->patchDispatch = patchDispatch;
    cpu->pacePatch = pacePatch;
    cpu->systemState = systemState;

    cpu->m68kTrap0Handlers = new M68kTrapHandlerMap();

    // CPAR is only for XScale
    if (!xscale) cpu->CPAR = 0xffff;

    cpuReset(cpu, pc);

#ifdef FROND_DYNARMIC
    if (memorySystemKind == ARM_MEMORY_SYSTEM_MMU && cpuPrvJitWanted(model)) {
        cpu->jit = jitCreate(cpu, jitSingleStep);
        icacheSetInvalHook(cpu->ic, cpuPrvJitInval, cpu);
        icacheSetFetchWatch(cpu->ic, cpuPrvJitWatchFetch, cpu);
    }
#endif

    return cpu;
}

ArmCpuModel cpuGetModel(const ArmCpu* cpu) { return cpu->model; }
uint64_t cpuExceptionSequence(const ArmCpu* cpu) { return cpu->exceptionSequence; }

uint32_t cpuArm920ClockMode(const ArmCpu* cpu) {
    return cp15MMUClockMode(cpu->cp15.cp15mmu);
}

#ifndef __EMSCRIPTEN__
void cpuClearNativeRuntime(ArmCpu* cpu) {
    cpu->nativePc[0] = cpu->nativePc[1] = 0;
    memset(cpu->nativeCallbacks, 0, sizeof(cpu->nativeCallbacks));
    cpu->nativeCallDepth = 0;
    cpu->nativeSessionActive = false;
    cpu->nativeStackLower = cpu->nativeStackUpper = 0;
    cpu->nativeStackOwnerAddress = cpu->nativeStackOwnerValue = 0;
    cpu->nativeScratchData = nullptr;
    cpu->nativeScratchBase = cpu->nativeScratchSize = 0;
    cpuClearSlowPath(cpu, SLOW_PATH_REASON_NATIVE_PC | SLOW_PATH_REASON_NATIVE_FAULT);
}

bool cpuAcquireNativeSession(ArmCpu* cpu) {
    if (cpu->nativeSessionActive || cpu->nativeCallDepth || cpu->nativePc[0] || cpu->nativeScratchData) return false;
    for (const auto& h:cpu->nativeCallbacks) if (h.callback) return false;
    cpu->nativeSessionActive=true; return true;
}

bool cpuSetNativePcBreak(ArmCpu* cpu, uint32_t first, uint32_t second) {
    if (cpu->nativeCallDepth || (first && !(first & 1) && (first & 3)) ||
        (second && !(second & 1) && (second & 3))) return false;
    cpu->nativePc[0] = first; cpu->nativePc[1] = second;
    cpuClearSlowPath(cpu, SLOW_PATH_REASON_NATIVE_PC);
    return true;
}

bool cpuRegisterNativeCallback(ArmCpu* cpu, uint32_t address, NativeCallback callback, void* context) {
    if (!address || !callback || (!(address & 1) && (address & 3))) return false;
    for (const auto& slot : cpu->nativeCallbacks)
        if (slot.callback && slot.address == address) return false;
    for (auto& slot : cpu->nativeCallbacks) if (!slot.callback) {
        slot.address = address; slot.callback = callback; slot.context = context; return true;
    }
    return false;
}
void cpuUnregisterNativeCallback(ArmCpu* cpu, uint32_t address) {
    for (auto& slot : cpu->nativeCallbacks) if (slot.address == address) slot = {};
}

bool cpuStartNativeCall(ArmCpu* cpu, uint32_t entry) {
    if (cpu->nativeCallDepth >= 4 || !entry || (!(entry & 1) && (entry & 3))) return false;
    ++cpu->nativeCallDepth;
    cpuClearSlowPath(cpu, SLOW_PATH_REASON_NATIVE_FAULT | SLOW_PATH_REASON_INJECTED_CALL_DONE);
    cpu->modePace = false;
    cpuPrvSetReg(cpu, REG_NO_PC, entry);
    cpu->regs[REG_NO_LR] = INJECTED_CALL_LR_MAGIC;
    return true;
}
void cpuSetNativeStackBounds(ArmCpu* cpu, uint32_t lower, uint32_t upper,
                             uint32_t ownerAddress, uint32_t ownerValue) {
    cpu->nativeStackLower=lower; cpu->nativeStackUpper=upper;
    cpu->nativeStackOwnerAddress=ownerAddress; cpu->nativeStackOwnerValue=ownerValue;
}
void cpuEndNativeCall(ArmCpu* cpu) {
    if (cpu->nativeCallDepth) --cpu->nativeCallDepth;
    cpuClearSlowPath(cpu, SLOW_PATH_REASON_NATIVE_FAULT | SLOW_PATH_REASON_INJECTED_CALL_DONE);
}

// Validate each 1KB mapping segment before any transfer. Virtual buffers may
// span discontiguous physical pages. MMIO is never read as a side effect.
static bool nativeSegment(ArmCpu* cpu, uint32_t address, uint32_t size, bool write, uint32_t& pa, bool inspect = false, bool privileged = true) {
    if (cpu->memorySystemKind != ARM_MEMORY_SYSTEM_MMU) return false;
    if (cpu->model == ArmCpuModel::arm920t) address = cpuPrvMva(cpu, address);
    // Inspected reads skip the permission check so the debugger sees every mapped page.
    auto t = inspect ? (write ? mmuTranslateInspectAccess(cpu->memorySystem.mmu,address,privileged,true) :
                                mmuTranslateInspect(cpu->memorySystem.mmu,address,false)) :
                       mmuTranslate(cpu->memorySystem.mmu,address,privileged,write);
    if (!MMU_TRANSLATE_RESULT_OK(t)) return false;
    pa = MMU_TRANSLATE_RESULT_PA(t);
    return memIsBufferRange(cpu->mem,pa,size,write);
}
bool cpuInspectMapping(ArmCpu* cpu, uint32_t address, uint32_t size, uint32_t& physical) {
    if (!size || uint64_t(address) + size > 0x100000000ull) return false;
    for (uint32_t offset = 0; offset < size;) {
        const auto count = std::min<uint32_t>(0x400 - ((address + offset) & 0x3ff), size - offset);
        uint32_t pa;
        if (!nativeSegment(cpu, address + offset, count, false, pa, true)) return false;
        if (!offset) physical = pa;
        else if (uint64_t(physical) + offset != pa) return false;
        offset += count;
    }
    return true;
}
static bool nativeRange(ArmCpu* cpu, uint32_t address, uint32_t size, bool write, bool inspect = false, bool privileged = true) {
    if (!size) return true;
    if (uint64_t(address)+size>0x100000000ull) return false;
    for (uint64_t va=address; va<uint64_t(address)+size;) {
        const uint32_t count=std::min<uint64_t>(0x400-(va&0x3ff),uint64_t(address)+size-va);
        uint32_t pa;
        if (!nativeSegment(cpu,va,count,write,pa,inspect,privileged)) return false;
        va+=count;
    }
    return true;
}
bool cpuNativeRamRange(ArmCpu* cpu, uint32_t address, uint32_t size) {
    return nativeRange(cpu,address,size,true);
}
bool cpuNativeBuffer(ArmCpu* cpu, uint32_t address, void* data, uint32_t size, bool write, bool privileged) {
    if (!size) return true;
    if (!data || !nativeRange(cpu,address,size,write,false,privileged)) return false;
    for (uint32_t offset=0; offset<size;) {
        const uint32_t count=std::min<uint32_t>(0x400-((address+offset)&0x3ff),size-offset);
        uint32_t pa;
        if (!nativeSegment(cpu,address+offset,count,write,pa,false,privileged)) return false;
        for (uint32_t i=0;i<count;++i) {
            auto* byte=static_cast<uint8_t*>(data)+offset+i;
            if (write ? !memAccess<1,true>(cpu->mem,pa+i,byte) : !memAccess<1,false>(cpu->mem,pa+i,byte)) return false;
        }
        offset+=count;
    }
    return true;
}

static bool inspectRamRange(ArmCpu* cpu,uint32_t address,uint32_t size,bool write,bool privileged = true) {
    if (!size || uint64_t(address)+size>0x100000000ull) return false;
    for (uint64_t va=address;va<uint64_t(address)+size;) {
        uint32_t count=std::min<uint64_t>(0x400-(va&0x3ff),uint64_t(address)+size-va),pa;
        if (!nativeSegment(cpu,va,count,write,pa,true,privileged) || !memIsBufferRange(cpu->mem,pa,count,true)) return false;
        va+=count;
    }
    return true;
}
bool cpuInspectRamRange(ArmCpu* cpu,uint32_t address,uint32_t size) {
    return inspectRamRange(cpu,address,size,false);
}
bool cpuInspectWritableRamRange(ArmCpu* cpu,uint32_t address,uint32_t size,bool privileged) {
    return inspectRamRange(cpu,address,size,true,privileged);
}
bool cpuInspectRead(ArmCpu* cpu,uint32_t address,void* dest,uint32_t size) {
    if (!size || size>1048576 || !dest || !nativeRange(cpu,address,size,false,true)) return false;
    for (uint32_t offset=0;offset<size;) {
        uint32_t count=std::min<uint32_t>(0x400-((address+offset)&0x3ff),size-offset),pa;
        if (!nativeSegment(cpu,address+offset,count,false,pa,true)) return false;
        for (uint32_t i=0;i<count;++i)
            if (!memAccess<1,false>(cpu->mem,pa+i,static_cast<uint8_t*>(dest)+offset+i)) return false;
        offset+=count;
    }
    return true;
}
void cpuSetDebugSoc(ArmCpu* cpu,SoC* soc) { gdbStubSetSoc(cpu->debugStub,soc); }

bool cpuSetNativeScratch(ArmCpu* cpu, uint32_t base, void* data, uint32_t size) {
    if (cpu->nativeCallDepth) return false;
    if (!data) {
        cpu->nativeScratchData = nullptr; cpu->nativeScratchBase = cpu->nativeScratchSize = 0;
        return true;
    }
    if (cpu->memorySystemKind != ARM_MEMORY_SYSTEM_MMU || !mmuIsOn(cpu->memorySystem.mmu) ||
        (base & 0xfff) || !size || (size & 0xfff) || size > 0x40000 ||
        uint64_t(base) + size > 0xffff0000ull) return false;
    for (uint32_t offset = 0; offset < size; offset += 0x400) {
        if (MMU_TRANSLATE_RESULT_OK(mmuTranslate(cpu->memorySystem.mmu, base + offset, true, false)) ||
            MMU_TRANSLATE_RESULT_OK(mmuTranslate(cpu->memorySystem.mmu, base + offset, true, true))) return false;
    }
    cpu->nativeScratchBase = base; cpu->nativeScratchSize = size;
    cpu->nativeScratchData = static_cast<uint8_t*>(data);
    return true;
}

static bool nativeHook(ArmCpu* cpu, bool injected) {
    const uint32_t address = cpu->regs[REG_NO_PC] | (cpu->T ? 1u : 0u);
    if (!injected) {
        if (address && (address == cpu->nativePc[0] || address == cpu->nativePc[1])) {
            cpuSetSlowPath(cpu, SLOW_PATH_REASON_NATIVE_PC); return true;
        }
        return false;
    }
    if (!cpu->nativeCallDepth) return false;
    if ((cpu->M==ARM_SR_MODE_SYS || cpu->M==ARM_SR_MODE_USR) && cpu->nativeStackUpper &&
        (cpu->regs[REG_NO_SP]<cpu->nativeStackLower || cpu->regs[REG_NO_SP]>cpu->nativeStackUpper)) {
        // A full call may legitimately schedule other tasks. The owned UI
        // scratch bounds do not describe their stacks. Invalid metadata fails closed.
        uint32_t owner=0;
        if (!cpu->nativeStackOwnerAddress ||
            !cpuNativeBuffer(cpu,cpu->nativeStackOwnerAddress,&owner,4,false) ||
            owner==cpu->nativeStackOwnerValue) {
            cpuSetSlowPath(cpu,SLOW_PATH_REASON_NATIVE_FAULT); return true;
        }
    }
    for (const auto& slot : cpu->nativeCallbacks) if (slot.callback && slot.address == address) {
        // Copy before invoking: a callback may unregister itself. Exceptions propagate to recovery.
        const auto callback = slot.callback; void* context = slot.context;
        callback(cpu, context);
        cpuPrvSetReg(cpu, REG_NO_PC, cpu->regs[REG_NO_LR]);
        if (cpu->regs[REG_NO_PC] == INJECTED_CALL_LR_MAGIC)
            cpuSetSlowPath(cpu, SLOW_PATH_REASON_INJECTED_CALL_DONE);
        return true;
    }
    return false;
}

#endif

void cpuSetM68kSyscallHandler(ArmCpu* cpu, M68kSyscallHandler handler, void* context) {
    cpu->m68kSyscallHandler=handler; cpu->m68kSyscallContext=context;
}

uint32_t *cpuGetRegisters(struct ArmCpu *cpu) { return cpu->regs; }

static bool cpuPrvPaceCallout(struct ArmCpu *cpu, uint32_t destination) {
    if (!paceSave68kState()) {
        cpuPrvHandlePaceMemoryFault(cpu, __func__);
        return false;
    }

    cpu->regs[REG_NO_LR] = cpu->pacePatch->returnFromCallout + cpu->paceOffset;
    cpuPrvSetReg(cpu, REG_NO_PC, destination + cpu->paceOffset);

    cpu->modePace = false;
    cpuSetSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE);

    return true;
}

static void cpuPrvPaceSyscall(struct ArmCpu *cpu, uint16_t trapWord) {
    cpu->regs[1] = trapWord;

    if (!cpuPrvPaceCallout(cpu, cpu->pacePatch->calloutSyscall)) return;
}

static void cpuPrvPaceSyscall(struct ArmCpu *cpu) {
    const uint16_t trapWord = paceReadTrapWord();
    if (paceGetFsr() != 0) return cpuPrvHandlePaceMemoryFault(cpu, __func__);

    if (cpu->breakPaceSyscall && cpu->breakPaceSyscall == trapWord) {
        paceSetPC(paceGetPC() - 4);
        cpuSetSlowPath(cpu, SLOW_PATH_REASON_PACE_SYSCALL_BREAK);

        return;
    }

    if (cpu->m68kSyscallHandler && cpu->m68kSyscallHandler(cpu->m68kSyscallContext, cpu, trapWord)) return;
    if (patch68kHandle(trapWord)) return;

    cpuPrvPaceSyscall(cpu, trapWord);

#ifdef TRACE_PACE
    fprintf(stderr, "PACE syscall to %#06x\n", trapWord);
#endif
}

static void cpuPrvPaceReturn(struct ArmCpu *cpu) {
    uint_fast8_t fsr = 0;

    if (!paceSave68kState()) return cpuPrvHandlePaceMemoryFault(cpu, __func__);

    for (uint_fast8_t reg : {1, 4, 5, 6, 7, 8, 9, 10, 11, REG_NO_LR}) {
        if (!cpuPrvMemOpGeneric<4, false>(cpu, &cpu->regs[reg], cpu->regs[REG_NO_SP],
                                          cpu->privileged, &fsr))
            return cpuPrvHandleMemErr(cpu, cpu->regs[REG_NO_SP], true, false, fsr);

        cpu->regs[REG_NO_SP] += 4;
    }

    cpu->regs[0] = 0;
    cpuPrvSetPC(cpu, cpu->regs[REG_NO_LR]);

    cpu->modePace = false;
    cpuSetSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE);

#ifdef TRACE_PACE
    fprintf(stderr, "return from PACE\n");
#endif
}

static void cpuPrvPaceDivisionByZero(struct ArmCpu *cpu) {
    const uint16_t lastOpcode = paceGetLastOpcode();

    cpu->regs[1] = (lastOpcode >> 9) & 0x07;
    cpu->regs[2] = 0;

    if (!cpuPrvPaceCallout(
            cpu, lastOpcode & 0x0100 ? cpu->pacePatch->calloutDivs : cpu->pacePatch->calloutDivu))
        return;

#ifdef TRACE_PACE
    fprintf(stderr, "PACE division by zero\n");
#endif
}

static void cpuPrvPaceIllegalInstruction(struct ArmCpu *cpu) {
    if (!cpuPrvPaceCallout(cpu, cpu->pacePatch->calloutIllegalInstr)) return;

#ifdef TRACE_PACE
    fprintf(stderr, "PACE invalid instruction\n");
#endif
}

static void cpuPrvPaceLine1010(struct ArmCpu *cpu) {
    if (!cpuPrvPaceCallout(cpu, cpu->pacePatch->calloutLine1010)) return;

#ifdef TRACE_PACE
    fprintf(stderr, "PACE line 1010\n");
#endif
}

static void cpuPrvPaceLine1111(struct ArmCpu *cpu) {
    if (!cpuPrvPaceCallout(cpu, cpu->pacePatch->calloutLine1111)) return;

#ifdef TRACE_PACE
    fprintf(stderr, "PACE line 1111\n");
#endif
}

static void cpuPrvPaceTrap0(struct ArmCpu *cpu) {
    const uint32_t location = paceGetPC() - 2;
    auto itHandler = cpu->m68kTrap0Handlers->find(location);

    if (itHandler != cpu->m68kTrap0Handlers->end() && itHandler->second(cpu, location)) {
        if (paceGetPC() == PACE_RETURN_MAGIC_PC) cpuPrvPaceReturn(cpu);
        return;
    }

    if (!cpuPrvPaceCallout(cpu, cpu->pacePatch->calloutTrap0)) return;

#ifdef TRACE_PACE
    fprintf(stderr, "PACE trap 0\n");
#endif
}

static void cpuPrvPaceTrap8(struct ArmCpu *cpu) {
    if (!cpuPrvPaceCallout(cpu, cpu->pacePatch->calloutTrap8)) return;

#ifdef TRACE_PACE
    fprintf(stderr, "PACE trap 8\n");
#endif
}

static void cpuPrvPaceUnimplementedlInstruction(struct ArmCpu *cpu) {
    if (!cpuPrvPaceCallout(cpu, cpu->pacePatch->calloutUnimplementedInstr)) return;

#ifdef TRACE_PACE
    fprintf(stderr, "PACE unimplemented instruction\n");
#endif
}

// Native PACE faulted on a 68k memory access past the opcode fetch. Runs that instruction
// again from the state before it on the ROM's own PACE, so each access is the ARM load or
// store the device makes, in the same mode. The OS's data abort handler then does what it
// does on the device (Palm OS 5 skips an access that hits a permission fault, so a 68k read
// of address 0 gives whatever the ROM PACE's register held). The registers the ROM PACE has
// at dispatch are set as its loop sets them; its scratch registers keep their ARM values.
// INSTR_PACE_DISPATCH returns to native PACE at the next instruction.
static bool cpuPrvPaceRomFallback(struct ArmCpu *cpu) {
    const PacePatch *patch = cpu->pacePatch;
    if (!patch->romDispatchHooked) return false;

    const uint16_t opcode = paceGetLastOpcode();
    const uint32_t table = patch->enterPace + cpu->paceOffset + 0x68;
    uint_fast8_t fsr = 0;
    uint32_t handler = 0;
    // A data read: the table is the ROM's own, also where the instructions are hooked.
    if (!cpuPrvMemOpExGeneric<4, false>(cpu, &handler, table + 4 * (opcode >> 12), cpu->privileged,
                                        &fsr))
        return false;

    uint32_t faultAddr;
    bool faultWrite;
    paceGetMemeryFault(&faultAddr, &faultWrite, &fsr);
    if (!paceRestoreFaultedInstruction()) return false;
    const uint32_t state = paceGetStatePtr(), pc = paceGetPC();
#ifndef __EMSCRIPTEN__
    // The first 8, then each power of two, so an app that faults every frame can't flood the log.
    static uint32_t faultCount = 0;
    faultCount++;
    if (faultCount <= 8 || !(faultCount & (faultCount - 1)))
        fprintf(stderr, "68k %s 0x%08x faulted (fsr=%u, pc=0x%08x), running it on the ROM PACE (fault %u)\n",
                faultWrite ? "write to" : "read of", faultAddr, unsigned(fsr), pc, faultCount);
#endif
    if (!paceSave68kState()) {
        cpuPrvHandlePaceMemoryFault(cpu, __func__);
        return true;
    }

    uint32_t sr = 0;
    if (!cpuPrvMemOpExGeneric<4, false>(cpu, &sr, state + 0x48, cpu->privileged, &fsr)) {
        cpuPrvHandleMemErr(cpu, state + 0x48, false, false, fsr);
        return true;
    }

    // ldrh r7, [r10], #2 after the opcode: a little-endian read of the 68k word. If it faults,
    // the abort handler leaves r7 as it was, the opcode as fetched.
    const uint32_t opcodeLe = __builtin_bswap16(opcode);
    uint16_t prefetch = opcodeLe;
    if (!cpuPrvMemOpExGeneric<2, false>(cpu, &prefetch, pc + 2, cpu->privileged, &fsr))
        prefetch = opcodeLe;

    cpu->regs[0] = handler;
    cpu->regs[4] = sr;
    cpu->regs[5] = state + 4;
    cpu->regs[6] = state + 0x24;
    cpu->regs[7] = prefetch;
    cpu->regs[8] = (opcodeLe << 8) | (opcodeLe >> 8);
    cpu->regs[10] = pc + 4;
    cpu->regs[11] = table;
    cpu->regs[REG_NO_PC] = table + handler;

    cpu->modePace = false;
    cpuSetSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE);

    return true;
}

FORCE_INLINE static void cpuPrvCyclePace(struct ArmCpu *cpu) {
    const bool diagnose = cpuPrvPaceFaultDiagnostics();
    const uint32_t instructionPc = (cpu->debugStub || diagnose) ? paceGetPC() : 0;
    PaceDebugState before;
    if (diagnose) before = paceDebugState();
    const auto status = paceExecute();
    if (cpu->debugStub) gdbStubPaceInstructionCompleted(cpu->debugStub, instructionPc, status);
    switch (status) {
        case pace_status_ok:
            return;

        case pace_status_division_by_zero:
            cpuPrvPaceDivisionByZero(cpu);
            break;

        case pace_status_illegal_instr:
            cpuPrvPaceIllegalInstruction(cpu);
            break;

        case pace_status_line_1010:
            cpuPrvPaceLine1010(cpu);
            break;

        case pace_status_line_1111:
            cpuPrvPaceLine1111(cpu);
            break;

        case pace_status_memory_fault:
            if (cpuPrvPaceRomFallback(cpu)) break;
            cpuPrvHandlePaceMemoryFault(cpu, __func__, instructionPc, diagnose ? &before : nullptr);
            break;

        case pace_status_syscall:
            cpuPrvPaceSyscall(cpu);
            break;

        case pace_status_trap0:
            cpuPrvPaceTrap0(cpu);
            break;

        case pace_status_trap8:
            cpuPrvPaceTrap8(cpu);
            break;

        case pace_status_return:
            cpuPrvPaceReturn(cpu);
            break;

        default:
            // This also encompasses CHK errors, TRAPV and other unhandled
            // exceptions
            cpuPrvPaceUnimplementedlInstruction(cpu);
            break;
    }
}

#ifndef __EMSCRIPTEN__
// Expose progress only while a CPU batch is on the stack. Peripheral reads can
// observe the current instruction's time without forcing a batch every clock tick.
struct CycleProgress {
    ArmCpu* cpu;
    const uint32_t* previous;
    CycleProgress(ArmCpu* cpu, const uint32_t* count) : cpu(cpu), previous(cpu->cyclesInProgress) {
        cpu->cyclesInProgress = count;
    }
    ~CycleProgress() { cpu->cyclesInProgress = previous; }
};
uint32_t cpuCyclesInProgress(const ArmCpu* cpu) {
    return cpu->cyclesInProgress ? *cpu->cyclesInProgress + cpu->jitHint.pending : 0;
}
#endif

ATTR_EMCC_NOINLINE static uint32_t cpuCyclePace(struct ArmCpu *cpu, uint32_t cycles) {
    uint32_t cycleAcc = 0;
#ifndef __EMSCRIPTEN__
    CycleProgress progress(cpu, &cycleAcc);
#endif

    while (cycleAcc < cycles) {
        if (cpu->debugStub) gdbStubReportPc(cpu->debugStub, paceGetPC(), false);
        cpuPrvCyclePace(cpu);
        cycleAcc += cpu->paceCost;

        if (cpu->slowPath) break;
    }

    return cycleAcc;
}

#ifndef __EMSCRIPTEN__
// icacheFetchCached for a va in the same 32-byte line as the previous fetch. The line
// was valid for lineVa when ic->epoch was lineEpoch, and the epoch moves on every
// invalidation and refill, so an unchanged epoch means the line still holds lineVa.
// The ARM920T copy never uses it: it fetches every instruction through icacheFetch.
// The decoded slot is still checked, since execFns can decode other slots of the line.
template <int size>
static FORCE_INLINE bool cpuPrvFetchSameLine(const struct icache* ic, const struct icacheline* line,
                                             uint32_t lineVa, uint32_t lineEpoch, uint32_t va,
                                             uint32_t& instr, uint32_t& decoded, uint32_t& cost) {
    // lineVa is line aligned, so this also requires va to be aligned to size.
    if ((va & ~uint32_t((1u << CACHE_LINE_WIDTH_BITS) - size)) != lineVa) return false;
    if (ic->epoch != lineEpoch) return false;
    const unsigned offset = calculateLineIndex(va);
    const uint32_t cached = line->decoded[offset >> 1];
    constexpr uint32_t kind = size == 4 ? DECODED_BITS_ARM : DECODED_BITS_THUMB;
    if ((cached & DECODED_BITS) != kind) return false;
    decoded = int32_t(cached) >> DECODED_BITS_SHIFT;
    cost = line->cost[offset >> 1];
    if constexpr (size == 4) std::memcpy(&instr, line->data + offset, sizeof(instr));
    else instr = line->translatedThumbInstructions[offset >> 1];
    return true;
}

// After a fetch outside the fast path: remember pc's line for cpuPrvFetchSameLine if the
// cache holds it now, otherwise make the fast path miss until the next slow fetch.
static FORCE_INLINE void cpuPrvRememberLine(const struct icache* ic, uint32_t pc, uint32_t key,
                                            const struct icacheline*& line, uint32_t& lineVa,
                                            uint32_t& lineEpoch) {
    line = ic->cache + calculateIndex(pc);
    lineVa = line->revision == ic->revision && line->tag == calculateTag(pc) && line->fetchKey == key
                 ? maskLine(pc)
                 : 0xffffffff;
    lineEpoch = ic->epoch;
}
#endif

#ifndef __EMSCRIPTEN__
// cpuCycleArm and cpuCycleThumb without a debugger, injected call or native PC break.
// After a fetch from a valid icache line it runs on through that line for as long as
// each instruction leaves the PC at the next one, the epoch holds and the next slot is
// decoded. Per instruction it keeps the general loop's order: curInstrPC and the cycle
// count, then the PC, the execFn and the slow path check.
// The ARM920T copy fetches every instruction through icacheFetch.
template <int memorySystemKind, int size>
ATTR_EMCC_NOINLINE static uint32_t cpuCycleLine(struct ArmCpu *cpu, uint32_t cycles) {
    constexpr bool arm920 = armKindIsArm920(memorySystemKind);
    uint32_t cycleAcc = 0;
    uint32_t acc = 0;
    CycleProgress progress(cpu, &cycleAcc);
    struct icache *const ic = cpu->ic;
    constexpr uint32_t kind = size == 4 ? DECODED_BITS_ARM : DECODED_BITS_THUMB;
    constexpr uint32_t lineSize = 1u << CACHE_LINE_WIDTH_BITS;

    while (acc < cycles) {
        uint32_t pc = cpu->regs[REG_NO_PC];
        cpu->curInstrPC = pc;

        const struct icacheline *line = ic->cache + calculateIndex(pc);
        uint32_t offset = calculateLineIndex(pc);
        uint32_t cached = line->decoded[offset >> 1];
        if (unlikely(arm920 || (pc & (size - 1)) || line->revision != ic->revision ||
                     line->tag != calculateTag(pc) || (cached & DECODED_BITS) != kind ||
                     line->fetchKey != icacheFetchKey(ic, cpu->privileged))) {
            uint32_t instr = 0, decoded = 0;
            uint_fast8_t fsr;
            // A fetch sees the first clock of its instruction; a failed one costs just that.
            acc += 1;
            cycleAcc = acc;
            if (!icacheFetch<memorySystemKind, size, false>(ic, pc, cpu->privileged, &fsr, instr, decoded)) {
                cpuPrvHandleMemErr<memorySystemKind>(cpu, pc, false, true, fsr);
                return acc;
            }
            acc += cpuPrvSlotCharge(cpu, ic->lastCost) - 1;
            cycleAcc = acc;
            cpu->regs[REG_NO_PC] = pc + size;
            cpuPrvDecompressExecFn(decoded)(cpu, instr);
            cpuPrvTakeCharge(cpu, acc, cycleAcc);
            if (cpu->slowPath) break;
            continue;
        }

        const uint32_t epoch = ic->epoch;
        for (;;) {
            uint32_t instr;
            if constexpr (size == 4) std::memcpy(&instr, line->data + offset, sizeof(instr));
            else instr = line->translatedThumbInstructions[offset >> 1];
            acc += cpuPrvSlotCharge(cpu, line->cost[offset >> 1]);
            cycleAcc = acc;
            cpu->regs[REG_NO_PC] = pc + size;
            cpuPrvDecompressExecFn(int32_t(cached) >> DECODED_BITS_SHIFT)(cpu, instr);
            cpuPrvTakeCharge(cpu, acc, cycleAcc);
            if (cpu->slowPath) return acc;

            pc += size;
            offset += size;
            if (cpu->regs[REG_NO_PC] != pc || offset == lineSize || ic->epoch != epoch ||
                acc >= cycles)
                break;
            cached = line->decoded[offset >> 1];
            if ((cached & DECODED_BITS) != kind) break;

            cpu->curInstrPC = pc;
        }
    }

    return acc;
}
#endif

// debug is cpu->debugStub != NULL, which is fixed when the CPU is created.
// cycleAcc is what peripherals see through cyclesInProgress; acc is the loop's copy.
template <int memorySystemKind, bool injected, bool debug>
ATTR_EMCC_NOINLINE static uint32_t cpuCycleThumb(struct ArmCpu *cpu, uint32_t cycles) {
    uint32_t cycleAcc = 0;
    uint32_t acc = 0;
#ifndef __EMSCRIPTEN__
    CycleProgress progress(cpu, &cycleAcc);
    struct icache* const ic = cpu->ic;
    const struct icacheline* line = ic->cache;
    uint32_t lineVa = 0xffffffff;  // never matches: the fast path mask clears bits 1-4
    uint32_t lineEpoch = ic->epoch;
#endif
    bool ok;
    uint32_t translatedInstr;
    uint32_t decoded;
    uint32_t cost = ICACHE_COST_ONE;
    uint_fast8_t fsr;

    while (acc < cycles) {
#ifndef __EMSCRIPTEN__
        if (unlikely(cpu->nativePc[0] || (injected && cpu->nativeCallDepth)) && nativeHook(cpu, injected))
            return acc + (injected ? 1 : 0);
#endif
        cpu->curInstrPC = cpu->regs[REG_NO_PC];  // needed for stub to get proper pc
        if constexpr (debug) {
            gdbStubReportPc(cpu->debugStub, cpu->regs[REG_NO_PC], true);
            // Register commands may redirect the next fetch or change ARM/Thumb.
            cpu->curInstrPC = cpu->regs[REG_NO_PC];
            if (!cpu->T) return acc;
        }

        acc += 1;
        cycleAcc = acc;

        // FCSE
#ifdef SUPPORT_FCSE
        if (fetchPc < 0x02000000UL) fetchPc |= cpu->pid;
#endif

#ifndef __EMSCRIPTEN__
        const uint32_t pc = cpu->curInstrPC;
        if constexpr (armKindIsArm920(memorySystemKind)) {
            ok = icacheFetch<memorySystemKind, 2, injected>(ic, pc, cpu->privileged, &fsr, translatedInstr, decoded);
            cost = ic->lastCost;
        } else {
            ok = cpuPrvFetchSameLine<2>(ic, line, lineVa, lineEpoch, pc, translatedInstr, decoded, cost);
            if (!ok) {
                // Fetch through copies, so the fast path's values stay in registers.
                uint32_t slowInstr = 0, slowDecoded = 0, slowCost = 0;
                const uint32_t key = icacheFetchKey(ic, cpu->privileged);
                ok = icacheFetchCached<2>(ic, pc, key, slowInstr, slowDecoded, slowCost);
                if (!ok) {
                    ok = icacheFetch<memorySystemKind, 2, injected>(ic, pc, cpu->privileged, &fsr, slowInstr,
                                                                    slowDecoded);
                    slowCost = ic->lastCost;
                }
                translatedInstr = slowInstr;
                decoded = slowDecoded;
                cost = slowCost;
                cpuPrvRememberLine(ic, pc, key, line, lineVa, lineEpoch);
            }
        }
#else
        ok = icacheFetch<memorySystemKind, 2, injected>(cpu->ic, cpu->curInstrPC, cpu->privileged, &fsr,
                                                        translatedInstr, decoded);
        cost = cpu->ic->lastCost;
#endif

        if (!ok) {
            cpuPrvHandleMemErr<memorySystemKind>(cpu, cpu->curInstrPC, false, true, fsr);
            return acc;  // exit here so that debugger can see us execute first instr
                         // of execption handler
        }
        acc += cpuPrvSlotCharge(cpu, cost) - 1;
        cycleAcc = acc;

        cpu->regs[REG_NO_PC] += 2;

#ifdef __EMSCRIPTEN__
        cpuPrvDispatchExecFnThumb<memorySystemKind, injected>(cpuPrvDecompressExecFn(decoded), cpu,
                                                              translatedInstr);
#else
        cpuPrvDecompressExecFn(decoded)(cpu, translatedInstr);
#endif

        cpuPrvTakeCharge(cpu, acc, cycleAcc);
        if constexpr (debug) gdbStubInstructionExecuted(cpu->debugStub);

        if constexpr (injected) {
            if (cpu->regs[REG_NO_PC] == INJECTED_CALL_LR_MAGIC)
                cpuSetSlowPath(cpu, SLOW_PATH_REASON_INJECTED_CALL_DONE);
        }

        if (cpu->slowPath) break;
    }

    return acc;
}

template <int memorySystemKind, bool injected, bool debug>
ATTR_EMCC_NOINLINE static uint32_t cpuCycleArm(struct ArmCpu *cpu, uint32_t cycles) {
    uint32_t cycleAcc = 0;
    uint32_t acc = 0;
#ifndef __EMSCRIPTEN__
    CycleProgress progress(cpu, &cycleAcc);
    struct icache* const ic = cpu->ic;
    const struct icacheline* line = ic->cache;
    uint32_t lineVa = 0xffffffff;  // never matches: the fast path mask clears bits 1-4
    uint32_t lineEpoch = ic->epoch;
#endif
    bool ok;
    uint32_t instr, decoded;
    uint32_t cost = ICACHE_COST_ONE;
    uint_fast8_t fsr;

    while (acc < cycles) {
#ifndef __EMSCRIPTEN__
        if (unlikely(cpu->nativePc[0] || (injected && cpu->nativeCallDepth)) && nativeHook(cpu, injected))
            return acc + (injected ? 1 : 0);
#endif
        cpu->curInstrPC = cpu->regs[REG_NO_PC];  // needed for stub to get proper pc
        if constexpr (debug) {
            gdbStubReportPc(cpu->debugStub, cpu->regs[REG_NO_PC], false);
            // Register commands may redirect the next fetch or change ARM/Thumb.
            cpu->curInstrPC = cpu->regs[REG_NO_PC];
            if (cpu->T) return acc;
        }

        acc += 1;
        cycleAcc = acc;

        // FCSE
#ifdef SUPPORT_FCSE
        if (fetchPc < 0x02000000UL) fetchPc |= cpu->pid;
#endif

#ifndef __EMSCRIPTEN__
        const uint32_t pc = cpu->curInstrPC;
        if constexpr (armKindIsArm920(memorySystemKind)) {
            ok = icacheFetch<memorySystemKind, 4, injected>(ic, pc, cpu->privileged, &fsr, instr, decoded);
            cost = ic->lastCost;
        } else {
            ok = cpuPrvFetchSameLine<4>(ic, line, lineVa, lineEpoch, pc, instr, decoded, cost);
            if (!ok) {
                // Fetch through copies, so the fast path's values stay in registers.
                uint32_t slowInstr = 0, slowDecoded = 0, slowCost = 0;
                const uint32_t key = icacheFetchKey(ic, cpu->privileged);
                ok = icacheFetchCached<4>(ic, pc, key, slowInstr, slowDecoded, slowCost);
                if (!ok) {
                    ok = icacheFetch<memorySystemKind, 4, injected>(ic, pc, cpu->privileged, &fsr, slowInstr,
                                                                    slowDecoded);
                    slowCost = ic->lastCost;
                }
                instr = slowInstr;
                decoded = slowDecoded;
                cost = slowCost;
                cpuPrvRememberLine(ic, pc, key, line, lineVa, lineEpoch);
            }
        }
#else
        ok = icacheFetch<memorySystemKind, 4, injected>(cpu->ic, cpu->curInstrPC, cpu->privileged, &fsr, instr,
                                                        decoded);
        cost = cpu->ic->lastCost;
#endif

        if (!ok) {
            cpuPrvHandleMemErr<memorySystemKind>(cpu, cpu->curInstrPC, false, true, fsr);
            return acc;  // exit here so that debugger can see us execute first instr
                         // of execption handler
        }
        acc += cpuPrvSlotCharge(cpu, cost) - 1;
        cycleAcc = acc;

        cpu->regs[REG_NO_PC] += 4;

#ifdef __EMSCRIPTEN__
        cpuPrvDispatchExecFnArm<memorySystemKind, injected>(cpuPrvDecompressExecFn(decoded), cpu,
                                                            instr);
#else
        cpuPrvDecompressExecFn(decoded)(cpu, instr);
#endif

        cpuPrvTakeCharge(cpu, acc, cycleAcc);
        if constexpr (debug) gdbStubInstructionExecuted(cpu->debugStub);

        if constexpr (injected) {
            if (cpu->regs[REG_NO_PC] == INJECTED_CALL_LR_MAGIC)
                cpuSetSlowPath(cpu, SLOW_PATH_REASON_INJECTED_CALL_DONE);
        }

        if (cpu->slowPath) break;
    }

    return acc;
}

#ifndef __EMSCRIPTEN__
// ARM920T Fast engine: the interpreter over the decode cache (ic->cache, which the ARM920T Exact path never
// uses) and host pages, with no I- or D-cache contents. Each instruction is classed when it is decoded:
// plain forms run as they are; forms the probe guard decides from registers go through a check first; the
// rest stop the run before them (leanExit), so the guarded step takes them with Exact's checks and stops.
enum class LeanClass { plain, check, exit, thumbBx, blPrefix, blSuffix };

// The probe guard's encoding-only answers (imx1_boot_probe.cpp bootstrapInstruction). Misaligned accesses
// stop the run in the memory path, which covers the guard's alignment checks. SWI, CP15, MSR of the
// control field, S-bit LDM/STM and exception returns always go to the step.
static LeanClass cpuPrvLeanClassArm(uint32_t i) {
    if (i >> 28 == 15 || (i & 0x0f000000) == 0x0f000000) return LeanClass::exit;
    if ((i & 0x0ffffff0) == 0x012fff10) return LeanClass::plain;  // BX
    if ((i & 0x0e000000) == 0x0a000000) return LeanClass::plain;  // B, BL
    if ((i & 0x0fbf0fff) == 0x010f0000) return LeanClass::plain;  // MRS
    if ((i & 0x0fb0fff0) == 0x0120f000 || (i & 0x0fb0f000) == 0x0320f000)  // MSR: SPSR, or CPSR flags only
        return (i & 0x00400000) || ((i >> 16) & 15) == 8 ? LeanClass::plain : LeanClass::exit;
    if ((i & 0x0f000f10) == 0x0e000f10) return LeanClass::exit;  // CP15
    if ((i & 0x0e000000) == 0x08000000) {
        const unsigned base = (i >> 16) & 15;
        if (i & 0x00400000) return LeanClass::exit;
        return (i & 0xffff) && base != 15 && (!(i & 0x00200000) || !(i & (1u << base))) ?
               LeanClass::plain : LeanClass::exit;
    }
    if ((i & 0x0c000000) == 0x04000000) {
        const bool pc = ((i >> 12) & 15) == 15;
        if (pc && !(i & 0x00100000)) {
            const bool pre = i & 0x01000000, immediate = !(i & 0x02000000);
            const bool writeback = !pre || (i & 0x00200000);
            if ((i & 0x00400000) || (!pre && (i & 0x00200000)) ||
                (writeback && ((i >> 16) & 15) == 15) || (!immediate && (i & 15) == 15)) return LeanClass::exit;
        }
        return (!pc || !(i & 0x00400000)) && !((i & 0x02000010) == 0x02000010) ? LeanClass::plain : LeanClass::exit;
    }
    if ((i & 0x0c000000) != 0) return LeanClass::exit;
    if ((i & 0x0fc000f0) == 0x00000090) {
        const unsigned dest = (i >> 16) & 15, add = (i >> 12) & 15, rs = (i >> 8) & 15, rm = i & 15;
        return dest != 15 && rs != 15 && rm != 15 && ((i & 0x00200000) ? add != 15 : add == 0) ?
               LeanClass::plain : LeanClass::exit;
    }
    if ((i & 0x0f8000f0) == 0x00800090) {
        const unsigned hi = (i >> 16) & 15, lo = (i >> 12) & 15, rs = (i >> 8) & 15, rm = i & 15;
        return hi != 15 && lo != 15 && rs != 15 && rm != 15 && hi != lo && hi != rm && lo != rm ?
               LeanClass::plain : LeanClass::exit;
    }
    if ((i & 0x0e000090) == 0x00000090 && (i & 0x60)) {
        const bool load = i & 0x00100000, pre = i & 0x01000000;
        const bool immediate = i & 0x00400000, writeback = !pre || (i & 0x00200000);
        const unsigned base = (i >> 16) & 15, dest = (i >> 12) & 15, rm = i & 15;
        if ((!load && (i & 0x60) != 0x20) || dest == 15 || (!pre && (i & 0x00200000)) ||
            (writeback && (base == 15 || base == dest || (!immediate && base == rm))) ||
            (!immediate && ((i & 0xf00) || rm == 15))) return LeanClass::exit;
        return LeanClass::plain;
    }
    if (((i >> 12) & 15) == 15) {
        if ((i & 0x0ffffff0) == 0x01b0f000 || (i & 0x0fffffff) == 0x025ef004) return LeanClass::exit;
        if ((i & 0x0fff0ff0) == 0x01a00000) return LeanClass::plain;  // MOV pc, Rm
        return LeanClass::check;
    }
    const unsigned opcode = (i >> 21) & 15;
    if (opcode >= 8 && opcode <= 11 && !(i & 0x00100000)) return LeanClass::exit;
    return (i & 0x02000000) || !(i & 0x10) || !(i & 0x80) ? LeanClass::plain : LeanClass::exit;
}

// bootstrapThumb's encoding-only answers, as for ARM.
static LeanClass cpuPrvLeanClassThumb(uint16_t i) {
    if (i < 0x4000) return LeanClass::plain;
    if ((i & 0xfc00) == 0x4000)
        return ((i >> 6) & 15) != 13 || (i & 7) != ((i >> 3) & 7) ? LeanClass::plain : LeanClass::exit;
    if ((i & 0xfc00) == 0x4400) {
        const unsigned op = (i >> 8) & 3, source = (i >> 3) & 15, dest = (i & 7) | ((i >> 4) & 8);
        if (op == 3) return !(i & 0x87) ? LeanClass::thumbBx : LeanClass::exit;
        if (op == 1 && (source == 15 || dest == 15)) return LeanClass::exit;
        return op != 0 || dest != 15 || source != 15 ? LeanClass::plain : LeanClass::exit;
    }
    if ((i & 0xf800) == 0x4800 || (i & 0xf000) == 0x5000 || (i & 0xe000) == 0x6000 ||
        (i & 0xf000) == 0x8000 || (i & 0xf000) == 0x9000 || (i & 0xf000) == 0xa000 || (i & 0xff00) == 0xb000)
        return LeanClass::plain;
    if ((i & 0xf600) == 0xb400) return (i & 0x1ff) ? LeanClass::plain : LeanClass::exit;
    if ((i & 0xf000) == 0xc000) {
        const unsigned base = (i >> 8) & 7;
        return (i & 0xff) && ((i & 0x0800) || !(i & (1u << base))) ? LeanClass::plain : LeanClass::exit;
    }
    if ((i & 0xff00) == 0xdf00) return LeanClass::exit;  // SWI
    if ((i & 0xf000) == 0xd000) return (i & 0xf00) < 0xe00 ? LeanClass::plain : LeanClass::exit;
    if ((i & 0xf800) == 0xe000) return LeanClass::plain;
    if ((i & 0xf800) == 0xf000) return LeanClass::blPrefix;
    if ((i & 0xf800) == 0xf800) return LeanClass::blSuffix;
    return LeanClass::exit;
}

static void execFn_leanExit(struct ArmCpu *cpu, uint32_t) { cpu->leanExit = true; }

// Data processing that writes pc: the guard looks at the target, so ask it with the registers the step
// would show it (pc at the instruction).
static void execFn_leanCheckArm(struct ArmCpu *cpu, uint32_t instr) {
    if (!cpuPrvConditionFails(cpu, instr)) {
        const uint32_t next = cpu->regs[REG_NO_PC];
        cpu->regs[REG_NO_PC] = cpu->curInstrPC;
        const bool admitted = cpu->leanGuard &&
            cpu->leanGuard(instr, cpu->regs, cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR),
                           cpuGetRegExternal(cpu, ARM_REG_NUM_SPSR));
        cpu->regs[REG_NO_PC] = next;
        if (!admitted) {
            cpu->leanExit = true;
            return;
        }
    }
    cpuPrvDecoderArm<ARM_MEMORY_SYSTEM_ARM920T, false>(instr)(cpu, instr);
}

// Thumb BX: the guard refuses a halfword-aligned ARM target, except BX PC, which goes to the word below.
template <bool pcS>
static void execFn_leanThumbBx(struct ArmCpu *cpu, uint32_t instr) {
    const unsigned source = (instr >> 3) & 15;
    const uint32_t target = source == 15 ? cpu->curInstrPC + 4 : cpu->regs[source];
    if (!pcS && (target & 3) == 2) {
        cpu->leanExit = true;
        return;
    }
    execFn_thumb_bl_x<ARM_MEMORY_SYSTEM_ARM920T, false, pcS>(cpu, instr);
}

// The raw halfword at va if the decode cache holds its line under the current fetch key.
static bool cpuPrvLeanPeekHalf(const struct ArmCpu *cpu, uint32_t va, uint16_t &half) {
    va = cpuPrvMva(cpu, va);
    const struct icache *ic = cpu->ic;
    const struct icacheline *line = ic->cache + calculateIndex(va);
    if (line->revision != ic->revision || line->tag != calculateTag(va) ||
        line->fetchKey != icacheFetchKey(ic, cpu->privileged)) return false;
    memcpy(&half, line->data + calculateLineIndex(va), sizeof(half));
    return true;
}

// A BL prefix runs here only when its suffix follows, as the step would stop at anything else. The suffix
// runs only straight after its prefix in the same run; others go to the step, which knows interrupted ones.
static void execFn_leanBlPrefix(struct ArmCpu *cpu, uint32_t instr) {
    uint16_t next;
    if (!cpuPrvLeanPeekHalf(cpu, cpu->curInstrPC + 2, next) || (next & 0xf800) != 0xf800) {
        cpu->leanExit = true;
        return;
    }
    execFn_thumb_bl_x_prefix<ARM_MEMORY_SYSTEM_ARM920T>(cpu, instr);
    cpu->leanBlNext = cpu->curInstrPC + 2;
}

static void execFn_leanBlSuffix(struct ArmCpu *cpu, uint32_t instr) {
    if (cpu->leanBlNext != cpu->curInstrPC) {
        cpu->leanExit = true;
        return;
    }
    cpu->leanBlNext = ~0u;
    execFn_thumb_bl_suffix<ARM_MEMORY_SYSTEM_ARM920T>(cpu, instr);
}

static uint32_t cpuPrvLeanDecodeArm(uint32_t instr) {
    switch (cpuPrvLeanClassArm(instr)) {
        case LeanClass::plain: return cpuDecodeArm<ARM_MEMORY_SYSTEM_ARM920T>(instr);
        case LeanClass::check: return cpuPrvCompressExecFn(execFn_leanCheckArm);
        default: return cpuPrvCompressExecFn(execFn_leanExit);
    }
}

static uint32_t cpuPrvLeanDecodeThumb(uint16_t raw, uint32_t &translated) {
    translated = raw;
    switch (cpuPrvLeanClassThumb(raw)) {
        case LeanClass::plain: return cpuDecodeThumb<ARM_MEMORY_SYSTEM_ARM920T>(raw, translated);
        case LeanClass::thumbBx:
            return ((raw >> 3) & 15) == 15 ? cpuPrvCompressExecFn(execFn_leanThumbBx<true>)
                                           : cpuPrvCompressExecFn(execFn_leanThumbBx<false>);
        case LeanClass::blPrefix: return cpuPrvCompressExecFn(execFn_leanBlPrefix);
        case LeanClass::blSuffix: return cpuPrvCompressExecFn(execFn_leanBlSuffix);
        default: return cpuPrvCompressExecFn(execFn_leanExit);
    }
}

template <int size>
static FORCE_INLINE void cpuPrvLeanDecodeSlot(struct icache *ic, struct icacheline *line, unsigned offset,
                                              uint32_t &instr, uint32_t &decoded, uint32_t &cost) {
    if constexpr (size == 4) {
        memcpy(&instr, line->data + offset, 4);
        decoded = cpuPrvLeanDecodeArm(instr);
        line->decoded[offset >> 1] = (decoded << DECODED_BITS_SHIFT) | DECODED_BITS_ARM;
        line->cost[offset >> 1] = icacheCostOf(ic, instr, false);
    } else {
        uint16_t raw;
        memcpy(&raw, line->data + offset, 2);
        decoded = cpuPrvLeanDecodeThumb(raw, instr);
        line->decoded[offset >> 1] = (decoded << DECODED_BITS_SHIFT) | DECODED_BITS_THUMB;
        line->translatedThumbInstructions[offset >> 1] = instr;
        line->cost[offset >> 1] = icacheCostOf(ic, raw, true);
    }
    cost = line->cost[offset >> 1];
}

// Notes that the decode cache holds code from the RAM line at pa. A newly noted page loses its lean TLB
// write entries, so every later store to it takes the slow path, which drops the decoded code when the
// store lands on a noted line.
static void cpuPrvLeanNoteCode(struct ArmCpu *cpu, uint32_t pa) {
    if (cpu->leanCode->note(pa)) hostTlbFlush(&cpu->mem->hostTlb);
}

// Fills the decode cache line for va from the host page at pa, as the ARM920T I-cache would fill its line.
static struct icacheline *cpuPrvLeanFillLine(struct ArmCpu *cpu, uint32_t va, bool privileged, uint32_t pa,
                                             const uint8_t *page, bool ram) {
    struct icache *ic = cpu->ic;
    if (ram) cpuPrvLeanNoteCode(cpu, pa);
    struct icacheline *line = ic->cache + calculateIndex(va);
    const uint8_t *data = page + (pa & 0xfe0u);
    // Decoding depends only on the word, so slots whose words are unchanged keep their decode.
    for (unsigned i = 0; i < sizeof(line->data); i += 4) {
        if (memcmp(line->data + i, data + i, 4)) {
            line->decoded[i >> 1] = 0;
            line->decoded[(i >> 1) + 1] = 0;
            memcpy(line->data + i, data + i, 4);
        }
    }
    line->revision = ic->revision;
    line->tag = calculateTag(va);
    line->fetchKey = icacheFetchKey(ic, privileged);
    ic->epoch++;
    return line;
}

// The guarded step's fetch in Fast: the word from the decode cache, filling its line from a plain page
// where the I-cache would cache it, or straight from a plain page where it wouldn't. False for anything
// else (faults, pages that aren't plain), which the I-cache model then reads uncached.
static bool cpuPrvLeanStepWord(void *ctx, uint32_t va, unsigned size, bool privileged, uint32_t *word) {
    struct ArmCpu *cpu = (struct ArmCpu *)ctx;
    struct icache *ic = cpu->ic;
    struct icacheline *line = ic->cache + calculateIndex(va);
    if (!(line->revision == ic->revision && line->tag == calculateTag(va) &&
          line->fetchKey == icacheFetchKey(ic, privileged))) {
        struct ArmMmu *mmu = cpu->memorySystem.mmu;
        const MMUTranslateResult r = mmuTranslate(mmu, va, privileged, false);
        if (!MMU_TRANSLATE_RESULT_OK(r)) return false;
        const uint32_t pa = MMU_TRANSLATE_RESULT_PA(r);
        bool ram = false;
        const uint8_t *page = cpu->leanPage(cpu->leanContext, pa & ~0xfffu, false, &ram);
        if (!page) return false;
        if (!ic->arm920Enabled || (mmuIsOn(mmu) && !MMU_TRANSLATE_RESULT_CACHEABLE(r))) {
            if (size == 2) {
                uint16_t half;
                memcpy(&half, page + (pa & 0xfffu), 2);
                *word = half;
            } else {
                memcpy(word, page + (pa & 0xfffu), 4);
            }
            return true;
        }
        line = cpuPrvLeanFillLine(cpu, va, privileged, pa, page, ram);
    }
    if (size == 2) {
        uint16_t half;
        memcpy(&half, line->data + calculateLineIndex(va), 2);
        *word = half;
    } else {
        memcpy(word, line->data + calculateLineIndex(va), 4);
    }
    return true;
}

// A fetch the decode cache can't answer: translate as the ARM920T I-cache does, and fill the line from the
// host page when Exact's I-cache would cache it. Uncached fetches are read and decoded every time, so they
// see memory as Exact does. False (nothing run) for anything that isn't a plain page or faults.
template <int size>
static __attribute__((noinline)) bool cpuPrvLeanFetchSlow(struct ArmCpu *cpu, uint32_t pc, uint32_t &instr,
                                                         uint32_t &decoded, uint32_t &cost) {
    struct icache *ic = cpu->ic;
    if ((pc & (size - 1)) || icacheArm920ContinuationPending(ic) || !cpu->leanPage) return false;
    struct ArmMmu *mmu = cpu->memorySystem.mmu;
    const MMUTranslateResult r = mmuTranslate(mmu, pc, cpu->privileged, false);
    if (!MMU_TRANSLATE_RESULT_OK(r)) return false;
    const uint32_t pa = MMU_TRANSLATE_RESULT_PA(r);
    bool ram = false;
    const uint8_t *page = cpu->leanPage(cpu->leanContext, pa & ~0xfffu, false, &ram);
    if (!page) return false;
    if (!ic->arm920Enabled || (mmuIsOn(mmu) && !MMU_TRANSLATE_RESULT_CACHEABLE(r))) {
        if constexpr (size == 4) {
            memcpy(&instr, page + (pa & 0xfffu), 4);
            decoded = cpuPrvLeanDecodeArm(instr);
            cost = icacheCostOf(ic, instr, false);
        } else {
            uint16_t raw;
            memcpy(&raw, page + (pa & 0xfffu), 2);
            decoded = cpuPrvLeanDecodeThumb(raw, instr);
            cost = icacheCostOf(ic, raw, true);
        }
        return true;
    }
    struct icacheline *line = cpuPrvLeanFillLine(cpu, pc, cpu->privileged, pa, page, ram);
    cpuPrvLeanDecodeSlot<size>(ic, line, calculateLineIndex(pc), instr, decoded, cost);
    return true;
}

template <int size>
static FORCE_INLINE bool cpuPrvLeanFetch(struct ArmCpu *cpu, uint32_t pc, uint32_t &instr, uint32_t &decoded,
                                         uint32_t &cost) {
    // Lines are by MVA, as the I-cache model keeps them (icacheArm920Word).
    pc = cpuPrvMva(cpu, pc);
    struct icache *ic = cpu->ic;
    struct icacheline *line = ic->cache + calculateIndex(pc);
    if (likely(!(pc & (size - 1)) && line->revision == ic->revision && line->tag == calculateTag(pc) &&
               line->fetchKey == icacheFetchKey(ic, cpu->privileged))) {
        const unsigned offset = calculateLineIndex(pc);
        const uint32_t cached = line->decoded[offset >> 1];
        constexpr uint32_t kind = size == 4 ? DECODED_BITS_ARM : DECODED_BITS_THUMB;
        if (likely((cached & DECODED_BITS) == kind)) {
            decoded = int32_t(cached) >> DECODED_BITS_SHIFT;
            if constexpr (size == 4) memcpy(&instr, line->data + offset, 4);
            else instr = line->translatedThumbInstructions[offset >> 1];
            cost = line->cost[offset >> 1];
            return true;
        }
        cpuPrvLeanDecodeSlot<size>(ic, line, offset, instr, decoded, cost);
        return true;
    }
    return cpuPrvLeanFetchSlow<size>(cpu, pc, instr, decoded, cost);
}

static FORCE_INLINE bool cpuPrvCodeHookAt(const struct ArmCpu *cpu, uint32_t pc) {
    for (uint32_t i = 0; i < cpu->codeHookCount; ++i)
        if (cpu->codeHooks[i] == pc) return true;
    return false;
}

// Runs while the clocks charged are under the budget and the instructions under the count. An instruction
// whose charge would pass the budget is left for the step, so the run never passes it.
template <int size>
static uint32_t cpuPrvLeanLoop(struct ArmCpu *cpu) {
    uint32_t done = 0;
    while (cpu->leanDone < cpu->leanBudget && cpu->leanRetired < cpu->leanCount) {
        const uint32_t pc = cpu->regs[REG_NO_PC];
        uint32_t instr, decoded, cost;
        if (unlikely(cpu->codeHookCount) && cpuPrvCodeHookAt(cpu, pc)) break;
        cpu->curInstrPC = pc;
        if (!cpuPrvLeanFetch<size>(cpu, pc, instr, decoded, cost)) break;
        const uint32_t charge = cpuPrvSlotCharge(cpu, cost);
        if (charge > cpu->leanBudget - cpu->leanDone) break;
        cpu->leanCharge = charge;
        cpu->regs[REG_NO_PC] = pc + size;
        cpu->leanInstr = instr;
        // Accesses of an instruction the step runs again are counted there, not here.
        const uint64_t bus = cpu->leanBus;
        cpuPrvDecompressExecFn(decoded)(cpu, instr);
        if (unlikely(cpu->leanExit)) {
            cpu->regs[REG_NO_PC] = pc;
            cpu->leanBus = bus;
            break;
        }
        cpu->leanDone += charge;
        ++cpu->leanRetired;
        ++done;
        if (cpu->slowPath) break;
    }
    return done;
}

uint32_t cpuArm920RunLean(ArmCpu *cpu, uint32_t budget, uint32_t *blNext, uint64_t *busAccesses, uint32_t *clocks) {
    return cpuArm920RunLeanFor(cpu, budget, UINT32_MAX, blNext, busAccesses, clocks);
}

uint32_t cpuArm920RunLeanFor(ArmCpu *cpu, uint32_t budget, uint32_t count, uint32_t *blNext, uint64_t *busAccesses,
                             uint32_t *clocks) {
    *blNext = ~0u;
    *busAccesses = 0;
    if (clocks) *clocks = 0;
    if (cpu->model != ArmCpuModel::arm920t || !cpu->leanPage) return 0;
    cpuClearSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE | SLOW_PATH_REASON_RESCHEDULE |
                              SLOW_PATH_REASON_PACE_SYSCALL_BREAK | SLOW_PATH_REASON_INJECTED_CALL_DONE);
    if (cpu->slowPath || cpu->arm920Waiting || cpu->modePace || cpu->nativeCallDepth || cpu->nativePc[0] ||
        cpu->debugStub || icacheArm920ContinuationPending(cpu->ic))
        return 0;
    cpu->leanRun = true;
    cpu->leanExit = false;
    cpu->leanBlNext = ~0u;
    cpu->leanBus = 0;
    cpu->leanDone = 0;
    cpu->leanRetired = 0;
    cpu->leanCharge = 1;
    cpu->leanBudget = budget;
    cpu->leanCount = count;
    uint32_t done = 0;
    for (;;) {
        done += cpu->T ? cpuPrvLeanLoop<2>(cpu) : cpuPrvLeanLoop<4>(cpu);
        // Carry on only past an ARM/Thumb switch; a failed fetch leaves no slow path reason.
        if (cpu->leanDone >= cpu->leanBudget || done >= count || cpu->leanExit || cpu->slowPath != SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE) break;
        cpuClearSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE);
    }
    cpu->leanRun = false;
    cpu->leanExit = false;
    *blNext = cpu->leanBlNext;
    *busAccesses = cpu->leanBus;
    if (clocks) *clocks = cpu->leanDone;
    return done;
}

bool cpuArm920AlignmentFaults(const ArmCpu *cpu) {
    return cpu->model == ArmCpuModel::arm920t && cp15MMUAlignmentEnabled(cpu->cp15.cp15mmu);
}

void cpuArm920SetLean(ArmCpu *cpu, Arm920LeanPage page, Arm920LeanGuard guard, void *context) {
    if (cpu->model != ArmCpuModel::arm920t) return;
    for (auto &e : cpu->leanDeviceWrite) e = {};
    cpu->leanPage = page;
    cpu->leanGuard = guard;
    cpu->leanContext = context;
    cp15MMUArm920DataCache(cpu->cp15.cp15mmu)->lean = page != nullptr;
    if (page && !cpu->leanCode) {
        cpu->leanCode = new Arm920CodeMap;
    }
    icacheArm920SetLeanWord(cpu->ic, page ? cpuPrvLeanStepWord : nullptr, cpu);
    hostTlbFlush(&cpu->mem->hostTlb);
}

void cpuArm920LeanReset(ArmCpu *cpu) {
    if (cpu->model != ArmCpuModel::arm920t) return;
    auto *dc = cp15MMUArm920DataCache(cpu->cp15.cp15mmu);
    dc->mem = cpu->mem;
    for (auto &segment : dc->lines)
        for (auto &line : segment) dc->clean(line);
    dc->invalidate();
    icacheInval(cpu->ic);
    if (cpu->leanCode) cpu->leanCode->clear();
    hostTlbFlush(&cpu->mem->hostTlb);
}

void cpuArm920LeanFlush(ArmCpu *cpu) { hostTlbFlush(&cpu->mem->hostTlb); }

void cpuArm920SetLeanRead(ArmCpu *cpu, Arm920LeanRead read) { cpu->leanRead = read; }

void cpuArm920SetLeanWrite(ArmCpu *cpu, Arm920LeanWrite write) { cpu->leanWrite = write; }

bool cpuArm920LeanInJit(const ArmCpu *cpu) { return cpu->leanJit; }

// A DMA or other write over the whole page at pa.
void cpuArm920LeanStored(ArmCpu *cpu, uint32_t pa) { cpuPrvArm920Stored(cpu, pa & ~0xfffu, 0x1000); }

void cpuArm920LeanStoredBytes(ArmCpu *cpu, uint32_t pa, uint32_t size) {
    if (!size) size = 1;
    while (size) {
        const uint32_t room = 0x1000 - (pa & 0xfffu), part = size < room ? size : room;
        cpuPrvArm920Stored(cpu, pa, part);
        pa += part;
        size -= part;
    }
}

uint32_t cpuArm920LeanGeneration(const ArmCpu *cpu) { return cpu->mem->hostTlb.gen; }

uint64_t cpuArm920LeanCodeDrops(const ArmCpu *cpu) { return cpu->leanCodeDrops; }

uint32_t cpuArm920TakeJitAccessPc(ArmCpu *cpu) { return std::exchange(cpu->jitAccessPc, 0); }

void cpuArm920JitCharge(ArmCpu *cpu, uint32_t cycles, uint32_t span) {
    cpu->jitCharged += cycles;
    cpu->leanBudget = span;
}

uint32_t cpuArm920JitCharged(const ArmCpu *cpu) { return cpu->jitCharged; }

void cpuArm920SetJitAccessPcs(ArmCpu *cpu, const uint32_t *pcs, uint32_t count) {
    cpu->jitAccessPcs = pcs;
    cpu->jitAccessPcCount = count;
}

bool cpuArm920SetCodeHooks(ArmCpu *cpu, const uint32_t *pcs, uint32_t count) {
    if (cpu->model != ArmCpuModel::arm920t || count > ARM920_MAX_CODE_HOOKS) return false;
    bool same = count == cpu->codeHookCount;
    for (uint32_t i = 0; same && i < count; ++i) same = cpu->codeHooks[i] == (pcs[i] & ~1u);
    if (same) return true;
    for (uint32_t i = 0; i < count; ++i) cpu->codeHooks[i] = pcs[i] & ~1u;
    cpu->codeHookCount = count;
#ifdef FROND_DYNARMIC
    // Translated code may run through a new hook's pc; it is compiled again with the hooks left out.
    if (cpu->jit && cpu->jitBlocks) jitClear(cpu->jit);
#endif
    return true;
}

bool cpuArm920CodeHookAt(const ArmCpu *cpu, uint32_t pc) {
    return cpu->codeHookCount && cpuPrvCodeHookAt(cpu, pc & ~1u);
}

bool cpuArm920PeekWord(ArmCpu *cpu, uint32_t va, uint32_t *word, uint32_t *pa) {
    // The lean and JIT loads' translation, with the current mode's permissions, of the MVA.
    const MMUTranslateResult r = mmuTranslateCached<false>(cpu->memorySystem.mmu, cpuPrvMva(cpu, va & ~3u), cpu->privileged);
    if (!MMU_TRANSLATE_RESULT_OK(r)) return false;
    *pa = MMU_TRANSLATE_RESULT_PA(r);
    if (!word) return true;
    bool counted;
    const uint8_t *page = cpu->leanPage ? cpu->leanPage(cpu->leanContext, *pa & ~0xfffu, false, &counted) : nullptr;
    if (!page) return false;
    memcpy(word, page + (*pa & 0xfffu), 4);
    return true;
}
#else
void cpuArm920SetJitAccessPcs(ArmCpu *, const uint32_t *, uint32_t) {}
bool cpuArm920SetCodeHooks(ArmCpu *, const uint32_t *, uint32_t count) { return !count; }
bool cpuArm920CodeHookAt(const ArmCpu *, uint32_t) { return false; }
uint32_t cpuArm920TakeJitAccessPc(ArmCpu *) { return 0; }
void cpuArm920JitCharge(ArmCpu *, uint32_t, uint32_t) {}
uint32_t cpuArm920JitCharged(const ArmCpu *) { return 0; }
bool cpuArm920PeekWord(ArmCpu *, uint32_t, uint32_t *, uint32_t *) { return false; }
uint32_t cpuArm920RunLean(ArmCpu *, uint32_t, uint32_t *blNext, uint64_t *busAccesses) {
    *blNext = ~0u;
    *busAccesses = 0;
    return 0;
}
uint32_t cpuArm920RunLeanFor(ArmCpu *, uint32_t, uint32_t, uint32_t *blNext, uint64_t *busAccesses) {
    *blNext = ~0u;
    *busAccesses = 0;
    return 0;
}
void cpuArm920SetLean(ArmCpu *, Arm920LeanPage, Arm920LeanGuard, void *) {}
void cpuArm920LeanReset(ArmCpu *) {}
void cpuArm920LeanFlush(ArmCpu *) {}
void cpuArm920LeanStored(ArmCpu *, uint32_t) {}
void cpuArm920LeanStoredBytes(ArmCpu *, uint32_t, uint32_t) {}
void cpuArm920SetLeanRead(ArmCpu *, Arm920LeanRead) {}
void cpuArm920SetLeanWrite(ArmCpu *, Arm920LeanWrite) {}
bool cpuArm920LeanInJit(const ArmCpu *) { return false; }
uint32_t cpuArm920LeanGeneration(const ArmCpu *) { return 0; }
uint64_t cpuArm920LeanCodeDrops(const ArmCpu *) { return 0; }
#endif

#ifdef FROND_DYNARMIC
// JIT glue. The JIT runs only outside injected calls, the debugger and native PC breaks,
// with the MMU, and only while no slow path reason is set.

// The JIT's accesses, for the XScale's Fast engine and the ARM920T's Fast blocks.
template <int size, bool write>
static FORCE_INLINE bool cpuPrvJitMemOp(ArmCpu *cpu, void *buf, uint32_t va, uint_fast8_t *fsr) {
    if (cpu->model == ArmCpuModel::arm920t)
        return cpuPrvMemOpEx<ARM_MEMORY_SYSTEM_ARM920T, size, write>(cpu, buf, va, cpu->privileged, fsr);
    return cpuPrvMemOpEx<ARM_MEMORY_SYSTEM_MMU, size, write, true>(cpu, buf, va, cpu->privileged, fsr);
}

// A device page from cpuPrvJitFill: the access goes straight to its region's handler, with the
// handler's result in *ok. 0 if it didn't (anything else, for cpuJitRead or cpuJitWrite), 1 if it
// did, 2 if it did and the page generation (cpuJitPageGen) is no longer gen.
static inline uint64_t cpuPrvJitPageGen(const ArmCpu *cpu) {
    const struct HostTlb &t = cpu->mem->hostTlb;
    return (uint64_t(t.epoch) << 32) | uint32_t(t.gen + t.gains);
}

// The io tables are keyed by the raw va, so they assume no FCSE PID remap.
#ifndef NO_SUPPORT_FCSE
#error "cpuPrvJitIo keys on the raw va; add the FCSE PID before the lookup"
#endif
template <bool write>
static FORCE_INLINE int cpuPrvJitIo(ArmCpu *cpu, uint32_t va, unsigned size, void *buf, bool *ok, uint64_t gen) {
    if ((va & (size - 1)) || cpu->nativeCallDepth || cpu->model == ArmCpuModel::arm920t) return 0;
    const struct HostTlb &t = cpu->mem->hostTlb;
    const struct HostTlbIoEntry &e = (write ? t.ioWrite : t.ioRead)[hostTlbIoIndex(va)];
    if (e.key != hostTlbKey(t.gen, va, cpu->privileged)) return 0;
    const uint32_t pa = va + e.delta;
    if (pa < e.pa || uint64_t(pa) + size > e.ub) return 0;
    if (cpu->costMemory) {
        // Fast at Original: the region charge, before the access as in cpuPrvMemOpExModel. A cached
        // region needs the page's C and B bits, which only the full path has.
        const cost::Region region = cost::pxaRegion(pa);
        if (cost::regionCached(region)) return 0;
        const uint8_t info = cpuPrvCostInfo(false, false, false, region);
        cpuPrvCostAccess(cpu, va, info, write);
        if (size == 8) cpuPrvCostAccess(cpu, va + 4, info, write);
    }
    *ok = e.aF(e.uD, pa, size, write, buf);
    return cpuPrvJitPageGen(cpu) == gen ? 1 : 2;
}

int cpuJitIoRead(ArmCpu *cpu, uint32_t va, unsigned size, void *buf, bool *ok, uint64_t gen) {
    return cpuPrvJitIo<false>(cpu, va, size, buf, ok, gen);
}

int cpuJitIoWrite(ArmCpu *cpu, uint32_t va, unsigned size, const void *buf, bool *ok, uint64_t gen) {
    return cpuPrvJitIo<true>(cpu, va, size, const_cast<void *>(buf), ok, gen);
}

bool cpuJitRead(ArmCpu *cpu, uint32_t va, unsigned size, void *buf, uint_fast8_t *fsr) {
    if (va & (size - 1)) {
        *fsr = 0;
        return false;
    }
    switch (size) {
        case 1: return cpuPrvJitMemOp<1, false>(cpu, buf, va, fsr);
        case 2: return cpuPrvJitMemOp<2, false>(cpu, buf, va, fsr);
        case 4: return cpuPrvJitMemOp<4, false>(cpu, buf, va, fsr);
        case 8: return cpuPrvJitMemOp<8, false>(cpu, buf, va, fsr);
    }
    return false;
}

// An ARM920T Fast blocks store to a page an earlier store sent to leanWrite: the same call, without the translation
// and the page lookups. 1 if leanWrite took it, -1 if it refused (an exit, as cpuJitWrite's refusal with fsr 0), 0 for
// cpuJitWrite. A refusal drops the page, so the next store looks it up again.
int cpuJitLeanDeviceWrite(ArmCpu *cpu, uint32_t va, unsigned size, uint32_t value, uint32_t ranNow) {
    if (!cpu->leanRun || !cpu->leanJit || cpu->nativeCallDepth || size > 4 || (va & (size - 1))) return 0;
    auto &e = cpu->leanDeviceWrite[(va >> 12) & 15];
    if (!(e.pa & 1) || e.key != hostTlbKey(cpu->mem->hostTlb.gen, va, cpu->privileged)) return 0;
    const uint32_t ran = ranNow > cpu->jitCharged ? ranNow - cpu->jitCharged : 0;
    const uint32_t left = ran + 1 < cpu->leanBudget ? cpu->leanBudget - ran - 1 : 0;
    uint32_t cost = 0;
    if (cpu->leanWrite(cpu->leanContext, (e.pa & ~0xfffu) | (va & 0xfffu), size, value, ran, left, &cost)) {
        cpu->leanBudget -= cost;
        ++cpu->leanBus;
        return 1;
    }
    e = {};
    cpu->leanExit = true;
    return -1;
}

bool cpuJitWrite(ArmCpu *cpu, uint32_t va, unsigned size, const void *buf, uint_fast8_t *fsr) {
    if (va & (size - 1)) {
        *fsr = 0;
        return false;
    }
    void *b = const_cast<void *>(buf);
    switch (size) {
        case 1: return cpuPrvJitMemOp<1, true>(cpu, b, va, fsr);
        case 2: return cpuPrvJitMemOp<2, true>(cpu, b, va, fsr);
        case 4: return cpuPrvJitMemOp<4, true>(cpu, b, va, fsr);
        case 8: return cpuPrvJitMemOp<8, true>(cpu, b, va, fsr);
    }
    return false;
}

// ARM920T Fast blocks: the lean TLB's page for va, live only for plain pages the MMU allows now in this mode.
// Its write entries are dropped whenever a page gets translated code (cpuPrvJitNoteArm920Code), and the slow
// path that fills one again only does so once the page holds no code (cpuPrvArm920Stored), so a writable
// page holds no code.
static uint8_t *cpuPrvJitArm920Page(ArmCpu *cpu, uint32_t va, bool write) {
    if (!cpu->jitBlocks) return nullptr;
    const struct HostTlb &t = cpu->mem->hostTlb;
    const struct HostTlbEntry &e = (write ? t.write : t.read)[(va >> 12) & (HOST_TLB_SIZE - 1)];
    if (e.key != hostTlbKey(t.gen, va, cpu->privileged)) return nullptr;
    return (uint8_t *)((e.addend & ~uintptr_t(1)) + (va & ~0xfffu));
}

uint8_t *cpuJitReadPage(ArmCpu *cpu, uint32_t va) {
    if (cpu->model == ArmCpuModel::arm920t) return cpuPrvJitArm920Page(cpu, va, false);
    const struct HostTlb &t = cpu->mem->hostTlb;
    const struct HostTlbEntry &e = t.read[(va >> 12) & (HOST_TLB_SIZE - 1)];
    if (e.key != hostTlbKey(t.gen, va, cpu->privileged)) return nullptr;
    return (uint8_t *)(e.addend + (va & ~0xfffu));
}

// Host address of the RAM page holding va if a store there would take the host TLB's
// fast path for the current privilege (so the MMU allows it in this mode with the current
// domains), marked dirty as ramDirectWritePage does. Null for anything else.
uint8_t *cpuJitWritePage(ArmCpu *cpu, uint32_t va) {
    if (cpu->model == ArmCpuModel::arm920t) return cpuPrvJitArm920Page(cpu, va, true);
    if (cpu->nativeCallDepth) return nullptr;
    const struct HostTlb &t = cpu->mem->hostTlb;
    const struct HostTlbEntry &e = t.write[(va >> 12) & (HOST_TLB_SIZE - 1)];
    if (!t.ram || e.key != hostTlbKey(t.gen, va, cpu->privileged)) return nullptr;
    return ramDirectWritePage(t.ram, va + uint32_t(e.addend));
}

// Code is read like icacheFetch reads it: ROM from its peephole copy, which holds the PACE
// entry and peephole patches the data side doesn't see. This is the host TLB's page for va,
// moved to the peephole copy for ROM.
static uint8_t *cpuPrvJitHostCodePage(ArmCpu *cpu, uint32_t va) {
    uint8_t *page = cpuJitReadPage(cpu, va);
    const struct ArmMemRegion &region = cpu->mem->regions[REGION_ROM];
    if (!page || !region.uD) return page;
    struct ArmRom *rom = (struct ArmRom *)region.uD;
    uint8_t *data = (uint8_t *)romGetData(rom);
    if (page < data || page >= data + romGetSize(rom)) return page;
    return (uint8_t *)romGetPeepholeBuffer(rom) + (page - data);
}

bool cpuJitFetch(ArmCpu *cpu, uint32_t va, uint32_t *word) {
    va &= ~3u;
    if (cpu->model == ArmCpuModel::arm920t) {
        if (cpu->jitBlocks) {
            // Plain pages only, straight from memory: Fast keeps no I-cache contents. The MMU sees the MVA.
            const MMUTranslateResult r = mmuTranslate(cpu->memorySystem.mmu, cpuPrvMva(cpu, va), cpu->privileged, false);
            if (!MMU_TRANSLATE_RESULT_OK(r) || !cpu->leanPage) return false;
            bool counted;
            const uint32_t pa = MMU_TRANSLATE_RESULT_PA(r);
            const uint8_t *page = cpu->leanPage(cpu->leanContext, pa & ~0xfffu, false, &counted);
            if (!page) return false;
            memcpy(word, page + (pa & 0xfffu), 4);
            return true;
        }
        uint_fast8_t fsr;
        return icacheArm920Word(cpu->ic, va, cpu->privileged, &fsr, word);
    }
    if (const uint8_t *page = cpuPrvJitHostCodePage(cpu, va)) {
        memcpy(word, page + (va & 0xfffu), 4);
        return true;
    }
    const MMUTranslateResult r = mmuTranslateCached<false>(cpu->memorySystem.mmu, va, cpu->privileged);
    if (!MMU_TRANSLATE_RESULT_OK(r)) return false;
    if (cpu->jitMemoryOnly && !memIsMemory(cpu->mem, MMU_TRANSLATE_RESULT_PA(r), 4)) {
        cpu->jitRefused = true;
        return false;
    }
    return memInstructionFetch<4>(cpu->mem, MMU_TRANSLATE_RESULT_PA(r), word);
}

void cpuJitMemoryOnly(ArmCpu *cpu, bool on) {
    cpu->jitMemoryOnly = on;
    if (on) cpu->jitRefused = false;
}

bool cpuJitRefused(const ArmCpu *cpu) { return cpu->jitRefused; }

bool cpuJitCodePa(ArmCpu *cpu, uint32_t va, uint32_t *pa, uint32_t *access) {
    const MMUTranslateResult r = mmuTranslateCodeAccess(cpu->memorySystem.mmu, va, access);
    if (!MMU_TRANSLATE_RESULT_OK(r)) return false;
    *pa = MMU_TRANSLATE_RESULT_PA(r);
    return true;
}

bool cpuJitAccessReadable(ArmCpu *cpu, uint32_t access) {
    return mmuAccessReadable(cpu->memorySystem.mmu, access, cpu->privileged);
}

uint64_t cpuJitPermContext(ArmCpu *cpu) {
    ArmMmu *mmu = cpu->memorySystem.mmu;
    return uint64_t(mmuGetDomainCfg(mmu)) | (uint64_t(mmuGetS(mmu)) << 32) | (uint64_t(mmuGetR(mmu)) << 33);
}

// A store to a flagged RAM line of the page: drop the JIT's blocks it overlaps, any fetch buffer it hit
// and the idle, delay and poll matches on the page. With JIT code on the page the flags stay until
// jitRun sets them from the code left (cpuJitSetCodeLines); otherwise they're cleared now and the fetch
// buffers left are watched again.
static void cpuPrvJitCodeWritten(void *ctx, uint8_t *page, uint8_t *flags, uint32_t offset, uint32_t size) {
    ArmCpu *cpu = (ArmCpu *)ctx;
    cpuPrvIdleForget(cpu, page);
    icacheDropFetchedHost(cpu->ic, page + offset, size);
    if (cpu->jit && jitCodeWritten(cpu->jit, page, offset, size)) return;
    memset(flags, 0, 0x1000 >> 6);
    icacheWatchFetchBuffers(cpu->ic);
}

void cpuJitSetCodeLines(ArmCpu *cpu, uint8_t *hostPage, uint8_t *flags, uint64_t lines) {
    for (unsigned i = 0; i < (0x1000 >> 6); i++) flags[i] = (lines >> i) & 1;
    // A flag set here may be on a line no compile flagged since the page was last made writable.
    if (lines) jitWriteProtectPage(cpu->jit, hostPage);
    cpuPrvIdleForget(cpu, hostPage);
    icacheWatchFetchBuffers(cpu->ic);
}

void cpuJitSetArm920CodeWords(ArmCpu *cpu, uint32_t pa, const uint64_t *words) {
    if (!cpu->jitCode || !cpu->jitCode->page(pa)) return;
    uint64_t any = 0;
    for (unsigned i = 0; i < 16; i++) any |= words[i];
    if (any) cpu->jitCode->setWords(pa, words);
    else cpu->jitCode->clearPage(pa);
}

// Fast only. The fetch buffers keep a line's old words until the guest invalidates, as an
// XScale does, and Exact runs them. Fast drops a buffer when a store hits its line instead:
// the JIT runs code past a buffered line without moving the buffers on, so the old words
// would otherwise run long after Exact had refetched. Flags the line's code flag, which
// every store to RAM checks (interpreter, the JIT's callbacks, DMA, ramMarkDirty), and sends
// the JIT's direct stores to the page back through the callbacks.
static const uint8_t *cpuPrvJitWatchFetch(void *ctx, uint32_t pa) {
    ArmCpu *cpu = (ArmCpu *)ctx;
    uint8_t *lines;
    uint8_t *page = memWatchRamCode(cpu->mem, pa, cpuPrvJitCodeWritten, cpu, &lines);
    if (!page) return nullptr;
    uint8_t &flag = lines[(pa & 0xfffu) >> 6];
    if (!flag) {
        flag = 1;
        jitWriteProtectPage(cpu->jit, page);
    }
    return page + (pa & 0xfffu);
}

uint8_t *cpuJitWatchCode(ArmCpu *cpu, uint32_t pa, uint8_t **lines) {
    *lines = nullptr;
    if (cpu->model == ArmCpuModel::arm920t && cpu->jitBlocks) {
        // ARM920T Fast blocks: a RAM page gets its jitCode bit, and a newly marked one loses its lean TLB
        // entries (so the JIT's page table empties too), so every later store to it is checked against the
        // lines translation read (cpuJitNoteArm920Code).
        // The single-step JIT (no blocks) watches RAM lines like the other cores.
        if (!cpu->leanPage) return nullptr;
        bool ram = false;
        uint8_t *page = cpu->leanPage(cpu->leanContext, pa & ~0xfffu, false, &ram);
        if (page && ram && cpu->jitCode->note(pa, false)) hostTlbFlush(&cpu->mem->hostTlb);
        return page;
    }
    if (uint8_t *page = memWatchRamCode(cpu->mem, pa, cpuPrvJitCodeWritten, cpu, lines)) return page;
    const struct ArmMemRegion &region = cpu->mem->regions[REGION_ROM];
    if (!region.ub || !region.uD || pa < region.pa || pa >= region.ub) return nullptr;
    struct ArmRom *rom = (struct ArmRom *)region.uD;
    uint8_t *data = (uint8_t *)romResolveAddress(rom, pa & ~0xfffu, 0x1000);
    if (!data) return nullptr;
    return (uint8_t *)romGetPeepholeBuffer(rom) + (data - (uint8_t *)romGetData(rom));
}

void cpuJitNoteArm920Code(ArmCpu *cpu, uint32_t pa) {
    if (cpu->jitCode && cpu->jitCode->page(pa)) cpu->jitCode->noteWord(pa);
}

void cpuJitForgetCode(ArmCpu *cpu) {
    // The code lines' flags go, so stores to an idle loop's lines would no longer be seen.
    cpuPrvIdleForget(cpu);
    if (cpu->jitCode) cpu->jitCode->clear();
    memForgetRamCode(cpu->mem);
    icacheWatchFetchBuffers(cpu->ic);
}

uint32_t cpuJitMapGen(ArmCpu *cpu) { return mmuFlushCount(cpu->memorySystem.mmu); }

// The host TLB's keys hold the privilege, so its generation alone covers both levels. A
// domain change that only adds rights keeps the host TLB, so gains are counted in too.
uint64_t cpuJitPageGen(ArmCpu *cpu) { return cpuPrvJitPageGen(cpu); }

int cpuJitPageClientAccess(ArmCpu *cpu, uint32_t va, bool privileged) {
    if (cpu->model == ArmCpuModel::arm920t) va = cpuPrvMva(cpu, va);
    return mmuPageClientAccess(cpu->memorySystem.mmu, va, privileged);
}

bool cpuJitSlowPath(ArmCpu *cpu) { return cpu->slowPath != 0; }

uint32_t *cpuJitSpsr(ArmCpu *cpu) { return &cpu->SPSR; }

// Generated code loads and stores these as whole little-endian words.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
static_assert(offsetof(ArmCpu, waitingIrqs) % 4 == 0 && offsetof(ArmCpu, waitingFiqs) == offsetof(ArmCpu, waitingIrqs) + 2);
static_assert(offsetof(ArmCpu, Q) % 4 == 0 && offsetof(ArmCpu, I) == offsetof(ArmCpu, Q) + 2 &&
              offsetof(ArmCpu, F) == offsetof(ArmCpu, Q) + 3 && sizeof(bool) == 1);
uint32_t *cpuJitWaitingWord(ArmCpu *cpu) { return reinterpret_cast<uint32_t *>(&cpu->waitingIrqs); }
uint32_t *cpuJitInterruptMaskWord(ArmCpu *cpu) { return reinterpret_cast<uint32_t *>(&cpu->Q); }

uint32_t *cpuJitAccessPcSlot(ArmCpu *cpu) { return &cpu->jitAccessPc; }

bool cpuJitAccessPcMarked(const ArmCpu *cpu, uint32_t pc) {
    for (uint32_t i = 0; i < cpu->jitAccessPcCount; ++i)
        if (cpu->jitAccessPcs[i] == pc) return true;
    return false;
}

bool cpuJitPrivileged(ArmCpu *cpu) { return cpu->privileged; }

Pxa270wmmx *cpuJitWmmx(ArmCpu *cpu) { return cpu->wmmx; }

bool cpuJitWmmxCompiles(ArmCpu *cpu, uint32_t word) { return cpu->jit && jitWmmxCompiles(cpu->jit, word); }

void cpuJitIcacheInvalLine(ArmCpu *cpu, uint32_t va) { icacheInvalAddr(cpu->ic, va); }

// MRC and MCR p15, 0, Rd, c3: what cp15prvCoprocRegXferFunc does for CRn 3, any CRm and op2.
uint64_t cpuJitDomainCalls() { return jitDomainCalls(); }
uint64_t cpuJitCpsrInterprets() { return jitPsrInterprets(); }
uint32_t cpuJitGetDomain(ArmCpu *cpu) { return mmuGetDomainCfg(cpu->memorySystem.mmu); }
void cpuJitSetDomain(ArmCpu *cpu, uint32_t value) { mmuSetDomainCfg(cpu->memorySystem.mmu, value); }

// Rd goes through cpu->regs as execFn_cp_dp passes it, and is put back after.
bool cpuJitCp15Xfer(ArmCpu *cpu, uint32_t word, uint32_t *value) {
    const struct ArmCoprocessor &cp = cpu->coproc[15];
    const uint8_t rd = (word >> 12) & 15;
    const uint32_t saved = cpu->regs[rd];
    cpu->regs[rd] = *value;
    const bool ok = cp.regXfer && cp.regXfer(cpu, cp.userData, false, !!(word & (1u << 20)), 0, rd,
                                             (word >> 16) & 15, word & 15, (word >> 5) & 7);
    *value = cpu->regs[rd];
    cpu->regs[rd] = saved;
    return ok;
}

uint32_t cpuJitSwi(ArmCpu *cpu, uint32_t *regs, uint32_t cpsr) {
    cpu->flags = cpsr & 0xf0000000UL;
    cpu->Q = !!(cpsr & ARM_SR_Q);
    cpu->T = !!(cpsr & ARM_SR_T);
    memcpy(cpu->regs + 8, regs + 8, 7 * sizeof(uint32_t));
    cpu->curInstrPC = regs[REG_NO_PC] - (cpu->T ? 2 : 4);
    cpuPrvException(cpu, cpu->vectorBase + ARM_VECTOR_OFFT_SWI, regs[REG_NO_PC], ARM_SR_MODE_SVC | ARM_SR_I);
    memcpy(regs + 8, cpu->regs + 8, 8 * sizeof(uint32_t));
    return cpuPrvMaterializeCPSR(cpu);
}

static uint64_t jitCpsrWrites;
uint64_t cpuJitCpsrWrites() { return jitCpsrWrites; }

uint32_t cpuJitSetCpsr(ArmCpu *cpu, uint32_t *regs, uint32_t cpsr, uint32_t mask, uint32_t value) {
    ++jitCpsrWrites;
    cpu->flags = cpsr & 0xf0000000UL;
    cpu->Q = !!(cpsr & ARM_SR_Q);
    cpu->T = !!(cpsr & ARM_SR_T);
    memcpy(cpu->regs + 8, regs + 8, 7 * sizeof(uint32_t));
    cpuPrvSetPSR<false>(cpu, mask, value);
    memcpy(regs + 8, cpu->regs + 8, 7 * sizeof(uint32_t));
    return cpuPrvMaterializeCPSR(cpu);
}

static uint64_t jitExceptionReturns;
uint64_t cpuJitExceptionReturns() { return jitExceptionReturns; }

uint32_t cpuJitExceptionReturn(ArmCpu *cpu, uint32_t *regs, uint32_t cpsr, uint32_t target) {
    ++jitExceptionReturns;
    cpu->flags = cpsr & 0xf0000000UL;
    cpu->Q = !!(cpsr & ARM_SR_Q);
    cpu->T = !!(cpsr & ARM_SR_T);
    memcpy(cpu->regs + 8, regs + 8, 7 * sizeof(uint32_t));
    const uint32_t sr = cpu->SPSR;
    cpuPrvSetPSRlo8(cpu, sr);
    cpuPrvSetPSRhi8(cpu, sr);
    memcpy(regs + 8, cpu->regs + 8, 7 * sizeof(uint32_t));
    regs[REG_NO_PC] = target;
    return cpuPrvMaterializeCPSR(cpu);
}

// ARM encodings (or Thumb ones Frond translates to ARM) that dynarmic would get wrong
// for a privileged XScale, or that Frond gives special meaning.
static bool cpuPrvJitPlainArm(uint32_t i, bool thumb) {
    const uint32_t cond = i >> 28;
    if (cond == 0xf) {
        const uint32_t op = (i >> 24) & 0xf;
        if (op == 10 || op == 11) return true;                                  // BLX imm
        if ((op == 5 || op == 7) && (i & 0x0D70F000UL) == 0x0550F000UL) return true;  // PLD
        return false;  // coprocessor 2 forms, PACE, peepholes, undefined
    }
    if (thumb ? cpuPrvDecoderArm<ARM_MEMORY_SYSTEM_MMU, true>(i) ==
                    PREFIX_EXEC_FN(execFn_invalid<ARM_MEMORY_SYSTEM_MMU, true>)
              : cpuPrvDecoderArm<ARM_MEMORY_SYSTEM_MMU, false>(i) ==
                    PREFIX_EXEC_FN(execFn_invalid<ARM_MEMORY_SYSTEM_MMU, false>))
        return false;
    // MRS and MSR: the JIT's psr_coprocessor runs the SPSR and CPSR control forms.
    if ((i & 0x0fb000f0) == 0x01000000) return (i & 0x000f0fff) == 0x000f0000 && ((i >> 12) & 15) != 15;
    if ((i & 0x0fb0f0f0) == 0x0120f000) return !(i & 0xf00) && (i & 15) != 15 && (i & 0x000f0000);
    if ((i & 0x0fb0f000) == 0x0320f000) return (i & 0x000f0000) != 0;
    if ((i & 0x0ff000f0) == 0x01200070) return false;                           // BKPT
    // The JIT's p15 slot (jitCp15SlotWord), Rd not PC.
    if (jitCp15SlotWord(i)) return !thumb && ((i >> 12) & 15) != 15;
    // iWMMXt CDP, MCR, MRC, MCRR, MRRC, LDC and STC: the JIT's cp0 and cp1 slots, on a CPU
    // that has iWMMXt with the coprocessor's CPAR bit set (checked in cpuJitPlain, outside
    // the memo).
    if ((i & 0x0c000000) == 0x0c000000 && (i & 0x0f000000) != 0x0f000000 && ((i >> 8) & 14) == 0 && !thumb)
        return pxa270wmmxJitValid(i);
    // SWI, but not the semihosting ones execFn_swi answers itself. Only unconditional ones,
    // so each ends the run as a step through the interpreter did, even where the condition
    // would fail.
    if ((i & 0x0f000000) == 0x0f000000) return (i >> 28) == 0xe && (i & 0x00ffffff) != (thumb ? 0xabu : 0x123456u);
    if ((i & 0x0c000000) == 0x0c000000) return false;                           // coprocessor
    if ((i & 0x0e000000) == 0x08000000 && (i & (1 << 22))) return false;        // LDM^, STM^
    if ((i & 0x0d200000) == 0x04200000 && !(i & (1 << 24))) return false;       // LDRT, STRT...
    if ((i & 0x0c100000) == 0x04100000 && !(i & (1 << 22))) {                   // LDR word
        const uint32_t rn = (i >> 16) & 15, rd = (i >> 12) & 15;
        if ((rn == 9 && rd == 12) || (rn == 12 && rd == 15)) return false;      // syscall dispatch
    }
    if ((i & 0x0c000000) == 0 && ((i >> 12) & 15) == 15 && (i & (1 << 20))) {
        const uint32_t op = (i >> 21) & 15;
        const bool extra = !(i & (1 << 25)) && (i & 0x90) == 0x90;
        if (!extra && (op < 8 || op > 11)) {                                    // S with PC
            // MOVS PC, Rm and SUBS PC, Rn, #imm run through the psr_coprocessor.
            if ((i & 0x0ffffff0) == 0x01b0f000) return (i & 15) != 15;
            if ((i & 0x0ff0f000) == 0x0250f000) return ((i >> 16) & 15) != 15;
            return false;
        }
    }
    return true;
}

// cpuPrvJitPlainArm depends only on the word and the Thumb flag, so its answers are kept
// in a small direct-mapped table shared by every CPU. An entry holds the word in its top
// half and, in its low bits, whether the ARM (bits 0-1) and Thumb (bits 2-3) answers are
// known and plain. Entries are single 64-bit words, so threads racing on one only lose an
// answer, never mix two words.
static std::atomic<uint64_t> cpuPrvJitPlainMemo[1024];

static bool cpuPrvJitPlainArmMemo(uint32_t i, bool thumb) {
    std::atomic<uint64_t> &slot = cpuPrvJitPlainMemo[(i * 0x9e3779b1u) >> 22];
    uint64_t e = slot.load(std::memory_order_relaxed);
    const unsigned shift = thumb ? 2 : 0;
    if (uint32_t(e >> 32) != i) e = uint64_t(i) << 32;
    else if (e & (1u << shift)) return e & (2u << shift);
    const bool plain = cpuPrvJitPlainArm(i, thumb);
    slot.store(e | (1u << shift) | (plain ? 2u << shift : 0), std::memory_order_relaxed);
    return plain;
}

static bool cpuPrvJitLdrWord(uint32_t i, uint32_t rn, uint32_t rd) {
    return (i & 0x0c500000) == 0x04100000 && ((i >> 16) & 15) == rn && ((i >> 12) & 15) == rd;
}

// The syscall dispatch pair LDR R12, [R9, #-table] then LDR PC, [R12, #offset], which the
// interpreter reports to the patch dispatch. The JIT may run the pair when the syscall has
// no patch (patchDispatchAddPatch drops translated code). Other forms of the two loads
// stay with the interpreter. Returns -1 for any other instruction.
static int cpuPrvJitSyscallPlain(ArmCpu *cpu, uint32_t pc, uint32_t i) {
    const bool load = cpuPrvJitLdrWord(i, 9, 12), jump = cpuPrvJitLdrWord(i, 12, 15);
    if (!load && !jump) return -1;
    const bool exactLoad = (i & 0x0ffff000) == 0x0519c000, exactJump = (i & 0x0ffff000) == 0x059cf000;
    if (!exactLoad && !exactJump) return 0;
    uint32_t other;
    if (!cpuJitFetch(cpu, load ? pc + 4 : pc - 4, &other)) return 0;
    if (load) {
        if ((other & 0x0ffff000) == 0x059cf000) return !patchDispatchHasPatch(cpu->patchDispatch, i & 0xfff, other & 0xfff);
        return !cpuPrvJitLdrWord(other, 12, 15);
    }
    if ((other & 0x0ffff000) == 0x0519c000) return !patchDispatchHasPatch(cpu->patchDispatch, other & 0xfff, i & 0xfff);
    return !cpuPrvJitLdrWord(other, 9, 12);
}

// Fast idle skip (idle_loops.h). FROND_IDLE_SKIP=0 turns it off, for measuring.
static bool cpuPrvIdleSkipOn = [] {
    const char *v = getenv("FROND_IDLE_SKIP");
    return !(v && *v && !strcmp(v, "0"));
}();
static uint64_t cpuPrvIdleSkips = 0, cpuPrvIdleSkipCycles = 0;

void cpuSetIdleSkip(bool on) { cpuPrvIdleSkipOn = on; }
void cpuJitIdleSkips(uint64_t *skips, uint64_t *cycles) {
    *skips = cpuPrvIdleSkips;
    *cycles = cpuPrvIdleSkipCycles;
}

static bool cpuPrvIdleWordsAt(ArmCpu *cpu, uint32_t va, const uint32_t *words, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t w;
        if (!cpuJitFetch(cpu, va + 4 * i, &w) || w != words[i]) return false;
    }
    return true;
}

// Flags the code lines of [va, va + size) on RAM pages, so a store to them drops m (cpuPrvJitCodeWritten
// drops the page's matches). ROM needs nothing: it changes only through patches, which invalidate. False
// for anything else.
static bool cpuPrvIdleWatch(ArmCpu *cpu, ArmCpu::IdleMatch &m, uint32_t va, uint32_t size) {
    const struct ArmMemRegion &rom = cpu->mem->regions[REGION_ROM];
    for (uint32_t at = va, end = va + size; at < end;) {
        const uint32_t next = std::min(end, (at & ~0xfffu) + 0x1000);
        const MMUTranslateResult r = mmuTranslateCached<false>(cpu->memorySystem.mmu, at, cpu->privileged);
        if (!MMU_TRANSLATE_RESULT_OK(r)) return false;
        const uint32_t pa = MMU_TRANSLATE_RESULT_PA(r);
        if (rom.ub && rom.uD && pa >= rom.pa && pa - rom.pa + (next - at) <= rom.ub - rom.pa) {
            at = next;
            continue;
        }
        uint8_t *lines;
        uint8_t *page = memWatchRamCode(cpu->mem, pa, cpuPrvJitCodeWritten, cpu, &lines);
        if (!page) return false;
        bool flagged = false;
        for (uint32_t line = (pa & 0xfffu) >> 6; line <= ((pa + (next - at) - 1) & 0xfffu) >> 6; line++)
            if (!lines[line]) {
                lines[line] = 1;
                flagged = true;
            }
        if (flagged) jitWriteProtectPage(cpu->jit, page);
        if (std::find(m.pages, m.pages + m.pageCount, page) == m.pages + m.pageCount) {
            if (m.pageCount == 4) return false;
            m.pages[m.pageCount++] = page;
        }
        at = next;
    }
    return true;
}

// The idle loop whose head is at pc (word is its first instruction), or null. PXA Fast only. A match is
// kept per head (cpu->idleMatch) until its code lines are written, code is invalidated or dropped, the
// mappings change (a TLB flush, domains, S or R) or the privilege changes; *match is it, or null when
// the loop's pages can't be watched.
static const IdleLoop *cpuPrvIdleLoopAt(ArmCpu *cpu, uint32_t pc, uint32_t word, ArmCpu::IdleMatch **match = nullptr) {
    if (match) *match = nullptr;
    if (!cpuPrvIdleSkipOn || cpu->model == ArmCpuModel::arm920t || !cpu->jit || (pc & 3)) return nullptr;
    const uint32_t mapGen = mmuFlushCount(cpu->memorySystem.mmu);
    const uint64_t perm = cpuJitPermContext(cpu);
    for (auto &m : cpu->idleMatch)
        if (m.pc == pc && word == m.loop->words[0] && m.mapGen == mapGen && m.permContext == perm &&
            m.privileged == cpu->privileged) {
            if (match) *match = &m;
            return m.loop;
        }
    for (const IdleLoop &l : kIdleLoops) {
        if (word != l.words[0]) continue;
        if (!cpuPrvIdleWordsAt(cpu, pc, l.words, l.count) ||
            !cpuPrvIdleWordsAt(cpu, pc + uint32_t(l.window), kIdleWindowWords, kIdleWindowCount))
            continue;
        // Kept in the slot already at pc, else an empty one, else one not at a current head.
        ArmCpu::IdleMatch *slot = &cpu->idleMatch[0];
        for (auto &m : cpu->idleMatch)
            if (m.pc == pc) slot = &m;
        if (slot->pc != pc) {
            if (cpu->idleMatch[0].pc && (!cpu->idleMatch[1].pc || cpu->idleMatch[0].pc == cpu->idleHeads[0] ||
                                         cpu->idleMatch[0].pc == cpu->idleHeads[1]))
                slot = &cpu->idleMatch[1];
        }
        *slot = ArmCpu::IdleMatch{};
        slot->loop = &l;
        if (cpuPrvIdleWatch(cpu, *slot, pc, 4 * l.count) &&
            cpuPrvIdleWatch(cpu, *slot, pc + uint32_t(l.window), 4 * kIdleWindowCount)) {
            slot->pc = pc;
            slot->mapGen = mapGen;
            slot->permContext = perm;
            slot->privileged = cpu->privileged;
            if (match) *match = slot;
        }
        return &l;
    }
    return nullptr;
}

// At an idle loop's head (cpuJitPlain leaves it to the interpreter, so a run stops there),
// after a pass (LR is the window's return) and with no IRQ to take in the window and no
// unmasked FIQ, all but the last of the passes that fit in left pass at once. In a Fast run
// OSCR stands still (cyclesInProgress moves only between runs), so both reads of a pass give
// the same value and every pass leaves the same state: the counter as it is, the window's
// stores done again and the registers set below. The last pass runs as usual and the run ends
// partway through it, as it would without the skip, and cpuPrvCycleJit shows peripherals the
// time at the start of the run that reached the head until then, so the passes read the OSCR
// that one run without the skip would. When the end falls between a pass's two OSCR reads the
// counter takes the whole run's time, which is how it reaches the 50 ms threshold.
// A pass past the threshold calls the clock routine, so those aren't skipped. Returns the
// cycles passed, 0 if none.
static uint32_t cpuPrvIdleSkip(ArmCpu *cpu, uint32_t left) {
    uint32_t *r = cpu->regs;
    const uint32_t pc = r[REG_NO_PC];
    if ((pc != cpu->idleHeads[0] && pc != cpu->idleHeads[1]) || !pc || !cpuPrvIdleSkipOn || cpu->T || !left ||
        cpu->waitingIrqs || (cpu->waitingFiqs && !cpu->F) || cpu->nativeCallDepth || cpu->modePace)
        return 0;
    uint32_t word;
    if (!cpuJitFetch(cpu, pc, &word)) return 0;
    ArmCpu::IdleMatch *match;
    const IdleLoop *l = cpuPrvIdleLoopAt(cpu, pc, word, &match);
    if (!l || r[REG_NO_LR] != pc + l->windowReturn) return 0;
    uint_fast8_t fsr;
    auto read = [&](uint32_t va, uint32_t &v) { return cpuJitRead(cpu, va, 4, &v, &fsr); };
    const bool zire = l->kind == IdleLoopKind::zire72;
    // Globals pointer, idle counter address and value, timer base.
    uint32_t globals, counterVa, counter, timer;
    if (zire) {
        counterVa = r[8];
        timer = r[4];
        if (!read(r[5], globals)) return 0;
    } else {
        counterVa = r[5];
        timer = r[7];
        if (!read(r[8] + 20, globals)) return 0;
    }
    uint32_t wake, oscr, iccr;
    uint8_t byte;
    const uint32_t byteVa = globals + kIdleWindowByte;
    if (!read(counterVa, counter) || !read(globals + kIdleWakeWord, wake) || wake ||
        !cpuJitRead(cpu, byteVa, 1, &byte, &fsr) || !read(pc + uint32_t(l->window) + kIdleWindowLiteral, iccr))
        return 0;
    // A pass that goes straight to the body, as every pass will, and what it costs (the table's cost of
    // each instruction it runs, as Fast charges them).
    if (zire && r[0] != counter) return 0;
    const int variant = idleHeadVariant(*l, counter, r);
    if (variant < 0) return 0;
    const bool on = cpu->ic->costOn;
    uint32_t p;
    if (match) {
        const uint32_t key = ((uint32_t(on) << 8) | cpu->costCore) + 1;
        if (match->costKey != key) {
            match->costKey = key;
            std::fill(std::begin(match->cost), std::end(match->cost), 0u);
        }
        uint32_t &cached = match->cost[variant];
        if (!cached) cached = idleVariantCost(*l, variant, on, cost::Core(cpu->costCore));
        p = cached;
    } else {
        p = idleVariantCost(*l, variant, on, cost::Core(cpu->costCore));
    }
    const uint32_t passes = (left - 1) / p;
    if (!passes) return 0;
    // Both OSCR reads of each pass.
    if (!read(timer + 16, oscr)) return 0;
    // The window's stores: ICCR, then SPSR and LR pushed below SP; the byte with bit 0 clear.
    const uint32_t one = 1, sp = r[REG_NO_SP], ret = pc + l->windowReturn, spsr = cpu->SPSR;
    const uint8_t cleared = byte & ~1u;
    if (!cpuJitWrite(cpu, iccr, 4, &one, &fsr) || cpu->waitingIrqs || (cpu->waitingFiqs && !cpu->F) ||
        !cpuJitWrite(cpu, sp - 8, 4, &spsr, &fsr) || !cpuJitWrite(cpu, sp - 4, 4, &ret, &fsr) ||
        !cpuJitWrite(cpu, byteVa, 1, &cleared, &fsr))
        return 0;
    if (zire) {
        r[0] = counter;
        r[1] = wake;
        r[2] = globals;
        r[11] = oscr;
    } else {
        r[0] = wake;
        r[1] = counter;
        r[2] = spsr;
        r[6] = oscr;
        r[12] = counter - 0x27800 - (counter >= 0x27800 ? 0x2c4 : 0);
    }
    // The pass ends with a compare with 0: Z and C set.
    cpu->flags = ARM_SR_Z | ARM_SR_C;
    cpuPrvIdleSkips++;
    cpuPrvIdleSkipCycles += uint64_t(passes) * p;
    return passes * p;
}

// Fast OSCR delay skip (delay_loops.h). FROND_DELAY_SKIP=0 turns it off, for measuring.
static bool cpuPrvDelaySkipOn = [] {
    const char *v = getenv("FROND_DELAY_SKIP");
    return !(v && *v && !strcmp(v, "0"));
}();
static uint64_t cpuPrvDelaySkips = 0, cpuPrvDelaySkipCycles = 0;

void cpuSetDelaySkip(bool on) { cpuPrvDelaySkipOn = on; }
void cpuJitDelaySkips(uint64_t *skips, uint64_t *cycles) {
    *skips = cpuPrvDelaySkips;
    *cycles = cpuPrvDelaySkipCycles;
}

void cpuSetDelayTimer(ArmCpu *cpu, uint32_t oscrPa, bool (*peek)(void *, uint32_t, uint32_t *), void *context) {
    cpu->delayOscrPa = oscrPa;
    cpu->delayPeek = peek;
    cpu->delayPeekContext = context;
    cpuPrvIdleForget(cpu);
}

static bool cpuPrvDelayCodeAt(ArmCpu *cpu, uint32_t va, const DelayLoop &l) {
    for (uint32_t i = 0; i < l.count; ++i) {
        const uint32_t at = va + (l.thumb ? 2 : 4) * i;
        uint32_t w;
        if (!cpuJitFetch(cpu, at, &w)) return false;
        if (l.thumb) w = (at & 2) ? w >> 16 : w & 0xffff;
        if (w != l.code[i]) return false;
    }
    return true;
}

// Whether the instruction at pc (in word, as cpuJitPlain has it) is a delay loop's first.
static FORCE_INLINE bool cpuPrvDelayFirst(uint32_t pc, uint32_t word, bool thumb) {
    if (thumb) return ((pc & 2) ? word >> 16 : word & 0xffff) == kDelayLoopE2[0];
    return word == kDelayLoopTg50[0] || word == kDelayLoopZire72[0];
}

// The delay loop whose head is at pc, or null. PXA Fast with a timer hooked (cpuSetDelayTimer) only. Kept
// per head (cpu->delayMatch) and dropped as the idle loop matches are (cpuPrvIdleLoopAt); *match is it, or
// null when the loop's pages can't be watched.
static const DelayLoop *cpuPrvDelayLoopAt(ArmCpu *cpu, uint32_t pc, bool thumb, ArmCpu::IdleMatch **match = nullptr) {
    if (match) *match = nullptr;
    if (!cpuPrvDelaySkipOn || !cpu->delayPeek || cpu->model == ArmCpuModel::arm920t || !cpu->jit ||
        (pc & (thumb ? 1 : 3)))
        return nullptr;
    const uint32_t mapGen = mmuFlushCount(cpu->memorySystem.mmu);
    const uint64_t perm = cpuJitPermContext(cpu);
    for (auto &m : cpu->delayMatch)
        if (m.pc == pc && m.delay->thumb == thumb && m.mapGen == mapGen && m.permContext == perm &&
            m.privileged == cpu->privileged) {
            if (match) *match = &m;
            return m.delay;
        }
    for (const DelayLoop &l : kDelayLoops) {
        if (l.thumb != thumb || !cpuPrvDelayCodeAt(cpu, pc, l)) continue;
        // Kept in the slot already at pc, else an empty one, else one not at a current head.
        ArmCpu::IdleMatch *slot = &cpu->delayMatch[0];
        for (auto &m : cpu->delayMatch)
            if (m.pc == pc) slot = &m;
        if (slot->pc != pc) {
            if (cpu->delayMatch[0].pc && (!cpu->delayMatch[1].pc || cpu->delayMatch[0].pc == cpu->delayHeads[0] ||
                                          cpu->delayMatch[0].pc == cpu->delayHeads[1]))
                slot = &cpu->delayMatch[1];
        }
        *slot = ArmCpu::IdleMatch{};
        slot->delay = &l;
        if (cpuPrvIdleWatch(cpu, *slot, pc, (l.thumb ? 2 : 4) * l.count)) {
            slot->pc = pc;
            slot->mapGen = mapGen;
            slot->permContext = perm;
            slot->privileged = cpu->privileged;
            if (match) *match = slot;
        }
        return &l;
    }
    return nullptr;
}

// At a delay loop's head (cpuJitPlain leaves it to the interpreter, so a run stops there), the passes that
// stay in the loop go at once, each charged what it costs, up to all but the last pass that fits in left.
// Pass k loads OSCR at the cycles in progress now plus k passes plus the load's own charge (its cost, and
// its region charge with memory charges), as the interpreter would show it, so the loop leaves on the pass
// Exact's would. A Fast run without the skip reads the OSCR of the run's start until the run ends. The
// skip stops short of a load that would reach a timer match, so that load raises its interrupt. The loaded
// register and the flags are left as the last skipped pass leaves them. Returns the cycles passed, 0 if none.
static uint32_t cpuPrvDelaySkip(ArmCpu *cpu, uint32_t left) {
    uint32_t *r = cpu->regs;
    const uint32_t pc = r[REG_NO_PC];
    if ((pc != cpu->delayHeads[0] && pc != cpu->delayHeads[1]) || !pc || !left || !cpu->cyclesInProgress ||
        (cpu->waitingIrqs && !cpu->I) || (cpu->waitingFiqs && !cpu->F) || cpu->nativeCallDepth || cpu->modePace)
        return 0;
    ArmCpu::IdleMatch *match;
    const DelayLoop *l = cpuPrvDelayLoopAt(cpu, pc, cpu->T, &match);
    if (!l) return 0;
    const uint32_t va = r[l->base] + 16;
    if (va & 3) return 0;
    const MMUTranslateResult t = mmuTranslateCached<false, true>(cpu->memorySystem.mmu, va, cpu->privileged);
    if (!MMU_TRANSLATE_RESULT_OK(t) || MMU_TRANSLATE_RESULT_PA(t) != cpu->delayOscrPa) return 0;
    const bool on = cpu->ic->costOn;
    const cost::Core core = cost::Core(cpu->costCore);
    uint32_t pass, load;
    if (match) {
        const uint32_t key = ((uint32_t(on) << 8) | cpu->costCore) + 1;
        if (match->costKey != key) {
            match->costKey = key;
            match->cost[0] = delayPassCost(*l, on, core, &match->cost[1]);
        }
        pass = match->cost[0];
        load = match->cost[1];
    } else {
        pass = delayPassCost(*l, on, core, &load);
    }
    // The load's region charge (Fast at Original), which the interpreter adds before the access.
    if (cpu->costMemory) {
        const uint32_t charge = cpu->regionCharge[cost::pxaRegion(cpu->delayOscrPa)][0];
        pass += charge;
        load += charge;
    }
    const uint32_t most = (left - 1) / pass;
    if (!most) return 0;
    const uint32_t now = *cpu->cyclesInProgress + cpu->jitHint.pending + load;
    auto stays = [&](uint32_t k, uint32_t &v) {
        return cpu->delayPeek(cpu->delayPeekContext, now + k * pass, &v) && delayStays(*l, v, r);
    };
    uint32_t first, last;
    if (!stays(0, first)) return 0;
    // OSCR wrapping within the budget would make the passes that stay not a run from the first.
    cpu->delayPeek(cpu->delayPeekContext, now + (most - 1) * pass, &last);
    if (last < first) return 0;
    // Passes below lo stay; hi is the first that doesn't, or most.
    uint32_t lo = 1, hi = most;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        uint32_t v;
        if (stays(mid, v)) lo = mid + 1;
        else hi = mid;
    }
    const uint32_t passes = lo;
    cpu->delayPeek(cpu->delayPeekContext, now + (passes - 1) * pass, &last);
    cpu->flags = delayFlags(*l, last, r);
    r[l->value] = last;
    cpuPrvDelaySkips++;
    cpuPrvDelaySkipCycles += uint64_t(passes) * pass;
    return passes * pass;
}

// Fast GPIO level poll skip (poll_loops.h). FROND_POLL_SKIP=0 turns it off, for measuring.
static bool cpuPrvPollSkipOn = [] {
    const char *v = getenv("FROND_POLL_SKIP");
    return !(v && *v && !strcmp(v, "0"));
}();
static uint64_t cpuPrvPollSkips = 0, cpuPrvPollSkipCycles = 0;

void cpuSetPollSkip(bool on) { cpuPrvPollSkipOn = on; }
void cpuJitPollSkips(uint64_t *skips, uint64_t *cycles) {
    *skips = cpuPrvPollSkips;
    *cycles = cpuPrvPollSkipCycles;
}

void cpuSetPollPeek(ArmCpu *cpu, bool (*peek)(void *, uint32_t, uint32_t *), void *context) {
    cpu->pollPeek = peek;
    cpu->pollPeekContext = context;
    cpuPrvIdleForget(cpu);
}

static bool cpuPrvPollCodeAt(ArmCpu *cpu, uint32_t va, const PollLoop &l) {
    for (uint32_t i = 0; i < l.count; ++i) {
        uint32_t w;
        if (!cpuJitFetch(cpu, va + 4 * i, &w) || w != l.code[i]) return false;
    }
    return true;
}

// Whether word (at a word aligned pc, ARM) is a poll loop's first instruction.
static FORCE_INLINE bool cpuPrvPollFirst(uint32_t word, bool thumb) {
    return !thumb && (word == kPollLoopE2NandReady[0] || word == kPollLoopE2NandBusy[0]);
}

// The poll loop whose head is at pc, or null. PXA Fast with a level peek hooked (cpuSetPollPeek) only.
// Kept per head (cpu->pollMatch) and dropped as the delay loop matches are (cpuPrvDelayLoopAt).
static const PollLoop *cpuPrvPollLoopAt(ArmCpu *cpu, uint32_t pc, bool thumb, ArmCpu::IdleMatch **match = nullptr) {
    if (match) *match = nullptr;
    if (!cpuPrvPollSkipOn || !cpu->pollPeek || cpu->model == ArmCpuModel::arm920t || !cpu->jit || thumb || (pc & 3))
        return nullptr;
    const uint32_t mapGen = mmuFlushCount(cpu->memorySystem.mmu);
    const uint64_t perm = cpuJitPermContext(cpu);
    for (auto &m : cpu->pollMatch)
        if (m.pc == pc && m.mapGen == mapGen && m.permContext == perm && m.privileged == cpu->privileged) {
            if (match) *match = &m;
            return m.poll;
        }
    for (const PollLoop &l : kPollLoops) {
        if (!cpuPrvPollCodeAt(cpu, pc, l)) continue;
        // Kept in the slot already at pc, else an empty one, else one not at a current head.
        ArmCpu::IdleMatch *slot = &cpu->pollMatch[0];
        for (auto &m : cpu->pollMatch)
            if (m.pc == pc) slot = &m;
        if (slot->pc != pc) {
            if (cpu->pollMatch[0].pc && (!cpu->pollMatch[1].pc || cpu->pollMatch[0].pc == cpu->pollHeads[0] ||
                                         cpu->pollMatch[0].pc == cpu->pollHeads[1]))
                slot = &cpu->pollMatch[1];
        }
        *slot = ArmCpu::IdleMatch{};
        slot->poll = &l;
        if (cpuPrvIdleWatch(cpu, *slot, pc, 4 * l.count)) {
            slot->pc = pc;
            slot->mapGen = mapGen;
            slot->permContext = perm;
            slot->privileged = cpu->privileged;
            if (match) *match = slot;
        }
        return &l;
    }
    return nullptr;
}

// At a poll loop's head (cpuJitPlain leaves it to the interpreter, so a run stops there), the passes that
// stay in the loop go at once, each charged what it costs, up to all but the last pass that fits in left,
// or up to the pass its counter leaves on. The polled level changes only at scheduler events, which come
// between runs, so every pass in this run reads what the peek gives now, as in Exact; the loop leaves on
// the first pass after the event that changes it. The loaded register, counter and flags are left as the
// last skipped pass leaves them. Returns the cycles passed, 0 if none.
static uint32_t cpuPrvPollSkip(ArmCpu *cpu, uint32_t left) {
    uint32_t *r = cpu->regs;
    const uint32_t pc = r[REG_NO_PC];
    if ((pc != cpu->pollHeads[0] && pc != cpu->pollHeads[1]) || !pc || !left || cpu->T ||
        (cpu->waitingIrqs && !cpu->I) || (cpu->waitingFiqs && !cpu->F) || cpu->nativeCallDepth || cpu->modePace)
        return 0;
    ArmCpu::IdleMatch *match;
    const PollLoop *l = cpuPrvPollLoopAt(cpu, pc, false, &match);
    if (!l) return 0;
    const uint32_t va = r[l->base];
    if (va & 3) return 0;
    const MMUTranslateResult t = mmuTranslateCached<false, true>(cpu->memorySystem.mmu, va, cpu->privileged);
    if (!MMU_TRANSLATE_RESULT_OK(t)) return 0;
    const uint32_t pa = MMU_TRANSLATE_RESULT_PA(t);
    uint32_t v;
    if (!cpu->pollPeek(cpu->pollPeekContext, pa, &v)) return 0;
    uint32_t most = pollStaying(*l, v, r);
    if (!most) return 0;
    const bool on = cpu->ic->costOn;
    const cost::Core core = cost::Core(cpu->costCore);
    uint32_t pass, load;
    if (match) {
        const uint32_t key = ((uint32_t(on) << 8) | cpu->costCore) + 1;
        if (match->costKey != key) {
            match->costKey = key;
            match->cost[0] = pollPassCost(*l, on, core, &match->cost[1]);
        }
        pass = match->cost[0];
    } else {
        pass = pollPassCost(*l, on, core, &load);
    }
    // The load's region charge (Fast at Original), which the interpreter adds before the access.
    if (cpu->costMemory) pass += cpu->regionCharge[cost::pxaRegion(pa)][0];
    most = std::min(most, (left - 1) / pass);
    if (!most) return 0;
    cpu->flags = pollAfter(*l, v, most, r, cpu->flags);
    cpuPrvPollSkips++;
    cpuPrvPollSkipCycles += uint64_t(most) * pass;
    return most * pass;
}

bool cpuJitPlain(ArmCpu *cpu, uint32_t pc, uint32_t word, bool thumb) {
    // A code hook's pc goes to the step.
    if (unlikely(cpu->codeHookCount) && cpuPrvCodeHookAt(cpu, pc)) return false;
    // A delay loop's head ends a run, for cpuPrvDelaySkip.
    if (cpuPrvDelayFirst(pc, word, thumb) && cpuPrvDelayLoopAt(cpu, pc, thumb)) {
        if (pc != cpu->delayHeads[0] && pc != cpu->delayHeads[1]) {
            cpu->delayHeads[1] = cpu->delayHeads[0];
            cpu->delayHeads[0] = pc;
        }
        return false;
    }
    // A poll loop's head ends a run, for cpuPrvPollSkip.
    if (cpuPrvPollFirst(word, thumb) && cpuPrvPollLoopAt(cpu, pc, thumb)) {
        if (pc != cpu->pollHeads[0] && pc != cpu->pollHeads[1]) {
            cpu->pollHeads[1] = cpu->pollHeads[0];
            cpu->pollHeads[0] = pc;
        }
        return false;
    }
    // An idle loop's head ends a run, for cpuPrvIdleSkip.
    if (!thumb && (word == kIdleLoopZire72[0] || word == kIdleLoopTx[0]) && cpuPrvIdleLoopAt(cpu, pc, word)) {
        if (pc != cpu->idleHeads[0] && pc != cpu->idleHeads[1]) {
            cpu->idleHeads[1] = cpu->idleHeads[0];
            cpu->idleHeads[0] = pc;
        }
        return false;
    }
    if (cpu->model == ArmCpuModel::arm920t) {
        // Fast blocks: MRC and MCR p15, 0, Rd, c3, c0, 0 (the domain access control register) run in the JIT's
        // p15 slot, which does what the ARM920T's coprocessor 15 does for them. So do the slot's other words
        // (jitCp15SlotWord920: ID, control and table base reads, cache operations), through its handler.
        const bool dacr = cpu->jitBlocks && !thumb && (word & 0x0fef0fff) == 0x0e030f10 && ((word >> 12) & 15) != 15;
        if (cpu->jitBlocks && !thumb && !dacr && ((word >> 12) & 15) != 15 && (word >> 28) != 15 &&
            jitCp15SlotWord920(word)) {
            // As below, a fetch buffer word other than memory's stays with the interpreter.
            uint32_t buffered;
            return !(icacheBufferedWord(cpu->ic, pc & ~3u, &buffered) && buffered != word);
        }
        if (!thumb) {
            // ARM920T word stores use PC+12; the JIT uses the ARMv5 PC+8 value.
            if ((word & 0x0e108000) == 0x08008000 ||
                (word & 0x0c50f000) == 0x0400f000) return false;
            if ((word >> 28) == 15 || (word & 0x0ffffff0) == 0x012fff30 ||
                (word & 0x0fff0ff0) == 0x016f0f10 ||
                (word & 0x0f9000f0) == 0x01000050 ||
                (word & 0x0f900090) == 0x01000080 ||
                (word & 0x0e1000d0) == 0x000000d0 ||
                (word & 0x0ff000f0) == 0x01200070 ||
                (word & 0x0fb00ff0) == 0x01000090 || // Swap shares the backing-range preflight.

                ((word & 0x0c000000) == 0x0c000000 && !dacr)) return false;
        } else {
            const uint16_t h = (pc & 2) ? word >> 16 : word;
            if ((h & 0xff80) == 0x4780 || (h & 0xf800) == 0xe800 ||
                (h & 0xff00) == 0xbe00) return false;
            // CMP with two low registers (Zodiac only): dynarmic calls it unpredictable, so the interpreter runs
            // it. PXA devices keep handing it to dynarmic as before.
            if ((h & 0xffc0) == 0x4500) return false;
            // The second half is interpreted separately at a line boundary (Fast blocks: at a page boundary).
            if (h >= 0xf000 && h < 0xf800) {
                if (cpu->jit && jitMaxUnchecked(cpu->jit) == 1) return false;
                uint32_t next;
                if ((pc & (cpu->jitBlocks ? 0xfff : 31)) == (cpu->jitBlocks ? 0xffe : 30) ||
                    !cpuJitFetch(cpu, (pc + 2) & ~3u, &next)) return false;
                if ((((pc + 2) & 2) ? next >> 16 : next & 0xffff) < 0xf800) return false;
            }
        }
        // Fast blocks: what the lean path leaves to the step stays out too (CP15, exception returns, S-bit
        // LDM/STM, unpredictable forms), except MSR of the CPSR control field, which the JIT runs through
        // cpuJitSetCpsr (a pending interrupt it unmasks ends the run), the two exception returns below and SWI,
        // which cpuJitSwi takes as the step would (the run ends at the vector).
        if (cpu->jitBlocks) {
            if (!thumb) {
                const bool msrCpsr = (word & 0x0ff0fff0) == 0x0120f000 || (word & 0x0ff0f000) == 0x0320f000;
                // MOVS PC, LR and SUBS PC, LR, #4 return through cpuJitExceptionReturn.
                const bool exceptionReturn = (word & 0x0fffffff) == 0x01b0f00e || (word & 0x0fffffff) == 0x025ef004;
                const bool swi = (word & 0x0f000000) == 0x0f000000 && (word >> 28) != 15;
                if (!msrCpsr && !exceptionReturn && !dacr && !swi && cpuPrvLeanClassArm(word) == LeanClass::exit)
                    return false;
            } else {
                const uint16_t h = (pc & 2) ? word >> 16 : word & 0xffff;
                if ((h & 0xff00) != 0xdf00 && cpuPrvLeanClassThumb(h) == LeanClass::exit) return false;
            }
        }
    }
    // ARM coprocessor encodings other than the p15 slot's (jitCp15SlotWord) and iWMMXt
    // never go to the JIT (cpuPrvJitPlainArm), so they're turned down before any other
    // check, and so is everything with cond NV there. iWMMXt (cp0 and cp1) also needs the
    // unit and its CPAR bit. SWI goes on to cpuPrvJitPlainArm.
    if (!thumb && (word & 0x0c000000) == 0x0c000000 && !jitCp15SlotWord(word)) {
        const uint32_t cp = (word >> 8) & 15;
        if ((word & 0x0f000000) == 0x0f000000) {
            if ((word >> 28) == 15) return false;
        } else if (cp > 1 || !cpu->wmmx || !(cpu->CPAR & (1u << cp))) {
            return false;
        }
    }
    // The interpreter runs a fetch buffer's words before memory's. A buffer can hold other
    // words than memory, like the patched PACE entry a session restores while its RAM has
    // the original, so those instructions go to the interpreter.
    uint32_t buffered;
    if (icacheBufferedWord(cpu->ic, pc & ~3u, &buffered) && buffered != word) return false;
    if (!thumb) {
        const int syscall = cpuPrvJitSyscallPlain(cpu, pc, word);
        return syscall >= 0 ? syscall : cpuPrvJitPlainArmMemo(word, false);
    }
    const uint16_t h = (pc & 2) ? word >> 16 : word & 0xffff;
    const uint32_t top = h >> 11;
    // Dynarmic reads any halfword from 0xe800 up as the first half of a Thumb-2 pair. Only a
    // BL or BLX prefix with its suffix next goes to it; a suffix on its own, or a prefix
    // followed by anything else, runs in the interpreter.
    if (h >= 0xe800) {
        if (top != 0x1e) return false;
        const uint32_t next = pc + 2;
        uint32_t w;
        if (next & 2) {
            w = word;
        } else {
            if (!cpuJitFetch(cpu, next, &w)) return false;
            if (icacheBufferedWord(cpu->ic, next, &buffered) && buffered != w) return false;
        }
        const uint16_t s = (next & 2) ? w >> 16 : w & 0xffff;
        if (!(s >= 0xf800 || ((s >> 11) == 0x1d && !(s & 1)))) return false;
        return !(cpu->earlyAudioHook &&
                 (next == cpu->earlyAudioFrom[0] || next == cpu->earlyAudioFrom[1]));
    }
    if (const uint32_t t = table_thumb2arm[h]) return cpuPrvJitPlainArmMemo(t, true);
    // Forms the interpreter runs as Thumb: LDR PC-relative, ADD Rd, PC/SP, and ADD, MOV,
    // BX and BLX with high registers.
    if (top == 0x09 || (h >> 12) == 0xa || (h & 0xfc00) == 0x4400) return true;
    return false;
}

static void cpuPrvJitInval(void *ctx, uint32_t va, uint32_t size, bool guest) {
    ArmCpu *cpu = (ArmCpu *)ctx;
    // Host writes (patches, a test's code) don't flag code lines; they invalidate instead.
    cpuPrvIdleForget(cpu);
    if (size)
        jitInvalidate(cpu->jit, va, size);
    else if (guest)
        jitGuestInvalidateAll(cpu->jit, cpu->curInstrPC);
    else
        jitClear(cpu->jit);
}

// FROND_JIT overrides the engine for the PXA and OMAP cores. The ARM920T's per-instruction JIT is its
// machine's own choice (imx1MachineCreate's fast): with FROND_JIT=1 the Zodiac's Fast engine would find
// it in place and run without its Fast blocks (cpuArm920SetJit).
static bool cpuPrvJitWanted(ArmCpuModel model) {
    if (model == ArmCpuModel::arm920t) return cpuPrvFastEngine;
    const char *v = getenv("FROND_JIT");
    if (v && *v) return strcmp(v, "0") != 0;
    return cpuPrvFastEngine;
}

// One instruction through the interpreter, as cpuCycleLine's slow path does it. With abort,
// the instruction is fetched as usual and then takes that data abort instead of running
// (FROND_JIT_ABORT: the JIT already ran it up to its failed access).
template <int memorySystemKind>
static void cpuPrvJitInterpretOne(struct ArmCpu *cpu, uint32_t &acc, const FrondJitAbort *abort = nullptr) {
    const uint32_t pc = cpu->regs[REG_NO_PC];
    cpu->curInstrPC = pc;
    acc += 1;
    uint32_t shown = acc;
    uint32_t instr = 0, decoded = 0;
    uint_fast8_t fsr;
    const uint32_t size = cpu->T ? 2 : 4;
    const bool ok = cpu->T ? icacheFetch<memorySystemKind, 2, false>(cpu->ic, pc, cpu->privileged, &fsr, instr, decoded)
                           : icacheFetch<memorySystemKind, 4, false>(cpu->ic, pc, cpu->privileged, &fsr, instr, decoded);
    // The JIT fetched this instruction before its access, so a fetch that fails now (the
    // access changed the mapping) doesn't stop the abort.
    if (!ok && !abort) {
        cpuPrvHandleMemErr<memorySystemKind>(cpu, pc, false, true, fsr);
        return;
    }
    if (ok) acc += cpuPrvSlotCharge(cpu, cpu->ic->lastCost) - 1;
    cpu->regs[REG_NO_PC] = pc + size;
    if (abort)
        cpuPrvDataAbort(cpu, abort->addr, abort->size, abort->write, abort->fsr);
    else
        cpuPrvDecompressExecFn(decoded)(cpu, instr);
    cpuPrvTakeCharge(cpu, acc, shown);
}

template <int memorySystemKind>
static uint32_t cpuPrvCycleJit(struct ArmCpu *cpu, uint32_t cycles) {
    uint32_t acc = 0;
    CycleProgress progress(cpu, &acc);
    static bool faultReported = false;
    // An idle skip and the run after it show peripherals the time at the start of the run
    // that reached the loop's head, as one run without the skip would (see cpuPrvIdleSkip).
    uint32_t held = 0;
    bool holding = false;

    while (acc < cycles) {
        if (armKindIsArm920(memorySystemKind) && icacheArm920ContinuationPending(cpu->ic)) {
            cpuPrvJitInterpretOne<memorySystemKind>(cpu, acc);
            if (cpu->slowPath) break;
            continue;
        }
        uint32_t cpsr = cpuPrvMaterializeCPSR(cpu);
        int stop;
        const uint32_t runStart = acc;
        cpu->costInJitRun = true;
        acc += jitRun(cpu->jit, cpu->regs, cpsr, cycles - acc, stop);
        cpu->costInJitRun = false;
        // The run's memory charges (Fast at Original).
        acc += cpu->jitHint.pending;
        cpu->jitHint.pending = 0;
        cpu->flags = cpsr & 0xf0000000UL;
        cpu->Q = !!(cpsr & ARM_SR_Q);
        cpu->T = !!(cpsr & ARM_SR_T);
        // An idle loop's head: all but its last pass in the budget go at once, then the head
        // goes to the interpreter below as usual.
        if (stop == FROND_JIT_INTERPRET && !cpu->slowPath && acc < cycles &&
            (cpu->regs[REG_NO_PC] == cpu->idleHeads[0] || cpu->regs[REG_NO_PC] == cpu->idleHeads[1])) {
            if (!holding) held = runStart;
            cpu->cyclesInProgress = &held;
            if (const uint32_t skipped = cpuPrvIdleSkip(cpu, cycles - acc)) {
                acc += skipped;
                holding = true;
            }
        } else if (holding) {
            holding = false;
        }
        if (!holding) cpu->cyclesInProgress = &acc;
        // An OSCR delay loop's head: the passes before the one that leaves go at once (cpuPrvDelaySkip),
        // then the head goes to the interpreter below as usual.
        if (stop == FROND_JIT_INTERPRET && !cpu->slowPath && acc < cycles &&
            (cpu->regs[REG_NO_PC] == cpu->delayHeads[0] || cpu->regs[REG_NO_PC] == cpu->delayHeads[1]))
            acc += cpuPrvDelaySkip(cpu, cycles - acc);
        // A GPIO level poll's head: the same (cpuPrvPollSkip).
        if (stop == FROND_JIT_INTERPRET && !cpu->slowPath && acc < cycles &&
            (cpu->regs[REG_NO_PC] == cpu->pollHeads[0] || cpu->regs[REG_NO_PC] == cpu->pollHeads[1]))
            acc += cpuPrvPollSkip(cpu, cycles - acc);
        if (stop == FROND_JIT_FAULT && !faultReported) {
            faultReported = true;
            fprintf(stderr, "jit: data access fault near pc=0x%08x, guest state is not exact\n",
                    cpu->regs[REG_NO_PC]);
        }
        // Before the slow path check: in Exact the data abort is taken inside the same
        // instruction as the access, before any interrupt an earlier access raised.
        const FrondJitAbort *abort = stop == FROND_JIT_ABORT ? &jitLastAbort(cpu->jit) : nullptr;
        if (cpu->slowPath && !abort) break;
        if (stop == FROND_JIT_INTERPRET || abort) {
            // Stay in the interpreter while the next instruction would come straight back.
            for (bool first = true;; first = false) {
                cpuPrvJitInterpretOne<memorySystemKind>(cpu, acc, first ? abort : nullptr);
                if (cpu->slowPath || acc >= cycles) break;
                const uint32_t pc = cpu->regs[REG_NO_PC];
                uint32_t word;
                if (cpuJitFetch(cpu, pc, &word) && cpuJitPlain(cpu, pc, word, cpu->T)) break;
            }
            if (cpu->slowPath) break;
        } else if (stop == FROND_JIT_COLD) {
            // Code not compiled yet: the interpreter runs it up to its next taken branch,
            // where the JIT looks again.
            for (;;) {
                const uint32_t pc = cpu->regs[REG_NO_PC];
                const bool thumb = cpu->T;
                cpuPrvJitInterpretOne<memorySystemKind>(cpu, acc);
                if (cpu->slowPath || acc >= cycles) break;
                if (cpu->T != thumb || cpu->regs[REG_NO_PC] != pc + (thumb ? 2 : 4)) break;
            }
            if (cpu->slowPath) break;
        }
    }

    return acc;
}

bool cpuArm920SetJit(ArmCpu *cpu, bool on) {
    if (cpu->model != ArmCpuModel::arm920t) return false;
    if (!on) {
        cpu->jitBlocks = false;
        return true;
    }
    if (!cpu->leanPage) return false;
    if (!cpu->jit) {
        cpu->jitCode = new Arm920CodeMap;
        cpu->jitLoopOnly = true;
        cpu->jitBlocks = true;
        cpu->jit = jitCreate(cpu, false);
        icacheSetInvalHook(cpu->ic, cpuPrvJitInval, cpu);
    } else if (!cpu->jitLoopOnly) {
        return false;  // the single-step diagnostic JIT
    }
    cpu->jitBlocks = true;
    jitClear(cpu->jit);
    return true;
}

bool cpuArm920JitOn(const ArmCpu *cpu) { return cpu->jitBlocks; }

void cpuArm920DropJit(ArmCpu *cpu) {
    if (!cpu->jit || !cpu->jitLoopOnly) return;
    icacheSetInvalHook(cpu->ic, nullptr, nullptr);
    jitDestroy(cpu->jit);
    cpu->jit = nullptr;
    cpu->jitBlocks = cpu->jitLoopOnly = false;
    delete cpu->jitCode;
    cpu->jitCode = nullptr;
}

uint32_t cpuArm920JitMaxUnchecked(const ArmCpu *cpu) { return cpu->jit ? jitMaxUnchecked(cpu->jit) : 0; }

unsigned cpuArm920JitPidTags(const ArmCpu *cpu, uint64_t *restarts) {
    if (!cpu->jit) {
        if (restarts) *restarts = 0;
        return 0;
    }
    return jitPidTags(cpu->jit, restarts);
}

// Slow path reasons cpuArm920RunJit clears before it runs: none of them needs the step.
static constexpr uint32_t arm920JitClearedSlowPath = SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE |
                                                     SLOW_PATH_REASON_RESCHEDULE | SLOW_PATH_REASON_PACE_SYSCALL_BREAK |
                                                     SLOW_PATH_REASON_INJECTED_CALL_DONE;

bool cpuArm920JitCanRun(const ArmCpu *cpu) {
    return cpu->jitBlocks && cpuArm920StepSkipAllowed(cpu);
}

bool cpuArm920StepSkipAllowed(const ArmCpu *cpu) {
    return !(cpu->slowPath & ~arm920JitClearedSlowPath) && !cpu->arm920Waiting && !cpu->modePace &&
           !cpu->nativeCallDepth && !cpu->nativePc[0] && !cpu->debugStub &&
           !icacheArm920ContinuationPending(cpu->ic);
}

uint32_t cpuArm920RunJit(ArmCpu *cpu, uint32_t budget, int *stop, uint32_t span, uint32_t *instructions) {
    *stop = arm920JitExit;
    if (instructions) *instructions = 0;
    if (!cpu->jitBlocks || !budget) return 0;
    cpuClearSlowPath(cpu, arm920JitClearedSlowPath);
    if (!cpuArm920JitCanRun(cpu)) return 0;
    // Data goes through the lean memory path: host pages, or an exit (leanExit) that faults the access.
    cpu->leanRun = true;
    cpu->leanJit = true;
    cpu->leanExit = false;
    cpu->leanDone = 0;
    // In a JIT run leanBudget only gives the access hooks their left.
    cpu->leanBudget = span ? span : budget;
    cpu->jitAccessPc = 0;
    cpu->jitCharged = 0;
    uint32_t cpsr = cpuPrvMaterializeCPSR(cpu);
    int jitStop;
    const uint32_t ran = jitRun(cpu->jit, cpu->regs, cpsr, budget, jitStop);
    if (instructions) *instructions = jitLastInstructions(cpu->jit);
    *stop = jitStop == FROND_JIT_BUDGET ? arm920JitBudget :
            jitStop == FROND_JIT_HALT   ? arm920JitHalt :
            jitStop == FROND_JIT_COLD   ? arm920JitCold : arm920JitExit;
    cpu->flags = cpsr & 0xf0000000UL;
    cpu->Q = !!(cpsr & ARM_SR_Q);
    cpu->T = !!(cpsr & ARM_SR_T);
    cpu->leanRun = false;
    cpu->leanJit = false;
    cpu->leanExit = false;
    return ran;
}
#else
uint64_t cpuJitExceptionReturns() { return 0; }
uint64_t cpuJitDomainCalls() { return 0; }
uint64_t cpuJitCpsrWrites() { return 0; }
uint64_t cpuJitCpsrInterprets() { return 0; }
void cpuSetIdleSkip(bool) {}
void cpuJitIdleSkips(uint64_t *skips, uint64_t *cycles) { *skips = *cycles = 0; }
void cpuSetDelaySkip(bool) {}
void cpuJitDelaySkips(uint64_t *skips, uint64_t *cycles) { *skips = *cycles = 0; }
void cpuSetDelayTimer(ArmCpu *, uint32_t, bool (*)(void *, uint32_t, uint32_t *), void *) {}
void cpuSetPollSkip(bool) {}
void cpuJitPollSkips(uint64_t *skips, uint64_t *cycles) { *skips = *cycles = 0; }
void cpuSetPollPeek(ArmCpu *, bool (*)(void *, uint32_t, uint32_t *), void *) {}
bool cpuArm920SetJit(ArmCpu *, bool on) { return !on; }
bool cpuArm920JitOn(const ArmCpu *) { return false; }
void cpuArm920DropJit(ArmCpu *) {}
uint32_t cpuArm920JitMaxUnchecked(const ArmCpu *) { return 0; }
unsigned cpuArm920JitPidTags(const ArmCpu *, uint64_t *restarts) {
    if (restarts) *restarts = 0;
    return 0;
}
bool cpuArm920JitCanRun(const ArmCpu *) { return false; }
bool cpuArm920StepSkipAllowed(const ArmCpu *) { return false; }
uint32_t cpuArm920RunJit(ArmCpu *, uint32_t, int *stop, uint32_t, uint32_t *instructions) {
    *stop = arm920JitExit;
    if (instructions) *instructions = 0;
    return 0;
}
#endif

bool cpuProcessPendingInterrupts(struct ArmCpu* cpu) {
    if (cpu->arm920Waiting) return false;
    if (cpu->waitingFiqs && !cpu->F) {
        cpuPrvException(cpu, cpu->vectorBase + ARM_VECTOR_OFFT_FIQ, cpu->regs[REG_NO_PC] + 4,
                        ARM_SR_MODE_FIQ | ARM_SR_I | ARM_SR_F);
        return true;
    }
    if (cpu->waitingIrqs && !cpu->I) {
        cpuPrvException(cpu, cpu->vectorBase + ARM_VECTOR_OFFT_IRQ, cpu->regs[REG_NO_PC] + 4,
                        ARM_SR_MODE_IRQ | ARM_SR_I);
        return true;
    }
    return false;
}

// memorySystemKind is cpuInterpreterKind(cpu), picked by the caller when it made the CPU.
template <int memorySystemKind, bool injected>
static FORCE_INLINE uint32_t cpuPrvCycleBody(struct ArmCpu *cpu, uint32_t cycles) {
    if (armKindIsArm920(memorySystemKind) && cpu->arm920Waiting) return 0;
    cpuProcessPendingInterrupts(cpu);

    if (cpu->memorySystemKind == ARM_MEMORY_SYSTEM_MMU) {
        cp15MMUCycle(cpu->cp15.cp15mmu);
    }
    patchOnBeforeExecute(cpu->patchDispatch, cpu->regs);

    cpuClearSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE | SLOW_PATH_REASON_RESCHEDULE |
                              SLOW_PATH_REASON_PACE_SYSCALL_BREAK |
                              SLOW_PATH_REASON_INJECTED_CALL_DONE);

#ifdef FROND_DYNARMIC
    // Fetch buffers loaded from a save are watched once the guest state is all in.
    if (armMemorySystemOf(memorySystemKind) == ARM_MEMORY_SYSTEM_MMU && cpu->jit && unlikely(icacheFetchUnwatched(cpu->ic)))
        icacheWatchFetchBuffers(cpu->ic);
#endif
    if (cpu->modePace) return cpuCyclePace(cpu, cycles);

#ifndef __EMSCRIPTEN__
    // nativePc is only set by RunToNativePc around a whole run, so it holds for this batch.
#ifdef FROND_DYNARMIC
    if (armMemorySystemOf(memorySystemKind) == ARM_MEMORY_SYSTEM_MMU && cpu->jit && !cpu->jitLoopOnly && !injected && !cpu->debugStub &&
        !cpu->nativePc[0] && !cpu->slowPath)
        return cpuPrvCycleJit<memorySystemKind>(cpu, cycles);
#endif
    if (!injected && !cpu->debugStub && !cpu->nativePc[0])
        return cpu->T ? cpuCycleLine<memorySystemKind, 2>(cpu, cycles) : cpuCycleLine<memorySystemKind, 4>(cpu, cycles);
#endif

    if (cpu->T)
        return cpu->debugStub ? cpuCycleThumb<memorySystemKind, injected, true>(cpu, cycles)
                              : cpuCycleThumb<memorySystemKind, injected, false>(cpu, cycles);
    else
        return cpu->debugStub ? cpuCycleArm<memorySystemKind, injected, true>(cpu, cycles)
                              : cpuCycleArm<memorySystemKind, injected, false>(cpu, cycles);
}

// A batch's clocks, with any memory charge left over from outside the loops, go into costClock.
static FORCE_INLINE uint32_t cpuPrvEndBatch(struct ArmCpu *cpu, uint32_t ran) {
    ran += cpu->jitHint.pending;
    cpu->jitHint.pending = 0;
    cpu->costClock += ran;
    return ran;
}

template <int memorySystemKind, bool injected>
ATTR_EMCC_NOINLINE uint32_t cpuCycle(struct ArmCpu *cpu, uint32_t cycles) {
    return cpuPrvEndBatch(cpu, cpuPrvCycleBody<memorySystemKind, injected>(cpu, cycles));
}

template uint32_t cpuCycle<ARM_MEMORY_SYSTEM_MMU, true>(struct ArmCpu *cpu, uint32_t cycles);
template uint32_t cpuCycle<ARM_MEMORY_SYSTEM_MPU, true>(struct ArmCpu *cpu, uint32_t cycles);
template uint32_t cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(struct ArmCpu *cpu, uint32_t cycles);
template uint32_t cpuCycle<ARM_MEMORY_SYSTEM_MPU, false>(struct ArmCpu *cpu, uint32_t cycles);
template uint32_t cpuCycle<ARM_MEMORY_SYSTEM_ARM920T, true>(struct ArmCpu *cpu, uint32_t cycles);
template uint32_t cpuCycle<ARM_MEMORY_SYSTEM_ARM920T, false>(struct ArmCpu *cpu, uint32_t cycles);

uint32_t cpuCycleAnyKind(struct ArmCpu *cpu, uint32_t cycles) {
    switch (cpuInterpreterKind(cpu)) {
        case ARM_MEMORY_SYSTEM_ARM920T: return cpuCycle<ARM_MEMORY_SYSTEM_ARM920T, false>(cpu, cycles);
        case ARM_MEMORY_SYSTEM_MMU: return cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, cycles);
        default: return cpuCycle<ARM_MEMORY_SYSTEM_MPU, false>(cpu, cycles);
    }
}

template <int memorySystemKind, bool thumb>
static uint32_t cpuPrvCycleOne(struct ArmCpu *cpu) {
    if (cpu->debugStub)
        return thumb ? cpuCycleThumb<memorySystemKind, true, true>(cpu, 1)
                     : cpuCycleArm<memorySystemKind, true, true>(cpu, 1);
    return thumb ? cpuCycleThumb<memorySystemKind, true, false>(cpu, 1)
                 : cpuCycleArm<memorySystemKind, true, false>(cpu, 1);
}

static uint32_t cpuPrvCyclePureBody(struct ArmCpu *cpu) {
    if (cpu->arm920Waiting) return 0;
    const bool arm920 = cpu->model == ArmCpuModel::arm920t;
    cpuClearSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE | SLOW_PATH_REASON_RESCHEDULE |
                              SLOW_PATH_REASON_PACE_SYSCALL_BREAK |
                              SLOW_PATH_REASON_INJECTED_CALL_DONE);

    patchOnBeforeExecute(cpu->patchDispatch, cpu->regs);

    if (cpu->modePace)
        return cpuCyclePace(cpu, 1);
    else if (cpu->T)
        return arm920 ? cpuPrvCycleOne<ARM_MEMORY_SYSTEM_ARM920T, true>(cpu)
               : (cpu->memorySystemKind == ARM_MEMORY_SYSTEM_MMU)
                   ? cpuPrvCycleOne<ARM_MEMORY_SYSTEM_MMU, true>(cpu)
                   : cpuPrvCycleOne<ARM_MEMORY_SYSTEM_MPU, true>(cpu);
    else
        return arm920 ? cpuPrvCycleOne<ARM_MEMORY_SYSTEM_ARM920T, false>(cpu)
               : (cpu->memorySystemKind == ARM_MEMORY_SYSTEM_MMU)
                   ? cpuPrvCycleOne<ARM_MEMORY_SYSTEM_MMU, false>(cpu)
                   : cpuPrvCycleOne<ARM_MEMORY_SYSTEM_MPU, false>(cpu);
}

uint32_t cpuCyclePure(struct ArmCpu *cpu) { return cpuPrvEndBatch(cpu, cpuPrvCyclePureBody(cpu)); }

void cpuStartInjectedSyscall(struct ArmCpu *cpu, uint32_t syscall) {
    const uint8_t table = syscall >> 12;
    uint32_t tableAddr;
    if (!cpuPrvMemOpExGeneric<4, false>(cpu, &tableAddr, cpu->regs[9] - table, true, NULL))
        ERR("failed to dispatch syscall %#010x: unable to read table address\n", syscall);

    const uint32_t offset = syscall & 0xfff;
    uint32_t entryAddr;
    if (!cpuPrvMemOpExGeneric<4, false>(cpu, &entryAddr, tableAddr + offset, true, NULL))
        ERR("failed to dispatch syscall %#010x: unable to read entry point\n", syscall);

    cpu->modePace = false;
    cpuPrvSetReg(cpu, REG_NO_PC, entryAddr);

    cpu->regs[REG_NO_LR] = INJECTED_CALL_LR_MAGIC;
}

void cpuStartInjectedSyscall68k(struct ArmCpu *cpu, uint16_t syscall) {
    cpuPrvPaceSyscall(cpu, syscall);
    cpu->regs[REG_NO_LR] = INJECTED_CALL_LR_MAGIC;
}

void cpuFinishInjectedSyscall68k(struct ArmCpu *cpu) {
    uint_fast8_t fsr = 0;

    // restore r0 / state pointer from stack
    if (!cpuPrvMemOpGeneric<4, false>(cpu, &cpu->regs[0], cpu->regs[REG_NO_SP], cpu->privileged,
                                      &fsr))
        ERR("failed to restore r0 after injected PACE trap");

    paceSetPriviledged(cpu->privileged);
    paceSetStatePtr(cpu->regs[0]);

    if (!paceLoad68kState()) ERR("failed to reload m68k state after injected PACE trap");
}

void cpuIrq(struct ArmCpu *cpu, bool fiq, bool raise) {  // unraise when acknowledged

    if (fiq) {
        if (raise) {
            cpu->waitingFiqs++;
        } else if (cpu->waitingFiqs) {
            cpu->waitingFiqs--;
        } else {
            fprintf(stderr, "Cannot unraise FIQ when none raised");
        }
    } else {
        if (raise) {
            cpu->waitingIrqs++;
        } else if (cpu->waitingIrqs) {
            cpu->waitingIrqs--;
        } else {
            fprintf(stderr, "Cannot unraise IRQ when none raised");
        }
    }

    cpu->waitingEventsTotal = cpu->waitingFiqs + cpu->waitingIrqs;
    cpuUpdateSlowPath(cpu);
}

void cpuCoprocessorRegister(struct ArmCpu *cpu, uint8_t cpNum, struct ArmCoprocessor *coproc) {
    cpu->coproc[cpNum] = *coproc;
}

void cpuSetVectorAddr(struct ArmCpu *cpu, uint32_t adr) { cpu->vectorBase = adr; }

uint16_t cpuGetCPAR(struct ArmCpu *cpu) { return cpu->CPAR; }

// Fast only runs iWMMXt instructions whose coprocessor CPAR allows (cpuJitPlain), so a
// change to bit 0 or 1 drops the translated code.
void cpuSetCPAR(struct ArmCpu *cpu, uint16_t cpar) {
#ifdef FROND_DYNARMIC
    if (cpu->jit) jitCparWritten(cpu->jit, cpu->wmmx && ((cpu->CPAR ^ cpar) & 3));
#endif
    cpu->CPAR = cpar;
}

void cpuSetWmmx(struct ArmCpu *cpu, struct Pxa270wmmx *wmmx) { cpu->wmmx = wmmx; }

void cpuArm920EnableWait(ArmCpu* cpu) { cpu->arm920WaitEnabled = true; }
bool cpuArm920BeginWait(ArmCpu* cpu) {
    if (!cpu->arm920WaitEnabled) return false;
    cpu->arm920Waiting = true;
    cpuSetSlowPath(cpu, SLOW_PATH_REASON_RESCHEDULE);
    return true;
}
bool cpuArm920Waiting(const ArmCpu* cpu) { return cpu->arm920Waiting; }
void cpuArm920EndWait(ArmCpu* cpu) { cpu->arm920Waiting = false; }

void cpuSetSleeping(struct ArmCpu *cpu) {
    cpu->sleeping = true;
    cpuUpdateSlowPath(cpu);
}

void cpuWakeup(struct ArmCpu *cpu) {
    cpu->sleeping = false;
    cpuUpdateSlowPath(cpu);
}

void cpuSetPid(struct ArmCpu *cpu, uint32_t pid) {
    // The ARM920T Fast engine's lean host TLB and its JIT's page table are keyed by VA: a change flushes
    // them, so none outlives the address space it was filled in. Decode lines are by MVA and stay; Fast
    // blocks are keyed by PID (jitRun).
    if (cpu->model == ArmCpuModel::arm920t && pid != cpu->pid) hostTlbFlush(&cpu->mem->hostTlb);
    cpu->pid = pid;
}

uint32_t cpuJitCodeMva(const struct ArmCpu *cpu, uint32_t va) {
    return cpu->model == ArmCpuModel::arm920t ? cpuPrvMva(cpu, va) : va;
}

uint32_t cpuGetPid(struct ArmCpu *cpu) { return cpu->pid; }

uint64_t cpuUndefinedTaken(const struct ArmCpu *cpu, uint32_t *pc, uint32_t *instr) {
    if (pc) *pc = cpu->undefinedPc;
    if (instr) *instr = cpu->undefinedInstr;
    return cpu->undefinedCount;
}

void cpuSetUndefinedHook(struct ArmCpu *cpu, void (*hook)(void *, uint32_t, uint32_t), void *context) {
    cpu->undefinedHook = hook;
    cpu->undefinedHookContext = context;
}

void cpuSetAbortLogLimit(struct ArmCpu *cpu, bool limited) { cpu->abortLogLimited = limited; }

void cpuSetSlowPath(struct ArmCpu *cpu, uint32_t reason) { cpu->slowPath |= reason; }

void cpuClearSlowPath(struct ArmCpu *cpu, uint32_t reason) { cpu->slowPath &= ~reason; }

uint32_t cpuGetSlowPathReason(struct ArmCpu *cpu) { return cpu->slowPath; }

void cpuSetBreakPaceSyscall(struct ArmCpu *cpu, uint16_t syscall) {
    cpu->breakPaceSyscall = syscall;
}

struct ArmMmu *cpuGetMMU(struct ArmCpu *cpu) {
    if (cpu->memorySystemKind != ARM_MEMORY_SYSTEM_MMU) {
        ERR("this system does not use a MMU");
    }

    return cpu->memorySystem.mmu;
}

struct ArmMpu *cpuGetMPU(struct ArmCpu *cpu) {
    if (cpu->memorySystemKind != ARM_MEMORY_SYSTEM_MPU) {
        ERR("this system does not use a MPU");
    }

    return cpu->memorySystem.mpu;
}

struct ArmMem *cpuGetMem(struct ArmCpu *cpu) { return cpu->mem; }

uint32_t cpuGetMemorySystemKind(struct ArmCpu *cpu) { return cpu->memorySystemKind; }

int cpuInterpreterKind(const struct ArmCpu *cpu) {
    return cpu->model == ArmCpuModel::arm920t ? ARM_MEMORY_SYSTEM_ARM920T : cpu->memorySystemKind;
}

void cpuAddM68kTrap0Handler(struct ArmCpu *cpu, uint32_t address, M68kTrapHandler handler) {
    (*cpu->m68kTrap0Handlers)[address] = handler;
}

void cpuRemoveM68kTrap0Handler(struct ArmCpu *cpu, uint32_t address) {
    cpu->m68kTrap0Handlers->erase(address);
}

#ifndef __EMSCRIPTEN__
bool cpuOriginalTg50PaceEntryForDiagnostic(ArmCpu* cpu) {
    if (!std::getenv("FROND_TG50_ORIGINAL_PACE_DIAGNOSTIC") || cpu->modePace || cpu->T ||
        cpu->regs[REG_NO_PC] != cpu->curInstrPC || cpu->curInstrPC != 0x20100064 ||
        cpu->regs[REG_NO_LR] != 0x20106468) return false;
    uint32_t pc;
    if (cpu->regs[0] > UINT32_MAX - 0x48 ||
        !cpuInspectRead(cpu, cpu->regs[0] + 0x44, &pc, sizeof(pc)) || pc != 0x20c1ed14)
        return false;
    const auto& region = cpu->mem->regions[REGION_ROM];
    auto* rom = static_cast<ArmRom*>(region.uD);
    if (!region.ub || !rom ||
        !pacePatchOriginalTg50EntryForDiagnostic(cpu->pacePatch, region.pa, romGetData(rom),
                                                romGetPeepholeBuffer(rom), romGetSize(rom)))
        return false;
    cpuInvalidateCodeCache(cpu);
    return true;
}
#endif

bool cpuIsModePace(struct ArmCpu *cpu) { return cpu->modePace; }

void cpuSetModePace(struct ArmCpu *cpu, bool modePace) { cpu->modePace = modePace; }

bool cpuIsThumb(struct ArmCpu *cpu) { return cpu->T; }

bool cpuHasPendingInterrupt(struct ArmCpu *cpu) { return cpu->waitingEventsTotal > 0; }

void cpuSetCurInstrPC(struct ArmCpu *cpu, uint32_t curInstrPC) { cpu->curInstrPC = curInstrPC; }

uint32_t cpuGetCurInstrPC(struct ArmCpu *cpu) { return cpu->curInstrPC; }

template <typename T>
void cpuSave(ArmCpu *cpu, T &savestate) {
    paceSave(savestate, cpu->pacePatch && cpu->pacePatch->hooksInRam);

    if (cpu->memorySystemKind == ARM_MEMORY_SYSTEM_MMU) {
        mmuSave(cpu->memorySystem.mmu, savestate);
        cp15MMUSave(cpu->cp15.cp15mmu, savestate);
    } else {
        mpuSave(cpu->memorySystem.mpu, savestate);
        cp15MPUSave(cpu->cp15.cp15mpu, savestate);
    }

    patchDispatchSave(cpu->patchDispatch, savestate);
    cpuCostSave(cpu, savestate);

    auto chunk = savestate.GetChunk(ChunkType::cpu, SAVESTATE_VERSION);
    if (!chunk) ERR("unable to allocate chunk");

    SaveChunkHelper helper(*chunk);
    cpu->DoSaveLoad(helper, SAVESTATE_VERSION);
}

template <typename T>
void cpuLoad(ArmCpu *cpu, T &loader) {
#ifndef __EMSCRIPTEN__
    cpuClearNativeRuntime(cpu);
#endif
    gdbStubResetPace(cpu->debugStub);
    cpuPrvIdleForget(cpu);
    icacheInval(cpu->ic);

    paceLoad(loader);

    if (cpu->memorySystemKind == ARM_MEMORY_SYSTEM_MMU) {
        mmuLoad(cpu->memorySystem.mmu, loader);
        cp15MMULoad(cpu->cp15.cp15mmu, loader);
    } else {
        mpuLoad(cpu->memorySystem.mpu, loader);
        cp15MPULoad(cpu->cp15.cp15mpu, loader);
    }
    patchDispatchLoad(cpu->patchDispatch, loader);
    cpuCostLoad(cpu, loader);

    uint32_t version;
    auto chunk = loader.GetChunkOrFail(ChunkType::cpu, SAVESTATE_VERSION, "cpu", version);
    if (!chunk) return;

    if (version < 3) {
        std::cerr << "not restoring incompatible CPU savestate version " << version << std::endl;
        loader.NotifyError();
        return;
    }

    LoadChunkHelper helper(*chunk);
    cpu->DoSaveLoad(helper, version);

#ifndef __EMSCRIPTEN__
    cpuClearSlowPath(cpu, SLOW_PATH_REASON_NATIVE_PC | SLOW_PATH_REASON_NATIVE_FAULT);
#endif
    cpu->waitingEventsTotal = cpu->waitingFiqs + cpu->waitingIrqs;
    cpuUpdateSlowPath(cpu);
}

template <typename U>
void ArmCpu::DoSaveLoad(U &chunkHelper, uint32_t version) {
    chunkHelper.DoBuffer32(regs, sizeof(regs) >> 2)
        .DoBuffer32(extra_regs, sizeof(extra_regs) >> 2)
        .Do32(SPSR)
        .Do32(flags)
        .Do8(M)
        .Do32(curInstrPC)
        .Do(typename U::Pack16() << waitingIrqs << waitingFiqs)
        .Do16(CPAR)
        .Do32(vectorBase)
        .Do32(pid)
        .Do(typename U::BoolPack() << privileged << modePace << sleeping << Q << T << I << F);

    if (version > 0) chunkHelper.Do32(paceOffset);
    if (version > 1) chunkHelper.Do32(slowPath);

    bank_usr.DoSaveLoad(chunkHelper);
    bank_svc.DoSaveLoad(chunkHelper);
    bank_abt.DoSaveLoad(chunkHelper);
    bank_und.DoSaveLoad(chunkHelper);
    bank_irq.DoSaveLoad(chunkHelper);
    bank_fiq.DoSaveLoad(chunkHelper);
}

template <typename T>
void cpuPrepareInjectedCall(struct ArmCpu *cpu, T &savestate) {
    cpuSave(cpu, savestate);
}

template <typename T>
void cpuFinishInjectedCall(struct ArmCpu *cpu, T &loader) {
    auto sleeping = cpu->sleeping;
    auto waitingIrqs = cpu->waitingIrqs;
    auto waitingFiqs = cpu->waitingFiqs;

#ifndef __EMSCRIPTEN__
    const auto nativeDepth = cpu->nativeCallDepth;
    const auto nativeSession = cpu->nativeSessionActive;
    const auto stackLower=cpu->nativeStackLower, stackUpper=cpu->nativeStackUpper;
    const auto stackOwnerAddress=cpu->nativeStackOwnerAddress, stackOwnerValue=cpu->nativeStackOwnerValue;
    const auto scratchBase = cpu->nativeScratchBase, scratchSize = cpu->nativeScratchSize;
    auto* scratchData = cpu->nativeScratchData;
    decltype(cpu->nativeCallbacks) callbacks;
    memcpy(callbacks, cpu->nativeCallbacks, sizeof(callbacks));
#endif
    cpuLoad(cpu, loader);
#ifndef __EMSCRIPTEN__
    memcpy(cpu->nativeCallbacks, callbacks, sizeof(callbacks));
    cpu->nativeCallDepth = nativeDepth;
    cpu->nativeSessionActive = nativeSession;
    cpu->nativeStackLower=stackLower; cpu->nativeStackUpper=stackUpper;
    cpu->nativeStackOwnerAddress=stackOwnerAddress; cpu->nativeStackOwnerValue=stackOwnerValue;
    cpu->nativeScratchBase = scratchBase; cpu->nativeScratchSize = scratchSize;
    cpu->nativeScratchData = scratchData;
#endif

    cpu->sleeping = sleeping;
    cpu->waitingIrqs = waitingIrqs;
    cpu->waitingFiqs = waitingFiqs;
    cpu->waitingEventsTotal = cpu->waitingFiqs + cpu->waitingIrqs;

    cpuUpdateSlowPath(cpu);
}

template void cpuSave<Savestate<ChunkType>>(ArmCpu *cpu, Savestate<ChunkType> &savestate);
template void cpuSave<SavestateProbe<ChunkType>>(ArmCpu *cpu, SavestateProbe<ChunkType> &savestate);
template void cpuLoad<SavestateLoader<ChunkType>>(ArmCpu *cpu, SavestateLoader<ChunkType> &loader);

template void cpuPrepareInjectedCall<Savestate<ChunkType>>(ArmCpu *cpu,
                                                           Savestate<ChunkType> &savestate);
template void cpuPrepareInjectedCall<SavestateProbe<ChunkType>>(
    ArmCpu *cpu, SavestateProbe<ChunkType> &savestate);
template void cpuFinishInjectedCall<SavestateLoader<ChunkType>>(ArmCpu *cpu,
                                                                SavestateLoader<ChunkType> &loader);

// Execution-thread observer; ownership follows this CPU instance.
void cpuSetEarlyAudioHook(ArmCpu* cpu,void (*hook)(ArmCpu*,void*,uint32_t,uint32_t),void* context,uint32_t ready,uint32_t dispose) {
    cpu->earlyAudioHook=hook;cpu->earlyAudioContext=context;
    cpu->earlyAudioFrom[0]=ready;cpu->earlyAudioFrom[1]=dispose;
}

void cpuGetInstructionFetchState(ArmCpu* cpu,uint32_t* state) {icacheGetFetchState(cpu->ic,state);}
bool cpuRestoreInstructionFetchState(ArmCpu* cpu,const uint32_t* state) {return icacheSetFetchState(cpu->ic,state);}

void cpuArm920SetIcacheIndexCompatibility(ArmCpu* cpu, bool enabled) {
    if (cpu->model == ArmCpuModel::arm920t)
        cp15MMUArm920SetIcacheIndexCompatibility(cpu->cp15.cp15mmu, enabled);
}

void cpuArm920SetMemoryCheck(ArmCpu* cpu, Arm920MemoryCheck check, void* context) {
    if (cpu->model != ArmCpuModel::arm920t) return;
    auto* dc = cp15MMUArm920DataCache(cpu->cp15.cp15mmu);
    dc->mem = cpu->mem;
    dc->check = check;
    dc->context = context;
}
Arm920DataGuard cpuArm920DataGuard(ArmCpu* cpu) {
    return cpu->model == ArmCpuModel::arm920t ? cp15MMUArm920DataCache(cpu->cp15.cp15mmu)->guard : Arm920DataGuard{};
}

static bool cpuPrvArm920StateIdle(const ArmCpu* cpu) {
    if (cpu->model != ArmCpuModel::arm920t || cpu->memorySystemKind != ARM_MEMORY_SYSTEM_MMU) return false;
    if (cpu->modePace || cpu->sleeping || cpu->earlyAudioHook) return false;
#ifndef __EMSCRIPTEN__
    if (cpu->nativePc[0] || cpu->nativePc[1] || cpu->nativeCallDepth || cpu->nativeSessionActive) return false;
#endif
    return true;
}

bool cpuArm920CaptureState(const ArmCpu* cpu, Arm920SavedState& s) {
    if (!cpuPrvArm920StateIdle(cpu)) return false;
    s.exceptionSequence = cpu->exceptionSequence;
    memcpy(s.regs, cpu->regs, sizeof(s.regs));
    memcpy(s.extraRegs, cpu->extra_regs, sizeof(s.extraRegs));
    const ArmBankedRegs* banks[] = {&cpu->bank_usr, &cpu->bank_svc, &cpu->bank_abt,
                                    &cpu->bank_und, &cpu->bank_irq, &cpu->bank_fiq};
    for (unsigned n = 0; n < 6; ++n) {
        s.banks[n][0] = banks[n]->R13; s.banks[n][1] = banks[n]->R14; s.banks[n][2] = banks[n]->SPSR;
    }
    s.spsr = cpu->SPSR;
    s.flags = cpu->flags;
    s.curInstrPC = cpu->curInstrPC;
    s.vectorBase = cpu->vectorBase;
    s.pid = cpu->pid;
    s.slowPath = cpu->slowPath;
    s.waitingIrqs = cpu->waitingIrqs;
    s.waitingFiqs = cpu->waitingFiqs;
    s.cpar = cpu->CPAR;
    s.mode = cpu->M;
    s.q = cpu->Q; s.t = cpu->T; s.i = cpu->I; s.f = cpu->F;
    s.privileged = cpu->privileged;
    s.waiting = cpu->arm920Waiting;
    s.waitEnabled = cpu->arm920WaitEnabled;
    cp15MMUArm920Capture(cpu->cp15.cp15mmu, s);
    mmuArm920Capture(cpu->memorySystem.mmu, s);
    icacheArm920Capture(cpu->ic, s);
    return true;
}

bool cpuArm920StateFits(const ArmCpu* cpu, const Arm920SavedState& s) {
    return cpuPrvArm920StateIdle(cpu) && s.cpar == cpu->CPAR && s.waitEnabled == cpu->arm920WaitEnabled &&
           (!s.waiting || s.waitEnabled);
}

void cpuArm920ApplyState(ArmCpu* cpu, const Arm920SavedState& s) {
    cpu->exceptionSequence = s.exceptionSequence;
    memcpy(cpu->regs, s.regs, sizeof(s.regs));
    memcpy(cpu->extra_regs, s.extraRegs, sizeof(s.extraRegs));
    ArmBankedRegs* banks[] = {&cpu->bank_usr, &cpu->bank_svc, &cpu->bank_abt,
                              &cpu->bank_und, &cpu->bank_irq, &cpu->bank_fiq};
    for (unsigned n = 0; n < 6; ++n) {
        banks[n]->R13 = s.banks[n][0]; banks[n]->R14 = s.banks[n][1]; banks[n]->SPSR = s.banks[n][2];
    }
    cpu->SPSR = s.spsr;
    cpu->flags = s.flags;
    cpu->curInstrPC = s.curInstrPC;
    cpu->vectorBase = s.vectorBase;
    cpu->pid = s.pid;
    cpu->slowPath = s.slowPath;
    cpu->waitingIrqs = s.waitingIrqs;
    cpu->waitingFiqs = s.waitingFiqs;
    cpu->waitingEventsTotal = s.waitingIrqs + s.waitingFiqs;
    cpu->M = s.mode;
    cpu->Q = s.q; cpu->T = s.t; cpu->I = s.i; cpu->F = s.f;
    cpu->privileged = s.privileged;
    cpu->arm920Waiting = s.waiting;
    cp15MMUArm920Apply(cpu->cp15.cp15mmu, s);
    mmuArm920Apply(cpu->memorySystem.mmu, s);
    icacheArm920Apply(cpu->ic, s);
}

bool cpuJitSetExecutionCounts(ArmCpu* cpu, uint64_t blocks, uint64_t instructions) {
#ifdef FROND_DYNARMIC
    if (cpu->jit) {
        jitSetExecutionCounts(cpu->jit, blocks, instructions);
        return true;
    }
#endif
    (void)cpu; (void)blocks; (void)instructions;
    return false;
}

void cpuEmitArm920Diagnostic(const ArmCpu* cpu, const ArmDiagnosticSink& sink) {
    if (cpu->model != ArmCpuModel::arm920t || cpu->memorySystemKind != ARM_MEMORY_SYSTEM_MMU) return;
    sink.emit("registers", cpu->regs, 16);
    const uint32_t state[] = {uint32_t(cpu->flags | (cpu->Q ? ARM_SR_Q : 0) |
        (cpu->T ? ARM_SR_T : 0) | (cpu->I ? ARM_SR_I : 0) | (cpu->F ? ARM_SR_F : 0) | cpu->M),
        cpu->SPSR, cpu->pid, cpu->curInstrPC, cpu->vectorBase, uint32_t(cpu->privileged),
        uint32_t(cpu->sleeping), uint32_t(cpu->arm920WaitEnabled), uint32_t(cpu->arm920Waiting),
        cpu->waitingIrqs, cpu->waitingFiqs, cpu->waitingEventsTotal, cpu->slowPath,
        uint32_t(cpu->exceptionSequence), uint32_t(cpu->exceptionSequence >> 32)};
    sink.emit("cpu", state, sizeof(state)/4);
    // The current bank's R13/R14/SPSR live in registers/cpu, not its saved slot.
    const ArmBankedRegs* banks[] = {&cpu->bank_usr,&cpu->bank_svc,&cpu->bank_abt,
        &cpu->bank_und,&cpu->bank_irq,&cpu->bank_fiq};
    const uint32_t modes[] = {ARM_SR_MODE_USR,ARM_SR_MODE_SVC,ARM_SR_MODE_ABT,
        ARM_SR_MODE_UND,ARM_SR_MODE_IRQ,ARM_SR_MODE_FIQ};
    for (unsigned i=0;i<6;++i) {
        const auto& b = *banks[i];
        const uint32_t bank[] = {modes[i],b.R13,b.R14,b.SPSR};
        sink.emit("saved-bank",bank,4);
    }
    sink.emit("extra-r8-r12",cpu->extra_regs,5);
    cp15MMUEmitArm920Diagnostic(cpu->cp15.cp15mmu,sink);
    mmuEmitDiagnostic(cpu->memorySystem.mmu,sink);
}
