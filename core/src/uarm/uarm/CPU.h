//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _CPU_H_
#define _CPU_H_

#include <cstddef>
#include <cstdint>
#include "arm920_dcache.h"
#include <functional>

#include "mem.h"
#include "pace_patch.h"

struct ArmCpu;
enum class ArmCpuModel { armv5te, arm920t };
ArmCpuModel cpuGetModel(const ArmCpu* cpu);
// Contiguous physical mapping, inspected without filling the TLB or touching MMIO.
bool cpuInspectMapping(ArmCpu*, uint32_t address, uint32_t size, uint32_t& physical);
// Execution-thread-only observer, owned by the containing SoC. Null by default.
using EarlyAudioHook = void (*)(ArmCpu*, void*, uint32_t target, uint32_t source);
void cpuSetEarlyAudioHook(ArmCpu*, EarlyAudioHook, void*, uint32_t readySite, uint32_t disposeSite);

#define ARM_SR_N 0x80000000UL
#define ARM_SR_Z 0x40000000UL
#define ARM_SR_C 0x20000000UL
#define ARM_SR_V 0x10000000UL
#define ARM_SR_Q 0x08000000UL
#define ARM_SR_I 0x00000080UL
#define ARM_SR_F 0x00000040UL
#define ARM_SR_T 0x00000020UL
#define ARM_SR_M 0x0000001FUL

#define ARM_SR_MODE_USR 0x00000010UL
#define ARM_SR_MODE_FIQ 0x00000011UL
#define ARM_SR_MODE_IRQ 0x00000012UL
#define ARM_SR_MODE_SVC 0x00000013UL
#define ARM_SR_MODE_ABT 0x00000017UL
#define ARM_SR_MODE_UND 0x0000001BUL
#define ARM_SR_MODE_SYS 0x0000001FUL

#define ARV_VECTOR_OFFT_RST 0x00000000UL
#define ARM_VECTOR_OFFT_UND 0x00000004UL
#define ARM_VECTOR_OFFT_SWI 0x00000008UL
#define ARM_VECTOR_OFFT_P_ABT 0x0000000CUL
#define ARM_VECTOR_OFFT_D_ABT 0x00000010UL
#define ARM_VECTOR_OFFT_UNUSED 0x00000014UL
#define ARM_VECTOR_OFFT_IRQ 0x00000018UL
#define ARM_VECTOR_OFFT_FIQ 0x0000001CUL

// the following are for cpuGetRegExternal() and are generally used for debugging purposes
#define ARM_REG_NUM_CPSR 16
#define ARM_REG_NUM_SPSR 17

#define SLOW_PATH_REASON_EVENTS 0x01
#define SLOW_PATH_REASON_SLEEP 0x02
#define SLOW_PATH_REASON_CP15 0x04
#define SLOW_PATH_REASON_PATCH_PENDING 0x08
#define SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE 0x10
#define SLOW_PATH_REASON_RESCHEDULE 0x20
#define SLOW_PATH_REASON_PACE_SYSCALL_BREAK 0x40
#define SLOW_PATH_REASON_INJECTED_CALL_DONE 0x80
#define SLOW_PATH_REASON_NATIVE_PC 0x100
#define SLOW_PATH_REASON_NATIVE_FAULT 0x200

#define ARM_MEMORY_SYSTEM_MMU 0
#define ARM_MEMORY_SYSTEM_MPU 1
// The interpreter is compiled once per kind, the template argument of cpuCycle and the decoders: the
// MMU and MPU kinds for the ARMv5TE cores and one for the ARM920T (with its MMU), so no copy checks the
// CPU model as it runs. Callers pick the kind once, from the CPU they made (cpuInterpreterKind).
#define ARM_MEMORY_SYSTEM_ARM920T 2
// The memory system an interpreter kind uses.
constexpr int armMemorySystemOf(int kind) { return kind == ARM_MEMORY_SYSTEM_ARM920T ? ARM_MEMORY_SYSTEM_MMU : kind; }
constexpr bool armKindIsArm920(int kind) { return kind == ARM_MEMORY_SYSTEM_ARM920T; }

