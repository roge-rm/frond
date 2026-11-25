// dynarmic JIT for ordinary guest code (trial). Built as C++20 with FROND_DYNARMIC.
// MRS and MSR run in generated code through the fork's psr_coprocessor, and iWMMXt data
// processing and register transfers through the cp0 and cp1 slots, some p15 accesses
// through the p15 slot, and SWI through CallSVC. Each CPU has one JIT per privilege level
// (user, privileged), each with its own code cache and page table, so switching between
// them keeps both. Other privileged, coprocessor and Frond's special instructions end the
// block and go back to the interpreter, which also takes every other exception and IRQ
// between runs.
#include "jit_dynarmic.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "CPU.h"
#include "MMU.h"
#include "pxa270_WMMX.h"
#include "xscale_cost.h"
#include "dynarmic/frontend/A32/a32_ir_emitter.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A32/coprocessor.h"
#include "dynarmic/ir/terminal.h"

namespace {

using namespace Dynarmic;
using PageTable = std::array<std::uint8_t *, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;

// MRC and MCR p15 c3 run in privileged mode from generated code, for jitDomainCalls.
std::atomic<uint64_t> domainCalls{0};
std::atomic<uint64_t> psrInterprets{0};

// Counters for FROND_JIT_STATS=1.
struct Stats {
    uint64_t runs = 0, ran = 0, interpret = 0, halt = 0, fault = 0, aborts = 0, invalAll = 0, invalRange = 0, clears = 0;
    uint64_t cold = 0;  // runs ended at code not compiled yet
    uint64_t queued = 0;  // of those, at code waiting for the compile thread
    uint64_t psr = 0;  // MSR CPSR and MOVS PC calls from generated code
    uint64_t psrInterpret = 0;  // MSR CPSRs the in-place check sent to the interpreter
    uint64_t cp15 = 0, cp15User = 0;  // MRC and MCR p15 c3 and c7 c5 1 calls, and those in user mode
    uint64_t icacheLine = 0;  // of those, MCR p15 c7 c5 1 (I-cache line invalidate)
    uint64_t privSwitches = 0;  // runs ended by a privilege change in generated code
    uint64_t swi = 0;  // SWIs taken from generated code
    uint64_t wmmx = 0;  // iWMMXt calls from generated code
    uint64_t wmmxXfer = 0, wmmxFaults = 0;  // of those, WLDR and WSTR, and those that failed
    uint64_t cparWrites = 0, cparChanges = 0;  // CPAR writes, and those that dropped the code
    // Interpreter exits at cp0 and cp1 words (not SWI) by class: CDP, MCR/MRC, MCRR/MRRC,
    // LDC/STC, and cond NV forms of any of them. Counted with FROND_JIT_STATS only.
    uint64_t interpWmmx[5] = {};
    // Code pages noted, dropped for a write, a changed mapping, a guest invalidation
    // (unwatched pages only) or lost fetch permission, and how often the mappings were checked.
    uint64_t codePages = 0, unwatched = 0, pagesWritten = 0, mapChecks = 0, mapChanged = 0, unwatchedDropped = 0;
    uint64_t fetchDropped = 0;
    // ARM920T: code pages of another PID's slot a flush found moved, those found mapped as before when the
    // PID came back, compile-thread blocks installed under another PID than the one they were fetched under,
    // and suspended pages whose blocks at their MVA a later flush dropped.
    uint64_t suspends = 0, resumes = 0, otherPidInstalls = 0, suspendedMvaDrops = 0;
    // ARM920T Fast blocks: mapping checks that went through every code page (the rest took only the pages
    // of table lines that changed), the pages all checks walked, the changed lines and the most lines kept.
    uint64_t fullMapChecks = 0, mapPagesChecked = 0, tableLinesChanged = 0, tableLines = 0;
    // Fills of code pages the MMU didn't list (all live pages scanned), code pages taken for those and listed
    // fills, pages they dropped outside a check, and filled pages whose entry is the mapping their code has.
    uint64_t tlbLiveScans = 0, fillRechecks = 0, fillDrops = 0, fillSame = 0;
    uint64_t tableMoves = 0;  // mapping checks run with no flush, for table words that changed
    uint64_t tableFaultFills = 0;  // stores of an entry over a fault one in a walked table word (no check)
    std::unordered_map<uint32_t, uint64_t> interpPc;
    // Instruction at each interpreted PC (Thumb: the halfword), and the guest PC current
    // at each whole-cache invalidation with the word there.
    std::unordered_map<uint32_t, uint32_t> interpWord;
    std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> invalAllPc;
    std::unordered_map<uint32_t, uint64_t> writtenPage;  // virtual page dropped for a write
    std::unordered_map<uint32_t, uint64_t> writtenAt;    // virtual address of each write that dropped code
    // Stores to a flagged line that overlapped no block, and line flag updates after writes (pages
    // left with code, pages with none left).
    uint64_t writesMissed = 0, remasks = 0, remaskFreed = 0;
    std::atomic<uint64_t> translated{0};  // blocks translated, on either thread
    // Callback reads and writes by virtual page (bit 31 of the key set for writes).
    std::unordered_map<uint32_t, uint64_t> callbackPage;
    void NoteCallback(uint32_t va, bool write) {
        if (on) callbackPage[(va >> 12) | (write ? 0x80000000u : 0)]++;
    }
    // Refused accesses that ended a run, by virtual address, size and direction.
    std::unordered_map<uint64_t, uint64_t> faultAccess;
    std::unordered_map<uint32_t, uint64_t> faultPc;  // pc of the instruction a refused access ended the run on
    const bool on = [] {
        const char *v = getenv("FROND_JIT_STATS");
        return v && *v && strcmp(v, "0");
    }();
    uint64_t pageClears = 0, pageFills = 0, reads = 0, writes = 0, domainChecks = 0, pageDrops = 0, pageReadOnly = 0;
    // Pages mapped for direct stores, and pages sent back to the callbacks by
    // jitWriteProtect or a code flag.
    uint64_t writeMaps = 0, writeProtects = 0;
    // Compile thread counts: JITs still alive at exit, and those of JITs gone before.
    std::vector<const A32::Jit *> jits;  // under jitsMutex: user level JITs are made on emulation threads
    std::mutex jitsMutex;
    A32::Jit::CompileStats compiled;
    void AddCompiled(const A32::Jit &jit) {
        const A32::Jit::CompileStats c = jit.GetCompileStats();
        compiled.queued += c.queued;
        compiled.installed += c.installed;
        compiled.stale += c.stale;
        compiled.cancelled += c.cancelled;
        compiled.overflow += c.overflow;
        compiled.no_space += c.no_space;
        compiled.ahead_queued += c.ahead_queued;
        compiled.ahead_compiled += c.ahead_compiled;
        compiled.ahead_used += c.ahead_used;
        compiled.ahead_skipped += c.ahead_skipped;
    }
    ~Stats() {
        if (!on) return;
        for (const A32::Jit *j : jits) AddCompiled(*j);
        fprintf(stderr,
                "jit: compile thread queued %llu installed %llu stale %llu cancelled %llu overflow %llu "
                "noSpace %llu\n",
                (unsigned long long)compiled.queued, (unsigned long long)compiled.installed,
                (unsigned long long)compiled.stale, (unsigned long long)compiled.cancelled,
                (unsigned long long)compiled.overflow, (unsigned long long)compiled.no_space);
        fprintf(stderr, "jit: compile ahead queued %llu compiled %llu used %llu unused %llu skipped %llu\n",
                (unsigned long long)compiled.ahead_queued, (unsigned long long)compiled.ahead_compiled,
                (unsigned long long)compiled.ahead_used,
                (unsigned long long)(compiled.ahead_compiled - compiled.ahead_used),
                (unsigned long long)compiled.ahead_skipped);
        fprintf(stderr,
                "jit: runs %llu ran %llu interpret %llu cold %llu queued %llu halt %llu fault %llu aborts %llu invalAll %llu "
                "invalRange %llu clears %llu pageClears %llu domainChecks %llu pageDrops %llu pageReadOnly %llu pageFills %llu reads %llu writes %llu\n",
                (unsigned long long)runs, (unsigned long long)ran, (unsigned long long)interpret, (unsigned long long)cold,
                (unsigned long long)queued,
                (unsigned long long)halt, (unsigned long long)fault, (unsigned long long)aborts, (unsigned long long)invalAll,
                (unsigned long long)invalRange, (unsigned long long)clears, (unsigned long long)pageClears,
                (unsigned long long)domainChecks, (unsigned long long)pageDrops, (unsigned long long)pageReadOnly,
                (unsigned long long)pageFills, (unsigned long long)reads, (unsigned long long)writes);
        fprintf(stderr,
                "jit: codePages %llu unwatched %llu pagesWritten %llu mapChecks %llu mapChanged %llu "
                "unwatchedDropped %llu fetchDropped %llu msrCpsr %llu writeMaps %llu writeProtects %llu cp15 %llu "
                "cp15User %llu icacheLine %llu privSwitches %llu swi %llu msrCpsrInterpret %llu\n",
                (unsigned long long)codePages, (unsigned long long)unwatched, (unsigned long long)pagesWritten,
                (unsigned long long)mapChecks, (unsigned long long)mapChanged, (unsigned long long)unwatchedDropped,
                (unsigned long long)fetchDropped,
                (unsigned long long)psr, (unsigned long long)writeMaps, (unsigned long long)writeProtects,
                (unsigned long long)cp15, (unsigned long long)cp15User, (unsigned long long)icacheLine,
                (unsigned long long)privSwitches, (unsigned long long)swi, (unsigned long long)psrInterpret);
        fprintf(stderr, "jit: suspends %llu resumes %llu otherPidInstalls %llu suspendedMvaDrops %llu\n",
                (unsigned long long)suspends, (unsigned long long)resumes, (unsigned long long)otherPidInstalls,
                (unsigned long long)suspendedMvaDrops);
        fprintf(stderr,
                "jit: fullMapChecks %llu mapPagesChecked %llu tableLinesChanged %llu tableLines %llu tlbLiveScans %llu "
                "fillRechecks %llu fillDrops %llu fillSame %llu tableMoves %llu tableFaultFills %llu\n",
                (unsigned long long)fullMapChecks, (unsigned long long)mapPagesChecked,
                (unsigned long long)tableLinesChanged, (unsigned long long)tableLines,
                (unsigned long long)tlbLiveScans, (unsigned long long)fillRechecks, (unsigned long long)fillDrops,
                (unsigned long long)fillSame, (unsigned long long)tableMoves,
                (unsigned long long)tableFaultFills);
        fprintf(stderr,
                "jit: wmmx %llu ldc/stc %llu failed %llu cparWrites %llu cparChanges %llu interpret wmmx "
                "cdp %llu mcr/mrc %llu mcrr/mrrc %llu ldc/stc %llu nv %llu\n",
                (unsigned long long)wmmx, (unsigned long long)wmmxXfer, (unsigned long long)wmmxFaults,
                (unsigned long long)cparWrites, (unsigned long long)cparChanges,
                (unsigned long long)interpWmmx[0], (unsigned long long)interpWmmx[1],
                (unsigned long long)interpWmmx[2], (unsigned long long)interpWmmx[3],
                (unsigned long long)interpWmmx[4]);
        std::vector<std::pair<uint64_t, uint32_t>> top;
        for (auto &[pc, n] : interpPc) top.push_back({n, pc});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 12; i++)
            fprintf(stderr, "jit: interpret %08x %llu word %08x\n", top[i].second,
                    (unsigned long long)top[i].first, interpWord[top[i].second]);
        top.clear();
        for (auto &[pc, e] : invalAllPc) top.push_back({e.first, pc});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 12; i++)
            fprintf(stderr, "jit: invalAll at %08x %llu word %08x\n", top[i].second,
                    (unsigned long long)top[i].first, invalAllPc[top[i].second].second);
        top.clear();
        for (auto &[page, n] : writtenPage) top.push_back({n, page});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 8; i++)
            fprintf(stderr, "jit: written page %08x %llu\n", top[i].second << 12, (unsigned long long)top[i].first);
        top.clear();
        fprintf(stderr, "jit: translated %llu writesMissed %llu remasks %llu remaskFreed %llu\n",
                (unsigned long long)translated.load(), (unsigned long long)writesMissed, (unsigned long long)remasks,
                (unsigned long long)remaskFreed);
        for (auto &[va, n] : writtenAt) top.push_back({n, va});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 12; i++)
            fprintf(stderr, "jit: written at %08x %llu\n", top[i].second, (unsigned long long)top[i].first);
        top.clear();
        for (auto &[key, n] : callbackPage) top.push_back({n, key});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 16; i++)
            fprintf(stderr, "jit: callback %s page %08x %llu\n", top[i].second >> 31 ? "write" : "read",
                    top[i].second << 12, (unsigned long long)top[i].first);
        std::vector<std::pair<uint64_t, uint64_t>> faults;
        for (auto &[key, n] : faultAccess) faults.push_back({n, key});
        std::sort(faults.rbegin(), faults.rend());
        for (size_t i = 0; i < faults.size() && i < 12; i++)
            fprintf(stderr, "jit: fault %s %08x size %u %llu\n", faults[i].second >> 40 ? "write" : "read",
                    uint32_t(faults[i].second), unsigned(faults[i].second >> 32) & 0xff,
                    (unsigned long long)faults[i].first);
        top.clear();
        for (auto &[pc, n] : faultPc) top.push_back({n, pc});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 12; i++)
            fprintf(stderr, "jit: fault at %08x %llu\n", top[i].second, (unsigned long long)top[i].first);
    }
} stats;

constexpr HaltReason kInterpret = HaltReason::UserDefined2;
constexpr HaltReason kSlowPath = HaltReason::UserDefined3;
// A data access that faults ends the run on that instruction (check_halt_on_memory_access)
// with the fault in Env::dataAbort, and the CPU takes the abort there without running the
// instruction again (FROND_JIT_ABORT): the accesses before it in the same instruction (an
// LDM or STM's first words) are done once, as in Exact. An unaligned access is refused
// before it's tried, and the interpreter runs the instruction instead.
constexpr HaltReason kFault = HaltReason::MemoryAbort;
// The block at PC isn't compiled yet (ShouldCompile), for the interpreter.
constexpr HaltReason kCold = HaltReason::UserDefined4;
// MSR CPSR or an exception return changed the privilege: the run ends there and the next
// one goes to the other privilege level's JIT.
constexpr HaltReason kPrivilege = HaltReason::UserDefined5;
// A SWI from generated code: the run ends at the vector.
constexpr HaltReason kSwi = HaltReason::UserDefined6;
// Memory charges (Fast at Original) used up what was left of the budget: the run ends at the next
// link, as when the cycles run out.
constexpr HaltReason kCharged = HaltReason::UserDefined7;

// The instructions the p15 slot runs (jitCp15SlotWord). Each ends its block, so a user
// mode one can be undone and handed to the interpreter, which raises the undefined
// instruction, and the budget is checked right after it, as after an interpreted one.
inline bool Cp15Slot(uint32_t w, bool arm920) { return arm920 ? jitCp15SlotWord920(w) : jitCp15SlotWord(w); }

// The p15 slot's instructions end their block (Cp15Slot).
// Translation and emission of a block run back to back on one thread (the guest thread or
// the compile thread), so the block's one p15 instruction is still current there when the
// coprocessor compiles it.
// Slot for the fork's psr_coprocessor, one XScale doesn't have. Real coprocessor
// instructions never reach dynarmic (cpuJitPlain), so the slot is only used for MRS and MSR (and the
// ARM920T's access pcs).
constexpr std::size_t kPsrCoprocessor = 8;

thread_local uint32_t cp15Pc = ~0u, cp15Rd = 0, cp15Word = 0;

