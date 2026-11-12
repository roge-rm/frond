// dynarmic JIT for ordinary guest code (trial, FROND_DYNARMIC builds only).
// The JIT holds no guest state between runs: CPU.cpp passes the registers and CPSR in
// and takes them back after every run. Each CPU has one JIT per privilege level; jitRun
// picks by cpuJitPrivileged, and the other calls here go to both.
#ifndef _JIT_DYNARMIC_H_
#define _JIT_DYNARMIC_H_

#include <cstddef>
#include <cstdint>

struct ArmCpu;
struct FrondJit;
struct Pxa270wmmx;

enum FrondJitStop {
    FROND_JIT_BUDGET,     // ran out of cycles, or the privilege changed
    FROND_JIT_INTERPRET,  // regs[15] is an instruction the interpreter has to run, or one
                          // whose unaligned data access the JIT refused
    FROND_JIT_HALT,       // a callback set the slow path
    FROND_JIT_FAULT,      // a data access faulted; the guest state is not exact
    FROND_JIT_COLD,       // regs[15] starts code not compiled yet, for the interpreter to run
                          // up to its next taken branch
    FROND_JIT_ABORT,      // regs[15] is a load or store whose access failed after it ran in
                          // the JIT up to there; the CPU takes the data abort jitLastAbort
                          // gives without running it again
};

// The failed access behind FROND_JIT_ABORT: its address, fault status, size in bytes and
// direction.
struct FrondJitAbort {
    uint32_t addr;
    uint8_t fsr, size;
    bool write;
};

FrondJit *jitCreate(ArmCpu *cpu, bool singleStep);
// Sync mode for JITs made after this: each block is compiled where it was asked for, on the
// compile thread with the guest thread waiting, so Fast runs repeat exactly (FROND_JIT_SYNC=1
// does the same). Without it, blocks compile in the background and runs don't repeat.
void jitSetSync(bool sync);
void jitExecutionCounts(const FrondJit* jit, uint64_t* blocks, uint64_t* instructions);
void jitSetExecutionCounts(FrondJit* jit, uint64_t blocks, uint64_t instructions);
void jitDestroy(FrondJit *jit);
const FrondJitAbort &jitLastAbort(const FrondJit *jit);
// Runs up to about cycles instructions. Returns how many ran, at most
// cycles - 1 + jitMaxUnchecked(jit) for cycles >= 1.
uint32_t jitRun(FrondJit *jit, uint32_t *regs, uint32_t &cpsr, uint32_t cycles, int &stop);
// Most instructions generated code runs between two budget checks: no block is longer and
// every link checks. 0 means no limit (FROND_JIT_MAX_UNCHECKED=0, for measuring only).
uint32_t jitMaxUnchecked(const FrondJit *jit);
// Inside jitRun, from a memory read callback: the cycles the run has charged so far, its
// finished blocks (a block's cycles count at its end).
uint32_t jitRanNow(const FrondJit *jit);
// MRC and MCR p15 c3 (domain access control) run in privileged mode from generated code, in
// every JIT so far (tests).
uint64_t jitDomainCalls();
// Compile thread counts (A32::Jit::GetCompileStats): stale, ahead compiled, ahead used.
void jitCompileCounts(const FrondJit *jit, uint64_t *stale, uint64_t *aheadCompiled, uint64_t *aheadUsed);
// Code cache size in bytes for JITs made after this (tests); 0 for the default.
void jitSetCodeCacheSize(size_t bytes);
// Whole code cache clears so far, asked for or for lack of space, both privilege levels.
uint64_t jitCacheClears(const FrondJit *jit);
// Translated code is dropped when the RAM it came from is written (jitCodeWritten) or its
// mapping changes, so the guest's I-cache invalidations only matter for code from pages
// writes can't be seen on.
void jitInvalidate(FrondJit *jit, uint32_t va, uint32_t size);
// pc is the guest instruction being run when it was asked for, for the stats.
void jitGuestInvalidateAll(FrondJit *jit, uint32_t pc);
// Host side changes (reset, session load, PACE and peephole patching): drops everything.
void jitClear(FrondJit *jit);
// A CPAR write; changed: bit 0 or 1 changed on a CPU with iWMMXt, which drops all translated
// code, since whether the JIT runs iWMMXt instructions depends on those bits.
void jitCparWritten(FrondJit *jit, bool changed);
// For tests: whether the iWMMXt slot gives a callback for the ARM word (CDP, MCR, MRC, MCRR,
// MRRC, LDC or STC on coprocessor 0 or 1), as it would when dynarmic compiles it.
bool jitWmmxCompiles(FrondJit *jit, uint32_t word);
// The first write to a watched RAM line since code was translated from it.
void jitCodeWritten(FrondJit *jit, uint8_t *hostPage);
// ARM920T Fast blocks: a store to the RAM page at pa, which code was translated from.
void jitCodeWrittenPa(FrondJit *jit, uint32_t pa);
// Sends stores to the RAM pages in [host, host + size) back through the callbacks.
void jitWriteProtect(FrondJit *jit, const uint8_t *host, uint32_t size);
// The same for the one RAM page at hostPage, after a line of it was flagged.
void jitWriteProtectPage(FrondJit *jit, uint8_t *hostPage);