typedef bool (*ArmCoprocRegXferF)(struct ArmCpu *cpu, void *userData, bool two /* MCR2/MRC2 ? */,
                                  bool MRC, uint8_t op1, uint8_t Rx, uint8_t CRn, uint8_t CRm,
                                  uint8_t op2);
typedef bool (*ArmCoprocDatProcF)(struct ArmCpu *cpu, void *userData, bool two /* CDP2 ? */,
                                  uint8_t op1, uint8_t CRd, uint8_t CRn, uint8_t CRm, uint8_t op2);
typedef bool (*ArmCoprocMemAccsF)(
    struct ArmCpu *cpu, void *userData, bool two /* LDC2/STC2 ? */, bool N, bool store, uint8_t CRd,
    uint8_t addrReg, uint32_t addBefore, uint32_t addAfter,
    uint8_t
        *option /* NULL if none */);  /// addBefore/addAfter are UNSCALED. spec syas *4, but WMMX
                                      /// has other ideas. exercise caution. writeback is ON YOU!
typedef bool (*ArmCoprocTwoRegF)(struct ArmCpu *cpu, void *userData, bool MRRC, uint8_t op,
                                 uint8_t Rd, uint8_t Rn, uint8_t CRm);

struct ArmMmu;
struct ArmMpu;
struct ArmMem;
struct PatchDispatch;
struct SystemState;

struct ArmCoprocessor {
    ArmCoprocRegXferF regXfer;
    ArmCoprocDatProcF dataProcessing;
    ArmCoprocMemAccsF memAccess;
    ArmCoprocTwoRegF twoRegF;
    void *userData;
};

class SoC;
bool cpuInspectRead(ArmCpu*,uint32_t,void*,uint32_t);
bool cpuInspectRamRange(ArmCpu*,uint32_t,uint32_t);
bool cpuInspectWritableRamRange(ArmCpu*,uint32_t,uint32_t,bool privileged = true);
void cpuSetDebugSoc(ArmCpu*,SoC*);
struct ArmCpu *cpuInit(uint32_t pc, struct ArmMem *mem, uint8_t memorySystemKind, bool xscale,
                       bool omap, int debugPort, uint32_t cpuid, uint32_t cacheId,
                       struct PatchDispatch *patchDispatch, struct PacePatch *pacePatch,
                       struct SystemState *systemState,
                       ArmCpuModel model = ArmCpuModel::armv5te);
// Diagnostic stepping checks the budget after each generated instruction.
struct ArmCpu *cpuInit(uint32_t pc, struct ArmMem *mem, uint8_t memorySystemKind, bool xscale,
                       bool omap, int debugPort, uint32_t cpuid, uint32_t cacheId,
                       struct PatchDispatch *patchDispatch, struct PacePatch *pacePatch,
                       struct SystemState *systemState, ArmCpuModel model, bool jitSingleStep);

using M68kSyscallHandler = bool (*)(void*, ArmCpu*, uint16_t);
void cpuSetM68kSyscallHandler(ArmCpu* cpu, M68kSyscallHandler handler, void* context);

uint32_t *cpuGetRegisters(struct ArmCpu *cpu);

void cpuStartInjectedSyscall(struct ArmCpu *cpu, uint32_t syscall);

void cpuStartInjectedSyscall68k(struct ArmCpu *cpu, uint16_t syscall);
void cpuFinishInjectedSyscall68k(struct ArmCpu *cpu);

void cpuReset(struct ArmCpu *cpu, uint32_t pc);