// ARM920T access pcs (cpuArm920SetJitAccessPcs) that start their block: the block before one links to it, so
// only finished blocks' ticks come before its access. The slot is cleared again so a marked access that went to a
// host page, or none, leaves nothing set for a later read at another pc: by the marked block's second
// instruction, and by every other block as it starts, for a marked block that ended right after its access (a
// capped or single-step block, a load to pc, a line bound). clearPending: the clear is still to come in this
// block. The entry clear for a conditional block runs when the condition passes.
thread_local bool clearPending = false;

inline void ClearAccessPc(A32::IREmitter &ir) {
    ir.CoprocSendOneWord(kPsrCoprocessor, false, 3, A32::CoprocReg::C0, A32::CoprocReg::C0, 0, ir.Imm32(0));
}

// Before the code read. startBlocks: the access pcs start their blocks (cpuArm920JitAccessPcsStartBlocks).
bool AccessPcSplit(bool startBlocks, bool startsBlock, A32::VAddr pc, A32::IREmitter &ir) {
    const bool start = pc == A32::LocationDescriptor{ir.block.Location()}.PC();
    if (start) {
        clearPending = startBlocks;
    } else if (std::exchange(clearPending, false)) {
        ClearAccessPc(ir);
    }
    if (startsBlock && !start) {
        ir.SetTerm(IR::Term::LinkBlock{ir.current_location});
        return false;
    }
    return true;
}

// Not an instruction IsConditionPassed sees with a condition: ARM's cond field, Thumb's IT blocks and B<c> (T1).
// word is the aligned word holding the instruction.
inline bool Unconditional(bool thumb, uint32_t word, A32::VAddr pc, const A32::IREmitter &ir) {
    if (!thumb) return word >> 28 == 14;
    const uint32_t half = (pc & 2) ? word >> 16 : word & 0xffff;
    return !ir.current_location.IT().IsInITBlock() && !((half & 0xf000) == 0xd000 && (half & 0x0e00) != 0x0e00);
}

// After a code read that goes to the JIT. A marked pc's value goes to cpuJitAccessPcSlot before the instruction
// (psr slot, opc1 3). One that starts its block is only noted unconditional, as a failed one would leave the slot
// set when the block is skipped.
void NoteAccessPc(bool thumb, uint32_t word, bool marked, bool startsBlock, A32::VAddr pc, A32::IREmitter &ir) {
    const bool note = marked && (!startsBlock || Unconditional(thumb, word, pc, ir));
    if (clearPending && !note && pc == A32::LocationDescriptor{ir.block.Location()}.PC()) {
        ClearAccessPc(ir);
        clearPending = false;
    }
    if (!note) return;
    ir.CoprocSendOneWord(kPsrCoprocessor, false, 3, A32::CoprocReg::C0, A32::CoprocReg::C0, 0, ir.Imm32(pc));
    clearPending = startsBlock;
}

// PreCodeReadHook's work: the instruction at pc goes to the JIT if plain(pc, word) says
// so (word gets the aligned word holding it); a p15 c3 access ends its block after it.
template <typename Plain>
bool CodeReadHook(bool thumb, bool arm920, A32::VAddr pc, A32::IREmitter &ir, Plain plain) {
    if (pc == A32::LocationDescriptor{ir.block.Location()}.PC()) {
        cp15Pc = ~0u;
    } else if (cp15Pc != ~0u && pc == cp15Pc + 4) {
        ir.SetTerm(IR::Term::LinkBlock{ir.current_location});
        return false;
    }
    uint32_t w;
    if (plain(pc, w)) {
        if (!thumb && Cp15Slot(w, arm920)) {
            cp15Pc = pc;
            cp15Rd = (w >> 12) & 15;
            cp15Word = w;
        }
        return true;
    }
    ir.SetTerm(IR::Term::Interpret(ir.current_location));
    return false;
}

// Instructions after which dynarmic surely ends the block: branches and writes to PC. The
// snapshot stops there; if translation goes on anyway, it overflows and a whole one is taken.
bool EndsBlock(uint32_t w, uint32_t pc, bool thumb) {
    if (thumb) {
        const uint32_t h = (pc & 2) ? w >> 16 : w & 0xffff;
        if (h >= 0xe000 && h < 0xe800) return true;                        // B
        if ((h & 0xf000) == 0xd000 && (h & 0x0f00) < 0x0e00) return true;  // B<cond>
        if (h >= 0xf000) return true;                                      // BL, BLX pair
        if ((h & 0xff00) == 0xdf00) return true;                           // SWI
        if ((h & 0xff00) == 0x4700 || (h & 0xff00) == 0xbd00) return true;  // BX, BLX, POP {pc}
        return (h & 0xfd87) == 0x4487;                                     // ADD, MOV pc
    }
    if ((w & 0x0e000000) == 0x0a000000) return true;  // B, BL, BLX imm
    if ((w >> 28) == 15) return false;
    if ((w & 0x0f000000) == 0x0f000000) return true;  // SWI
    if ((w & 0x0ffffff0) == 0x012fff10 || (w & 0x0ffffff0) == 0x012fff30) return true;  // BX, BLX
    if ((w & 0x0e108000) == 0x08108000) return true;  // LDM with PC
    if ((w & 0x0c10f000) == 0x0410f000) return true;  // LDR PC
    return (w & 0x0c00f000) == 0x0000f000;            // data processing to PC
}

// The code one block may read, for the compile thread: the words from its start and
// whether each instruction may go to the JIT (cpuJitPlain), taken on the guest thread at
// the request, so translation there gives what it would have given in place then.
// Translation marks the words it reads; before the block is installed they are fetched
// again and compared (Env::CodeUnchanged).
// Ticks per instruction from the cost model (xscale_cost.h), as the interpreter charges them: the
// table's cost when the condition passed, 1 when it failed. On PXA a tick is a core clock. On the
// ARM920T a tick holds the clocks above bit 32 and the instructions below (a Thumb pair counts 2,
// as the board steps each half), since its board counts both; budgets there are clocks << 32, so a
// run still stops once its clocks reach the budget. With no cost model there a Thumb pair is 2 clocks
// too, so clocks are instructions and a budget can be given in instructions.
struct TickModel {
    bool on = false;
    uint8_t core = 0;
    unsigned shift = 0;

    uint64_t Ticks(bool thumb, uint32_t instruction) const {
        const uint64_t n = thumb && instruction > 0xffff ? 2 : 1;
        if (!shift) return on ? cost::jitInstructionCost(thumb, instruction, cost::Core(core)) : 1;
        const uint64_t c = on ? cost::jitInstructionCost(thumb, instruction, cost::Core(core)) : n;
        return (c << shift) | n;
    }
    uint64_t Failed() const { return shift ? (uint64_t(1) << shift) | 1 : 1; }
};

TickModel jitPrvTickModel(ArmCpu *cpu) {
    TickModel m;
    uint8_t core = 0;
    m.on = cpuJitCostCore(cpu, &core);
    m.core = core;
    m.shift = cpuGetModel(cpu) == ArmCpuModel::arm920t ? 32 : 0;
    return m;
}

struct Snapshot final : public A32::CodeSnapshot {
    uint32_t base = 0;  // the word holding the first instruction
    std::vector<uint32_t> words;
    std::vector<uint8_t> fetched, read;  // per word
    std::vector<std::pair<uint32_t, bool>> plain;  // each instruction in order, and its check
    std::vector<uint32_t> marked;  // access pcs among them
    uint32_t splitAt = ~0u;  // the marked pc the snapshot stopped before (startBlocks), which ends the block
    size_t next = 0;
    bool arm920 = false;
    bool startBlocks = false;  // cpuJitAccessPcsStartBlocks when fetched: a marked pc only ever starts the block
    uint32_t pid = 0;  // ARM920T: the FCSE PID the words were fetched under
    // ARM920T, fetched under a PID below 32 MB: each VA page's physical page and access then, for a check
    // at install when another PID runs and the slot isn't mapped (Env::CodeUnchanged).
    struct PageMap {
        uint32_t page, pa, access;
    };
    std::vector<PageMap> maps;
    const PageMap *MapOf(uint32_t va) const {
        for (const auto &m : maps)
            if (m.page == va >> 12) return &m;
        return nullptr;
    }
    TickModel model;

    std::uint64_t GetTicksForCode(bool thumb, A32::VAddr, std::uint32_t instruction) override {
        return model.Ticks(thumb, instruction);
    }
    std::uint64_t GetTicksForConditionFailed() override { return model.Failed(); }

    std::optional<std::uint32_t> MemoryReadCode(A32::VAddr va) override {
        const uint32_t i = (va - base) >> 2;
        if (va < base || i >= words.size()) {
            overflow = true;
            return std::nullopt;
        }
        read[i] = 1;
        if (!fetched[i]) return std::nullopt;
        return words[i];
    }

    bool PreCodeReadHook(bool thumb, A32::VAddr pc, A32::IREmitter &ir) override {
        if (pc == A32::LocationDescriptor{ir.block.Location()}.PC()) stats.translated++;
        const bool isMarked = pc == splitAt || std::find(marked.begin(), marked.end(), pc) != marked.end();
        if (!AccessPcSplit(startBlocks, isMarked && startBlocks, pc, ir)) return false;
        uint32_t word = 0;
        const bool ok = CodeReadHook(thumb, arm920, pc, ir, [&](uint32_t at, uint32_t &w) {
            while (next < plain.size() && plain[next].first != at) next++;
            if (next == plain.size()) {
                overflow = true;
                return false;
            }
            const uint32_t i = (at - base) >> 2;
            w = words[i];
            word = w;
            return plain[next].second;
        });
        if (ok) NoteAccessPc(thumb, word, isMarked, isMarked && startBlocks, pc, ir);
        return ok;
    }
};

struct Env final : public A32::UserCallbacks {
    ArmCpu *cpu = nullptr;
    bool arm920 = false;
    // ARM920T: lineBound for the probe's single-step diagnostic mode, blocks ended at each 32-byte line.
    // Otherwise (Fast blocks) code pages are watched by physical page and 64-byte line through the lean
    // path's stores (jitCodeWrittenPa), so the guest's I-cache invalidations change nothing.
    bool lineBound = false;
    std::unordered_map<uint32_t, std::vector<uint32_t>> byPa;  // Fast blocks: virtual code pages by physical page
    uint64_t blocks = 0, instructions = 0;
    A32::Jit *jit = nullptr;
    PageTable *pages = nullptr;
    // Filled page table entries by their page's domain (MMU_DOMAIN_NONE with the MMU off,
    // MMU_DOMAIN_UNKNOWN when the TLB didn't hold the page), each with what its TLB entry
    // allows while the domain is a client (cpuJitPageClientAccess).
    struct Filled {
        uint32_t page;
        uint8_t client;
    };
    std::array<std::vector<Filled>, MMU_DOMAIN_UNKNOWN + 1> filled;
    // The privilege level this JIT runs at, and the other level's Env (null until that one
    // first runs). Everything here is only used while the CPU is at this level, apart from
    // what both get: invalidations, clears, write protection and code writes.
    bool privileged = true;
    Env *other = nullptr;
    // ARM920T Fast blocks under the FCSE: blocks run at VAs, and below 32 MB the code there depends on the
    // PID, so each run's PID goes into the blocks' location as a tag (jitRun). Code pages (code, byPa) are
    // kept by MVA. pidTags is the FrondJit's tag by PID >> 25, 0 for none yet; tag the one this JIT has now.
    const uint8_t *pidTags = nullptr;
    uint8_t tag = 0;
    // ARM920T Fast blocks: the FrondJit's code page map for the MMU's fill watch (cpuJitSetFillWatch), with this
    // level's bit in it set for each page in code, and the watched fill count this level has taken (TakeFills).
    uint8_t *fillWatch = nullptr;
    uint8_t watchBit = 0;
    uint32_t seenFills = 0;

    // Page table entries with bit 0 set are read-only (the fork's page_table_write_protect_bit):
    // stores to them come to the callbacks. A RAM page is filled read-only and made writable
    // by its first store through Write, which marks it dirty (and the frame buffer, where it
    // overlaps) as if all of it was written. Whoever clears that dirty state write-protects the
    // page again (jitWriteProtect), and so does a code flag on any of its lines, so the
    // flag's check runs on stores. Writable entries by host page:
    std::unordered_map<uint8_t *, std::vector<uint32_t>> writable;

    static uint8_t *ReadOnly(uint8_t *host) { return (uint8_t *)(uintptr_t(host) | 1); }

#ifdef __aarch64__
    // arm64 generated code reads these instead (the fork's frond_read_page_table and frond_write_page_table):
    // the host page less the guest page for loads, and for stores where the entry is writable, null otherwise.
    PageTable *readPages = nullptr, *writePages = nullptr;
#endif
    // Every page table entry is set here, which keeps the arm64 tables in step.
    void SetPage(uint32_t page, uint8_t *entry) {
        (*pages)[page] = entry;
#ifdef __aarch64__
        const uintptr_t host = uintptr_t(entry) & ~uintptr_t(1), offset = uintptr_t(page) << 12;
        (*readPages)[page] = entry ? (uint8_t *)(host - offset) : nullptr;
        (*writePages)[page] = entry && !(uintptr_t(entry) & 1) ? (uint8_t *)(host - offset) : nullptr;
#endif
    }

    void ProtectHost(uint8_t *host) {
        const auto it = writable.find(host);
        if (it == writable.end()) return;
        for (uint32_t page : it->second)
            if ((*pages)[page] == host) SetPage(page, ReadOnly(host));
        stats.writeProtects++;
        writable.erase(it);
    }