// Provided by CPU.cpp for the JIT's callbacks. Accesses use the current privilege. On a
// guest fault they return false with *fsr set; an access not aligned to its size is
// refused without trying it, with *fsr 0, for the interpreter.
bool cpuJitRead(ArmCpu *cpu, uint32_t va, unsigned size, void *buf, uint_fast8_t *fsr);
bool cpuJitWrite(ArmCpu *cpu, uint32_t va, unsigned size, const void *buf, uint_fast8_t *fsr);
bool cpuJitFetch(ArmCpu *cpu, uint32_t va, uint32_t *word);
// While on, cpuJitFetch reads only RAM and ROM; a fetch from anything else fails and is
// noted for cpuJitRefused (turning it on clears that). For code compiled ahead, which the
// guest may never run, so no device sees a read the guest didn't make.
void cpuJitMemoryOnly(ArmCpu *cpu, bool on);
bool cpuJitRefused(const ArmCpu *cpu);
// Whether the JIT may run the instruction at pc (word is the aligned word holding it).
bool cpuJitPlain(ArmCpu *cpu, uint32_t pc, uint32_t word, bool thumb);
// Host address of the readable RAM or ROM page holding va, or null.
uint8_t *cpuJitReadPage(ArmCpu *cpu, uint32_t va);
// Host address of the RAM page holding va if the guest may store there now, marked dirty
// (and the frame buffer, where it overlaps) as if all of it was written, or null.
uint8_t *cpuJitWritePage(ArmCpu *cpu, uint32_t va);
// Physical address of va by the page tables, without touching the TLBs, and in *access the
// domain and AP of its page (MMU_ACCESS_*, see mmuTranslateCodeAccess), also on a fault.
bool cpuJitCodePa(ArmCpu *cpu, uint32_t va, uint32_t *pa, uint32_t *access);
// Whether code from a page with that access may be fetched now, in the current mode.
bool cpuJitAccessReadable(ArmCpu *cpu, uint32_t access);
// Domains, S and R: what cpuJitAccessReadable depends on besides the access and the
// privilege.
uint64_t cpuJitPermContext(ArmCpu *cpu);
// Host page holding the code at pa: RAM or ROM's peephole copy, null for anything else. For
// RAM, *lines gets the page's code flags, one per 64 bytes: set one and the first write to
// that line comes back through jitCodeWritten. Null for ROM.
uint8_t *cpuJitWatchCode(ArmCpu *cpu, uint32_t pa, uint8_t **lines);
// Stops watching every RAM page.
void cpuJitForgetCode(ArmCpu *cpu);
// Moves on with every TLB flush, which every mapping change goes through.
uint32_t cpuJitMapGen(ArmCpu *cpu);
// Moves on whenever cpuJitReadPage could answer differently at the same privilege, and with
// every domain (DACR) change. The top 32 bits (the epoch) move on with every change but a
// domain change: while they
// stay, a page table entry filled with cpuJitReadPage or cpuJitWritePage stays right
// unless its page's domain changed.
uint64_t cpuJitPageGen(ArmCpu *cpu);
// The MMU TLB entry for va: domain << 2 and what it allows while its domain is a client
// (mmuPageClientAccess). -1 if the TLB doesn't hold va.
int cpuJitPageClientAccess(ArmCpu *cpu, uint32_t va, bool privileged);
bool cpuJitSlowPath(ArmCpu *cpu);
// Status registers for MRS and MSR in generated code. The SPSR is read and written in
// place (an MSR writes all of it, as in the interpreter). cpuJitSetCpsr runs an MSR CPSR
// with the field mask as the interpreter does, from cpsr (the JIT's current CPSR) and the
// JIT's registers, switching their banked r8-r14 on a mode change. Returns the new CPSR.
uint32_t *cpuJitSpsr(ArmCpu *cpu);
// ARM920T Fast blocks: where generated code notes an access pc (cpuArm920SetJitAccessPcs), and whether pc is one.
uint32_t *cpuJitAccessPcSlot(ArmCpu *cpu);
bool cpuJitAccessPcMarked(const ArmCpu *cpu, uint32_t pc);
uint32_t cpuJitSetCpsr(ArmCpu *cpu, uint32_t *regs, uint32_t cpsr, uint32_t mask, uint32_t value);
// MOVS PC or SUBS PC: CPSR = SPSR in any mode and PC = target, as in the interpreter.
uint32_t cpuJitExceptionReturn(ArmCpu *cpu, uint32_t *regs, uint32_t cpsr, uint32_t target);
bool cpuJitPrivileged(ArmCpu *cpu);
// The iWMMXt unit, or null on a CPU without one.
Pxa270wmmx *cpuJitWmmx(ArmCpu *cpu);
// For tests: jitWmmxCompiles on the CPU's JIT, false without one.
bool cpuJitWmmxCompiles(ArmCpu *cpu, uint32_t word);
// MCR p15, 0, Rd, c7, c5, 1 (I-cache line invalidate) as the interpreter runs it,
// privileged modes only.
void cpuJitIcacheInvalLine(ArmCpu *cpu, uint32_t va);
// The domain access control register (MRC and MCR p15, 0, Rd, c3), privileged modes only.
uint32_t cpuJitGetDomain(ArmCpu *cpu);
void cpuJitSetDomain(ArmCpu *cpu, uint32_t value);
// Any other p15 slot instruction (jitCp15SlotWord) through the CPU's coprocessor 15
// handler, as the interpreter runs it, privileged modes only. *value holds Rd for an MCR
// and gets the result of an MRC. False where the interpreter would raise undefined.
bool cpuJitCp15Xfer(ArmCpu *cpu, uint32_t word, uint32_t *value);
// SWI from generated code as the interpreter takes it, with regs[15] past the SWI and
// cpsr the JIT's: enters SVC at the SWI vector, switching the banked r8-r14 in regs and
// setting regs[15] to the vector. Returns the new CPSR.
uint32_t cpuJitSwi(ArmCpu *cpu, uint32_t *regs, uint32_t cpsr);