// Peeks the current ARM/Thumb instruction view without consuming a retained prefetch.
bool cpuFetchInstruction(ArmCpu*, uint32_t va, uint32_t* word, uint_fast8_t* fsr);
// Host code patches must discard decoded instructions as well as guest cache lines.
void cpuInvalidateCodeCache(struct ArmCpu *cpu);
// Drops the JIT's translated code only, when something it was translated with changes.
void cpuDropTranslatedCode(struct ArmCpu *cpu);
// Stores to this host RAM range go through the memory callbacks again until each page's
// next first write, which marks it dirty. For whoever clears dirty state (the frame buffer).
void cpuWriteProtectHost(struct ArmCpu *cpu, const uint8_t *host, uint32_t size);
// Engine for CPUs made from now on: Fast is the JIT where it's built in, Exact the interpreter.
// FROND_JIT=1 or 0 in the environment overrides it. Returns false if Fast isn't built in.
bool cpuSetFastEngine(bool fast);
// Fast engine's compiles for CPUs made from now on: sync waits for each where it was asked
// for, so runs repeat exactly (tests and replays); otherwise they run on a thread of their
// own. FROND_JIT_SYNC=1 or 0 overrides it.
void cpuSetJitSync(bool sync);
// Exception returns (MOVS PC, LR and SUBS PC, LR, #4) run in generated code, in every CPU so far (tests).
uint64_t cpuJitExceptionReturns();
// MSR CPSR writes generated code hands to the interpreter's code, in every CPU so far (tests).
uint64_t cpuJitCpsrWrites();
// MSR CPSRs that couldn't run in place and went to the interpreter, in every CPU so far (tests).
uint64_t cpuJitCpsrInterprets();
// Fast idle skip of the PalmOS 5 kernel wait loop (idle_loops.h), on unless FROND_IDLE_SKIP=0.
// Code compiled while it was off keeps running the loop.
void cpuSetIdleSkip(bool on);
// Skips so far and the cycles they passed, in every CPU (tests).
void cpuJitIdleSkips(uint64_t *skips, uint64_t *cycles);
// Fast skip of the ROMs' OSCR delay loops (delay_loops.h), on unless FROND_DELAY_SKIP=0. Code compiled
// while it was off keeps running the loops.
void cpuSetDelaySkip(bool on);
// Skips so far and the cycles they passed, in every CPU (tests).
void cpuJitDelaySkips(uint64_t *skips, uint64_t *cycles);
// The OS timer the delay skip reads: OSCR's physical address, and peek, which gives what a load of it
// would read with that many cycles in progress (cpuCyclesInProgress) without changing anything, and
// returns false when that load would reach a timer match. Without it there is no delay skip.
void cpuSetDelayTimer(ArmCpu *cpu, uint32_t oscrPa, bool (*peek)(void *, uint32_t, uint32_t *), void *context);
// Fast skip of the ROMs' GPIO level polls (poll_loops.h), on unless FROND_POLL_SKIP=0. Code compiled
// while it was off keeps running the loops.
void cpuSetPollSkip(bool on);
// Skips so far and the cycles they passed, in every CPU (tests).
void cpuJitPollSkips(uint64_t *skips, uint64_t *cycles);
// What the poll skip reads: peek gives the word a load of physical address pa would read, without
// changing anything, and returns true only for registers that change only at scheduler events (between
// cpuCycle runs). Without it there is no poll skip.
void cpuSetPollPeek(ArmCpu *cpu, bool (*peek)(void *, uint32_t, uint32_t *), void *context);
// MRC and MCR p15 c3 (domain access control) run in generated code, in every CPU so far (tests).
uint64_t cpuJitDomainCalls();
// Translated ordinary blocks and instructions executed in generated code.
bool cpuJitExecutionCounts(const ArmCpu* cpu, uint64_t* blocks, uint64_t* instructions);
// Fast engine's compile thread counts for this CPU (tests): blocks dropped as stale, and
// blocks compiled ahead and those of them used. False without the Fast engine.
bool cpuJitCompileCounts(const struct ArmCpu *cpu, uint64_t *stale, uint64_t *aheadCompiled, uint64_t *aheadUsed);
// Fast engine's code cache size in bytes for CPUs made from now on (tests); 0 for the default.
void cpuSetJitCodeCacheSize(size_t bytes);
// Fast engine's whole code cache clears for this CPU (tests). False without the Fast engine.
bool cpuJitCacheClears(const struct ArmCpu *cpu, uint64_t *clears);
// Most cycles one cpuCycle call can return past the cycles it was asked for: 19 for the
// interpreter (PACE charges 20 per operation), more for the Fast engine. UINT32_MAX if
// generated code runs unchecked (FROND_JIT_MAX_UNCHECKED=0).
uint32_t cpuMaxOvershootCycles(const struct ArmCpu *cpu);
// Fast: whether the cost model charges a table, and its cost::Core in core.
bool cpuJitCostCore(const struct ArmCpu *cpu, uint8_t *core);
// Cost model (xscale_cost.h): on charges core's (cost::Core) table per instruction and a PACE op, memory
// adds the region and data cache charges (XScale only). Off: 1 per instruction, 20 per PACE op.
void cpuSetCostModel(struct ArmCpu *cpu, bool on, uint8_t core, bool memory);
// Fast: the data cache and region charges too (Original speed). Off, Fast charges the table only (Uncapped).
// Exact always charges them.
void cpuSetJitMemoryCost(struct ArmCpu *cpu, bool on);
// Whether loads and stores are charged now (Fast: its callbacks take every access then).
bool cpuJitMemoryCost(const struct ArmCpu *cpu);
// Memory charges since the batch's count was last brought up to date.
uint32_t cpuJitPendingCharge(const struct ArmCpu *cpu);
// Fast's data cache hint (A32::UserConfig::frond_cost_hint), at a fixed address for the CPU's life.
const uint32_t *cpuJitCostHint(struct ArmCpu *cpu);
// Before a JIT run with memory charges: the run's budget, for the hint's write buffer check (joins: the
// run counts whole clocks).
void cpuJitCostRunStart(struct ArmCpu *cpu, uint32_t budget, bool joins);
// The run is ending for its memory charges (at the next link).
void cpuJitCostHalted(struct ArmCpu *cpu);
// Empties the hint (a TLB flush, the page table cleared or the control register's C bit changed).
void cpuCostHintClear(struct ArmCpu *cpu);
// The clocks the region and data cache charges are worked out from, in kHz.
void cpuSetCostClocks(struct ArmCpu *cpu, uint32_t coreKHz, uint32_t busKHz, uint32_t memoryKHz);
// The SDRAM bus width the data cache charges use: 16 bits (MDCNFG DWID0 set) or 32.
void cpuSetCostSdramWidth(struct ArmCpu *cpu, bool sdram16);
// A CP15 c7 write (crm, op2, the register's value) for the data cache model.
void cpuCostCacheOp(struct ArmCpu *cpu, uint32_t crm, uint32_t op2, uint32_t va);
// Host checkpoint repair preserves the separately serialized XScale fetch buffers.
// Caller provides ICACHE_FETCH_STATE_WORDS words (see icache.h).
void cpuGetInstructionFetchState(ArmCpu*, uint32_t*);
bool cpuRestoreInstructionFetchState(ArmCpu*, const uint32_t*);

