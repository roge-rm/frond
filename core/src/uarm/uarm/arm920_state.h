#ifndef FROND_ARM920_STATE_H
#define FROND_ARM920_STATE_H

#include <cstdint>
#include <vector>

// Guest-visible ARM920T state between two run calls: registers, CP15, the TLB and both caches.
// cpuArm920CaptureState fills it and cpuArm920ApplyState puts it back; translated code and
// decoded lines are host caches and are dropped instead.
struct Arm920SavedState {
    static constexpr uint32_t stateVersion = 1;

    uint64_t exceptionSequence = 0;
    uint32_t regs[16]{}, extraRegs[5]{};
    // usr, svc, abt, und, irq, fiq: R13, R14, SPSR.
    uint32_t banks[6][3]{};
    uint32_t spsr = 0, flags = 0, curInstrPC = 0, vectorBase = 0, pid = 0;
    // Kept as it is: a set bit sends the next Fast step to the interpreter.
    uint32_t slowPath = 0;
    uint16_t waitingIrqs = 0, waitingFiqs = 0, cpar = 0;
    uint8_t mode = 0;
    bool q = false, t = false, i = false, f = false, privileged = false, waiting = false;
    // From the options at create.
    bool waitEnabled = false, icacheIndexCompatibility = false;

    uint32_t control = 0, ttb = 0, fsr = 0, far = 0, mmuSwitchCy = 0;

    uint32_t transTablPA = 0, domainCfg = 0;
    uint8_t mmuS = 0, mmuR = 0;
    // Host counters, kept so diagnostics read the same after a load.
    uint16_t tlbRevision = 1;
    uint32_t tlbFlushes = 0;
    // Live TLB entries in rising page order, two words each: the virtual page, then
    // pa | ap << 20 | domain << 22 | c << 26 | section << 27 | b << 28.
    std::vector<uint32_t> tlb;

    bool icacheEnabled = false, icacheRoundRobin = false;
    uint32_t continuationPc = 0, continuation[2]{};
    uint8_t continuationFault[2]{}, continuationCount = 0;
    uint32_t icacheRandom = 0;
    uint8_t icacheVictim[8]{};
    struct InstructionLine {
        uint32_t base, words[8];
        bool valid;
    } instructionLines[8][64]{};

    struct DataLine {
        uint32_t va, pa, words[8];
        uint8_t dirty;
        bool valid, bufferable;
    } dataLines[8][64]{};
    uint8_t dcacheVictim[8]{};
    uint32_t dcacheRandom = 0;
    uint32_t guardAddress = 0, guardSize = 0;
    bool guardWrite = false, guardActive = false;

    template <class A> void stateCpu(A& a) {
        a.Do64(exceptionSequence).DoBuffer32(regs, 16).DoBuffer32(extraRegs, 5);
        for (auto& bank : banks) a.DoBuffer32(bank, 3);
        a.Do32(spsr).Do32(flags).Do32(curInstrPC).Do32(vectorBase).Do32(pid).Do32(slowPath);
        a.Do16(waitingIrqs).Do16(waitingFiqs).Fixed16(cpar).Do8(mode);
        a.DoBool(q).DoBool(t).DoBool(i).DoBool(f).DoBool(privileged).DoBool(waiting);
        a.FixedBool(waitEnabled).FixedBool(icacheIndexCompatibility);
        a.Do32(control).Do32(ttb).Do32(fsr).Do32(far).Do32(mmuSwitchCy);
        a.Do32(transTablPA).Do32(domainCfg).Do8(mmuS).Do8(mmuR);
        a.Require(!(flags & 0x0fffffff) && mmuS <= 1 && mmuR <= 1 && waitingIrqs <= 1 && waitingFiqs <= 1, "cpu flags");
        a.Require(mode == 0x10 || mode == 0x11 || mode == 0x12 || mode == 0x13 || mode == 0x17 ||
                  mode == 0x1b || mode == 0x1f, "cpu mode");
        a.Require(privileged == (mode != 0x10), "cpu privilege");
    }
    template <class A> void stateTlb(A& a) {
        a.Do16(tlbRevision).Do32(tlbFlushes).DoVector(tlb, uint64_t(2) << 20);
        a.Require(tlbRevision >= 1 && tlbRevision <= 15 && !(tlb.size() & 1), "tlb revision");
        if (!A::loading) return;
        for (size_t n = 0; n + 1 < tlb.size(); n += 2)
            a.Require(tlb[n] < (1u << 20) && !(tlb[n + 1] >> 29) && (!n || tlb[n - 2] < tlb[n]), "tlb entry");
    }
    template <class A> void stateIcache(A& a) {
        a.DoBool(icacheEnabled).DoBool(icacheRoundRobin).Do32(continuationPc).DoBuffer32(continuation, 2);
        a.DoBuffer(continuationFault, 2).Do8(continuationCount).Do32(icacheRandom).DoBuffer(icacheVictim, 8);
        a.Require(continuationCount <= 2, "icache continuation");
        for (auto& segment : instructionLines)
            for (auto& line : segment) a.Do32(line.base).DoBuffer32(line.words, 8).DoBool(line.valid);
    }
    template <class A> void stateDcache(A& a) {
        for (auto& segment : dataLines)
            for (auto& line : segment) {
                a.Do32(line.va).Do32(line.pa).DoBuffer32(line.words, 8).Do8(line.dirty);
                a.DoBool(line.valid).DoBool(line.bufferable).Require(line.dirty <= 3, "dcache dirty");
            }
        a.DoBuffer(dcacheVictim, 8).Do32(dcacheRandom).Do32(guardAddress).Do32(guardSize);
        a.DoBool(guardWrite).DoBool(guardActive);
    }
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Arm920SavedState) == 43328, "update the state lists and stateVersion");
#endif

#endif