// The p15 instructions the JIT's p15 slot runs, by word (cond and Rd not checked): MRC and
// MCR p15, 0, Rd, c3 (domain access control), MCR c7, c5, 1 (I-cache line invalidate),
// MRC of c0 (ID, cache type), c1 (control) and c2 (translation table base), and the MCR c7
// cache operations the interpreter does nothing for. Each ends its block.
inline bool jitCp15SlotWord(uint32_t w) {
    if ((w & 0x0fe00f10) != 0x0e000f10) return false;  // MCR or MRC p15 with opc1 0
    const uint32_t crn = (w >> 16) & 15, crm = w & 15, op2 = (w >> 5) & 7;
    if (crn == 3) return true;
    if (w & (1u << 20)) return (crn == 0 && crm == 0 && op2 <= 1) || (crn == 1 && crm == 0 && op2 == 0) || crn == 2;
    if (crn != 7) return false;
    switch ((crm << 3) | op2) {
        case (5 << 3) | 1:   // invalidate I-cache line
        case (2 << 3) | 5:   // allocate D-cache line
        case (5 << 3) | 6:   // flush BTB
        case (6 << 3) | 0:   // invalidate D-cache
        case (6 << 3) | 1:   // invalidate D-cache line
        case (6 << 3) | 2:   // invalidate D-cache by set and way
        case (10 << 3) | 0:  // clean D-cache
        case (10 << 3) | 1:  // clean D-cache line
        case (10 << 3) | 2:  // clean D-cache by set and way
        case (10 << 3) | 4:  // drain write buffer
        case (14 << 3) | 2:  // clean and invalidate D-cache line
            return true;
    }
    return false;
}

// The ARM920T's p15 slot in Fast blocks: jitCp15SlotWord's words, and MCR c7, c5, 0 and c7, c5, 2 (I-cache
// invalidate, all and by index), which Fast blocks ignore as they watch code by physical page.
inline bool jitCp15SlotWord920(uint32_t w) {
    if (jitCp15SlotWord(w)) return true;
    if ((w & 0x0fff0f1f) != 0x0e070f15) return false;
    const uint32_t op2 = (w >> 5) & 7;
    return op2 == 0 || op2 == 2;
}

#endif