#ifndef __EMSCRIPTEN__
uint32_t cpuCyclesInProgress(const ArmCpu* cpu);
#endif

// Take one unmasked interrupt at this boundary, without fetching or executing an instruction.
bool cpuProcessPendingInterrupts(struct ArmCpu* cpu);
void cpuArm920EnableWait(ArmCpu*);
bool cpuArm920BeginWait(ArmCpu*);
bool cpuArm920Waiting(const ArmCpu*);
void cpuArm920EndWait(ArmCpu*);
void cpuIrq(struct ArmCpu *cpu, bool fiq, bool raise);  // unraise when acknowledged

uint32_t cpuGetRegExternal(struct ArmCpu *cpu, uint_fast8_t reg);
void cpuSetReg(struct ArmCpu *cpu, uint_fast8_t reg, uint32_t val);
bool cpuMemOpExternal(struct ArmCpu *cpu, void *buf, uint32_t vaddr, uint_fast8_t size,
                      bool write);  // for external use
// A coprocessor load or store's access (1, 2 or 4 bytes), checked for the current mode.
// False with *fsr set on a fault; cpuDataAbortExternal then logs it and takes the data abort
// for the instruction at curInstrPC as an ordinary load or store does.
bool cpuMemOpGuest(struct ArmCpu *cpu, void *buf, uint32_t vaddr, uint_fast8_t size, bool write,
                   uint_fast8_t *fsr);
