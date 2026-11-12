//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include <utility>
#include "CPU.h"
#include "arm920_state.h"
#include "cpu_diagnostics.h"
#ifdef FROND_DYNARMIC
#include "jit_dynarmic.h"
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
    // One bit per 4 KiB RAM page the decode cache holds code from (a 4 GiB physical map), and the data
    // accesses this lean run made that the step would count as bus accesses.
    uint64_t* leanCode;
    uint64_t leanBus, leanCodeDrops;
    // Instructions this lean run has finished, and the probe's answer for reads of pages that aren't plain.
    uint32_t leanDone;
    Arm920LeanRead leanRead;
    // The probe's answer for single stores to pages that aren't plain, the instruction the lean loop is running
    // and the run's budget, which such a store can shrink.
    Arm920LeanWrite leanWrite;
    uint32_t leanInstr, leanBudget;
    // Fast blocks: the pc of the last access-pc instruction generated code ran (cpuArm920SetJitAccessPcs), taken by
    // the lean read callback for the access it makes, then cleared.
    uint32_t jitAccessPc;
    const uint32_t *jitAccessPcs;
    uint32_t jitAccessPcCount;
    // Most instructions this lean run may retire (cpuArm920RunLeanFor), and whether the JIT's callbacks are
    // using the lean memory path (no read or write answers from the probe there).
    uint32_t leanCount;
    bool leanJit;
#ifdef FROND_DYNARMIC
    struct FrondJit *jit;
    // cpuJitMemoryOnly: code fetches outside RAM and ROM fail, and are noted.
    bool jitMemoryOnly, jitRefused;
    // ARM920T Fast blocks (cpuArm920SetJit): the JIT is run only by cpuArm920RunJit, never by cpuCycle
    // (jitLoopOnly), and while jitBlocks is on it reads code and data from the lean pages. jitCode has one
    // bit per 4 KiB RAM page it has translated code from, as leanCode.
    bool jitLoopOnly, jitBlocks;
    uint64_t *jitCode;
#endif

    template <typename T>
    void DoSaveLoad(T &chunkHelper, uint32_t version);
};

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