    void Protect(const uint8_t *host, uint32_t size) {
        for (auto it = writable.begin(); it != writable.end();) {
            if (it->first + 0x1000 > host && it->first < host + size) {
                for (uint32_t page : it->second)
                    if ((*pages)[page] == it->first) SetPage(page, ReadOnly(it->first));
                stats.writeProtects++;
                it = writable.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Each virtual page dynarmic has read code from, with the physical page it came from
    // and its host page. Each 64-byte RAM line code is read from is flagged, and the first
    // write to one comes back through CodeWritten and drops the code of every virtual page
    // on that host page. Lines keep code and data apart on pages that hold both, like the
    // vector page. ROM only changes from the host side, which
    // clears everything. Pages that are neither (host null) can't be watched, so the
    // guest's I-cache invalidations drop them instead.
    // Dynarmic keeps blocks by PC and CPSR state, not permissions, so each code page also
    // has its domain and AP (access). When the domains, S or R change, the code of every
    // page that can't be fetched now is dropped, and fetching it again takes the prefetch
    // abort. Each Env only runs at its own privilege, so that isn't part of the context.
    struct TableLine;
    struct CodePage {
        uint32_t pa;  // physical page, or ~0u when the page tables had no mapping
        uint8_t *host;
        uint8_t *lines;  // RAM code flags for the page, null for ROM and unwatched pages
        uint32_t access;
        bool suspended = false;  // in suspended[] until its PID runs again
        bool stale = false;  // suspended, and its blocks from before were read from another physical page
        bool noted = false;  // suspended, and code was noted from it since: blocks at its MVA may be live
        MmuCodeWalkMemo walk;  // the last mapping check's table words (CodePa)
        uint32_t watched = 0;  // walk.walks when its table words were last watched
        uint32_t stamp = 0;  // the watched mapping check that last took it
        bool rechecking = false;  // in recheck
        std::array<TableLine *, 10> walkLines;  // the line of each of walk's words, once watched
    };
    std::unordered_map<uint32_t, uint32_t> accessPages;  // code pages by access
    uint64_t permContext = ~0ull;
    std::unordered_map<uint32_t, CodePage> code;
    std::unordered_map<uint8_t *, std::vector<uint32_t>> byHost;
    std::vector<uint32_t> unwatched;
    uint32_t mapGen = 0;
    // ARM920T Fast blocks: code pages (MVAs) of another PID's slot whose mapping a TLB flush found moved, by
    // slot. WinCE unmaps a process's slot while others run and maps it back the same before it runs again.
    // The pages' blocks below 32 MB run under that PID's tag only, so they can't be reached until the PID
    // is back; jitRun then keeps the pages mapped as before and drops the rest (Resume). A page may be
    // listed more than once, or after it was dropped; its suspended flag says whether it still counts.
    std::array<std::vector<uint32_t>, 128> suspended;
    // The PID's slot still has suspended pages that aren't mapped yet: their VAs below 32 MB are guarded
    // (the JIT's frond_page_guard), so their blocks leave at entry and the interpreter fetches the code
    // (and takes the prefetch abort WinCE maps the slot back from). Guarded pages by VA page, and the
    // pages set. absentRuns counts runs with pages left by slot, up to kAbsentRuns.
    std::array<uint8_t, 0x2000> guard{};
    std::vector<uint32_t> guarded;
    std::array<uint16_t, 128> absentRuns{};
    // CheckMappings' and Resume's lists, kept so a run doesn't allocate them.
    std::vector<uint32_t> scratchMoved, scratchList, scratchLeft, scratchFilled, scratchFillMoved;
    // ARM920T Fast blocks: a copy of each 64-byte line of page table words the code pages' walk memos read,
    // with the pages that read it, so a mapping check compares those lines and walks only the pages of lines
    // that changed, and the pages in recheck: new ones, suspended ones with code newly noted, and those whose
    // walk couldn't be remembered or was answered by a live TLB entry. A TTB change checks everything.
    // A line's pages may include pages that left or no longer read it; they go when it changes, and lines
    // with no pages left go every kSweepChecks checks.
    // A page walked outside a check (Resume, NoteSuspended) against words other than a line's copy marks the
    // line dirty, so the next check takes its pages even if the words are back as copied by then.
    struct TableLine {
        const uint8_t *host;
        uint32_t pa;  // pa >> 6
        bool dirty = false;
        uint16_t words = 0;  // the words walks read, a bit per word; only these count as a change
        uint8_t copy[64];
        std::vector<uint32_t> pages;
        // Whether a word walks read differs from the copy.
        bool Moved() const {
            for (uint32_t m = words; m; m &= m - 1) {
                const unsigned at = unsigned(__builtin_ctz(m)) * 4;
                if (std::memcmp(host + at, copy + at, 4)) return true;
            }
            return false;
        }
    };
    static constexpr uint32_t kSweepChecks = 4096;
    bool watchTables = false, checking = false, tablesWritten = false;
    uint32_t checkedTtb = ~0u, checkStamp = 0;
    std::vector<std::unique_ptr<TableLine>> tableLines;
    std::unordered_map<uint32_t, TableLine *> tableLineAt;  // by pa >> 6
    std::vector<uint32_t> recheck, scratchRecheck, scratchCheck;

    // The page dynarmic keeps code by (the run's VA) for a code page here (an MVA), and the code page of a VA.
    // An MVA page past 32 MB whose PID has a tag may also have blocks at its VA below 32 MB under that tag.
    template <typename F>
    void PcPages(uint32_t page, F f) const {
        f(page);
        if (arm920 && page >= 0x2000 && pidTags && pidTags[page >> 13]) f(page & 0x1fff);
    }
    uint32_t CodePageOf(uint32_t va) const { return (arm920 ? cpuJitCodeMva(cpu, va) : va) >> 12; }

    uint32_t CodePa(uint32_t page, uint32_t &access) {
        uint32_t pa;
        return cpuJitCodePa(cpu, page << 12, &pa, &access) ? pa >> 12 : ~0u;
    }
    // The same for a noted page, through its walk memo: most flushes leave the tables a page uses alone.
    // With the table lines watched, a new walk's lines are copied, and a page the memo can't vouch for until
    // the next check goes to recheck.
    uint32_t CodePa(uint32_t page, CodePage &c, uint32_t &access) {
        uint32_t pa;
        const uint32_t r = cpuJitCodePaMemo(cpu, page << 12, &pa, &access, &c.walk) ? pa >> 12 : ~0u;
        if (watchTables) {
            if (c.walk.walks != c.watched) WatchWalk(page, c);
            if (c.walk.tlb || !c.walk.n) {
                Recheck(page, c);
            } else if (!checking) {
                for (uint8_t i = 0; i < c.walk.n; i++) {
                    uint32_t v;
                    std::memcpy(&v, c.walkLines[i]->copy + (c.walk.pa[i] & 60), 4);
                    if (v != c.walk.value[i]) c.walkLines[i]->dirty = true;
                }
            }
        }
        return r;
    }

    void Recheck(uint32_t page, CodePage &c) {
        if (c.rechecking) return;
        c.rechecking = true;
        recheck.push_back(page);
    }

    // The memo's words are in RAM or ROM host pages (cpuPrvTableWord), so a word's whole 64-byte line is too.
    void WatchWalk(uint32_t page, CodePage &c) {
        c.watched = c.walk.walks;
        for (uint8_t i = 0; i < c.walk.n; i++) {
            const uint32_t key = c.walk.pa[i] >> 6;
            auto [at, added] = tableLineAt.try_emplace(key, nullptr);
            if (added) {
                TableLine &l = *tableLines.emplace_back(std::make_unique<TableLine>());
                l.host = c.walk.word[i] - (c.walk.pa[i] & 63);
                l.pa = key;
                std::memcpy(l.copy, l.host, 64);
                at->second = &l;
            }
            TableLine &l = *at->second;
            c.walkLines[i] = &l;
            const unsigned word = (c.walk.pa[i] & 63) >> 2;
            if (!(l.words & (1u << word))) {
                // No page read this word before, so its copy only has to hold what this walk read.
                l.words |= uint16_t(1u << word);
                std::memcpy(l.copy + word * 4, &c.walk.value[i], 4);
                cpuJitWatchTableWord(cpu, (l.pa << 6) | (word << 2));
            }
            if (std::find(l.pages.begin(), l.pages.end(), page) == l.pages.end()) l.pages.push_back(page);
        }
    }

    // A line with pages left may be in their walkLines (WatchWalk), so only empty ones go.
    void SweepTableLines() {
        std::erase_if(tableLines, [&](std::unique_ptr<TableLine> &l) {
            std::erase_if(l->pages, [&](uint32_t page) {
                const auto c = code.find(page);
                return c == code.end() || !WalkUses(c->second, l->pa);
            });
            if (!l->pages.empty()) return false;
            tableLineAt.erase(l->pa);
            return true;
        });
        stats.tableLines = std::max<uint64_t>(stats.tableLines, tableLines.size());
    }

    // Whether the page's walk memo read a word in line (pa >> 6).
    static bool WalkUses(const CodePage &c, uint32_t line) {
        for (uint8_t i = 0; i < c.walk.n; i++)
            if (c.walk.pa[i] >> 6 == line) return true;
        return false;
    }

    void NoteCode(uint32_t va) { NoteCodeAt(CodePageOf(va), va & 0xfffu); }

    // va: an address in the code page (an MVA page on the ARM920T) at the code's offset.
    void NoteCodeAt(uint32_t page, uint32_t va) {
        auto it = code.find(page);
        if (it == code.end()) it = NotePage(page);
        else if (it->second.suspended) it = NoteSuspended(it, page);
        // A suspended page with code noted is checked again at flushes, the next one included.
        if (it->second.suspended && !it->second.noted) {
            it->second.noted = true;
            if (watchTables) Recheck(page, it->second);
        }
        // Fast blocks: the CPU keeps the 64-byte lines by physical page; a store to one drops the page's code.
        if (arm920 && !lineBound && it->second.host) cpuJitNoteArm920Code(cpu, (it->second.pa << 12) | (va & 0xfffu));
        uint8_t *lines = it->second.lines;
        if (lines && !lines[(va & 0xfffu) >> 6]) {
            // The flags are the CPU's, so both privilege levels' direct stores to the page
            // have to come back through the callbacks.
            lines[(va & 0xfffu) >> 6] = 1;
            ProtectHost(it->second.host);
            if (other) other->ProtectHost(it->second.host);
        }
    }

    std::unordered_map<uint32_t, CodePage>::iterator NotePage(uint32_t page) {
        uint32_t access;
        const uint32_t pa = CodePa(page, access);
        return NotePage(page, pa, access);
    }

    std::unordered_map<uint32_t, CodePage>::iterator NotePage(uint32_t page, uint32_t pa, uint32_t access) {
        uint8_t *lines = nullptr;
        uint8_t *host = pa == ~0u ? nullptr : cpuJitWatchCode(cpu, pa << 12, &lines);
        const auto it = code.emplace(page, CodePage{pa, host, lines, access}).first;
        if (fillWatch) fillWatch[page] |= watchBit;
        accessPages[access]++;
        stats.codePages++;
        if (watchTables) Recheck(page, it->second);
        if (arm920 && !lineBound && host) {
            // A newly watched RAM page may have dropped the lean TLB; empty the page table now, as code
            // can run straight on after this compile. The probe's single-step mode (lineBound) has no lean
            // path, so its pages are watched by host page like the other cores'.
            byPa[pa].push_back(page);
            CheckGen();
        } else if (host) {
            byHost[host].push_back(page);
        } else {
            stats.unwatched++;
            unwatched.push_back(page);
        }
        return it;
    }

    // New code from a suspended page (from 32 MB up, or another PID's below): if the page is mapped to
    // another physical page now, it's watched there, and the blocks from before (which can't run until
    // Resume) are marked stale, so Resume drops them. The page stays in suspended[].
    std::unordered_map<uint32_t, CodePage>::iterator NoteSuspended(std::unordered_map<uint32_t, CodePage>::iterator it,
                                                                   uint32_t page) {
        uint32_t access;
        const uint32_t pa = CodePa(page, it->second, access);
        if (pa == ~0u || (pa == it->second.pa && access == it->second.access)) return it;
        Forget(page);
        it = NotePage(page);
        it->second.suspended = it->second.stale = true;
        return it;
    }

    void Drop(uint32_t page) {
        PcPages(page, [&](uint32_t p) { jit->InvalidateCacheRange(p << 12, 4096); });
        Forget(page);
    }

    // Forgets a code page that holds no code now, so the next compile from it watches it again.
    void Forget(uint32_t page) {
        const auto it = code.find(page);
        if (it == code.end()) return;
        const auto n = accessPages.find(it->second.access);
        if (!--n->second) accessPages.erase(n);
        if (fillWatch) fillWatch[page] &= ~watchBit;
        code.erase(it);
    }

    // Drops the code of pages that can't be fetched with the current domains, S and R. Only a
    // change of those can make a page unfetchable without a TLB flush.
    void CheckFetch() {
        const uint64_t context = cpuJitPermContext(cpu);
        if (context == permContext) return;
        permContext = context;
        std::vector<uint32_t> lost;
        for (const auto &[access, n] : accessPages)
            if (!cpuJitAccessReadable(cpu, access)) lost.push_back(access);
        if (lost.empty()) return;
        std::vector<uint32_t> drop;
        for (const auto &[page, c] : code)
            if (std::find(lost.begin(), lost.end(), c.access) != lost.end()) drop.push_back(page);
        for (uint32_t page : drop) {
            stats.fetchDropped++;
            Drop(page);
        }
    }

    // A store to [offset, offset + size) of a watched code page (by physical page for Fast blocks on the
    // ARM920T, by host page otherwise): drops only the blocks whose code it overlaps, in each virtual page
    // mapped there. True if it dropped any. The page's line flags are worked out again from the code left
    // (CodeLines) before the next run.
    bool RangeWritten(const std::vector<uint32_t> &pages, uint32_t offset, uint32_t size, bool byPhysical,
                      uint32_t key, uint8_t *host) {
        bool dropped = false;
        for (uint32_t page : pages) {
            const auto c = code.find(page);
            if (c == code.end() || (byPhysical ? c->second.pa != key : c->second.host != host)) continue;
            PcPages(page, [&](uint32_t p) {
                const uint32_t va = (p << 12) | offset;
                if (!jit->HasCodeIn(va, size)) return;
                stats.pagesWritten++;
                if (stats.on) stats.writtenPage[p]++, stats.writtenAt[va]++;
                jit->InvalidateCacheRange(va, size);
                dropped = true;
            });
        }
        if (!dropped) stats.writesMissed++;
        return dropped;
    }

    // known: this level holds code from the page. True if the store dropped any.
    bool CodeWrittenPa(uint32_t pa, uint32_t offset, uint32_t size, const void *data) {
        if (watchTables) TableWordsWritten(pa, offset, size, data);
        const auto it = byPa.find(pa);
        return it != byPa.end() && RangeWritten(it->second, offset, size, true, pa, nullptr);
    }

    bool CodeWritten(uint8_t *host, uint32_t offset, uint32_t size, bool &known) {
        const auto it = byHost.find(host);
        if (it == byHost.end()) return false;
        known = true;
        return RangeWritten(it->second, offset, size, false, 0, host);
    }

    // ORs into words (16) the 4-byte words of the page that this level's live blocks hold code from, with
    // any requested invalidation done first. Not while running.
    void CodeWords(const std::vector<uint32_t> &pages, bool byPhysical, uint32_t key, uint8_t *host, uint64_t *words) {
        for (uint32_t page : pages) {
            const auto c = code.find(page);
            if (c == code.end() || (byPhysical ? c->second.pa != key : c->second.host != host)) continue;
            PcPages(page, [&](uint32_t p) { jit->CodeWords(p << 12, words); });
        }
    }
    void CodeWordsPa(uint32_t pa, uint64_t *words) {
        if (const auto it = byPa.find(pa); it != byPa.end()) CodeWords(it->second, true, pa, nullptr, words);
        // The table words walks read stay watched too.
        for (const auto &l : tableLines)
            if (l->pa >> 6 == pa) words[(l->pa & 63) >> 2] |= uint64_t(l->words) << ((l->pa & 3) * 16);
    }

    // A store to [offset, offset + size) of physical page pa hit a word marked in the CPU's code map: the table
    // lines there with words walks read are dirty, for the next mapping check. data is what an aligned word
    // store puts there, with the old word still in place (null if not known). Such a store that keeps the
    // word, or puts an entry where a fault one was (WinCE maps a slot back that way), moves no code page's
    // mapping: a page whose walk reads a fault entry there was dropped or suspended by the check after the
    // store that put it there, and Resume and NoteSuspended walk it again. A fill through the new entry
    // (TakeFills) is for such a page too, or for code compiled from it since, which has that mapping.
    void TableWordsWritten(uint32_t pa, uint32_t offset, uint32_t size, const void *data) {
        for (uint32_t line = offset >> 6; line <= (offset + size - 1) >> 6; line++) {
            const auto it = tableLineAt.find((pa << 6) | line);
            if (it == tableLineAt.end() || !it->second->words) continue;
            TableLine &l = *it->second;
            if (data && size == 4 && !(offset & 3) && !l.dirty) {
                if (!(l.words & (1u << ((offset & 63) >> 2)))) continue;
                uint32_t old, now;
                std::memcpy(&old, l.host + (offset & 63), 4);
                std::memcpy(&now, data, 4);
                if (old == now) continue;
                if (!(old & 3)) {
                    stats.tableFaultFills++;
                    continue;
                }
            }
            l.dirty = true;
            tablesWritten = true;
        }
    }
    void CodeWordsHost(uint8_t *host, uint64_t *words) {
        if (const auto it = byHost.find(host); it != byHost.end()) CodeWords(it->second, false, 0, host, words);
    }

    // The page holds no code at either level any more: forget it, so a compile from it watches it again.
    void ForgetPa(uint32_t pa) {
        const auto it = byPa.find(pa);
        if (it == byPa.end()) return;
        for (uint32_t page : it->second)
            if (const auto c = code.find(page); c != code.end() && c->second.pa == pa) Forget(page);
        byPa.erase(it);
    }
    void ForgetHost(uint8_t *host) {
        const auto it = byHost.find(host);
        if (it == byHost.end()) return;
        for (uint32_t page : it->second)
            if (const auto c = code.find(page); c != code.end() && c->second.host == host) Forget(page);
        byHost.erase(it);
    }

    // One page's mapping check: a page whose mapping moved is listed in moved, to be dropped. ARM920T Fast
    // blocks: a page of another PID's slot (with a tag) is suspended instead, dropping only blocks run at its
    // MVA, which any PID can reach. Suspended pages wait for Resume; one with code noted since (compiled at its
    // MVA, by another PID or the owner on a guarded page) drops the blocks at its MVA again when it moves.
    void CheckPage(uint32_t page, CodePage &c, std::vector<uint32_t> &moved) {
        stats.mapPagesChecked++;
        uint32_t access;
        if (CodePa(page, c, access) == c.pa && access == c.access) return;
        if (c.suspended) {
            c.noted = false;
            if (jit->HasCodeIn(page << 12, 4096)) {
                stats.suspendedMvaDrops++;
                jit->InvalidateCacheRange(page << 12, 4096);
            }
            return;
        }
        const uint32_t slot = page >> 13;
        if (arm920 && !lineBound && pidTags && slot && slot != cpuGetPid(cpu) >> 25 && pidTags[slot]) {
            stats.suspends++;
            c.suspended = true;
            c.noted = false;
            suspended[slot].push_back(page);
            if (jit->HasCodeIn(page << 12, 4096)) jit->InvalidateCacheRange(page << 12, 4096);
            return;
        }
        moved.push_back(page);
    }

    // After a TLB flush, or a change to the table words code pages' walks read (TablesMoved), drop the code
    // of every page whose mapping moved (CheckPage).
    void CheckMappings() {
        stats.mapChecks++;
        std::vector<uint32_t> &moved = scratchMoved;
        moved.clear();
        const auto check = [&](uint32_t page, CodePage &c) { CheckPage(page, c, moved); };
        // A watched check brings the copies of changed lines up to date before walking any page; a full one
        // leaves them, so its walks mark lines that differ dirty.
        checking = true;
        const bool watched = CheckWatched(check);
        checking = false;
        if (!watched) {
            stats.fullMapChecks++;
            for (uint32_t page : recheck)
                if (const auto c = code.find(page); c != code.end()) c->second.rechecking = false;
            recheck.clear();
            for (auto &[page, c] : code) {
                if (c.suspended && !c.noted) continue;
                check(page, c);
            }
        }
        for (uint32_t page : moved) {
            stats.mapChanged++;
            Drop(page);
        }
    }

    // ARM920T Fast blocks: whether a store hit a table word a code page's walk read since the last mapping
    // check (TableWordsWritten). A guest may edit a table word with no flush when the TLB holds no entry for
    // it, and fetches then walk the new word; jitRun checks the mappings before the next run when this says so.
    bool TablesMoved() const { return tablesWritten; }

    // TLB entries the MMU made live for code pages (cpuJitWatchedFills) since this level last looked: a fetch
    // goes by those first, so each of its pages is checked again now. In a check (inCheck) the pages go to
    // recheck; otherwise they are checked here, before the next block runs (moved pages are dropped, so a
    // run in progress stops after its current block). More fills than the MMU kept a list of take every code
    // page a live entry answers.
    void TakeFills(bool inCheck) {
        const uint32_t *ring;
        const uint32_t n = cpuJitWatchedFills(cpu, &ring), count = n - seenFills;
        if (!count) return;
        seenFills = n;
        // With no store to a walked table word since the last mapping check, a fill walked the words that
        // check saw, so it holds the mapping each code page has.
        if (!tablesWritten) return;
        std::vector<uint32_t> &pages = scratchFilled;
        pages.clear();
        const auto take = [&](uint32_t page, CodePage &c) {
            if (c.suspended && !c.noted) return;  // Resume walks it before its code can run
            // An entry a flush took again since answers nothing, and one with the mapping the code came from
            // changes nothing.
            if (!cpuJitTlbLive(cpu, page << 12)) return;
            uint32_t pa, access;
            if (cpuJitCodePa(cpu, page << 12, &pa, &access) && pa >> 12 == c.pa && access == c.access) {
                stats.fillSame++;
                return;
            }
            stats.fillRechecks++;
            if (inCheck) Recheck(page, c);
            else pages.push_back(page);
        };
        if (count > 8) {
            stats.tlbLiveScans++;
            for (auto &[page, c] : code)
                if (!c.rechecking && cpuJitTlbLive(cpu, page << 12)) take(page, c);
        } else {
            for (uint32_t i = n - count; i != n; i++) {
                const uint32_t first = ring[i & 7] & 0xfffff, last = first + (ring[i & 7] >> 20);
                for (uint32_t page = first; page <= last; page++)
                    if (fillWatch[page] & watchBit)
                        if (const auto c = code.find(page); c != code.end() && !c->second.rechecking)
                            take(page, c->second);
            }
        }
        if (inCheck || pages.empty()) return;
        std::vector<uint32_t> &moved = scratchFillMoved;
        moved.clear();
        for (uint32_t page : pages)
            if (const auto c = code.find(page); c != code.end()) CheckPage(page, c->second, moved);
        for (uint32_t page : moved) {
            stats.mapChanged++;
            stats.fillDrops++;
            Drop(page);
        }
    }

    // The watched mapping check: only the pages of table lines that changed, and recheck. False when
    // everything has to be checked instead (MMU off, the first check or a TTB change).
    template <typename F>
    bool CheckWatched(F &check) {
        watchTables = arm920 && !lineBound;
        if (!watchTables) return false;
        const uint32_t ttb = cpuJitTtb(cpu);
        if (ttb != checkedTtb || ttb == MMU_DISABLED_TTP) {
            checkedTtb = ttb;
            const uint32_t *ring;
            seenFills = cpuJitWatchedFills(cpu, &ring);  // the full check walks every page
            tablesWritten = false;
            return false;
        }
        // The TLB answers a code fetch before the tables, and its entries can differ from them (a table word
        // edited and put back with no flush in between, or entries kept over a TTB write). Pages filled since
        // the last look go to recheck, which keeps them while the TLB answers.
        TakeFills(true);
        tablesWritten = false;
        if (++checkStamp == 0) {
            for (auto &[page, c] : code) c.stamp = 0;
            checkStamp = 1;
        }
        if (checkStamp % kSweepChecks == 0) SweepTableLines();
        std::vector<uint32_t> &pages = scratchCheck;
        pages.clear();
        for (const auto &line : tableLines) {
            TableLine &l = *line;
            if (!l.dirty && !l.Moved()) continue;
            std::memcpy(l.copy, l.host, 64);
            l.dirty = false;
            stats.tableLinesChanged++;
            std::erase_if(l.pages, [&](uint32_t page) {
                const auto c = code.find(page);
                if (c == code.end() || !WalkUses(c->second, l.pa)) return true;
                if (c->second.stamp != checkStamp) {
                    c->second.stamp = checkStamp;
                    pages.push_back(page);
                }
                return false;
            });
        }
        std::vector<uint32_t> &again = scratchRecheck;
        again.clear();
        again.swap(recheck);
        for (uint32_t page : again) {
            const auto c = code.find(page);
            if (c == code.end()) continue;
            c->second.rechecking = false;
            if (c->second.stamp == checkStamp) continue;
            c->second.stamp = checkStamp;
            pages.push_back(page);
        }
        for (uint32_t page : pages) {
            const auto c = code.find(page);
            if (c == code.end()) continue;
            // Resume walks a suspended page again before its code can run (a walk off its line's copy marks
            // the line dirty), and one with code noted is in recheck.
            if (c->second.suspended && !c->second.noted) continue;
            check(page, c->second);
        }
        return true;
    }

    // Before running under the PID of slot: its suspended pages mapped as when they were noted keep their
    // code, pages mapped elsewhere are dropped, and pages not mapped stay suspended (the run goes under the
    // guard) for up to kAbsentRuns runs, then are dropped too. True if any stay.
    static constexpr uint16_t kAbsentRuns = 4096;
    bool Resume(uint32_t slot) {
        std::vector<uint32_t> &list = scratchList, &left = scratchLeft;
        list.clear();
        left.clear();
        list.swap(suspended[slot]);
        const bool last = ++absentRuns[slot] > kAbsentRuns;
        for (uint32_t page : list) {
            const auto c = code.find(page);
            if (c == code.end() || !c->second.suspended) continue;
            uint32_t access;
            const uint32_t pa = CodePa(page, c->second, access);
            if (!c->second.stale && pa == c->second.pa && access == c->second.access) {
                stats.resumes++;
                c->second.suspended = false;
                continue;
            }
            if (pa == ~0u && !last && !c->second.stale) {
                left.push_back(page);
                continue;
            }
            c->second.suspended = false;
            stats.mapChanged++;
            Drop(page);
        }
        if (left.empty()) {
            absentRuns[slot] = 0;
            return false;
        }
        suspended[slot].swap(left);
        return true;
    }

    // Guards the VAs below 32 MB of slot's suspended pages (none for slot 0), for a run under its PID.
    void Guard(uint32_t slot) {
        for (uint32_t p : guarded) guard[p] = 0;
        guarded.clear();
        if (!slot) return;
        for (uint32_t page : suspended[slot]) {
            const uint32_t p = page & 0x1fff;
            if (!guard[p]) {
                guard[p] = 1;
                guarded.push_back(p);
            }
        }
    }

    void DropUnwatched(uint32_t first, uint32_t last) {
        std::erase_if(unwatched, [&](uint32_t page) {
            if (page < first || page > last) return false;
            const auto c = code.find(page);
            if (c != code.end() && !c->second.host) {
                stats.unwatchedDropped++;
                Drop(page);
            }
            return true;
        });
    }

    // Host side clears (reset, session load, patching) also reset dynarmic's own state at
    // the next run (return stack buffer, halt flags, exclusive monitor, FP control), so a
    // load starts from the same state as a fresh process.
    void Clear() {
        stats.clears++;
        ClearCode();
        resetState = true;
    }

    // Drops all translated code but keeps dynarmic's state, for a CPAR change made by the
    // guest between runs.
    void ClearCode() {
        ClearPages();
        jit->ClearCache();
        std::fill(heat.begin(), heat.end(), Heat{});
        if (fillWatch)
            for (const auto &[page, c] : code) fillWatch[page] &= ~watchBit;
        code.clear();
        accessPages.clear();
        permContext = cpuJitPermContext(cpu);
        byHost.clear();
        byPa.clear();
        unwatched.clear();
        for (auto &list : suspended) list.clear();
        absentRuns.fill(0);
        Guard(0);
        tableLines.clear();
        tableLineAt.clear();
        recheck.clear();
        checkedTtb = ~0u;
        cpuJitForgetCode(cpu);
        gen = ~0ull;
        mapGen = cpuJitMapGen(cpu);
    }

    bool resetState = false;

    uint64_t gen = ~0ull;
    uint64_t budget = 0, ran = 0;

    uint32_t domains = 0;  // DACR at the last CheckGen

    void ClearPages() {
        stats.pageClears++;
        // The data cache hint is keyed by va alone and assumes the policy each page had when its
        // entry was noted, so it goes with the mappings.
        cpuCostHintClear(cpu);
        for (auto &list : filled) {
            for (const Filled &f : list) SetPage(f.page, nullptr);
            list.clear();
        }
        writable.clear();
    }

    void NoteFilled(uint32_t page) {
        const int access = cpuJitPageClientAccess(cpu, page << 12, privileged);
        if (access < 0)
            filled[MMU_DOMAIN_UNKNOWN].push_back({page, 0});
        else
            filled[access >> 2].push_back({page, uint8_t(access & 3)});
    }

    // Only the DACR changed since the pages were filled, so each page has the mapping and
    // TLB entry it was filled with. Pages in domains that lost rights (and pages whose
    // domain wasn't known) are checked again: a writable entry the MMU now allows reads
    // only on goes read-only, and an entry it allows no reads on is dropped. The rest grant
    // no more than the MMU does now, so the work goes with the pages in those domains.
    void CheckDomains(uint32_t from, uint32_t to) {
        stats.domainChecks++;
        for (unsigned d = 0; d < 16; d++) {
            const uint32_t before = (from >> (2 * d)) & 3, now = (to >> (2 * d)) & 3;
            if (mmuDomainRank(now) < mmuDomainRank(before)) CheckDomain(filled[d], now);
        }
        CheckDomain(filled[MMU_DOMAIN_UNKNOWN], 0);
    }

    // The pages of one domain whose two DACR bits are now bits.
    void CheckDomain(std::vector<Filled> &list, uint32_t bits) {
        size_t kept = 0;
        for (const Filled f : list) {
            const unsigned access = bits == 3 ? 3 : bits == 1 ? f.client : 0;
            uint8_t *const entry = (*pages)[f.page];
            const bool writableEntry = entry && !(uintptr_t(entry) & 1);
            if (!(access & 1) || (writableEntry && !(access & 2))) {
                if (writableEntry) {
                    const auto it = writable.find(entry);
                    if (it != writable.end()) {
                        std::erase(it->second, f.page);
                        if (it->second.empty()) writable.erase(it);
                    }
                }
                if (access & 1) {
                    SetPage(f.page, ReadOnly(entry));
                    stats.pageReadOnly++;
                } else {
                    SetPage(f.page, nullptr);
                    stats.pageDrops++;
                    continue;
                }
            }
            list[kept++] = f;
        }
        list.resize(kept);
    }

    void CheckGen() {
        const uint64_t g = cpuJitPageGen(cpu);
        if (g != gen) GenChanged(g);
    }

    // After a callback access through the MMU, which may have filled a code page's TLB entry.
    void AfterFill() {
        if (fillWatch) TakeFills(false);
    }

    [[gnu::noinline]] void GenChanged(uint64_t g) {
        const uint32_t d = cpuJitGetDomain(cpu);
        if (gen == ~0ull || (g >> 32) != (gen >> 32))
            ClearPages();
        else if (d != domains)
            CheckDomains(domains, d);
        gen = g;
        domains = d;
        CheckFetch();
    }

    void Fault() { jit->HaltExecution(kFault); }

    // A failed access: a guest fault (fsr set) for the data abort, or a refused one for
    // the interpreter. The run leaves at the check right after the access, so it's the
    // instruction's last; the first fault is kept all the same.
    [[gnu::cold, gnu::noinline]] void AccessFailed(uint32_t va, unsigned size, bool write, uint_fast8_t fsr) {
        if (fsr && !dataAbort.set) {
            stats.aborts++;
            dataAbort = {true, {va, uint8_t(fsr), uint8_t(size), write}};
        }
        if (stats.on) stats.faultAccess[va | uint64_t(size) << 32 | uint64_t(write) << 40]++;
        Fault();
    }

    // Interpret first: a block is compiled once its start has been reached `hot` times.
    // Before that the run ends there and the interpreter runs the code up to its next taken
    // branch, so code that runs once (an application starting) is never compiled. Counts are
    // kept by PC and Thumb state in a small direct-mapped table; another PC landing on the
    // same entry starts it again. Emptied with the cache.
    struct Heat {
        uint32_t key = 0, count = 0;
    };
    static constexpr unsigned kHeatBits = 14;
    std::vector<Heat> heat = std::vector<Heat>(size_t(1) << kHeatBits);
    uint32_t hot = 1;

    bool ShouldCompile(A32::VAddr pc, bool thumb) override {
        if (pc < 0x02000000u && guard[pc >> 12]) {
            stats.cold++;
            jit->HaltExecution(kCold);
            return false;
        }
        if (hot <= 1) return true;
        const uint32_t key = pc | uint32_t(thumb);  // PC is even
        Heat &h = heat[(key * 0x9e3779b1u) >> (32 - kHeatBits)];
        if (h.key != key) h = Heat{key, 0};
        if (++h.count >= hot) return true;
        stats.cold++;
        jit->HaltExecution(kCold);
        return false;
    }

    void AfterIo() {
        if (cpuJitSlowPath(cpu)) jit->HaltExecution(kSlowPath);
    }
    // jitRanNow for this run, which is the one running.
    uint32_t RanNow() const {
        const int64_t ran = (int64_t(budget) - jit->TicksRemainingNow()) >> model.shift;
        return ran <= 0 ? 0 : uint32_t(std::min<int64_t>(ran, UINT32_MAX));
    }

    // Fast at Original (cpuJitMemoryCost): generated loads and stores check the CPU's data cache
    // hint (cpuJitCostHint) before the page table, and come to the callbacks unless the hint
    // says the access hits or joins a write buffer entry, at no charge. The callbacks charge the
    // data cache and region model. The charges wait in the CPU's pending charge until the run
    // ends; once they reach what's left of the budget (as of the last block end), the run ends
    // at the next link. Until a callback has ended it that way, generated code checks that too
    // before it takes a hit, and leaves it to the callback, so the run ends where it would if
    // every access came here. Code is compiled with the hint check while this is set
    // (frond_cost_hint_on).
    bool memoryCost = false;
    bool hinted = true;  // without the hint (jitSetCostHint), the page table stays empty while memoryCost is set

    void Charged() {
        if (int64_t(cpuJitPendingCharge(cpu)) >= jit->TicksRemainingNow()) {
            jit->HaltExecution(kCharged);
            cpuJitCostHalted(cpu);
        }
    }

    template <typename T>
    T Read(A32::VAddr va) {
        T v{};
        stats.reads++;
        stats.NoteCallback(va, false);
        // A device page: no page to fill.
        bool ok;
        if (const int io = cpuJitIoRead(cpu, va, sizeof(T), &v, &ok, gen)) {
            if (!ok) {
                AccessFailed(va, sizeof(T), false, 10);
                return 0;
            }
            if (io == 2) GenChanged(cpuJitPageGen(cpu));
            AfterIo();
            if (memoryCost) Charged();
            return v;
        }
        // The ARM920T's lean path refuses without setting fsr: that's an exit, not a guest abort.
        uint_fast8_t fsr = 0;
        if (!cpuJitRead(cpu, va, sizeof(T), &v, &fsr)) {
            AccessFailed(va, sizeof(T), false, fsr);
            return 0;
        }
        // Only aligned reads land here for pages the table could hold; fill it.
        CheckGen();
        AfterFill();
        if (memoryCost) {
            AfterIo();
            Charged();
            if (!hinted) return v;
        }
        const uint32_t page = va >> 12;
        if (!(*pages)[page]) {
            uint8_t *host = cpuJitReadPage(cpu, va);
            if (host && !(uintptr_t(host) & 1)) {
                stats.pageFills++;
                SetPage(page, ReadOnly(host));
                NoteFilled(page);
            } else if (!memoryCost) {
                AfterIo();
            }
        }
        return v;
    }

    template <typename T>
    void Write(A32::VAddr va, T v) {
        stats.writes++;
        stats.NoteCallback(va, true);
        if constexpr (sizeof(T) <= 4) {
            if (arm920) {
                // Device pages and RAM code pages never share a page, so the order of the two tables doesn't
                // matter; the device one (ring words) is the busier.
                if (const int device = cpuJitLeanDeviceWrite(cpu, va, sizeof(T), uint32_t(v), RanNow())) {
                    if (device < 0) {
                        AccessFailed(va, sizeof(T), true, 0);
                        return;
                    }
                    CheckGen();
                    AfterFill();
                    AfterIo();
                    if (memoryCost) Charged();
                    return;
                }
                if (!memoryCost && cpuJitCodePageWrite(cpu, va, sizeof(T), uint32_t(v))) {
                    AfterIo();
                    return;
                }
            }
        }
        bool ok;
        if (const int io = cpuJitIoWrite(cpu, va, sizeof(T), &v, &ok, gen)) {
            if (!ok) {
                AccessFailed(va, sizeof(T), true, 10);
                return;
            }
            if (io == 2) GenChanged(cpuJitPageGen(cpu));
            AfterIo();
            if (memoryCost) Charged();
            return;
        }
        uint_fast8_t fsr = 0;
        if (!cpuJitWrite(cpu, va, sizeof(T), &v, &fsr)) {
            AccessFailed(va, sizeof(T), true, fsr);
            return;
        }
        // A RAM page the guest may store to in this mode: map it writable. Unaligned stores
        // also land here for pages that already are.
        CheckGen();
        AfterFill();
        if (memoryCost) {
            Charged();
            if (!hinted) {
                AfterIo();
                return;
            }
        }
        const uint32_t page = va >> 12;
        uint8_t *const entry = (*pages)[page];
        if (!entry || (uintptr_t(entry) & 1)) {
            uint8_t *host = cpuJitWritePage(cpu, va);
            if (host && !(uintptr_t(host) & 1)) {
                stats.writeMaps++;
                if (!entry) NoteFilled(page);
                SetPage(page, host);
                writable[host].push_back(page);
            }
        }
        AfterIo();
    }

    std::optional<std::uint32_t> MemoryReadCode(A32::VAddr va) override {
        NoteCode(va);
        uint32_t w;
        if (!cpuJitFetch(cpu, va, &w)) return std::nullopt;
        return w;
    }

    bool PreCodeReadHook(bool thumb, A32::VAddr pc, A32::IREmitter &ir) override {
        if (pc == A32::LocationDescriptor{ir.block.Location()}.PC()) stats.translated++;
        if (lineBound && (pc & ~31u) != (A32::LocationDescriptor{ir.block.Location()}.PC() & ~31u)) {
            ir.SetTerm(IR::Term::LinkBlock{ir.current_location});
            return false;
        }
        const bool marked = arm920 && cpuJitAccessPcMarked(cpu, pc);
        const bool startBlocks = arm920 && cpuJitAccessPcsStartBlocks(cpu);
        const bool startsBlock = marked && startBlocks;
        if (!AccessPcSplit(startBlocks, startsBlock, pc, ir)) return false;
        uint32_t word = 0;
        const bool ok = CodeReadHook(thumb, arm920, pc, ir, [&](uint32_t at, uint32_t &w) {
            const bool plain = cpuJitFetch(cpu, at & ~3u, &w) && cpuJitPlain(cpu, at, w, thumb);
            if (plain && pc == A32::LocationDescriptor{ir.block.Location()}.PC()) blocks++;
            word = w;
            return plain;
        });
        if (ok) NoteAccessPc(thumb, word, marked, startsBlock, pc, ir);
        return ok;
    }

    // Compile thread. The snapshot runs from pc up to the first instruction that surely
    // ends the block (whole: up to the block length limit), stopping early at one the JIT
    // can't run, as the hook would.
    uint32_t maxInstructions = 0;

    std::unique_ptr<A32::CodeSnapshot> SnapshotCode(A32::VAddr pc, bool thumb, bool whole) override {
        auto s = std::make_unique<Snapshot>();
        s->arm920 = arm920;
        s->startBlocks = arm920 && cpuJitAccessPcsStartBlocks(cpu);
        s->pid = arm920 ? cpuGetPid(cpu) : 0;
        s->model = model;
        s->base = pc & ~3u;
        const bool mapped = arm920 && pidTags && s->pid && pc < 0x02000000u;
        auto word = [&](uint32_t va) {
            const uint32_t i = (va - s->base) >> 2;
            while (s->words.size() <= i) {
                uint32_t w = 0;
                const uint32_t at = s->base + uint32_t(s->words.size()) * 4;
                const bool ok = cpuJitFetch(cpu, at, &w);
                if (ok && mapped && at < 0x02000000u && !s->MapOf(at)) {
                    uint32_t access;
                    s->maps.push_back({at >> 12, CodePa(CodePageOf(at), access), access});
                }
                s->words.push_back(w);
                s->fetched.push_back(ok);
                s->read.push_back(0);
            }
            return i;
        };
        const uint32_t limit = maxInstructions ? maxInstructions : 1024;
        uint32_t at = pc;
        for (uint32_t n = 0; n < limit; n++) {
            const uint32_t i = word(at);
            const uint32_t w = s->words[i];
            const bool plain = s->fetched[i] && cpuJitPlain(cpu, at, w, thumb);
            // A marked pc that starts its block ends the snapshot before it unless it's the first.
            const bool marked = arm920 && cpuJitAccessPcMarked(cpu, at);
            if (marked && s->startBlocks && at != pc) {
                s->splitAt = at;
                break;
            }
            s->plain.push_back({at, plain});
            if (!plain) break;
            if (marked) s->marked.push_back(at);
            uint32_t size = 4;
            if (thumb) {
                const uint32_t h = (at & 2) ? w >> 16 : w & 0xffff;
                size = h >= 0xe800 ? 4 : 2;
                if (size == 4) word(at + 2);
            }
            if (!whole && EndsBlock(w, at, thumb)) break;
            if (!thumb && Cp15Slot(w, arm920)) break;  // p15 slot instructions end their block
            at += size;
        }
        s->runnable = s->plain.front().second;
        return s;
    }

    // Ahead of need: as SnapshotCode, reading only RAM and ROM. Code that reaches anything
    // else isn't compiled ahead.
    std::unique_ptr<A32::CodeSnapshot> SnapshotCodeAhead(A32::VAddr pc, bool thumb, bool whole) override {
        // A guarded page can't be fetched now.
        if (pc < 0x02000000u && guard[pc >> 12]) return nullptr;
        cpuJitMemoryOnly(cpu, true);
        auto s = SnapshotCode(pc, thumb, whole);
        cpuJitMemoryOnly(cpu, false);
        if (cpuJitRefused(cpu)) return nullptr;
        return s;
    }

    // The words translation read are fetched again; if any changed (or no longer fetch),
    // the block is dropped. Otherwise they are noted as code, as MemoryReadCode does.
    // A word of a block fetched under another PID below 32 MB, whose MVA isn't mapped now (WinCE unmaps a
    // slot while other processes run): read at the physical page it was fetched from, if the page is
    // still watched there or not watched at all. Resume then keeps the block only if the slot maps back
    // the same.
    bool FetchUnmapped(const Snapshot &s, uint32_t va, uint32_t mva, uint32_t *w) {
        const auto *m = s.MapOf(va);
        if (!m || m->pa == ~0u) return false;
        uint32_t access;
        if (CodePa(mva >> 12, access) != ~0u) return false;
        if (const auto c = code.find(mva >> 12); c != code.end() && (c->second.pa != m->pa || c->second.access != m->access))
            return false;
        return cpuJitFetchPa(cpu, (m->pa << 12) | (va & 0xfffu), w);
    }

    bool CodeUnchanged(A32::CodeSnapshot &snapshot) override {
        auto &s = static_cast<Snapshot &>(snapshot);
        // Fetched under another PID below 32 MB: the words are checked again at their MVA, which reads the
        // same under every PID. The block keeps the tag it was queued under, that PID's, so it only runs
        // there. PID 0's slot 0 has no MVA of its own. Code from 32 MB up reads the same under every PID.
        const bool otherPid = arm920 && s.pid != cpuGetPid(cpu) && s.base < 0x02000000u;
        if (otherPid && !s.pid) return false;
        for (size_t i = 0; i < s.words.size(); i++) {
            if (!s.read[i]) continue;
            uint32_t w = 0;
            const uint32_t va = s.base + uint32_t(i) * 4;
            const bool inSlot = otherPid && va < 0x02000000u;
            const uint32_t mva = va | s.pid;
            bool ok = inSlot ? cpuJitFetchMva(cpu, mva, &w) : cpuJitFetch(cpu, va, &w);
            if (!ok && inSlot && s.fetched[i]) ok = FetchUnmapped(s, va, mva, &w);
            if (ok != bool(s.fetched[i]) || (ok && w != s.words[i])) return false;
        }
        if (otherPid) stats.otherPidInstalls++;
        for (size_t i = 0; i < s.words.size(); i++) {
            if (!s.read[i]) continue;
            const uint32_t va = s.base + uint32_t(i) * 4;
            if (otherPid && va < 0x02000000u) {
                const uint32_t mva = va | s.pid;
                if (!code.count(mva >> 12)) {
                    // Only FetchUnmapped passes a page that isn't mapped now: watch it at the snapshot's
                    // physical page, suspended until its PID runs again.
                    const auto *m = s.MapOf(va);
                    uint32_t access;
                    if (m && CodePa(mva >> 12, access) == ~0u) {
                        auto it = NotePage(mva >> 12, m->pa, m->access);
                        it->second.suspended = true;
                        suspended[mva >> 25].push_back(mva >> 12);
                    }
                }
                NoteCodeAt(mva >> 12, mva & 0xfffu);
            } else {
                NoteCode(va);
            }
        }
        return true;
    }

    void CompileQueued(A32::VAddr, bool) override {
        stats.cold++;
        stats.queued++;
        jit->HaltExecution(kCold);
    }

    // A p15 access in user mode: leave the registers as they were and end the run with the
    // instruction next, for the interpreter.
    uint32_t rewindPc = ~0u;
    void Cp15User(uint32_t pc) {
        stats.cp15User++;
        rewindPc = pc;
        undo = TicksAt(pc, false);
        jit->HaltExecution(kInterpret);
    }

    std::uint8_t MemoryRead8(A32::VAddr va) override { return Read<uint8_t>(va); }
    std::uint16_t MemoryRead16(A32::VAddr va) override { return Read<uint16_t>(va); }
    std::uint32_t MemoryRead32(A32::VAddr va) override { return Read<uint32_t>(va); }
    std::uint64_t MemoryRead64(A32::VAddr va) override { return Read<uint64_t>(va); }
    void MemoryWrite8(A32::VAddr va, std::uint8_t v) override { Write(va, v); }
    void MemoryWrite16(A32::VAddr va, std::uint16_t v) override { Write(va, v); }
    void MemoryWrite32(A32::VAddr va, std::uint32_t v) override { Write(va, v); }
    void MemoryWrite64(A32::VAddr va, std::uint64_t v) override { Write(va, v); }

    // An in-place MSR CPSR that couldn't run in place (psrInterpret set) has had its tick
    // counted, so it's taken back for the interpreter's.
    uint32_t psrInterpret = 0;
    void InterpreterFallback(A32::VAddr pc, size_t) override {
        if (psrInterpret) {
            psrInterpret = 0;
            stats.psrInterpret++;
            psrInterprets.fetch_add(1, std::memory_order_relaxed);
            undo = TicksAt(pc, jit->Cpsr() & 0x20);
            jit->HaltExecution(kInterpret);
            return;
        }
        if (stats.on) {
            uint32_t w = 0;
            cpuJitFetch(cpu, pc & ~3u, &w);
            const bool thumb = jit->Cpsr() & 0x20;
            if (!stats.interpPc[pc]++) stats.interpWord[pc] = thumb ? ((pc & 2) ? w >> 16 : w & 0xffff) : w;
            if (!thumb && (w & 0x0c000000) == 0x0c000000 && (w & 0x0f000000) != 0x0f000000 && ((w >> 8) & 14) == 0) {
                if ((w >> 28) == 15) stats.interpWmmx[4]++;
                else if ((w & 0x0f000010) == 0x0e000000) stats.interpWmmx[0]++;
                else if ((w & 0x0f000000) == 0x0e000000) stats.interpWmmx[1]++;
                else if ((w & 0x0fe00000) == 0x0c400000) stats.interpWmmx[2]++;
                else stats.interpWmmx[3]++;
            }
        }
        // Its block didn't count it (the fork's frond_interpreted); the interpreter charges it.
        jit->HaltExecution(kInterpret);
    }
    // SWI (not the semihosting ones, cpuJitPlain): PC is past it. The run ends at the
    // vector, so the next one starts there in the right privilege level's JIT with the
    // budget and slow path checked, as after an interpreted SWI. An ARM920T SWI from a
    // privileged mode with budget left goes on at the vector in this run: the block returns
    // to the dispatcher, which takes the new PC and ARM state, with what is left of the
    // budget (AddTicks has the cycles so far, GetTicksRemaining gives the rest).
    void CallSVC(std::uint32_t) override {
        stats.swi++;
        jit->SetCpsr(cpuJitSwi(cpu, jit->Regs().data(), jit->Cpsr()));
        // The board looks at the state at the vector (cpuArm920SetJitSwiHalt); regs[14] is the return address.
        if (arm920 && cpuJitSwiHalts(cpu, jit->Regs()[14])) {
            ClearSetChange();
            jit->HaltExecution(kSwi);
            return;
        }
        if (cpuJitPrivileged(cpu) != privileged) stats.privSwitches++;
        else if (arm920 && ran < budget && (ClearSetChange(), !cpuJitSlowPath(cpu))) return;
        jit->HaltExecution(kSwi);
    }
    // The interpreter's ARM/Thumb switch flag: generated code follows the Thumb bit by
    // itself, so on the ARM920T a switch alone doesn't end the run.
    void ClearSetChange() {
        if (arm920) cpuClearSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE);
    }

    // Undefined, unpredictable and no-execute: PC is past the instruction and its tick
    // is counted, so step back and let the interpreter run it.
    void ExceptionRaised(A32::VAddr pc, A32::Exception exception) override {
        jit->Regs()[15] = pc;
        // A fetch that failed counted 1 (dynarmic's default).
        undo = exception == A32::Exception::NoExecuteFault ? 1 : TicksAt(pc, jit->Cpsr() & 0x20);
        jit->HaltExecution(kInterpret);
    }

    // MSR CPSR with more than the flags, but not one psr_inline_interrupt_mask runs in place
    // (only I and F changed, nothing waiting). The fork ends the block after this call, so the
    // banked registers and CPSR can change under it. A set slow path (an unmasked pending
    // interrupt, a Thumb switch) ends the run there, and so does a privilege change, with
    // nothing run or checked at the new privilege in this JIT.
    static std::uint64_t SetCpsr(void *arg, std::uint32_t value, std::uint32_t mask) {
        Env &e = *static_cast<Env *>(arg);
        stats.psr++;
        const uint32_t cpsr = cpuJitSetCpsr(e.cpu, e.jit->Regs().data(), e.jit->Cpsr(), mask, value);
        e.jit->SetCpsr(cpsr);
        e.AfterPsr();
        return 0;
    }

    void AfterPsr() {
        if (cpuJitPrivileged(cpu) != privileged) {
            stats.privSwitches++;
            jit->HaltExecution(kPrivilege);
        } else {
            CheckGen();
        }
        ClearSetChange();
        AfterIo();
    }

    // MOVS PC or SUBS PC. The fork returns to the dispatcher after this call, which picks up
    // the new PC and Thumb state.
    static std::uint64_t ExceptionReturn(void *arg, std::uint32_t target, std::uint32_t) {
        Env &e = *static_cast<Env *>(arg);
        stats.psr++;
        const uint32_t cpsr = cpuJitExceptionReturn(e.cpu, e.jit->Regs().data(), e.jit->Cpsr(), target);
        e.jit->SetCpsr(cpsr);
        e.AfterPsr();
        return 0;
    }

    struct Cp15Op {
        Env *env;
        uint32_t pc, rd, word;
    };
    // By pc and word, kept for the process. Added to by whichever thread compiles; generated
    // code holds pointers to the entries, which don't move. An entry is written only when it is
    // added, since generated code on the guest thread may be reading it.
    std::unordered_map<uint64_t, Cp15Op> cp15Ops;
    std::mutex cp15Mutex;

    Cp15Op *Cp15Current() {
        std::lock_guard lock{cp15Mutex};
        return &cp15Ops.try_emplace((uint64_t(cp15Word) << 32) | cp15Pc, Cp15Op{this, cp15Pc, cp15Rd, cp15Word})
                    .first->second;
    }

    static std::uint64_t Cp15Read(void *arg, std::uint32_t, std::uint32_t) {
        const Cp15Op &op = *static_cast<Cp15Op *>(arg);
        Env &e = *op.env;
        stats.cp15++;
        if (!cpuJitPrivileged(e.cpu)) {
            e.Cp15User(op.pc);
            return e.jit->Regs()[op.rd];
        }
        domainCalls.fetch_add(1, std::memory_order_relaxed);
        return cpuJitGetDomain(e.cpu);
    }

    // A domain change flushes the host TLB; the page table drops the pages in changed domains.
    static std::uint64_t Cp15Write(void *arg, std::uint32_t value, std::uint32_t) {
        const Cp15Op &op = *static_cast<Cp15Op *>(arg);
        Env &e = *op.env;
        stats.cp15++;
        if (!cpuJitPrivileged(e.cpu)) {
            e.Cp15User(op.pc);
            return 0;
        }
        domainCalls.fetch_add(1, std::memory_order_relaxed);
        cpuJitSetDomain(e.cpu, value);
        e.CheckGen();
        e.AfterIo();
        return 0;
    }

    // The other p15 slot instructions, through the interpreter's coprocessor handler: the
    // result for an MRC, ignored for an MCR.
    static std::uint64_t Cp15Xfer(void *arg, std::uint32_t value, std::uint32_t) {
        const Cp15Op &op = *static_cast<Cp15Op *>(arg);
        Env &e = *op.env;
        stats.cp15++;
        if (!cpuJitPrivileged(e.cpu) || !cpuJitCp15Xfer(e.cpu, op.word, &value)) {
            e.Cp15User(op.pc);
            return e.jit->Regs()[op.rd];
        }
        return value;
    }

    // MCR p15 c7 c5 1: the interpreter's I-cache line invalidate, which also drops the code
    // of unwatched pages in that line from both JITs (jitInvalidate). If that drops the
    // running block, dynarmic finishes it first; the block ends after this instruction.
    static std::uint64_t Cp15InvalLine(void *arg, std::uint32_t value, std::uint32_t) {
        const Cp15Op &op = *static_cast<Cp15Op *>(arg);
        Env &e = *op.env;
        stats.cp15++;
        // The ARM920T refuses an address with SBZ bits 4:0 set; the interpreter takes that one.
        if (!cpuJitPrivileged(e.cpu) || (e.arm920 && (value & 31))) {
            e.Cp15User(op.pc);
            return 0;
        }
        stats.icacheLine++;
        cpuJitIcacheInvalLine(e.cpu, value);
        return 0;
    }

    // iWMMXt instructions, by encoding without the condition and core registers: the
    // fields for pxa270_WMMX's cores. Kept for the process like cp15Ops.
    struct WmmxOp {
        Pxa270wmmx *w;
        bool cp1;
        uint8_t op1, a, b, c, op2;  // CDP: CRd, CRn, CRm; MCR/MRC: CRn, CRm; MCRR/MRRC: CRm
    };
    std::unordered_map<uint32_t, WmmxOp> wmmxOps;
    std::mutex wmmxMutex;

    WmmxOp *Wmmx(uint32_t key, WmmxOp op) {
        std::lock_guard lock{wmmxMutex};
        return &wmmxOps.try_emplace(key, op).first->second;
    }

    // None of these can fail: the plain check let only encodings in that
    // pxa270wmmxJitValid accepts, and a false return depends only on the encoding.
    static std::uint64_t WmmxCdp(void *arg, std::uint32_t, std::uint32_t) {
        const WmmxOp &op = *static_cast<WmmxOp *>(arg);
        stats.wmmx++;
        pxa270wmmxCdp(op.w, op.cp1, op.op1, op.a, op.b, op.c, op.op2);
        return 0;
    }
    static std::uint64_t WmmxMcr(void *arg, std::uint32_t value, std::uint32_t) {
        const WmmxOp &op = *static_cast<WmmxOp *>(arg);
        stats.wmmx++;
        pxa270wmmxMcr(op.w, op.cp1, op.op1, op.a, op.b, op.op2, value);
        return 0;
    }
    static std::uint64_t WmmxMrc(void *arg, std::uint32_t, std::uint32_t) {
        const WmmxOp &op = *static_cast<WmmxOp *>(arg);
        stats.wmmx++;
        uint32_t value = 0;
        pxa270wmmxMrc(op.w, op.cp1, op.op1, op.a, op.b, op.op2, &value);
        return value;
    }
    static std::uint64_t WmmxMcrr(void *arg, std::uint32_t lo, std::uint32_t hi) {
        const WmmxOp &op = *static_cast<WmmxOp *>(arg);
        stats.wmmx++;
        pxa270wmmxMcrr(op.w, op.cp1, op.op1, op.c, lo, hi);
        return 0;
    }
    static std::uint64_t WmmxMrrc(void *arg, std::uint32_t, std::uint32_t) {
        const WmmxOp &op = *static_cast<WmmxOp *>(arg);
        stats.wmmx++;
        uint64_t value = 0;
        pxa270wmmxMrrc(op.w, op.cp1, op.op1, op.c, &value);
        return value;
    }

    // WLDR and WSTR, by coprocessor, size (N), direction and wR; arg0 is the address
    // dynarmic worked out (cp0 offsets unscaled). The access is the interpreter's, so a
    // failed one ends the run on the instruction with the base not written back (the
    // dynarmic patch checks right after the call) and the fault kept in dataAbort: jitRun
    // reports FROND_JIT_ABORT and the CPU takes the data abort there as Exact does, without
    // running the instruction again, since a WLDRD or WSTRD whose second word failed has
    // already done the first word's access and a device would see it twice.
    struct Abort {
        bool set = false;
        FrondJitAbort fault{};
    } dataAbort;
    struct WmmxXfer {
        Env *env;
        bool cp1, N, store;
        uint8_t wR;
    };
    static std::uint64_t WmmxTransfer(void *arg, std::uint32_t ea, std::uint32_t) {
        const WmmxXfer &x = *static_cast<WmmxXfer *>(arg);
        Env &e = *x.env;
        stats.wmmx++;
        stats.wmmxXfer++;
        Pxa270wmmxFault f;
        if (!pxa270wmmxTransfer(cpuJitWmmx(e.cpu), e.cpu, x.cp1, x.N, x.store, x.wR, ea, &f)) {
            stats.wmmxFaults++;
            e.dataAbort = Abort{true, {f.addr, f.fsr, f.size, f.write}};
            e.Fault();
            return 0;
        }
        e.CheckGen();
        e.AfterIo();
        return 0;
    }

    void AddTicks(std::uint64_t ticks) override { ran += ticks; }
    std::uint64_t GetTicksForCode(bool thumb, A32::VAddr, std::uint32_t instruction) override {
        return model.Ticks(thumb, instruction);
    }
    std::uint64_t GetTicksForConditionFailed() override { return model.Failed(); }
    // Called at the start of a run and again after a CallSVC that doesn't halt, once AddTicks has taken the cycles so
    // far: the rest of the run's budget, so a run that goes on past a SWI still ends on time.
    std::uint64_t GetTicksRemaining() override { return budget > ran ? budget - ran : 0; }

    // The ticks of an instruction the run counted and the interpreter runs again, taken off at the end.
    uint64_t undo = 0;
    TickModel model;

    // What translation counted for the instruction at pc, read as dynarmic reads it.
    uint64_t TicksAt(uint32_t pc, bool thumb) {
        uint32_t w;
        if (!cpuJitFetch(cpu, pc & ~3u, &w)) return 1;
        if (!thumb) return model.Ticks(false, w);
        uint32_t i = (pc & 2) ? w >> 16 : w & 0xffff;
        if (i >= 0xe800) {
            const uint32_t next = pc + 2;
            if (!cpuJitFetch(cpu, next & ~3u, &w)) return 1;
            i = (i << 16) | ((next & 2) ? w >> 16 : w & 0xffff);
        }
        return model.Ticks(true, i);
    }
};


struct PsrCoprocessor final : public A32::Coprocessor {
    Env *env;
    std::uint32_t ignored = 0;  // MSR SPSR's field mask
    explicit PsrCoprocessor(Env *e) : env(e) {}

    std::optional<Callback> CompileInternalOperation(bool, unsigned, A32::CoprocReg, A32::CoprocReg,
                                                     A32::CoprocReg, unsigned) override {
        return std::nullopt;
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned opc, A32::CoprocReg, A32::CoprocReg, unsigned) override {
        if (opc == 2) return cpuJitInterruptMaskWord(env->cpu);
        if (opc == 3) return cpuJitAccessPcSlot(env->cpu);
        if (opc == 5) return &env->psrInterpret;
        return std::monostate{};
    }
    // opc 0: MSR CPSR, opc 1: MSR SPSR (written whole, mask ignored), opc 2: MOVS PC.
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned opc, A32::CoprocReg) override {
        if (opc == 0) return Callback{&Env::SetCpsr, env};
        if (opc == 2) return Callback{&Env::ExceptionReturn, env};
        return std::array<std::uint32_t *, 2>{cpuJitSpsr(env->cpu), &ignored};
    }
    // opc1 0: MRS SPSR. 1 and 2: the waiting interrupts and the I and F bytes, for an
    // in-place MSR CPSR (psr_inline_interrupt_mask).
    CallbackOrAccessOneWord CompileGetOneWord(bool, unsigned opc, A32::CoprocReg, A32::CoprocReg, unsigned) override {
        if (opc == 1) return cpuJitWaitingWord(env->cpu);
        if (opc == 2) return cpuJitInterruptMaskWord(env->cpu);
        return cpuJitSpsr(env->cpu);
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    std::optional<Callback> CompileLoadWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override {
        return std::nullopt;
    }
    std::optional<Callback> CompileStoreWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override {
        return std::nullopt;
    }
};

// p15 slot: only the words jitCp15SlotWord takes get here (cpuJitPlain). c3 and the
// I-cache line invalidate have their own callbacks, the rest go through Cp15Xfer.
struct Cp15Coprocessor final : public A32::Coprocessor {
    Env *env;
    explicit Cp15Coprocessor(Env *e) : env(e) {}

    std::optional<Callback> CompileInternalOperation(bool, unsigned, A32::CoprocReg, A32::CoprocReg,
                                                     A32::CoprocReg, unsigned) override {
        return std::nullopt;
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, A32::CoprocReg crn, A32::CoprocReg crm,
                                               unsigned opc2) override {
        if (two || opc1 || cp15Pc == ~0u) return std::monostate{};
        if (crn == A32::CoprocReg::C3) return Callback{&Env::Cp15Write, env->Cp15Current()};
        if (crn == A32::CoprocReg::C7 && crm == A32::CoprocReg::C5 && opc2 == 1)
            return Callback{&Env::Cp15InvalLine, env->Cp15Current()};
        if (crn == A32::CoprocReg::C7) return Callback{&Env::Cp15Xfer, env->Cp15Current()};
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, A32::CoprocReg crn, A32::CoprocReg,
                                              unsigned) override {
        if (two || opc1 || cp15Pc == ~0u) return std::monostate{};
        if (crn == A32::CoprocReg::C3) return Callback{&Env::Cp15Read, env->Cp15Current()};
        return Callback{&Env::Cp15Xfer, env->Cp15Current()};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    std::optional<Callback> CompileLoadWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override {
        return std::nullopt;
    }
    std::optional<Callback> CompileStoreWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override {
        return std::nullopt;
    }
};

// iWMMXt slots, coprocessors 0 and 1. Only encodings pxa270wmmxJitValid accepts get here
// (cpuJitPlain), so each Compile* rebuilds the word, with Rt r0 and Rt2 r1 standing for
// the core registers dynarmic already checked, and asks it again: an encoding that got
// here and isn't valid would be a bug in the plain check, and dynarmic asserts on it.
// For LDC and STC the word is the pre-indexed form with base r0; every addressing mode the
// plain check lets in is valid alike.
struct WmmxCoprocessor final : public A32::Coprocessor {
    Env *env;
    bool cp1;
    std::array<Env::WmmxXfer, 64> xfers;  // by N, store and wR; filled here, then read only
    WmmxCoprocessor(Env *e, bool c) : env(e), cp1(c) {
        for (unsigned i = 0; i < xfers.size(); i++)
            xfers[i] = Env::WmmxXfer{e, c, bool(i & 32), bool(i & 16), uint8_t(i & 15)};
    }

    std::optional<Callback> Transfer(bool store, bool two, bool N, A32::CoprocReg CRd,
                                     std::optional<std::uint8_t> option) {
        const uint32_t w = Word((store ? 0x0d800000 : 0x0d900000) | (N ? 1u << 22 : 0) | (R(CRd) << 12));
        if (two || option || !cpuJitWmmx(env->cpu) || !pxa270wmmxJitValid(w)) return std::nullopt;
        return Callback{&Env::WmmxTransfer, &xfers[(N ? 32 : 0) | (store ? 16 : 0) | R(CRd)]};
    }

    static uint8_t R(A32::CoprocReg r) { return uint8_t(r); }
    uint32_t Word(uint32_t bits) const { return 0xe0000000 | bits | (cp1 ? 0x100 : 0); }
    Env::WmmxOp *Op(uint32_t word, unsigned op1, unsigned a, unsigned b, unsigned c, unsigned op2) {
        return env->Wmmx(word, Env::WmmxOp{cpuJitWmmx(env->cpu), cp1, uint8_t(op1), uint8_t(a), uint8_t(b),
                                           uint8_t(c), uint8_t(op2)});
    }

    std::optional<Callback> CompileInternalOperation(bool two, unsigned opc1, A32::CoprocReg CRd, A32::CoprocReg CRn,
                                                     A32::CoprocReg CRm, unsigned opc2) override {
        const uint32_t w = Word(0x0e000000 | (opc1 << 20) | (R(CRn) << 16) | (R(CRd) << 12) | (opc2 << 5) | R(CRm));
        if (two || !cpuJitWmmx(env->cpu) || !pxa270wmmxJitValid(w)) return std::nullopt;
        return Callback{&Env::WmmxCdp, Op(w, opc1, R(CRd), R(CRn), R(CRm), opc2)};
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm,
                                               unsigned opc2) override {
        const uint32_t w = Word(0x0e000010 | (opc1 << 21) | (R(CRn) << 16) | (opc2 << 5) | R(CRm));
        if (two || !cpuJitWmmx(env->cpu) || !pxa270wmmxJitValid(w)) return std::monostate{};
        return Callback{&Env::WmmxMcr, Op(w, opc1, R(CRn), R(CRm), 0, opc2)};
    }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm,
                                              unsigned opc2) override {
        const uint32_t w = Word(0x0e100010 | (opc1 << 21) | (R(CRn) << 16) | (opc2 << 5) | R(CRm));
        if (two || !cpuJitWmmx(env->cpu) || !pxa270wmmxJitValid(w)) return std::monostate{};
        return Callback{&Env::WmmxMrc, Op(w, opc1, R(CRn), R(CRm), 0, opc2)};
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool two, unsigned opc, A32::CoprocReg CRm) override {
        const uint32_t w = Word(0x0c410000 | (opc << 4) | R(CRm));
        if (two || !cpuJitWmmx(env->cpu) || !pxa270wmmxJitValid(w)) return std::monostate{};
        return Callback{&Env::WmmxMcrr, Op(w, opc, 0, 0, R(CRm), 0)};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, A32::CoprocReg CRm) override {
        const uint32_t w = Word(0x0c510000 | (opc << 4) | R(CRm));
        if (two || !cpuJitWmmx(env->cpu) || !pxa270wmmxJitValid(w)) return std::monostate{};
        return Callback{&Env::WmmxMrrc, Op(w, opc, 0, 0, R(CRm), 0)};
    }
    std::optional<Callback> CompileLoadWords(bool two, bool N, A32::CoprocReg CRd,
                                             std::optional<std::uint8_t> option) override {
        return Transfer(false, two, N, CRd, option);
    }
    std::optional<Callback> CompileStoreWords(bool two, bool N, A32::CoprocReg CRd,
                                              std::optional<std::uint8_t> option) override {
        return Transfer(true, two, N, CRd, option);
    }
};

}  // namespace

// One privilege level's JIT. The page table is calloc'd, so only the entries used take
// memory.
struct Level {
    Env env;
    PageTable *pages = nullptr;
#ifdef __aarch64__
    PageTable *readPages = nullptr, *writePages = nullptr;
#endif
    std::unique_ptr<A32::Jit> jit;
    std::shared_ptr<WmmxCoprocessor> wmmx[2];
    ~Level() {
        jit.reset();
        free(pages);
#ifdef __aarch64__
        free(readPages);
        free(writePages);
#endif
    }
};

struct FrondJit {
    ArmCpu *cpu = nullptr;
    // [0] user, [1] privileged. Both are made by jitCreate.
    std::unique_ptr<Level> level[2];
    uint32_t maxUnchecked = 0, hot = 1;
    bool singleStep = false;  // ARM920T probe diagnostic: one instruction per check, blocks end at each line
    A32::Jit::CompileThread mode{};
    unsigned ahead = 0;
    FrondJitAbort abort{};  // the last run's, for FROND_JIT_ABORT
    const Env *running = nullptr;  // the level jitRun is in, for jitRanNow
    // Pages a store dropped code from, whose line flags are worked out again before the next run
    // (jitPrvRemask): physical pages (ARM920T Fast blocks) and host pages.
    std::vector<uint32_t> remaskPa;
    std::vector<uint8_t *> remaskHost;
    uint32_t lastInstructions = 0;  // the last run's (jitLastInstructions)
    // ARM920T FCSE tags by PID >> 25 (Env::pidTags): 0 for PID 0 and PIDs not seen since the last
    // reassignment, then 1 to kPidTags - 1 in the order PIDs first run.
    uint8_t pidTags[128] = {};
    uint8_t nextTag = 1;
    uint64_t tagRestarts = 0;
    // ARM920T Fast blocks: the code pages of both levels by MVA page (Env::fillWatch), for the MMU's fill watch.
    std::unique_ptr<uint8_t[]> fillWatch;
};

// Tags ride in FPSCR bits that only VFP vector code reads, which the ARM920T has none of: Len (16-18) and
// Stride (20-21). Neither reaches the host (MXCSR on x64; the arm64 prelude masks them out of FPCR). AHP,
// rounding mode, FZ and DN stay 0, as they would reach the host's FP state.
constexpr unsigned kPidTags = 32;
constexpr unsigned kAbsentTag = kPidTags - 1;  // not handed out (it was the absent tag)
constexpr uint32_t jitPrvTagFpscr(unsigned tag) { return ((tag & 7u) << 16) | (((tag >> 3) & 3u) << 20); }
static_assert(jitPrvTagFpscr(kPidTags - 1) == 0x00370000);


const FrondJitAbort &jitLastAbort(const FrondJit *j) { return j->abort; }

template <typename F>
void jitPrvEach(FrondJit *j, F f) {
    for (auto &l : j->level)
        if (l) f(l->env);
}

// Instructions between budget checks in generated code. It bounds how far a run can pass
// its budget, which audio admission reserves for. 64, 128 and 256 ran Bike and FrondBench
// equally fast, so the smallest. FROND_JIT_MAX_UNCHECKED overrides it for measuring, 0 for
// no limit.
static constexpr uint32_t kMaxUnchecked = 64;

// Times a block's start is reached before it's compiled. FROND_JIT_HOT overrides it for
// measuring and tests, 1 compiles everything at once.
static constexpr uint32_t kHot = 4;

static uint32_t jitPrvHot() {
    const char *v = getenv("FROND_JIT_HOT");
    if (v && *v) return std::max<uint32_t>(1, uint32_t(strtoul(v, nullptr, 10)));
    return kHot;
}

// Compile thread: on, unless FROND_JIT_THREAD=0 (compiles in place, as before). Sync mode
// (FROND_JIT_SYNC=1 or jitSetSync) waits for each compile where it was asked for, so runs
// stop at the same places every time; tests and replays use it.
static bool jitPrvSync = false;

void jitSetSync(bool sync) { jitPrvSync = sync; }

static A32::Jit::CompileThread jitPrvCompileThread() {
    const char *t = getenv("FROND_JIT_THREAD");
    if (t && *t && !strcmp(t, "0")) return A32::Jit::CompileThread::Off;
    const char *v = getenv("FROND_JIT_SYNC");
    if (v && *v) return strcmp(v, "0") ? A32::Jit::CompileThread::Sync : A32::Jit::CompileThread::On;
    return jitPrvSync ? A32::Jit::CompileThread::Sync : A32::Jit::CompileThread::On;
}

// Compiling ahead on the compile thread: blocks reached but not hot yet, and blocks up to
// kAhead links on from one asked for (FROND_JIT_AHEAD sets it, 0 is off). Off by default:
// about half of what it compiles is never used, and the guest thread pays for the snapshots,
// so it costs more CPU than it saves (TG50 and E2 on arm64). Off in sync mode unless set; it
// gives the same results there.
static constexpr unsigned kAhead = 0;

static unsigned jitPrvAhead(A32::Jit::CompileThread mode) {
    const char *v = getenv("FROND_JIT_AHEAD");
    if (v && *v) return unsigned(strtoul(v, nullptr, 10));
    return mode == A32::Jit::CompileThread::On ? kAhead : 0;
}

static uint32_t jitPrvMaxUnchecked() {
    const char *v = getenv("FROND_JIT_MAX_UNCHECKED");
    if (v && *v) return uint32_t(strtoul(v, nullptr, 10));
    return kMaxUnchecked;
}

// Code cache per JIT, address space reserved up front and used as code is written. The user
// level's is smaller on Android.
static constexpr size_t kCodeCache = 64 << 20;
#ifdef __ANDROID__
static constexpr size_t kUserCodeCache = 16 << 20;
#else
static constexpr size_t kUserCodeCache = kCodeCache;
#endif

static size_t jitPrvCodeCache = 0;
static bool jitPrvCostHint = true;

void jitSetCostHint(bool on) { jitPrvCostHint = on; }

void jitSetCodeCacheSize(size_t bytes) { jitPrvCodeCache = bytes; }

static Level *jitPrvMakeLevel(FrondJit *j, bool privileged) {
    auto l = std::make_unique<Level>();
    l->pages = static_cast<PageTable *>(calloc(1, sizeof(PageTable)));
    if (!l->pages) abort();
#ifdef __aarch64__
    l->readPages = static_cast<PageTable *>(calloc(1, sizeof(PageTable)));
    l->writePages = static_cast<PageTable *>(calloc(1, sizeof(PageTable)));
    if (!l->readPages || !l->writePages) abort();
#endif
    Env &e = l->env;
    e.cpu = j->cpu;
    e.pages = l->pages;
#ifdef __aarch64__
    e.readPages = l->readPages;
    e.writePages = l->writePages;
#endif
    e.pidTags = j->pidTags;
    e.privileged = privileged;
    e.hot = j->hot;
    A32::UserConfig conf;
    conf.callbacks = &e;
    e.arm920 = cpuGetModel(j->cpu) == ArmCpuModel::arm920t;
    e.lineBound = e.arm920 && j->singleStep;
    conf.arch_version = e.arm920 ? A32::ArchVersion::v4T : A32::ArchVersion::v5TE;
    if (e.lineBound)
        conf.optimizations &= ~(OptimizationFlag::BlockLinking | OptimizationFlag::ReturnStackBuffer |
                                OptimizationFlag::FastDispatch);
    conf.page_table = l->pages;
#ifdef __aarch64__
    conf.frond_read_page_table = l->readPages;
    conf.frond_write_page_table = l->writePages;
#endif
    conf.page_table_writes_via_callbacks = false;
    conf.page_table_write_protect_bit = true;
    e.hinted = jitPrvCostHint;
    conf.frond_cost_hint = e.hinted ? cpuJitCostHint(j->cpu) : nullptr;
    conf.frond_cost_hint_on = &e.memoryCost;
    conf.page_table_pointer_mask_bits = 1;
    conf.check_halt_on_link = true;
    conf.check_halt_on_memory_access = true;
    conf.max_unchecked_instructions = j->maxUnchecked;
    if (e.arm920 && !e.lineBound) {
        if (!j->fillWatch) {
            j->fillWatch = std::make_unique<uint8_t[]>(size_t(1) << 20);
            cpuJitSetFillWatch(j->cpu, j->fillWatch.get());
        }
        e.fillWatch = j->fillWatch.get();
        e.watchBit = privileged ? 2 : 1;
        const uint32_t *ring;
        e.seenFills = cpuJitWatchedFills(j->cpu, &ring);
        conf.frond_page_guard = e.guard.data();
        conf.frond_page_guard_end = 0x02000000u;
        conf.frond_page_guard_fpscr = jitPrvTagFpscr(kPidTags - 1);  // tag 0 (PID 0) runs are never guarded
    }
    // Unaligned accesses go through the callbacks, which fault on them.
    conf.detect_misaligned_access_via_page_table = 16 | 32 | 64;
    conf.only_detect_misalignment_via_page_table_on_page_boundary = false;
    conf.always_little_endian = true;
    conf.code_cache_size = jitPrvCodeCache ? jitPrvCodeCache : privileged ? kCodeCache : kUserCodeCache;
    conf.coprocessors[kPsrCoprocessor] = std::make_shared<PsrCoprocessor>(&e);
    conf.psr_coprocessor = kPsrCoprocessor;
    // An MSR CPSR that only masks or unmasks interrupts, with none waiting, runs in place.
    // User mode can't write the control byte, so its MSRs all take the callback.
    conf.psr_inline_interrupt_mask = privileged;
    conf.coprocessors[15] = std::make_shared<Cp15Coprocessor>(&e);
    l->wmmx[0] = std::make_shared<WmmxCoprocessor>(&e, false);
    l->wmmx[1] = std::make_shared<WmmxCoprocessor>(&e, true);
    conf.coprocessors[0] = l->wmmx[0];
    conf.coprocessors[1] = l->wmmx[1];
    conf.coprocessor_unscaled_offsets = 1;  // iWMMXt WLDRB/H and WSTRB/H offsets are in bytes
    l->jit = std::make_unique<A32::Jit>(conf);
    e.jit = l->jit.get();
    e.maxInstructions = j->maxUnchecked;
    e.model = jitPrvTickModel(j->cpu);
    l->jit->SetCompileThread(e.lineBound ? A32::Jit::CompileThread::Off : j->mode);
    l->jit->SetCompileAhead(e.lineBound ? 0 : j->ahead);
    if (Level *o = j->level[!privileged].get()) {
        e.other = &o->env;
        o->env.other = &e;
    }
    if (stats.on) {
        std::lock_guard<std::mutex> lock(stats.jitsMutex);
        stats.jits.push_back(l->jit.get());
    }
    j->level[privileged] = std::move(l);
    return j->level[privileged].get();
}

FrondJit *jitCreate(ArmCpu *cpu, bool singleStep) {
    auto *j = new FrondJit;
    j->cpu = cpu;
    j->singleStep = singleStep;
    j->maxUnchecked = singleStep ? 1 : jitPrvMaxUnchecked();
    j->hot = cpuGetModel(cpu) == ArmCpuModel::arm920t && singleStep ? 1 : jitPrvHot();
    j->mode = jitPrvCompileThread();
    j->ahead = jitPrvAhead(j->mode);
    jitPrvMakeLevel(j, true);
    // The user level too, though some guests never run in user mode: making a JIT takes
    // 5-10 ms (its prelude and memory fallbacks), which here falls in session loading
    // rather than in the slice that first enters user mode.
    jitPrvMakeLevel(j, false);
    return j;
}

void jitDestroy(FrondJit *j) {
    if (j->fillWatch) cpuJitSetFillWatch(j->cpu, nullptr);
    if (stats.on) {
        std::lock_guard<std::mutex> lock(stats.jitsMutex);
        for (auto &l : j->level) {
            if (!l) continue;
            stats.AddCompiled(*l->jit);
            std::erase(stats.jits, l->jit.get());
        }
    }
    delete j;
}

uint32_t jitMaxUnchecked(const FrondJit *j) { return j->maxUnchecked; }

uint64_t jitDomainCalls() { return domainCalls.load(std::memory_order_relaxed); }

uint64_t jitPsrInterprets() { return psrInterprets.load(std::memory_order_relaxed); }

uint32_t jitRanNow(const FrondJit *j) {
    const Env *e = j->running;
    if (!e) return 0;
    const int64_t ran = (int64_t(e->budget) - e->jit->TicksRemainingNow()) >> e->model.shift;
    return ran <= 0 ? 0 : uint32_t(std::min<int64_t>(ran, UINT32_MAX));
}

uint32_t jitRanInstructionsNow(const FrondJit *j) {
    const Env *e = j->running;
    if (!e || !e->model.shift) return jitRanNow(j);
    const int64_t ran = int64_t(e->budget) - e->jit->TicksRemainingNow();
    return ran <= 0 ? 0 : uint32_t(ran);
}

void jitExecutionCounts(const FrondJit *j, uint64_t *blocks, uint64_t *instructions) {
    *blocks = *instructions = 0;
    for (const auto &l : j->level) {
        if (!l) continue;
        *blocks += l->env.blocks;
        *instructions += l->env.instructions;
    }
}

// The counts go to the privileged level, the user level's start again.
void jitSetExecutionCounts(FrondJit *j, uint64_t blocks, uint64_t instructions) {
    jitPrvEach(j, [](Env &e) { e.blocks = e.instructions = 0; });
    j->level[1]->env.blocks = blocks;
    j->level[1]->env.instructions = instructions;
}

void jitCompileCounts(const FrondJit *j, uint64_t *stale, uint64_t *aheadCompiled, uint64_t *aheadUsed) {
    *stale = *aheadCompiled = *aheadUsed = 0;
    for (const auto &l : j->level) {
        if (!l) continue;
        const A32::Jit::CompileStats c = l->jit->GetCompileStats();
        *stale += c.stale;
        *aheadCompiled += c.ahead_compiled;
        *aheadUsed += c.ahead_used;
    }
}

uint64_t jitCacheClears(const FrondJit *j) {
    uint64_t clears = 0;
    for (const auto &l : j->level) {
        if (l) clears += l->jit->GetCompileStats().cache_clears;
    }
    return clears;
}

void jitInvalidate(FrondJit *j, uint32_t va, uint32_t size) {
    stats.invalRange++;
    jitPrvEach(j, [&](Env &e) {
        if (e.lineBound) {
            // va is an MVA (I-cache lines are by MVA). Blocks run at VAs, so a tagged slot's code below
            // 32 MB goes too.
            e.jit->InvalidateCacheRange(va, size);
            if (e.arm920 && va >= 0x02000000u && j->pidTags[va >> 25]) {
                if ((va ^ (va + size - 1)) >> 25) e.jit->ClearCache();
                else e.jit->InvalidateCacheRange(va & 0x01ffffffu, size);
            }
        } else if (!e.arm920 && !e.unwatched.empty())
            e.DropUnwatched(va >> 12, uint32_t((uint64_t(va) + size - 1) >> 12));
    });
}

void jitGuestInvalidateAll(FrondJit *j, uint32_t pc) {
    stats.invalAll++;
    if (stats.on) {
        auto &st = stats.invalAllPc[pc];
        if (!st.first++) cpuJitFetch(j->cpu, pc & ~3u, &st.second);
    }
    jitPrvEach(j, [](Env &e) {
        if (e.lineBound) e.jit->ClearCache();
        else if (!e.arm920 && !e.unwatched.empty()) e.DropUnwatched(0, ~0u);
    });
}

// Both levels clear together: a clear forgets the CPU's RAM code flags, which the other
// level's code relies on too.
void jitClear(FrondJit *j) {
    jitPrvEach(j, [](Env &e) { e.Clear(); });
}

void jitCparWritten(FrondJit *j, bool changed) {
    stats.cparWrites++;
    if (!changed) return;
    stats.cparChanges++;
    jitPrvEach(j, [](Env &e) { e.ClearCode(); });
}

// Fields as dynarmic's decoder takes them from an ARM word with a condition other than NV.
bool jitWmmxCompiles(FrondJit *j, uint32_t w) {
    const unsigned cp = (w >> 8) & 15;
    if (cp > 1 || (w >> 28) == 15) return false;
    WmmxCoprocessor &c = *j->level[1]->wmmx[cp];
    const auto reg = [&](unsigned shift) { return A32::CoprocReg((w >> shift) & 15); };
    const auto isCallback = [](const auto &v) { return std::holds_alternative<A32::Coprocessor::Callback>(v); };
    if ((w & 0x0f000010) == 0x0e000000)
        return c.CompileInternalOperation(false, (w >> 20) & 15, reg(12), reg(16), reg(0), (w >> 5) & 7).has_value();
    if ((w & 0x0f100010) == 0x0e000010)
        return isCallback(c.CompileSendOneWord(false, (w >> 21) & 7, reg(16), reg(0), (w >> 5) & 7));
    if ((w & 0x0f100010) == 0x0e100010)
        return isCallback(c.CompileGetOneWord(false, (w >> 21) & 7, reg(16), reg(0), (w >> 5) & 7));
    if ((w & 0x0ff00000) == 0x0c400000) return isCallback(c.CompileSendTwoWords(false, (w >> 4) & 15, reg(0)));
    if ((w & 0x0ff00000) == 0x0c500000) return isCallback(c.CompileGetTwoWords(false, (w >> 4) & 15, reg(0)));
    if ((w & 0x0e000000) == 0x0c000000) {  // LDC, STC; P = U = D = W = 0 is undefined
        const bool p = w & (1 << 24), u = w & (1 << 23), d = w & (1 << 22), wb = w & (1 << 21);
        if (!p && !u && !d && !wb) return false;
        std::optional<std::uint8_t> option;
        if (!p && !wb && u) option = uint8_t(w);
        return (w & (1 << 20)) ? c.CompileLoadWords(false, d, reg(12), option).has_value()
                               : c.CompileStoreWords(false, d, reg(12), option).has_value();
    }
    return false;
}

bool jitCodeWritten(FrondJit *j, uint8_t *hostPage, uint32_t offset, uint32_t size) {
    bool known = false, dropped = false;
    jitPrvEach(j, [&](Env &e) { dropped |= e.CodeWritten(hostPage, offset, size, known); });
    if (dropped && std::find(j->remaskHost.begin(), j->remaskHost.end(), hostPage) == j->remaskHost.end())
        j->remaskHost.push_back(hostPage);
    return known;
}

void jitCodeWrittenPa(FrondJit *j, uint32_t pa, uint32_t size, const void *data) {
    bool dropped = false;
    jitPrvEach(j, [&](Env &e) { dropped |= e.CodeWrittenPa(pa >> 12, pa & 0xfffu, size, data); });
    if (dropped && std::find(j->remaskPa.begin(), j->remaskPa.end(), pa >> 12) == j->remaskPa.end())
        j->remaskPa.push_back(pa >> 12);
}

// Between runs: the code marks of each page a store dropped code from become the words (ARM920T Fast
// blocks) or lines both levels' live blocks still hold code from. Until then they keep every line flagged before, so no store to code is
// missed. A page left with no code is forgotten, so its stores go the fast way again.
static void jitPrvRemask(FrondJit *j) {
    for (uint32_t pa : j->remaskPa) {
        uint64_t words[16] = {}, any = 0;
        jitPrvEach(j, [&](Env &e) { e.CodeWordsPa(pa, words); });
        for (uint64_t w : words) any |= w;
        stats.remasks++;
        if (!any) {
            stats.remaskFreed++;
            jitPrvEach(j, [&](Env &e) { e.ForgetPa(pa); });
        }
        cpuJitSetArm920CodeWords(j->cpu, pa << 12, words);
    }
    j->remaskPa.clear();
    for (uint8_t *host : j->remaskHost) {
        uint64_t words[16] = {}, lines = 0;
        uint8_t *flags = nullptr;
        jitPrvEach(j, [&](Env &e) {
            e.CodeWordsHost(host, words);
            if (const auto it = e.byHost.find(host); it != e.byHost.end())
                for (uint32_t page : it->second)
                    if (const auto c = e.code.find(page); c != e.code.end() && c->second.host == host && c->second.lines)
                        flags = c->second.lines;
        });
        for (unsigned line = 0; line < 64; line++)
            if ((words[line >> 2] >> ((line & 3) * 16)) & 0xffff) lines |= uint64_t(1) << line;
        stats.remasks++;
        if (!lines) {
            stats.remaskFreed++;
            jitPrvEach(j, [&](Env &e) { e.ForgetHost(host); });
        }
        if (flags) cpuJitSetCodeLines(j->cpu, host, flags, lines);
    }
    j->remaskHost.clear();
}

void jitWriteProtect(FrondJit *j, const uint8_t *host, uint32_t size) {
    jitPrvEach(j, [&](Env &e) {
        if (!e.writable.empty()) e.Protect(host, size);
    });
}

void jitWriteProtectPage(FrondJit *j, uint8_t *hostPage) {
    jitPrvEach(j, [&](Env &e) { e.ProtectHost(hostPage); });
}

// ARM920T Fast blocks: the tag for the CPU's PID, so its blocks are found and linked only under that PID.
// The PID only changes between runs (MCR c13 isn't a p15 slot word). Once every tag is taken, both levels
// drop their code and tags start over.
static uint8_t jitPrvPidTag(FrondJit *j) {
    const uint32_t index = cpuGetPid(j->cpu) >> 25;
    uint8_t tag = j->pidTags[index];
    if (index && !tag) {
        if (j->nextTag == kAbsentTag) {
            std::fill(std::begin(j->pidTags), std::end(j->pidTags), 0);
            j->nextTag = 1;
            j->tagRestarts++;
            jitPrvEach(j, [](Env &level) { level.ClearCode(); });
        }
        tag = j->pidTags[index] = j->nextTag++;
    }
    return tag;
}

// The tag into the JIT's FPSCR.
static void jitPrvSetTag(Env &e, A32::Jit &jit, uint8_t tag) {
    if (tag == e.tag) return;
    e.tag = tag;
    jit.SetFpscr(jitPrvTagFpscr(tag));
}

// Runs the JIT for the CPU's current privilege level. A privilege change in generated code
// ends the run as if the budget ran out, and the next run goes to the other level.
uint32_t jitRun(FrondJit *j, uint32_t *regs, uint32_t &cpsr, uint32_t cycles, int &stop) {
    const bool privileged = cpuJitPrivileged(j->cpu);
    Level *l = j->level[privileged].get();
    if (!l) l = jitPrvMakeLevel(j, privileged);
    Env &e = l->env;
    A32::Jit &jit = *l->jit;
    if (e.resetState) {
        e.resetState = false;
        jit.Reset();
        jit.ClearCache();  // Reset drops the pending cache clear's halt flag
        e.tag = 0;  // Reset sets FPSCR to 0
    }
    const uint8_t tag = e.arm920 ? jitPrvPidTag(j) : 0;
    e.CheckGen();
    e.AfterFill();
    if (const uint32_t g = cpuJitMapGen(e.cpu); g != e.mapGen) {
        e.mapGen = g;
        if (!e.code.empty()) e.CheckMappings();
    } else if (e.watchTables && e.TablesMoved()) {
        stats.tableMoves++;
        e.CheckMappings();
    }
    e.CheckGen();  // a check may have marked new table words' pages (cpuJitWatchTableWord)
    if (e.arm920) {
        const uint32_t slot = cpuGetPid(j->cpu) >> 25;
        const bool left = !e.suspended[slot].empty() && e.Resume(slot);
        e.Guard(left ? slot : 0);
        jitPrvSetTag(e, jit, tag);
    }
    if (!j->remaskPa.empty() || !j->remaskHost.empty()) jitPrvRemask(j);
    std::memcpy(jit.Regs().data(), regs, 16 * sizeof(uint32_t));
    jit.SetCpsr(cpsr);
    e.model = jitPrvTickModel(j->cpu);
    if (const bool m = cpuJitMemoryCost(j->cpu); m != e.memoryCost) {
        // Code compiled before has the hint check, or not, for the old setting.
        e.memoryCost = m;
        e.ClearPages();
        if (!e.code.empty()) jit.ClearCache();
    }
    // ARM920T ticks hold the clocks above bit 32 (TickModel); dynarmic's tick count is signed.
    e.budget = e.model.shift ? std::min<uint64_t>(cycles, INT32_MAX) << e.model.shift : cycles;
    if (e.memoryCost && e.hinted) cpuJitCostRunStart(j->cpu, cycles, !e.model.shift);
    e.ran = 0;
    e.undo = 0;
    e.dataAbort.set = false;
    j->running = &e;
    const HaltReason hr = jit.Run();
    j->running = nullptr;
    jit.ClearHalt(kInterpret | kSlowPath | kFault | kCold | kPrivilege | kSwi | kCharged);
    if (e.rewindPc != ~0u) {
        jit.Regs()[15] = e.rewindPc;
        e.rewindPc = ~0u;
    }
    std::memcpy(regs, jit.Regs().data(), 16 * sizeof(uint32_t));
    cpsr = jit.Cpsr();
    uint64_t ran = e.ran;
    ran -= std::min(ran, e.undo);
    const uint32_t clocks = uint32_t(ran >> e.model.shift);
    // PXA ticks are clocks: instructions counts them there.
    j->lastInstructions = e.model.shift ? uint32_t(ran) : clocks;
    e.instructions += j->lastInstructions;
    stats.runs++;
    stats.ran += j->lastInstructions;
    if (Has(hr, kFault)) {
        stats.fault++;
        if (stats.on) stats.faultPc[regs[15]]++;
    }
    if (Has(hr, kInterpret)) stats.interpret++;
    if (Has(hr, kSlowPath)) stats.halt++;
    if (e.dataAbort.set) {
        j->abort = e.dataAbort.fault;
        e.dataAbort.set = false;
        stop = FROND_JIT_ABORT;
    } else if (Has(hr, kFault) || Has(hr, kInterpret))
        stop = FROND_JIT_INTERPRET;
    else if (Has(hr, kSlowPath))
        stop = FROND_JIT_HALT;
    else if (Has(hr, kCold))
        stop = FROND_JIT_COLD;
    else
        stop = FROND_JIT_BUDGET;
    return clocks;
}

uint32_t jitLastInstructions(const FrondJit *j) { return j->lastInstructions; }

unsigned jitPidTags(const FrondJit *j, uint64_t *restarts) {
    if (restarts) *restarts = j->tagRestarts;
    return j->nextTag - 1u;
}

uint64_t jitFaultExits() { return stats.fault; }

uint64_t jitTranslated() { return stats.translated.load(); }

uint64_t jitMemoryCallbacks() { return stats.reads + stats.writes; }