void cpuDataAbortExternal(struct ArmCpu *cpu, uint32_t vaddr, uint_fast8_t size, bool write, uint_fast8_t fsr);

void cpuCoprocessorRegister(struct ArmCpu *cpu, uint8_t cpNum, struct ArmCoprocessor *coproc);

void cpuSetVectorAddr(struct ArmCpu *cpu, uint32_t adr);
void cpuSetPid(struct ArmCpu *cpu, uint32_t pid);
uint32_t cpuGetPid(struct ArmCpu *cpu);
// How many undefined instruction exceptions the CPU has taken, with the last one's pc and instruction.
uint64_t cpuUndefinedTaken(const struct ArmCpu *cpu, uint32_t *pc, uint32_t *instr);
// Calls hook with the pc and instruction of every undefined instruction exception as it's taken
// (instruction 0 for a data cache guard trap). Null turns it off.
void cpuSetUndefinedHook(struct ArmCpu *cpu, void (*hook)(void *context, uint32_t pc, uint32_t instr), void *context);
// For guests that take data aborts as part of normal running (Windows CE's demand paging): log the first 16
// failed data accesses and then every power of two.
void cpuSetAbortLogLimit(struct ArmCpu *cpu, bool limited);

uint16_t cpuGetCPAR(struct ArmCpu *cpu);
void cpuSetCPAR(struct ArmCpu *cpu, uint16_t cpar);
// The iWMMXt unit on coprocessors 0 and 1 (PXA27x), for the Fast engine.
struct Pxa270wmmx;
void cpuSetWmmx(struct ArmCpu *cpu, struct Pxa270wmmx *wmmx);

void cpuSetSleeping(struct ArmCpu *cpu);
void cpuWakeup(struct ArmCpu *cpu);

void cpuSetSlowPath(struct ArmCpu *cpu, uint32_t reason);
void cpuClearSlowPath(struct ArmCpu *cpu, uint32_t reason);
uint32_t cpuGetSlowPathReason(struct ArmCpu *cpu);

void cpuSetBreakPaceSyscall(struct ArmCpu *cpu, uint16_t syscall);

struct ArmMmu *cpuGetMMU(struct ArmCpu *cpu);
struct ArmMpu *cpuGetMPU(struct ArmCpu *cpu);
struct ArmMem *cpuGetMem(struct ArmCpu *cpu);

uint32_t cpuGetMemorySystemKind(struct ArmCpu *cpu);
// The interpreter kind for this CPU: its memory system, or ARM_MEMORY_SYSTEM_ARM920T.
int cpuInterpreterKind(const struct ArmCpu *cpu);

template <int memorySystemKind>
uint32_t cpuDecodeArm(uint32_t instr);

template <int memorySystemKind>
uint32_t cpuDecodeThumb(uint16_t instr, uint32_t &translatedInstr);

template <int memorySystemKind, bool monitorPC>
uint32_t cpuCycle(struct ArmCpu *cpu, uint32_t cycles);
// cpuCycle (not injected) in the copy cpuInterpreterKind picks, for tests and tools that switch models.
uint32_t cpuCycleAnyKind(struct ArmCpu *cpu, uint32_t cycles);