static FORCE_INLINE void cpuPrvSetPC(struct ArmCpu *cpu, uint32_t pc)  // with interworking
{
    if (cpu->model == ArmCpuModel::arm920t) icacheArm920BranchInline(cpu->ic);
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

static FORCE_INLINE void cpuPrvSetReg(struct ArmCpu *cpu, uint_fast8_t reg, uint32_t val) {
    if (reg == REG_NO_PC) {
        if (cpu->model == ArmCpuModel::arm920t) val = (val & (cpu->T ? ~1u : ~3u)) | uint32_t(cpu->T);
        cpuPrvSetPC(cpu, val);
    } else
        cpuPrvSetRegNotPC(cpu, reg, val);
}

template <bool pc>
static FORCE_INLINE void cpuPrvSetReg(struct ArmCpu *cpu, uint_fast8_t reg, uint32_t val) {
    if constexpr (pc) {
        if (cpu->model == ArmCpuModel::arm920t) val = (val & (cpu->T ? ~1u : ~3u)) | uint32_t(cpu->T);
        cpuPrvSetPC(cpu, val);
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

static void cpuUpdateSlowPath(struct ArmCpu *cpu) {
    // The ARM920T stops for an interrupt it can take; the other cores for any event while F
    // is clear, as they always have, so their timing stays.
    if (cpu->model == ArmCpuModel::arm920t ? (cpu->waitingFiqs && !cpu->F) || (cpu->waitingIrqs && !cpu->I)
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

static FORCE_INLINE void cpuPrvSetPSRlo8(struct ArmCpu *cpu, uint_fast8_t val) {
    const bool wasT = cpu->T;

    cpuPrvSwitchToMode(cpu, val & ARM_SR_M);
    cpu->T = !!(val & ARM_SR_T);
    cpu->F = !!(val & ARM_SR_F);
    cpu->I = !!(val & ARM_SR_I);

    cpuUpdateSlowPath(cpu);
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

static void cpuPrvException(struct ArmCpu *cpu, uint32_t vector_pc, uint32_t lr,
                            uint_fast8_t newLowBits)  // enters arm mode
{
    ++cpu->exceptionSequence;
    if (cpu->model == ArmCpuModel::arm920t) icacheArm920BranchInline(cpu->ic);
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
    cpuPrvSetPSRlo8(cpu, cpu->model == ArmCpuModel::arm920t ? newLowBits | (spsr & ARM_SR_F) : newLowBits);
    cpu->SPSR = spsr;
    cpu->regs[REG_NO_LR] = lr;
    cpu->regs[REG_NO_PC] = vector_pc;
}

// input addr is VA not MVA
static void cpuPrvHandleMemErr(struct ArmCpu *cpu, uint32_t addr, bool write, bool instrFetch,
                               uint_fast8_t fsr) {
    // A lean access that stops the lean run takes no abort: the guarded step runs the instruction again.
    if (unlikely(cpu->leanExit)) return;
    if (cpu->model == ArmCpuModel::arm920t &&
        cp15MMUArm920DataCache(cpu->cp15.cp15mmu)->guard.active) {
        // An unmodelled cache/buffer target is a diagnostic trap, with FSR/FAR retained.
        cpuPrvException(cpu, cpu->vectorBase + ARM_VECTOR_OFFT_UND, cpu->curInstrPC + (cpu->T ? 2 : 4),
                        ARM_SR_MODE_UND | ARM_SR_I);
        return;
    }
// FCSE
#ifdef SUPPORT_FCSE
    if (addr < 0x02000000UL)  // report addr is MVA
        addr |= cpu->pid;
#endif

    if (cpu->memorySystemKind == ARM_MEMORY_SYSTEM_MMU &&
        !(instrFetch && cpu->model == ArmCpuModel::arm920t)) {
        cp15MMUSetFaultStatus(cpu->cp15.cp15mmu, addr, fsr);
    }

    if (instrFetch) {
        // handle prefetch abort (LR is addr of aborted instr + 4)
        cpuPrvException(cpu, cpu->vectorBase + ARM_VECTOR_OFFT_P_ABT, cpu->curInstrPC + 4,
                        ARM_SR_MODE_ABT | ARM_SR_I);
    } else {
        // handle data abort (LR is addr of aborted instr + 8)
        cpuPrvException(cpu, cpu->vectorBase + ARM_VECTOR_OFFT_D_ABT, cpu->curInstrPC + 8,
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

template <bool spsr>
static FORCE_INLINE void cpuPrvSetPSR(struct ArmCpu *cpu, uint_fast8_t mask, uint32_t val) {
    if constexpr (spsr)  // setting SPSR in sys or usr mode is no harm since they arent used, so
                         // just do it without any checks
        cpu->SPSR = val;
    else {
        if (cpu->privileged && (mask & 1)) cpuPrvSetPSRlo8(cpu, val);

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
                                            bool privileged) {
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
    }
}
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

template <int size, bool write>
static __attribute__((noinline)) bool cpuPrvLeanMemOpSlow(struct ArmCpu *cpu, void *buf, uint32_t va,
                                                         bool privileged) {
    if (va & (size - 1)) {
        cpu->leanExit = true;
        return false;
    }
    struct ArmMmu *mmu = cpu->memorySystem.mmu;
    const MMUTranslateResult r = mmuTranslateCached<write>(mmu, va, privileged);
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
            uint32_t ran = cpu->leanDone;
#ifdef FROND_DYNARMIC
            if (cpu->leanJit) ran = jitRanNow(cpu->jit);
#endif
            const uint32_t left = ran + 1 < cpu->leanBudget ? cpu->leanBudget - ran - 1 : 0;
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
            uint32_t ran = cpu->leanDone;
#ifdef FROND_DYNARMIC
            if (cpu->leanJit) ran = jitRanNow(cpu->jit);
#endif
            const uint32_t left = ran + 1 < cpu->leanBudget ? cpu->leanBudget - ran - 1 : 0;
            if (cpu->leanWrite(cpu->leanContext, MMU_TRANSLATE_RESULT_PA(r), size, value, ran, left, &cost)) {
                cpu->leanBudget -= cost;
                ++cpu->leanBus;
                return true;
            }
        }
        cpu->leanExit = true;
        return false;
    }
    if constexpr (write) cpuArm920LeanStored(cpu, MMU_TRANSLATE_RESULT_PA(r));
    uint8_t *host = page + (MMU_TRANSLATE_RESULT_PA(r) & 0xfffu);
    if constexpr (write) memcpy(host, buf, size); else memcpy(buf, host, size);
    cpu->leanBus += counted;
    // Bit 0 of the addend marks a counted page; host pages are at least 2-byte aligned.
    if ((mmu->transTablPA == MMU_DISABLED_TTP || mmu->tlb[va >> 12].revision == mmu->revision) &&
        !(uintptr_t(page) & 1)) {
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

// arm920 is cpu->model == ArmCpuModel::arm920t, decided once per access by cpuPrvMemOpEx.
template <int memorySystemKind, int size, bool write, bool arm920>
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
    if constexpr (arm920 && memorySystemKind == ARM_MEMORY_SYSTEM_MMU && size <= 4) {
        if (unlikely(cpu->leanRun)) return cpuPrvLeanMemOp<size, write>(cpu, buf, vaddr, priviledged);
    }
#endif

    if constexpr (size > 1) {
        if (vaddr & (size - 1)) {
            if constexpr (memorySystemKind == ARM_MEMORY_SYSTEM_MMU && size == 4) {
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

// FCSE
#ifdef SUPPORT_FCSE
    if (vaddr < 0x02000000UL) vaddr |= cpu->pid;
#endif

#ifndef __EMSCRIPTEN__
    if constexpr (memorySystemKind == ARM_MEMORY_SYSTEM_MMU && size <= 8) {
      if constexpr (!arm920) {
        struct HostTlb &hostTlb = cpu->mem->hostTlb;
        if constexpr (write) {
            const struct HostTlbEntry &e = hostTlb.write[(vaddr >> 12) & (HOST_TLB_SIZE - 1)];
            if (likely(e.key == hostTlbKey(hostTlb.gen, vaddr, priviledged)))
                return ramAccessInline<size, true>(hostTlb.ram, vaddr + uint32_t(e.addend), buf);
        } else {
            const struct HostTlbEntry &e = hostTlb.read[(vaddr >> 12) & (HOST_TLB_SIZE - 1)];
            if (likely(e.key == hostTlbKey(hostTlb.gen, vaddr, priviledged))) {
                cpuPrvHostRead<size>((const uint8_t *)(e.addend + vaddr), buf);
                return true;
            }
        }
      }
    }
#endif

    uint32_t pa;
    if constexpr (memorySystemKind == ARM_MEMORY_SYSTEM_MMU) {
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
    if constexpr (memorySystemKind == ARM_MEMORY_SYSTEM_MMU && size <= 8 && !arm920)
        cpuPrvHostTlbFill<write>(cpu, vaddr, pa, priviledged);
#endif

    return true;
}

// The ARM920T's lean run, data cache model and missing host TLB stay out of the other cores' path
// behind one model check.
template <int memorySystemKind, int size, bool write>
static FORCE_INLINE bool cpuPrvMemOpEx(struct ArmCpu *cpu, void *buf, uint32_t vaddr,
                                       bool priviledged, uint_fast8_t *fsrP) {
    if constexpr (memorySystemKind == ARM_MEMORY_SYSTEM_MMU) {
        if (unlikely(cpu->model == ArmCpuModel::arm920t))
            return cpuPrvMemOpExModel<memorySystemKind, size, write, true>(cpu, buf, vaddr, priviledged, fsrP);
    }
    return cpuPrvMemOpExModel<memorySystemKind, size, write, false>(cpu, buf, vaddr, priviledged, fsrP);
}

template <int size, bool write>
static FORCE_INLINE bool cpuPrvMemOpExGeneric(struct ArmCpu *cpu, void *buf, uint32_t vaddr,
                                              bool priviledged, uint_fast8_t *fsrP) {
    if (cpu->memorySystemKind == ARM_MEMORY_SYSTEM_MMU) {
        return cpuPrvMemOpEx<ARM_MEMORY_SYSTEM_MMU, size, write>(cpu, buf, vaddr, priviledged,
                                                                 fsrP);
    } else {
        return cpuPrvMemOpEx<ARM_MEMORY_SYSTEM_MPU, size, write>(cpu, buf, vaddr, priviledged,
                                                                 fsrP);
    }
}

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

    fprintf(stderr, "%c of %u bytes to 0x%08lx failed (pc=0x%08x, fsr=%u)!\n",
            (int)(write ? 'W' : 'R'), (unsigned)size, (unsigned long)vaddr,
            cpu->curInstrPC, fsrP ? unsigned(*fsrP) : 0);

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

    fprintf(stderr, "%c of %u bytes to 0x%08lx failed (pc=0x%08x, fsr=%u)!\n",
            (int)(write ? 'W' : 'R'), (unsigned)size, (unsigned long)vaddr,
            cpu->curInstrPC, fsrP ? unsigned(*fsrP) : 0);

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

uint32_t cpuMaxOvershootCycles(const struct ArmCpu *cpu) {
    constexpr uint32_t interpreter = 19;
#ifdef FROND_DYNARMIC
    if (cpu && cpu->jit) {
        // cpuPrvCycleJit: a run passes its budget by at most L - 1 (every link checks, no
        // block is longer than L), then a run that stopped for the interpreter runs one
        // more instruction before the budget check. PACE batches stay at 19.
        const uint32_t l = jitMaxUnchecked(cpu->jit);
        if (!l) return UINT32_MAX;
        return l > interpreter ? l : interpreter;
    }
#else
    (void)cpu;
#endif
    return interpreter;
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
    fprintf(stderr, "%c of %u bytes to 0x%08lx failed (pc=0x%08x, fsr=%u)!\n", (int)(write ? 'W' : 'R'), size,
            (unsigned long)vaddr, cpu->curInstrPC, unsigned(fsr));
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

template <int memorySystemKind, bool wasT>
static void execFn_noop(struct ArmCpu *cpu, uint32_t instr) {
    if (cpu->model == ArmCpuModel::arm920t && (instr >> 28) == 15)
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
            return cpuPrvHandleMemErr(cpu, cpu->regs[REG_NO_SP], true, false, fsr);
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
        return cpuPrvHandleMemErr(cpu, cpu->regs[REG_NO_SP], false, false, fsr);

    // restore r9 / dispatch table pointer from stack
    if (!cpuPrvMemOp<memorySystemKind, 4, false>(cpu, &cpu->regs[9], cpu->regs[REG_NO_SP] + 24,
                                                 cpu->privileged, &fsr))
        return cpuPrvHandleMemErr(cpu, cpu->regs[REG_NO_SP] + 24, false, false, fsr);

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
        return cpuPrvHandleMemErr(cpu, cpu->regs[REG_NO_SP], false, false, fsr);

    const struct {
        uint32_t offset, value;
    } stores[] = {{0x44, cpu->regs[10] - 4}, {0x48, cpu->regs[4]}, {0, cpu->regs[8]}};
    for (auto store : stores) {
        if (!cpuPrvMemOp<memorySystemKind, 4, true>(cpu, &store.value, state + store.offset,
                                                    cpu->privileged, &fsr))
            return cpuPrvHandleMemErr(cpu, state + store.offset, true, false, fsr);
    }

    // r9 is the caller's again, as native PACE callouts expect.
    if (!cpuPrvMemOp<memorySystemKind, 4, false>(cpu, &cpu->regs[9], cpu->regs[REG_NO_SP] + 24,
                                                 cpu->privileged, &fsr))
        return cpuPrvHandleMemErr(cpu, cpu->regs[REG_NO_SP] + 24, false, false, fsr);

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

static void execFn_peephole_ADC_udiv10(struct ArmCpu *cpu, uint32_t instr) {
    const uint32_t num = cpuPrvGetRegNotPC(cpu, 0);

    cpuPrvSetRegNotPC(cpu, 0, num / 10ul);
    cpuPrvSetRegNotPC(cpu, 1, num % 10ul);

    cpuPrvSetPC(cpu, cpuPrvGetRegNotPC(cpu, REG_NO_LR));
}

static void execFn_peephole_ADC_sdiv10(struct ArmCpu *cpu, uint32_t instr) {
    const int32_t num = cpuPrvGetRegNotPC(cpu, 0);

    cpuPrvSetRegNotPC(cpu, 0, num / 10l);
    cpuPrvSetRegNotPC(cpu, 1, num % 10l);

    cpuPrvSetPC(cpu, cpuPrvGetRegNotPC(cpu, REG_NO_LR));
}

template <int memorySystemKind>
static void execFn_peephole_ADC_memcpy(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t src = cpuPrvGetRegNotPC(cpu, 1);
    uint32_t dest = cpuPrvGetRegNotPC(cpu, 0);
    uint32_t size = cpuPrvGetRegNotPC(cpu, 2);

    // This should not be necessary, but it works around a bug where UDMH fails to
    // invalidate the cache...
    icacheInvalRange(cpu->ic, dest, size);

    MemcpyResult result;
    if constexpr (memorySystemKind == ARM_MEMORY_SYSTEM_MMU) {
        memcpy_armToArm(dest, src, size, cpu->privileged, cpu->mem, cpu->memorySystem.mmu, &result);
    } else {
        memcpy_armToArm(dest, src, size, cpu->privileged, cpu->mem, cpu->memorySystem.mpu, &result);
    }

    if (!result.ok) {
        cpuPrvHandleMemErr(cpu, result.faultAddr, result.wasWrite, false, result.fsr);
    } else {
        cpuPrvSetPC(cpu, cpuPrvGetRegNotPC(cpu, REG_NO_LR));
    }
}

template <int memorySystemKind, bool wasT>
static void execFn_invalid(struct ArmCpu *cpu, uint32_t instr) {
    fprintf(stderr, "Invalid instr 0x%08lx seen at 0x%08lx with CPSR 0x%08lx\n",
            (unsigned long)instr, (unsigned long)cpu->curInstrPC,
            (unsigned long)cpuPrvMaterializeCPSR(cpu));

    cpuPrvException(cpu, cpu->vectorBase + ARM_VECTOR_OFFT_UND, cpu->curInstrPC + (wasT ? 2 : 4),
                    ARM_SR_MODE_UND | ARM_SR_I);
}

template <int memorySystemKind, bool wasT, bool align2>
static void execFn_b2thumb(struct ArmCpu *cpu, uint32_t instr) {
    if (cpu->model == ArmCpuModel::arm920t) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

    uint32_t ea;

    ea = instr << 8;
    ea = ((int32_t)ea) >> 7;
    ea += 4;
    if (!wasT) ea <<= 1;
    ea += cpu->curInstrPC;

    if (align2) ea += 2;
    cpu->regs[REG_NO_LR] = cpu->curInstrPC + (wasT ? 2 : 4);
    if (!cpu->T) ea |= 1UL;  // set T flag if needed

    cpuPrvSetPC(cpu, ea);
}

template <int memorySystemKind, bool wasT, bool two>
static void execFn_cp_mem2reg(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t addBefore, addAfter;
    uint_fast8_t mode, cpNo;
    uint8_t memVal8;

    if (two && cpu->model == ArmCpuModel::arm920t) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);
    if (cpuPrvConditionFails(cpu, instr)) return;

    cpNo = (instr >> 8) & 0x0F;
    mode = cpuPrvArmAdrMode_5(cpu, instr, &addBefore, &addAfter, &memVal8);

    if (cpNo >= 14) {  // cp14 and cp15 are for priviledged users only
        if (!cpu->privileged) goto invalid_instr;
    } else if (!(cpu->CPAR & (1UL << cpNo)))  // others are access-controlled by CPAR
        goto invalid_instr;

    if (mode & ARM_MODE_5_RR) {  // handle MCRR, MRCC
        if (cpu->model == ArmCpuModel::arm920t) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

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

    if (two && cpu->model == ArmCpuModel::arm920t) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);
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

    if constexpr (memorySystemKind == ARM_MEMORY_SYSTEM_MMU) {
        if (cpu->model == ArmCpuModel::arm920t && (tag == 0 || tag == 4)) {
            ea = cpuPrvGetRegNotPC(cpu, (instr >> 16) & 0x0f);
            const unsigned size = tag == 0 ? 4 : 1;
            if (size == 4 && (ea & 3)) {
                if (cp15MMUAlignmentEnabled(cpu->cp15.cp15mmu))
                    return cpuPrvHandleMemErr(cpu, ea, false, false, 1);
                ea &= ~3u;
            }
            for (bool writing : {false, true}) {
                const auto translation = mmuTranslate(cpu->memorySystem.mmu, ea, cpu->privileged, writing);
                if (!MMU_TRANSLATE_RESULT_OK(translation))
                    return cpuPrvHandleMemErr(cpu, ea, writing, false, MMU_TRANSLATE_RESULT_FSR(translation));
                auto* dc = cp15MMUArm920DataCache(cpu->cp15.cp15mmu);
                dc->mem = cpu->mem;
                if (!dc->supported(MMU_TRANSLATE_RESULT_PA(translation), size, writing))
                    return cpuPrvHandleMemErr(cpu, ea, writing, false, 0);
            }
        }
    }

    switch (tag) {
        case 0:  // SWP

            ea = cpuPrvGetRegNotPC(cpu, (instr >> 16) & 0x0F);
            ok = cpuPrvMemOp<memorySystemKind, 4, false>(cpu, &memVal32, ea, cpu->privileged, &fsr);
            if (!ok) {
                cpuPrvHandleMemErr(cpu, ea, false, false, fsr);
                return;
            }
            op1 = (ea & 3) ? cpuPrvROR(memVal32, (ea & 3) * 8) : memVal32;
            memVal32 = cpuPrvGetRegNotPC(cpu, instr & 0x0F);
            ok = cpuPrvMemOp<memorySystemKind, 4, true>(cpu, &memVal32, ea, cpu->privileged, &fsr);
            if (!ok) {
                cpuPrvHandleMemErr(cpu, ea, true, false, fsr);
                return;
            }
            cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, op1);
            break;

        case 4:  // SWPB

            ea = cpuPrvGetRegNotPC(cpu, (instr >> 16) & 0x0F);
            ok = cpuPrvMemOp<memorySystemKind, 1, false>(cpu, &memVal8, ea, cpu->privileged, &fsr);
            if (!ok) {
                cpuPrvHandleMemErr(cpu, ea, false, false, fsr);
                return;
            }
            op1 = memVal8;
            memVal8 = cpuPrvGetRegNotPC(cpu, instr & 0x0F);
            ok = cpuPrvMemOp<memorySystemKind, 1, true>(cpu, &memVal8, ea, cpu->privileged, &fsr);
            if (!ok) {
                cpuPrvHandleMemErr(cpu, ea, true, false, fsr);
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
    if ((mode & ARM_MODE_3_TYPE) == ARM_MODE_3_D && cpu->model == ArmCpuModel::arm920t)
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
                    cpuPrvHandleMemErr(cpu, ea, false, false, fsr);
                    return;
                }
                cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, memVal16);
                break;

            case ARM_MODE_3_SH:

                ok = cpuPrvMemOp<memorySystemKind, 2, false>(cpu, &memVal16, ea, cpu->privileged,
                                                             &fsr);
                if (!ok) {
                    cpuPrvHandleMemErr(cpu, ea, false, false, fsr);
                    return;
                }
                cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, (int32_t)(int16_t)memVal16);
                break;

            case ARM_MODE_3_SB:

                ok = cpuPrvMemOp<memorySystemKind, 1, false>(cpu, &memVal8, ea, cpu->privileged,
                                                             &fsr);
                if (!ok) {
                    cpuPrvHandleMemErr(cpu, ea, false, false, fsr);
                    return;
                }
                cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, (int32_t)(int8_t)memVal8);
                break;

            case ARM_MODE_3_D:

                ok = cpuPrvMemOp<memorySystemKind, 8, false>(cpu, doubleMem, ea, cpu->privileged,
                                                             &fsr);
                if (!ok) {
                    cpuPrvHandleMemErr(cpu, ea, false, false, fsr);
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
                    cpuPrvHandleMemErr(cpu, ea, true, false, fsr);
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
                    cpuPrvHandleMemErr(cpu, ea, true, false, fsr);
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
    if (cpu->model == ArmCpuModel::arm920t) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

    cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, cpuPrvClz(cpuPrvGetRegNotPC(cpu, instr & 0xF)));
}

template <int memorySystemKind, bool wasT, bool link>
static void execFn_bl_reg(struct ArmCpu *cpu, uint32_t instr) {
    if (cpuPrvConditionFails(cpu, instr)) return;
    if (link && cpu->model == ArmCpuModel::arm920t) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

    cpuPrvSetPC(cpu, cpuPrvGetReg<wasT>(cpu, instr & 0x0F));

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

    cpuPrvSetPSR<spsr>(cpu, (instr >> 16) & 0x0F, cpuPrvGetReg<wasT, pc>(cpu, instr & 0x0F));
}

template <int memorySystemKind, bool wasT, int tag>
static void execFn_dspadd(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t op1, op2, res;

    if (cpuPrvConditionFails(cpu, instr)) return;
    if (cpu->model == ArmCpuModel::arm920t) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

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
    if (cpu->model == ArmCpuModel::arm920t) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

    cpuPrvException(cpu, cpu->vectorBase + ARM_VECTOR_OFFT_P_ABT, cpu->curInstrPC + 4,
                    ARM_SR_MODE_ABT | ARM_SR_I);
}

template <int memorySystemKind, bool wasT, int tag>
static void execFn_dspmul(struct ArmCpu *cpu, uint32_t instr) {
    uint32_t op1, op2, res;
    uint64_t res64;

    if (cpuPrvConditionFails(cpu, instr)) return;
    if (cpu->model == ArmCpuModel::arm920t) return execFn_invalid<memorySystemKind, wasT>(cpu, instr);

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

                cpuPrvSetPSR<false>(cpu, (instr >> 16) & 0x0F,
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

                cpuPrvSetPSR<true>(cpu, (instr >> 16) & 0x0F,
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
        cpuPrvSetReg<destPc>(cpu, (instr >> 12) & 0x0F, res);
    else if (destPc) {  // copy SPSR to CPSR. we allow in user
                        // mode too - allowed and faster

        if (cpu->model == ArmCpuModel::arm920t) icacheArm920BranchInline(cpu->ic);
        sr = cpu->SPSR;
        cpuPrvSetPSRlo8(cpu, sr);
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
                cpuPrvHandleMemErr(cpu, ea, false, false, fsr);
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
                    cpuPrvSetReg<destPc>(cpu, destReg, memVal32);
                    patchDispatchOnLoadPcFromR12(cpu->patchDispatch, addBefore ? increment : 0,
                                                 cpu->regs);
                } else {
                    // syscall && !destPc -> sourceReg == 12 && destReg == 15
                    cpuPrvSetReg<destPc>(cpu, destReg, memVal32);
                    patchDispatchOnLoadR12FromR9(cpu->patchDispatch, addBefore ? increment : 0);
                }
            } else {
                cpuPrvSetReg<destPc>(cpu, destReg, memVal32);
            }
        } else {
            ok = cpuPrvMemOp<memorySystemKind, 1, false>(cpu, &memVal8, ea, privileged, &fsr);
            if (!ok) {
                cpuPrvHandleMemErr(cpu, ea, false, false, fsr);
                return;
            }
            cpuPrvSetRegNotPC(cpu, (instr >> 12) & 0x0F, memVal8);
        }
    } else {
        op1 = cpuPrvGetReg<wasT, destPc>(cpu, (instr >> 12) & 0x0F);
        if constexpr (!wasT && destPc && (mode & ARM_MODE_2_WORD))
            if (cpu->model == ArmCpuModel::arm920t) op1 = cpu->curInstrPC + 12;

        if (mode & ARM_MODE_2_WORD) {
            memVal32 = op1;
            ok = cpuPrvMemOp<memorySystemKind, 4, true>(cpu, &memVal32, ea, privileged, &fsr);
            if (!ok) {
                cpuPrvHandleMemErr(cpu, ea, true, false, fsr);
                return;
            }
        } else {
            memVal8 = op1;
            ok = cpuPrvMemOp<memorySystemKind, 1, true>(cpu, &memVal8, ea, privileged, &fsr);
            if (!ok) {
                cpuPrvHandleMemErr(cpu, ea, true, false, fsr);
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

    if (sBit &&
        !((cpu->M == ARM_SR_MODE_USR) || (cpu->M == ARM_SR_MODE_SYS))) {  // sort out what "S" means
        if (isLoad && (regsList & (1 << REG_NO_PC)))
            copySPSR = true;
        else
            userModeRegs = true;
    }

#define LOAD_STORE_MULTI_ITER(idx)                                                                 \
    do {                                                                                           \
        constexpr uint8_t regNo = (mode & ARM_MODE_4_INC) ? idx : 15 - idx;                        \
        if (!(regsList & (1 << regNo))) continue;                                                  \
                                                                                                   \
        if (!isLoad) {                                                                             \
            memVal32 = cpuPrvGetReg<wasT>(cpu, regNo);                                             \
            if (!wasT && regNo == REG_NO_PC && cpu->model == ArmCpuModel::arm920t)                  \
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
        if (mode & ARM_MODE_4_BFR) ea += (mode & ARM_MODE_4_INC) ? 4 : -4;                         \
        ok = cpuPrvMemOp<memorySystemKind, 4, !isLoad>(cpu, &memVal32, ea, cpu->privileged, &fsr); \
        if (!ok) {                                                                                 \
            /* Put a loaded base back before the abort switches mode and bank. */                 \
            if (regsList & (1 << reg)) cpuPrvSetReg(cpu, reg, origBaseRegVal);                     \
            cpuPrvHandleMemErr(cpu, ea, !isLoad, false, fsr);                                      \
                                                                                                   \
            return;                                                                                \
        }                                                                                          \
        if (!(mode & ARM_MODE_4_BFR)) ea += (mode & ARM_MODE_4_INC) ? 4 : -4;                      \
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
                cpuPrvSetReg(cpu, regNo, memVal32);                                                \
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

    if (mode & ARM_MODE_4_WBK) cpuPrvSetRegNotPC(cpu, reg, ea);
    if (copySPSR) {
        if (cpu->model == ArmCpuModel::arm920t) icacheArm920BranchInline(cpu->ic);
        sr = cpu->SPSR;
        cpuPrvSetPSRlo8(cpu, sr);
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

    cpuPrvSetPC(cpu, ea);
}

template <int memorySystemKind, bool wasT>
static void execFn_swi(struct ArmCpu *cpu, uint32_t instr) {
    uint_fast8_t fsr;

    if (cpuPrvConditionFails(cpu, instr)) return;

    // ARM920T always enters the guest SWI vector, for every immediate and operation.
    if (cpu->model != ArmCpuModel::arm920t &&
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

    cpuPrvException(cpu, cpu->vectorBase + ARM_VECTOR_OFFT_SWI, cpu->curInstrPC + (wasT ? 2 : 4),
                    ARM_SR_MODE_SVC | ARM_SR_I);
}

template <int memorySystemKind>
static void execFn_peephole_ADC_udivmod(struct ArmCpu *cpu, uint32_t instr) {
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

    cpuPrvSetPC(cpu, cpuPrvGetRegNotPC(cpu, REG_NO_LR));
}

template <int memorySystemKind>
static void execFn_peephole_ADC_sdivmod(struct ArmCpu *cpu, uint32_t instr) {
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

    cpuPrvSetPC(cpu, cpuPrvGetRegNotPC(cpu, REG_NO_LR));
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
        cpuPrvHandleMemErr(cpu, addr, false, false, fsr);
    else
        cpuPrvSetRegNotPC(cpu, (instr >> 8) & 0x07, memVal32);
}

template <int memorySystemKind, bool pcD, bool pcS>
static void execFn_thumb_4_add(struct ArmCpu *cpu, uint32_t instr) {
    uint_fast8_t vD = (instr & 7) | ((instr >> 4) & 0x08);
    uint_fast8_t v8 = (instr >> 3) & 0xF;

    uint32_t v32 = cpuPrvGetReg<true, pcD>(cpu, vD) + cpuPrvGetReg<true, pcS>(cpu, v8);
    if constexpr (pcD) v32 |= 1;

    cpuPrvSetReg<pcD>(cpu, vD, v32);
}

template <int memorySystemKind, bool pcD, bool pcS>
static void execFn_thumb_3_mov(struct ArmCpu *cpu, uint32_t instr) {
    uint_fast8_t vD = (instr & 7) | ((instr >> 4) & 0x08);
    uint_fast8_t v8 = (instr >> 3) & 0xF;

    uint32_t v32 = cpuPrvGetReg<true, pcS>(cpu, v8);
    if constexpr (pcD) v32 |= 1;

    cpuPrvSetReg<pcD>(cpu, vD, v32);
}

template <int memorySystemKind, bool blx, bool pcS>
static void execFn_thumb_bl_x(struct ArmCpu *cpu, uint32_t instr) {
    if (blx && cpu->model == ArmCpuModel::arm920t) return execFn_invalid<memorySystemKind, true>(cpu, instr);

    if constexpr (blx) cpu->regs[REG_NO_LR] = cpu->regs[REG_NO_PC] + 1;

    if constexpr (pcS)
        cpuPrvSetPC(cpu, (cpu->regs[REG_NO_PC] + 2) & ~3UL);
    else
        cpuPrvSetPC(cpu, cpu->regs[(instr >> 3) & 0xF]);
}

template <int memorySystemKind, bool sp>
static void execFn_thumb_load_addr(struct ArmCpu *cpu, uint32_t instr) {
    cpuPrvSetReg<false>(
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
    if (cpu->model == ArmCpuModel::arm920t) return execFn_invalid<memorySystemKind, true>(cpu, instr);

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
static bool cpuPrvJitWanted();
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

    cpu->debugStub = gdbStubInit(cpu, debugPort);

    cpu->mem = mem;

    cpu->memorySystemKind = memorySystemKind;
    if (memorySystemKind == ARM_MEMORY_SYSTEM_MMU) {
        cpu->memorySystem.mmu = mmuInit(mem, xscale);
        cpu->ic = icacheInit(mem, cpu->memorySystem.mmu);
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
    if (memorySystemKind == ARM_MEMORY_SYSTEM_MMU && cpuPrvJitWanted()) {
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
#ifdef SUPPORT_FCSE
    if (address < 0x02000000u) address |= cpu->pid;
#endif
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
    return cpu->cyclesInProgress ? *cpu->cyclesInProgress : 0;
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
        cycleAcc += 20;

        if (cpu->slowPath) break;
    }

    return cycleAcc;
}

#ifndef __EMSCRIPTEN__
// icacheFetchCached for a va in the same 32-byte line as the previous fetch. The line
// was valid for lineVa when ic->epoch was lineEpoch, and the epoch moves on every
// invalidation and refill, so an unchanged epoch means the line still holds lineVa.
// On the ARM920T lineVa never matches (cpuPrvRememberLine).
// The decoded slot is still checked, since execFns can decode other slots of the line.
template <int size>
static FORCE_INLINE bool cpuPrvFetchSameLine(const struct icache* ic, const struct icacheline* line,
                                             uint32_t lineVa, uint32_t lineEpoch, uint32_t va,
                                             uint32_t& instr, uint32_t& decoded) {
    // lineVa is line aligned, so this also requires va to be aligned to size.
    if ((va & ~uint32_t((1u << CACHE_LINE_WIDTH_BITS) - size)) != lineVa) return false;
    if (ic->epoch != lineEpoch) return false;
    const unsigned offset = calculateLineIndex(va);
    const uint32_t cached = line->decoded[offset >> 1];
    constexpr uint32_t kind = size == 4 ? DECODED_BITS_ARM : DECODED_BITS_THUMB;
    if ((cached & DECODED_BITS) != kind) return false;
    decoded = int32_t(cached) >> DECODED_BITS_SHIFT;
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
    lineVa = !ic->arm920 && line->revision == ic->revision && line->tag == calculateTag(pc) && line->fetchKey == key
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
// arm920 is ic->arm920: the ARM920T fetches every instruction through icacheFetch.
template <int memorySystemKind, int size, bool arm920>
ATTR_EMCC_NOINLINE static uint32_t cpuCycleLine(struct ArmCpu *cpu, uint32_t cycles) {
    uint32_t cycleAcc = 0;
    uint32_t acc = 0;
    CycleProgress progress(cpu, &cycleAcc);
    struct icache *const ic = cpu->ic;
    constexpr uint32_t kind = size == 4 ? DECODED_BITS_ARM : DECODED_BITS_THUMB;
    constexpr uint32_t lineSize = 1u << CACHE_LINE_WIDTH_BITS;

    while (acc < cycles) {
        uint32_t pc = cpu->regs[REG_NO_PC];
        cpu->curInstrPC = pc;
        acc += 1;
        cycleAcc = acc;

        const struct icacheline *line = ic->cache + calculateIndex(pc);
        uint32_t offset = calculateLineIndex(pc);
        uint32_t cached = line->decoded[offset >> 1];
        if (unlikely(arm920 || (pc & (size - 1)) || line->revision != ic->revision ||
                     line->tag != calculateTag(pc) || (cached & DECODED_BITS) != kind ||
                     line->fetchKey != icacheFetchKey(ic, cpu->privileged))) {
            uint32_t instr = 0, decoded = 0;
            uint_fast8_t fsr;
            if (!icacheFetch<memorySystemKind, size, false>(ic, pc, cpu->privileged, &fsr, instr, decoded)) {
                cpuPrvHandleMemErr(cpu, pc, false, true, fsr);
                return acc;
            }
            cpu->regs[REG_NO_PC] = pc + size;
            cpuPrvDecompressExecFn(decoded)(cpu, instr);
            if (cpu->slowPath) break;
            continue;
        }

        const uint32_t epoch = ic->epoch;
        for (;;) {
            uint32_t instr;
            if constexpr (size == 4) std::memcpy(&instr, line->data + offset, sizeof(instr));
            else instr = line->translatedThumbInstructions[offset >> 1];
            cpu->regs[REG_NO_PC] = pc + size;
            cpuPrvDecompressExecFn(int32_t(cached) >> DECODED_BITS_SHIFT)(cpu, instr);
            if (cpu->slowPath) return acc;

            pc += size;
            offset += size;
            if (cpu->regs[REG_NO_PC] != pc || offset == lineSize || ic->epoch != epoch ||
                acc >= cycles)
                break;
            cached = line->decoded[offset >> 1];
            if ((cached & DECODED_BITS) != kind) break;

            cpu->curInstrPC = pc;
            acc += 1;
            cycleAcc = acc;
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
        ok = cpuPrvFetchSameLine<2>(ic, line, lineVa, lineEpoch, pc, translatedInstr, decoded);
        if (!ok) {
            // Fetch through copies, so the fast path's values stay in registers.
            uint32_t slowInstr = 0, slowDecoded = 0;
            const uint32_t key = icacheFetchKey(ic, cpu->privileged);
            ok = icacheFetchCached<2>(ic, pc, key, slowInstr, slowDecoded);
            if (!ok)
                ok = icacheFetch<memorySystemKind, 2, injected>(ic, pc, cpu->privileged, &fsr, slowInstr,
                                                                slowDecoded);
            translatedInstr = slowInstr;
            decoded = slowDecoded;
            cpuPrvRememberLine(ic, pc, key, line, lineVa, lineEpoch);
        }
#else
        ok = icacheFetch<memorySystemKind, 2, injected>(cpu->ic, cpu->curInstrPC, cpu->privileged, &fsr,
                                                        translatedInstr, decoded);
#endif

        if (!ok) {
            cpuPrvHandleMemErr(cpu, cpu->curInstrPC, false, true, fsr);
            return acc;  // exit here so that debugger can see us execute first instr
                         // of execption handler
        }

        cpu->regs[REG_NO_PC] += 2;

#ifdef __EMSCRIPTEN__
        cpuPrvDispatchExecFnThumb<memorySystemKind, injected>(cpuPrvDecompressExecFn(decoded), cpu,
                                                              translatedInstr);
#else
        cpuPrvDecompressExecFn(decoded)(cpu, translatedInstr);
#endif

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
        ok = cpuPrvFetchSameLine<4>(ic, line, lineVa, lineEpoch, pc, instr, decoded);
        if (!ok) {
            // Fetch through copies, so the fast path's values stay in registers.
            uint32_t slowInstr = 0, slowDecoded = 0;
            const uint32_t key = icacheFetchKey(ic, cpu->privileged);
            ok = icacheFetchCached<4>(ic, pc, key, slowInstr, slowDecoded);
            if (!ok)
                ok = icacheFetch<memorySystemKind, 4, injected>(ic, pc, cpu->privileged, &fsr, slowInstr,
                                                                slowDecoded);
            instr = slowInstr;
            decoded = slowDecoded;
            cpuPrvRememberLine(ic, pc, key, line, lineVa, lineEpoch);
        }
#else
        ok = icacheFetch<memorySystemKind, 4, injected>(cpu->ic, cpu->curInstrPC, cpu->privileged, &fsr, instr,
                                                        decoded);
#endif

        if (!ok) {
            cpuPrvHandleMemErr(cpu, cpu->curInstrPC, false, true, fsr);
            return acc;  // exit here so that debugger can see us execute first instr
                         // of execption handler
        }

        cpu->regs[REG_NO_PC] += 4;

#ifdef __EMSCRIPTEN__
        cpuPrvDispatchExecFnArm<memorySystemKind, injected>(cpuPrvDecompressExecFn(decoded), cpu,
                                                            instr);
#else
        cpuPrvDecompressExecFn(decoded)(cpu, instr);
#endif

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
    cpuPrvDecoderArm<ARM_MEMORY_SYSTEM_MMU, false>(instr)(cpu, instr);
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
    execFn_thumb_bl_x<ARM_MEMORY_SYSTEM_MMU, false, pcS>(cpu, instr);
}

// The raw halfword at va if the decode cache holds its line under the current fetch key.
static bool cpuPrvLeanPeekHalf(const struct ArmCpu *cpu, uint32_t va, uint16_t &half) {
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
    execFn_thumb_bl_x_prefix<ARM_MEMORY_SYSTEM_MMU>(cpu, instr);
    cpu->leanBlNext = cpu->curInstrPC + 2;
}

static void execFn_leanBlSuffix(struct ArmCpu *cpu, uint32_t instr) {
    if (cpu->leanBlNext != cpu->curInstrPC) {
        cpu->leanExit = true;
        return;
    }
    cpu->leanBlNext = ~0u;
    execFn_thumb_bl_suffix<ARM_MEMORY_SYSTEM_MMU>(cpu, instr);
}

static uint32_t cpuPrvLeanDecodeArm(uint32_t instr) {
    switch (cpuPrvLeanClassArm(instr)) {
        case LeanClass::plain: return cpuDecodeArm<ARM_MEMORY_SYSTEM_MMU>(instr);
        case LeanClass::check: return cpuPrvCompressExecFn(execFn_leanCheckArm);
        default: return cpuPrvCompressExecFn(execFn_leanExit);
    }
}

static uint32_t cpuPrvLeanDecodeThumb(uint16_t raw, uint32_t &translated) {
    translated = raw;
    switch (cpuPrvLeanClassThumb(raw)) {
        case LeanClass::plain: return cpuDecodeThumb<ARM_MEMORY_SYSTEM_MMU>(raw, translated);
        case LeanClass::thumbBx:
            return ((raw >> 3) & 15) == 15 ? cpuPrvCompressExecFn(execFn_leanThumbBx<true>)
                                           : cpuPrvCompressExecFn(execFn_leanThumbBx<false>);
        case LeanClass::blPrefix: return cpuPrvCompressExecFn(execFn_leanBlPrefix);
        case LeanClass::blSuffix: return cpuPrvCompressExecFn(execFn_leanBlSuffix);
        default: return cpuPrvCompressExecFn(execFn_leanExit);
    }
}

template <int size>
static FORCE_INLINE void cpuPrvLeanDecodeSlot(struct icacheline *line, unsigned offset, uint32_t &instr,
                                              uint32_t &decoded) {
    if constexpr (size == 4) {
        memcpy(&instr, line->data + offset, 4);
        decoded = cpuPrvLeanDecodeArm(instr);
        line->decoded[offset >> 1] = (decoded << DECODED_BITS_SHIFT) | DECODED_BITS_ARM;
    } else {
        uint16_t raw;
        memcpy(&raw, line->data + offset, 2);
        decoded = cpuPrvLeanDecodeThumb(raw, instr);
        line->decoded[offset >> 1] = (decoded << DECODED_BITS_SHIFT) | DECODED_BITS_THUMB;
        line->translatedThumbInstructions[offset >> 1] = instr;
    }
}

// Notes that the decode cache holds code from the RAM page at pa. A newly noted page loses its lean TLB
// write entries, so every later store to it takes the slow path, which drops the decoded code first.
static void cpuPrvLeanNoteCode(struct ArmCpu *cpu, uint32_t pa) {
    uint64_t &bits = cpu->leanCode[pa >> 18];
    const uint64_t bit = uint64_t(1) << ((pa >> 12) & 63);
    if (bits & bit) return;
    bits |= bit;
    hostTlbFlush(&cpu->mem->hostTlb);
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
                                                         uint32_t &decoded) {
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
        } else {
            uint16_t raw;
            memcpy(&raw, page + (pa & 0xfffu), 2);
            decoded = cpuPrvLeanDecodeThumb(raw, instr);
        }
        return true;
    }
    struct icacheline *line = cpuPrvLeanFillLine(cpu, pc, cpu->privileged, pa, page, ram);
    cpuPrvLeanDecodeSlot<size>(line, calculateLineIndex(pc), instr, decoded);
    return true;
}

template <int size>
static FORCE_INLINE bool cpuPrvLeanFetch(struct ArmCpu *cpu, uint32_t pc, uint32_t &instr, uint32_t &decoded) {
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
            return true;
        }
        cpuPrvLeanDecodeSlot<size>(line, offset, instr, decoded);
        return true;
    }
    return cpuPrvLeanFetchSlow<size>(cpu, pc, instr, decoded);
}

template <int size>
static uint32_t cpuPrvLeanLoop(struct ArmCpu *cpu) {
    uint32_t done = 0;
    const uint32_t before = cpu->leanDone;
    while (before + done < cpu->leanBudget && before + done < cpu->leanCount) {
        cpu->leanDone = before + done;
        const uint32_t pc = cpu->regs[REG_NO_PC];
        uint32_t instr, decoded;
        cpu->curInstrPC = pc;
        if (!cpuPrvLeanFetch<size>(cpu, pc, instr, decoded)) break;
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
        ++done;
        if (cpu->slowPath) break;
    }
    cpu->leanDone = before + done;
    return done;
}

uint32_t cpuArm920RunLean(ArmCpu *cpu, uint32_t budget, uint32_t *blNext, uint64_t *busAccesses) {
    return cpuArm920RunLeanFor(cpu, budget, UINT32_MAX, blNext, busAccesses);
}

uint32_t cpuArm920RunLeanFor(ArmCpu *cpu, uint32_t budget, uint32_t count, uint32_t *blNext, uint64_t *busAccesses) {
    *blNext = ~0u;
    *busAccesses = 0;
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
    cpu->leanBudget = budget;
    cpu->leanCount = count;
    uint32_t done = 0;
    for (;;) {
        done += cpu->T ? cpuPrvLeanLoop<2>(cpu) : cpuPrvLeanLoop<4>(cpu);
        // Carry on only past an ARM/Thumb switch; a failed fetch leaves no slow path reason.
        if (done >= cpu->leanBudget || done >= count || cpu->leanExit || cpu->slowPath != SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE) break;
        cpuClearSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE);
    }
    cpu->leanRun = false;
    cpu->leanExit = false;
    *blNext = cpu->leanBlNext;
    *busAccesses = cpu->leanBus;
    return done;
}

void cpuArm920SetLean(ArmCpu *cpu, Arm920LeanPage page, Arm920LeanGuard guard, void *context) {
    if (cpu->model != ArmCpuModel::arm920t) return;
    cpu->leanPage = page;
    cpu->leanGuard = guard;
    cpu->leanContext = context;
    cp15MMUArm920DataCache(cpu->cp15.cp15mmu)->lean = page != nullptr;
    if (page && !cpu->leanCode) {
        cpu->leanCode = (uint64_t *)calloc(1u << 14, sizeof(uint64_t));
        if (!cpu->leanCode) ERR("cannot alloc the Fast code page map");
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
    if (cpu->leanCode) memset(cpu->leanCode, 0, (1u << 14) * sizeof(uint64_t));
    hostTlbFlush(&cpu->mem->hostTlb);
}

void cpuArm920LeanFlush(ArmCpu *cpu) { hostTlbFlush(&cpu->mem->hostTlb); }

void cpuArm920SetLeanRead(ArmCpu *cpu, Arm920LeanRead read) { cpu->leanRead = read; }

void cpuArm920SetLeanWrite(ArmCpu *cpu, Arm920LeanWrite write) { cpu->leanWrite = write; }

// Decoded code isn't tied to the physical pages it came from, so a store to any of them drops it all.
void cpuArm920LeanStored(ArmCpu *cpu, uint32_t pa) {
#ifdef FROND_DYNARMIC
    // Translated code is dropped by page: the virtual pages it was translated from at pa.
    if (cpu->jitCode) {
        uint64_t &bits = cpu->jitCode[pa >> 18];
        const uint64_t bit = uint64_t(1) << ((pa >> 12) & 63);
        if (unlikely(bits & bit)) {
            bits &= ~bit;
            jitCodeWrittenPa(cpu->jit, pa);
        }
    }
#endif
    if (!cpu->leanCode) return;
    uint64_t &bits = cpu->leanCode[pa >> 18];
    if (likely(!(bits & (uint64_t(1) << ((pa >> 12) & 63))))) return;
    memset(cpu->leanCode, 0, (1u << 14) * sizeof(uint64_t));
    icacheArm920DropDecoded(cpu->ic);
    ++cpu->leanCodeDrops;
}

uint32_t cpuArm920LeanGeneration(const ArmCpu *cpu) { return cpu->mem->hostTlb.gen; }

uint64_t cpuArm920LeanCodeDrops(const ArmCpu *cpu) { return cpu->leanCodeDrops; }

void cpuArm920SetJitAccessPcs(ArmCpu *cpu, const uint32_t *pcs, uint32_t count) {
    cpu->jitAccessPcs = pcs;
    cpu->jitAccessPcCount = count;
}

bool cpuArm920PeekWord(ArmCpu *cpu, uint32_t va, uint32_t *word, uint32_t *pa) {
    // The lean and JIT loads' translation, with the current mode's permissions.
    const MMUTranslateResult r = mmuTranslateCached<false>(cpu->memorySystem.mmu, va & ~3u, cpu->privileged);
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
void cpuArm920SetLeanRead(ArmCpu *, Arm920LeanRead) {}
void cpuArm920SetLeanWrite(ArmCpu *, Arm920LeanWrite) {}
uint32_t cpuArm920LeanGeneration(const ArmCpu *) { return 0; }
uint64_t cpuArm920LeanCodeDrops(const ArmCpu *) { return 0; }
#endif

#ifdef FROND_DYNARMIC
// JIT glue. The JIT runs only outside injected calls, the debugger and native PC breaks,
// with the MMU, and only while no slow path reason is set.

bool cpuJitRead(ArmCpu *cpu, uint32_t va, unsigned size, void *buf, uint_fast8_t *fsr) {
    if (va & (size - 1)) {
        *fsr = 0;
        return false;
    }
    switch (size) {
        case 1: return cpuPrvMemOpEx<ARM_MEMORY_SYSTEM_MMU, 1, false>(cpu, buf, va, cpu->privileged, fsr);
        case 2: return cpuPrvMemOpEx<ARM_MEMORY_SYSTEM_MMU, 2, false>(cpu, buf, va, cpu->privileged, fsr);
        case 4: return cpuPrvMemOpEx<ARM_MEMORY_SYSTEM_MMU, 4, false>(cpu, buf, va, cpu->privileged, fsr);
        case 8: return cpuPrvMemOpEx<ARM_MEMORY_SYSTEM_MMU, 8, false>(cpu, buf, va, cpu->privileged, fsr);
    }
    return false;
}

bool cpuJitWrite(ArmCpu *cpu, uint32_t va, unsigned size, const void *buf, uint_fast8_t *fsr) {
    if (va & (size - 1)) {
        *fsr = 0;
        return false;
    }
    void *b = const_cast<void *>(buf);
    switch (size) {
        case 1: return cpuPrvMemOpEx<ARM_MEMORY_SYSTEM_MMU, 1, true>(cpu, b, va, cpu->privileged, fsr);
        case 2: return cpuPrvMemOpEx<ARM_MEMORY_SYSTEM_MMU, 2, true>(cpu, b, va, cpu->privileged, fsr);
        case 4: return cpuPrvMemOpEx<ARM_MEMORY_SYSTEM_MMU, 4, true>(cpu, b, va, cpu->privileged, fsr);
        case 8: return cpuPrvMemOpEx<ARM_MEMORY_SYSTEM_MMU, 8, true>(cpu, b, va, cpu->privileged, fsr);
    }
    return false;
}

// ARM920T Fast blocks: the lean TLB's page for va, live only for plain pages the MMU allows now in this mode.
// Its write entries are dropped whenever a page gets translated code (cpuPrvJitNoteArm920Code), and the slow
// path that fills one again drops the code first (cpuArm920LeanStored), so a writable page holds no code.
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
            // Plain pages only, straight from memory: Fast keeps no I-cache contents.
            const MMUTranslateResult r = mmuTranslate(cpu->memorySystem.mmu, va, cpu->privileged, false);
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

// A store to a flagged RAM line: drop the JIT's code from the page and any fetch buffer it
// hit. The page's flags are clear by now, so the buffers left are watched again.
static void cpuPrvJitCodeWritten(void *ctx, uint8_t *page, uint32_t offset, uint32_t size) {
    ArmCpu *cpu = (ArmCpu *)ctx;
    icacheDropFetchedHost(cpu->ic, page + offset, size);
    jitCodeWritten(cpu->jit, page);
    icacheWatchFetchBuffers(cpu->ic);
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
        // entries (so the JIT's page table empties too), so every later store to it drops its code.
        // The single-step JIT (no blocks) watches RAM lines like the other cores.
        if (!cpu->leanPage) return nullptr;
        bool ram = false;
        uint8_t *page = cpu->leanPage(cpu->leanContext, pa & ~0xfffu, false, &ram);
        if (page && ram) {
            uint64_t &bits = cpu->jitCode[pa >> 18];
            const uint64_t bit = uint64_t(1) << ((pa >> 12) & 63);
            if (!(bits & bit)) {
                bits |= bit;
                hostTlbFlush(&cpu->mem->hostTlb);
            }
        }
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

void cpuJitForgetCode(ArmCpu *cpu) {
    if (cpu->jitCode) memset(cpu->jitCode, 0, (1u << 14) * sizeof(uint64_t));
    memForgetRamCode(cpu->mem);
    icacheWatchFetchBuffers(cpu->ic);
}

uint32_t cpuJitMapGen(ArmCpu *cpu) { return mmuFlushCount(cpu->memorySystem.mmu); }

// The host TLB's keys hold the privilege, so its generation alone covers both levels. A
// domain change that only adds rights keeps the host TLB, so gains are counted in too.
uint64_t cpuJitPageGen(ArmCpu *cpu) {
    const struct HostTlb &t = cpu->mem->hostTlb;
    return (uint64_t(t.epoch) << 32) | uint32_t(t.gen + t.gains);
}

int cpuJitPageClientAccess(ArmCpu *cpu, uint32_t va, bool privileged) {
    return mmuPageClientAccess(cpu->memorySystem.mmu, va, privileged);
}

bool cpuJitSlowPath(ArmCpu *cpu) { return cpu->slowPath != 0; }

uint32_t *cpuJitSpsr(ArmCpu *cpu) { return &cpu->SPSR; }

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

uint32_t cpuJitSetCpsr(ArmCpu *cpu, uint32_t *regs, uint32_t cpsr, uint32_t mask, uint32_t value) {
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

bool cpuJitPlain(ArmCpu *cpu, uint32_t pc, uint32_t word, bool thumb) {
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
    if (size)
        jitInvalidate(cpu->jit, va, size);
    else if (guest)
        jitGuestInvalidateAll(cpu->jit, cpu->curInstrPC);
    else
        jitClear(cpu->jit);
}

static bool cpuPrvJitWanted() {
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
    uint32_t instr = 0, decoded = 0;
    uint_fast8_t fsr;
    const uint32_t size = cpu->T ? 2 : 4;
    const bool ok = cpu->T ? icacheFetch<memorySystemKind, 2, false>(cpu->ic, pc, cpu->privileged, &fsr, instr, decoded)
                           : icacheFetch<memorySystemKind, 4, false>(cpu->ic, pc, cpu->privileged, &fsr, instr, decoded);
    // The JIT fetched this instruction before its access, so a fetch that fails now (the
    // access changed the mapping) doesn't stop the abort.
    if (!ok && !abort) {
        cpuPrvHandleMemErr(cpu, pc, false, true, fsr);
        return;
    }
    cpu->regs[REG_NO_PC] = pc + size;
    if (abort)
        cpuPrvDataAbort(cpu, abort->addr, abort->size, abort->write, abort->fsr);
    else
        cpuPrvDecompressExecFn(decoded)(cpu, instr);
}

static uint32_t cpuPrvCycleJit(struct ArmCpu *cpu, uint32_t cycles) {
    uint32_t acc = 0;
    CycleProgress progress(cpu, &acc);
    static bool faultReported = false;

    while (acc < cycles) {
        if (cpu->model == ArmCpuModel::arm920t && icacheArm920ContinuationPending(cpu->ic)) {
            cpuPrvJitInterpretOne<ARM_MEMORY_SYSTEM_MMU>(cpu, acc);
            if (cpu->slowPath) break;
            continue;
        }
        uint32_t cpsr = cpuPrvMaterializeCPSR(cpu);
        int stop;
        acc += jitRun(cpu->jit, cpu->regs, cpsr, cycles - acc, stop);
        cpu->flags = cpsr & 0xf0000000UL;
        cpu->Q = !!(cpsr & ARM_SR_Q);
        cpu->T = !!(cpsr & ARM_SR_T);
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
                cpuPrvJitInterpretOne<ARM_MEMORY_SYSTEM_MMU>(cpu, acc, first ? abort : nullptr);
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
                cpuPrvJitInterpretOne<ARM_MEMORY_SYSTEM_MMU>(cpu, acc);
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
        cpu->jitCode = (uint64_t *)calloc(1u << 14, sizeof(uint64_t));
        if (!cpu->jitCode) ERR("cannot alloc the JIT code page map");
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
    free(cpu->jitCode);
    cpu->jitCode = nullptr;
}

uint32_t cpuArm920JitMaxUnchecked(const ArmCpu *cpu) { return cpu->jit ? jitMaxUnchecked(cpu->jit) : 0; }

// Slow path reasons cpuArm920RunJit clears before it runs: none of them needs the step.
static constexpr uint32_t arm920JitClearedSlowPath = SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE |
                                                     SLOW_PATH_REASON_RESCHEDULE | SLOW_PATH_REASON_PACE_SYSCALL_BREAK |
                                                     SLOW_PATH_REASON_INJECTED_CALL_DONE;

bool cpuArm920JitCanRun(const ArmCpu *cpu) {
    return cpu->jitBlocks && !(cpu->slowPath & ~arm920JitClearedSlowPath) && !cpu->arm920Waiting && !cpu->modePace &&
           !cpu->nativeCallDepth && !cpu->nativePc[0] && !cpu->debugStub &&
           !icacheArm920ContinuationPending(cpu->ic);
}

uint32_t cpuArm920RunJit(ArmCpu *cpu, uint32_t budget, int *stop, uint32_t span) {
    *stop = arm920JitExit;
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
    uint32_t cpsr = cpuPrvMaterializeCPSR(cpu);
    int jitStop;
    const uint32_t ran = jitRun(cpu->jit, cpu->regs, cpsr, budget, jitStop);
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
bool cpuArm920SetJit(ArmCpu *, bool on) { return !on; }
bool cpuArm920JitOn(const ArmCpu *) { return false; }
void cpuArm920DropJit(ArmCpu *) {}
uint32_t cpuArm920JitMaxUnchecked(const ArmCpu *) { return 0; }
bool cpuArm920JitCanRun(const ArmCpu *) { return false; }
uint32_t cpuArm920RunJit(ArmCpu *, uint32_t, int *stop, uint32_t) {
    *stop = arm920JitExit;
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

template <int memorySystemKind, bool injected>
ATTR_EMCC_NOINLINE uint32_t cpuCycle(struct ArmCpu *cpu, uint32_t cycles) {
    if (cpu->arm920Waiting) return 0;
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
    if (memorySystemKind == ARM_MEMORY_SYSTEM_MMU && cpu->jit && unlikely(icacheFetchUnwatched(cpu->ic)))
        icacheWatchFetchBuffers(cpu->ic);
#endif
    if (cpu->modePace) return cpuCyclePace(cpu, cycles);

#ifndef __EMSCRIPTEN__
    // nativePc is only set by RunToNativePc around a whole run, so it holds for this batch.
#ifdef FROND_DYNARMIC
    if (memorySystemKind == ARM_MEMORY_SYSTEM_MMU && cpu->jit && !cpu->jitLoopOnly && !injected && !cpu->debugStub &&
        !cpu->nativePc[0] && !cpu->slowPath)
        return cpuPrvCycleJit(cpu, cycles);
#endif
    if (!injected && !cpu->debugStub && !cpu->nativePc[0]) {
        if (unlikely(cpu->ic->arm920))
            return cpu->T ? cpuCycleLine<memorySystemKind, 2, true>(cpu, cycles)
                          : cpuCycleLine<memorySystemKind, 4, true>(cpu, cycles);
        return cpu->T ? cpuCycleLine<memorySystemKind, 2, false>(cpu, cycles)
                      : cpuCycleLine<memorySystemKind, 4, false>(cpu, cycles);
    }
#endif

    if (cpu->T)
        return cpu->debugStub ? cpuCycleThumb<memorySystemKind, injected, true>(cpu, cycles)
                              : cpuCycleThumb<memorySystemKind, injected, false>(cpu, cycles);
    else
        return cpu->debugStub ? cpuCycleArm<memorySystemKind, injected, true>(cpu, cycles)
                              : cpuCycleArm<memorySystemKind, injected, false>(cpu, cycles);
}

template uint32_t cpuCycle<ARM_MEMORY_SYSTEM_MMU, true>(struct ArmCpu *cpu, uint32_t cycles);
template uint32_t cpuCycle<ARM_MEMORY_SYSTEM_MPU, true>(struct ArmCpu *cpu, uint32_t cycles);
template uint32_t cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(struct ArmCpu *cpu, uint32_t cycles);
template uint32_t cpuCycle<ARM_MEMORY_SYSTEM_MPU, false>(struct ArmCpu *cpu, uint32_t cycles);

template <int memorySystemKind, bool thumb>
static uint32_t cpuPrvCycleOne(struct ArmCpu *cpu) {
    if (cpu->debugStub)
        return thumb ? cpuCycleThumb<memorySystemKind, true, true>(cpu, 1)
                     : cpuCycleArm<memorySystemKind, true, true>(cpu, 1);
    return thumb ? cpuCycleThumb<memorySystemKind, true, false>(cpu, 1)
                 : cpuCycleArm<memorySystemKind, true, false>(cpu, 1);
}

uint32_t cpuCyclePure(struct ArmCpu *cpu) {
    if (cpu->arm920Waiting) return 0;
    cpuClearSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE | SLOW_PATH_REASON_RESCHEDULE |
                              SLOW_PATH_REASON_PACE_SYSCALL_BREAK |
                              SLOW_PATH_REASON_INJECTED_CALL_DONE);

    patchOnBeforeExecute(cpu->patchDispatch, cpu->regs);

    if (cpu->modePace)
        return cpuCyclePace(cpu, 1);
    else if (cpu->T)
        return (cpu->memorySystemKind == ARM_MEMORY_SYSTEM_MMU)
                   ? cpuPrvCycleOne<ARM_MEMORY_SYSTEM_MMU, true>(cpu)
                   : cpuPrvCycleOne<ARM_MEMORY_SYSTEM_MPU, true>(cpu);
    else
        return (cpu->memorySystemKind == ARM_MEMORY_SYSTEM_MMU)
                   ? cpuPrvCycleOne<ARM_MEMORY_SYSTEM_MMU, false>(cpu)
                   : cpuPrvCycleOne<ARM_MEMORY_SYSTEM_MPU, false>(cpu);
}

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

void cpuSetPid(struct ArmCpu *cpu, uint32_t pid) { cpu->pid = pid; }

uint32_t cpuGetPid(struct ArmCpu *cpu) { return cpu->pid; }

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
    paceSave(savestate);

    if (cpu->memorySystemKind == ARM_MEMORY_SYSTEM_MMU) {
        mmuSave(cpu->memorySystem.mmu, savestate);
        cp15MMUSave(cpu->cp15.cp15mmu, savestate);
    } else {
        mpuSave(cpu->memorySystem.mpu, savestate);
        cp15MPUSave(cpu->cp15.cp15mpu, savestate);
    }

    patchDispatchSave(cpu->patchDispatch, savestate);

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