uint32_t cpuCyclePure(struct ArmCpu *cpu);
#ifndef __EMSCRIPTEN__
// Tagged addresses: bit 0 selects Thumb. Hooks run only during injected execution.
using NativeCallback = void (*)(ArmCpu*, void*);
bool cpuRegisterNativeCallback(ArmCpu*, uint32_t address, NativeCallback, void* context);
void cpuUnregisterNativeCallback(ArmCpu*, uint32_t address);
void cpuClearNativeRuntime(ArmCpu*);
bool cpuAcquireNativeSession(ArmCpu*);
bool cpuSetNativePcBreak(ArmCpu*, uint32_t first, uint32_t second);
bool cpuStartNativeCall(ArmCpu*, uint32_t entry);
void cpuEndNativeCall(ArmCpu*);
void cpuSetNativeStackBounds(ArmCpu*, uint32_t lower, uint32_t upper,
                             uint32_t ownerAddress = 0, uint32_t ownerValue = 0);
// Side-effect-free RAM/ROM access: rejects MMIO, wrap and invalid mappings.
bool cpuNativeBuffer(ArmCpu*, uint32_t address, void* data, uint32_t size, bool write,
                     bool privileged = true);
bool cpuNativeRamRange(ArmCpu*, uint32_t address, uint32_t size);
// Temporary host-owned bootstrap stack, accepted only over unmapped virtual pages.
bool cpuSetNativeScratch(ArmCpu*, uint32_t base, void* data, uint32_t size);
#endif


using M68kTrapHandler = std::function<bool(struct ArmCpu *, uint32_t address)>;
void cpuAddM68kTrap0Handler(struct ArmCpu *cpu, uint32_t address, M68kTrapHandler handler);
void cpuRemoveM68kTrap0Handler(struct ArmCpu *cpu, uint32_t address);

bool cpuIsModePace(struct ArmCpu *cpu);
void cpuSetModePace(struct ArmCpu *cpu, bool modePace);

bool cpuIsThumb(struct ArmCpu *cpu);

bool cpuHasPendingInterrupt(struct ArmCpu *cpu);

uint32_t cpuGetCurInstrPC(struct ArmCpu *cpu);
void cpuSetCurInstrPC(struct ArmCpu *cpu, uint32_t curInstrPC);

template <typename T>
void cpuSave(struct ArmCpu *cpu, T &savestate);

template <typename T>
void cpuLoad(struct ArmCpu *cpu, T &loader);

template <typename T>
void cpuPrepareInjectedCall(struct ArmCpu *cpu, T &savestate);

template <typename T>
void cpuFinishInjectedCall(struct ArmCpu *cpu, T &loader);


// Diagnostic cache/buffer guards do not report an architectural external abort.
void cpuArm920SetIcacheIndexCompatibility(ArmCpu*, bool);
void cpuArm920SetMemoryCheck(ArmCpu*, Arm920MemoryCheck, void*);
Arm920DataGuard cpuArm920DataGuard(ArmCpu*);
// Machine state between two run calls. Capture fails outside ARM920T, in PACE, asleep or
// inside a native call. Apply takes a state that cpuArm920StateFits accepted, and leaves
// translated code alone: drop it with cpuDropTranslatedCode.
struct Arm920SavedState;
bool cpuArm920CaptureState(const ArmCpu*, Arm920SavedState&);
bool cpuArm920StateFits(const ArmCpu*, const Arm920SavedState&);
void cpuArm920ApplyState(ArmCpu*, const Arm920SavedState&);
// The ARM920T Fast engine: the interpreter without cache contents, over host pages. page(context, pa, write,
// counted) gives the host address of the 4 KiB page at pa when every access to it is plain memory, else
// null, and sets counted when the guarded step would count a data access to it as a bus access (RAM, not
// ROM). guard is the probe's instruction check, for the forms it decides from registers. Exact never calls
// any of these. In Fast the guarded step's instruction fetches also go through the lean decode cache, so
// the ARM920T I-cache model keeps no lines.
using Arm920LeanPage = uint8_t* (*)(void* context, uint32_t pa, bool write, bool* counted);
using Arm920LeanGuard = bool (*)(uint32_t instruction, const uint32_t* registers, uint32_t cpsr, uint32_t spsr);
void cpuArm920SetLean(ArmCpu*, Arm920LeanPage page, Arm920LeanGuard guard, void* context);
// CP15 c1 A (alignment fault checking) on an ARM920T: misaligned word and halfword accesses take a data abort.
bool cpuArm920AlignmentFaults(const ArmCpu*);
// read(context, pa, size, pc, ran, left, single, value, cost) may answer a read of up to a word from a page that
// isn't plain without the step, pc being the instruction's, ran the instructions the lean run finished before this
// one and single whether the read is the instruction's only data access (a single load). left and cost are as for
// write below. False sends it to the step.
using Arm920LeanRead = bool (*)(void* context, uint32_t pa, uint32_t size, uint32_t pc, uint32_t ran, uint32_t left,
                                bool single, uint32_t* value, uint32_t* cost);
void cpuArm920SetLeanRead(ArmCpu*, Arm920LeanRead read);
// write(context, pa, size, value, ran, left, cost) may take a store of a page that isn't plain, for an
// instruction whose only data access it is (STR, STRB, STRH). left is what the run's budget holds after this
// instruction; cost is what the write takes from it on top. False sends the instruction to the step.
using Arm920LeanWrite = bool (*)(void* context, uint32_t pa, uint32_t size, uint32_t value, uint32_t ran,
                                 uint32_t left, uint32_t* cost);
void cpuArm920SetLeanWrite(ArmCpu*, Arm920LeanWrite write);
// From a read or write hook: whether translated code made the access (ran then counts the JIT's clocks, a
// Thumb BL pair as one) rather than the lean path (ran counts instructions).
bool cpuArm920LeanInJit(const ArmCpu*);
// Cleans dirty D-cache lines to memory, empties both caches and drops the lean TLB and decoded code. For
// entering Fast and after a load into it.
void cpuArm920LeanReset(ArmCpu*);
// Drops the lean TLB, after anything that can change which pages are plain.
void cpuArm920LeanFlush(ArmCpu*);
// A write over the whole RAM page at pa (DMA). Fast drops its decoded and translated code when the page
// holds some.
void cpuArm920LeanStored(ArmCpu*, uint32_t pa);
// A store the guarded step made to RAM at [pa, pa + size): as cpuArm920LeanStored, only when one of the
// 64-byte lines it touches held code.
void cpuArm920LeanStoredBytes(ArmCpu*, uint32_t pa, uint32_t size);
// Runs the lean path while its cost model clocks are under budget (an instruction that would pass it is left
// for the step) and returns the instructions it retired, with their clocks in clocks. It stops before an
// instruction or access it can't take as plain (MMIO, faults, misaligned accesses, CP15, SWI, MSR of the
// control field, exception returns and forms the guard rejects), leaving the CPU at that instruction for
// the guarded step. blNext is the Thumb BL suffix address after a prefix that ended the run, else ~0.
// busAccesses gets the data accesses the step would have counted as bus accesses.
uint32_t cpuArm920RunLean(ArmCpu*, uint32_t budget, uint32_t* blNext, uint64_t* busAccesses,
                          uint32_t* clocks = nullptr);
// The same, retiring at most count instructions.
uint32_t cpuArm920RunLeanFor(ArmCpu*, uint32_t budget, uint32_t count, uint32_t* blNext, uint64_t* busAccesses,
                             uint32_t* clocks = nullptr);
// ARM920T Fast blocks: Dynarmic in normal blocks over the lean pages, run only through cpuArm920RunJit. On
// needs cpuArm920SetLean first and makes the JIT on first use; false when there is no JIT in the build or the
// CPU has the single-step diagnostic JIT. Turning it on again drops translated code.
bool cpuArm920SetJit(ArmCpu*, bool on);
bool cpuArm920JitOn(const ArmCpu*);
// Frees the Fast blocks JIT and its compile thread: for a CPU that won't run again, or before a fork(), after
// which cpuArm920SetJit makes a new one.
void cpuArm920DropJit(ArmCpu*);
// Most instructions a JIT run can pass its budget by, plus one.
uint32_t cpuArm920JitMaxUnchecked(const ArmCpu*);
// Fast blocks' FCSE PID tags in use and the restarts so far (jitPidTags), 0 without the JIT.
unsigned cpuArm920JitPidTags(const ArmCpu*, uint64_t* restarts);
// Fast blocks, from a lean write hook: the pc of the marked access-pc instruction making the access (0 if none).
uint32_t cpuArm920TakeJitAccessPc(ArmCpu*);
// Fast blocks, from an access hook: the board charged cycles of the run (the hook's ran) before the access, and
// span is the quiet span from there. Later hooks get ran and left from that point. cpuArm920JitCharged is the
// run's total, for its end.
void cpuArm920JitCharge(ArmCpu*, uint32_t cycles, uint32_t span);
uint32_t cpuArm920JitCharged(const ArmCpu*);
// Runs about budget cost model clocks in translated code and returns the clocks run, at most budget - 1 +
// cpuArm920JitMaxUnchecked x the largest instruction cost, with the instructions in instructions (a Thumb
// pair counts 2). stop gets arm920JitBudget when the budget ran out (or translated code was
// dropped), arm920JitExit when the CPU is left at an instruction or access the JIT can't take (MMIO, CS5,
// faults, CP15...) for the lean path or the step, arm920JitCold at code not compiled yet, arm920JitHalt for a
// slow path reason (an interrupt). The JIT compiles as on the other devices (cpuSetJitSync for runs that
// repeat). The leanRead and leanWrite hooks get left from span (cycles the run's accesses can take with the
// instructions, budget when 0) instead of budget, which may count instructions only.
enum Arm920JitStop { arm920JitBudget, arm920JitExit, arm920JitCold, arm920JitHalt };
uint32_t cpuArm920RunJit(ArmCpu*, uint32_t budget, int* stop, uint32_t span = 0, uint32_t* instructions = nullptr);
// Whether cpuArm920RunJit would run translated code now: false with the JIT off, an interrupt or other slow path
// reason for the step, a wait for interrupt, mode pacing, a native call, the debug stub or an I-cache continuation.
bool cpuArm920JitCanRun(const ArmCpu*);
// The same checks without the JIT: whether the next instructions run as plain steps, with no slow path reason.
bool cpuArm920StepSkipAllowed(const ArmCpu*);
// The lean TLB's generation and the times Fast dropped its decoded code for a store to it, for tests.
uint32_t cpuArm920LeanGeneration(const ArmCpu*);
uint64_t cpuArm920LeanCodeDrops(const ArmCpu*);
// Fast blocks: generated code notes the pc of these instructions for the lean read callback's pc, which the
// graphics model keys some register reads on. The list stays the caller's and is set before the JIT compiles.
void cpuArm920SetJitAccessPcs(ArmCpu*, const uint32_t* pcs, uint32_t count);
// Fast: code hooks, pcs (bit 0 ignored) the lean path and translated code never run, so the guarded step runs
// every instruction at one of them and its caller can act there first. Setting replaces the ones before and drops
// translated code when they change. False on another model or for more than ARM920_MAX_CODE_HOOKS. None set
// runs as before.
#define ARM920_MAX_CODE_HOOKS 16
bool cpuArm920SetCodeHooks(ArmCpu*, const uint32_t* pcs, uint32_t count);
bool cpuArm920CodeHookAt(const ArmCpu*, uint32_t pc);
// Fast: the physical address of va as a load in the current mode would see it, and with word set the word there
// when it is on a plain page (cpuArm920SetLean). False when the MMU refuses it (translation, domain or permission
// fault in this mode) or the page isn't plain.
bool cpuArm920PeekWord(ArmCpu*, uint32_t va, uint32_t* word, uint32_t* pa);
// Sets the Fast engine's counters. False without a JIT.
bool cpuJitSetExecutionCounts(ArmCpu*, uint64_t blocks, uint64_t instructions);

#endif
