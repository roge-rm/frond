#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "CPU.h"
#include "cpu_diagnostics.h"
#include "imx1_boot_probe.h"
#include "patch_dispatch.h"
#include "system_state.h"

namespace {
constexpr uint32_t code = 0x1000, handler = 0x2000, trigger = 0x18000;
struct Machine {
    ArmCpu* cpu;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x20000);
    bool raiseOnStore = false, storeFiq = false;
    static bool memory(void* context, uint32_t address, uint_fast8_t size, bool write, void* data) {
        auto& m = *static_cast<Machine*>(context);
        if (address >= 0xffff0000) address -= 0xffff0000;
        if (uint64_t(address) + size > m.bytes.size()) return false;
        if (write) std::memcpy(m.bytes.data() + address, data, size);
        else std::memcpy(data, m.bytes.data() + address, size);
        if (write && address == trigger && m.raiseOnStore) {
            m.raiseOnStore = false;
            cpuIrq(m.cpu, m.storeFiq, true);
        }
        return true;
    }
    Machine(bool fast, ArmCpuModel model, bool single = true) {
        assert(cpuSetFastEngine(fast));
        auto* mem = memInit();
        assert(memRegionAdd(mem, 0, 0xffffffff, memory, this));
        auto* patches = initPatchDispatch();
        cpu = cpuInit(code, mem, ARM_MEMORY_SYSTEM_MMU, model == ArmCpuModel::armv5te, false,
                      -1, 0, 0, patches, new PacePatch{}, createSystemState(), model, single);
        patchDispatchSetCpu(patches, cpu);
    }
    void word(uint32_t address, uint32_t value) { std::memcpy(bytes.data() + address, &value, 4); }
    void half(uint32_t address, uint16_t value) { std::memcpy(bytes.data() + address, &value, 2); }
    void reset(uint32_t psr, bool high = false) {
        assert(!cpuHasPendingInterrupt(cpu));
        std::fill(bytes.begin(), bytes.end(), 0);
        cpuReset(cpu, code);
        for (uint32_t mode : {0x10u, 0x11u, 0x12u, 0x13u, 0x17u, 0x1bu, 0x1fu}) {
            cpuSetReg(cpu, ARM_REG_NUM_CPSR, mode | 0xc0);
            for (unsigned r = 8; r <= 14; ++r) cpuSetReg(cpu, r, mode * 0x1000 + r * 4);
        }
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, psr);
        for (unsigned r = 0; r < 8; ++r) cpuSetReg(cpu, r, 0x100 + r);
        cpuSetReg(cpu, 15, code | ((psr & ARM_SR_T) ? 1 : 0));
        cpuSetVectorAddr(cpu, high ? 0xffff0000 : 0);
        word(0x18, 0xe59ff000); word(0x20, handler);
        word(0x1c, 0xe59ff000); word(0x24, handler);
        word(handler, 0xe25ef004); // SUBS pc,lr,#4.
        word(code, 0xe1a00000); half(code + 8, 0x46c0);
    }
    uint32_t psr() const { return cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR); }
    void step() { assert((cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, 1) == 1)); }
};
void same(const Machine& a, const Machine& b) {
    assert(!std::memcmp(cpuGetRegisters(a.cpu), cpuGetRegisters(b.cpu), 16 * sizeof(uint32_t)));
    assert(a.psr() == b.psr());
    assert(cpuGetRegExternal(a.cpu, ARM_REG_NUM_SPSR) == cpuGetRegExternal(b.cpu, ARM_REG_NUM_SPSR));
    assert(cpuExceptionSequence(a.cpu) == cpuExceptionSequence(b.cpu));
    assert(a.bytes == b.bytes);
}
void entryMatrix() {
    for (auto model : {ArmCpuModel::arm920t, ArmCpuModel::armv5te}) {
        Machine exact(false, model), fast(true, model);
        for (bool fiq : {false, true}) for (bool thumb : {false, true}) for (bool high : {false, true})
        for (uint32_t mode : {0x10u, 0x11u, 0x12u, 0x13u, 0x17u, 0x1bu, 0x1fu})
        for (uint32_t masks : {0u, 0x40u, 0x80u, 0xc0u}) {
            const uint32_t old = 0xb0000000 | masks | mode | (thumb ? ARM_SR_T : 0);
            const bool taken = !(masks & (fiq ? ARM_SR_F : ARM_SR_I));
            const uint32_t entryMode = fiq ? 0x11 : 0x12;
            // The ARMv5TE core keeps the pre-Zodiac behaviour on purpose (patch 0786): the EVENTS
            // slow path is set for any pending event while F is clear, and entry clears F
            // instead of preserving it. The ARM920T checks are unchanged.
            const bool v5 = model == ArmCpuModel::armv5te;
            const bool slow = v5 ? !(masks & ARM_SR_F) : taken;
            uint32_t saved[16]{};
            for (auto* m : {&exact, &fast}) {
                m->reset(old, high);
                if (thumb) m->half(code, 0x46c0);
                std::memcpy(saved, cpuGetRegisters(m->cpu), sizeof(saved));
                cpuIrq(m->cpu, fiq, true);
                assert(bool(cpuGetSlowPathReason(m->cpu) & SLOW_PATH_REASON_EVENTS) == slow);
                const auto before = cpuExceptionSequence(m->cpu);
                assert(cpuProcessPendingInterrupts(m->cpu) == taken);
                assert(cpuExceptionSequence(m->cpu) == before + taken);
                assert(!cpuProcessPendingInterrupts(m->cpu));
                assert(cpuHasPendingInterrupt(m->cpu));
                if (taken) {
                    assert(m->psr() == ((old & ~0xbfu & ~(v5 ? ARM_SR_F : 0u)) | entryMode | ARM_SR_I | (fiq ? ARM_SR_F : 0)));
                    assert(cpuGetRegExternal(m->cpu, ARM_REG_NUM_SPSR) == old);
                    assert(cpuGetRegisters(m->cpu)[14] == code + 4);
                    assert(cpuGetRegisters(m->cpu)[15] == (high ? 0xffff0000u : 0) + (fiq ? 0x1c : 0x18));
                    assert(cpuGetRegisters(m->cpu)[13] == (mode == entryMode ? saved[13] : entryMode * 0x1000 + 52));
                    for (unsigned r = 0; r < ((fiq && mode != 0x11) || (!fiq && mode == 0x11) ? 8u : 13u); ++r)
                        assert(cpuGetRegisters(m->cpu)[r] == saved[r]);
                } else {
                    assert(m->psr() == old && !std::memcmp(cpuGetRegisters(m->cpu), saved, sizeof(saved)));
                }
                cpuIrq(m->cpu, fiq, false);
                assert(!cpuHasPendingInterrupt(m->cpu));
                if (taken) {
                    m->step(); assert(cpuGetRegisters(m->cpu)[15] == handler);
                    m->step(); assert(m->psr() == old && cpuGetRegisters(m->cpu)[15] == code);
                    assert(cpuGetRegisters(m->cpu)[13] == saved[13]);
                    for (unsigned r = 0; r < 13; ++r) assert(cpuGetRegisters(m->cpu)[r] == saved[r]);
                    assert(cpuGetRegisters(m->cpu)[14] == (mode == entryMode ? code + 4 : saved[14]));
                }
            }
            same(exact, fast);
        }
        // Both lines stay level asserted; FIQ enters first and IRQ follows its return.
        for (auto* m : {&exact, &fast}) {
            m->reset(0x13);
            cpuIrq(m->cpu, false, true); cpuIrq(m->cpu, true, true);
            assert(cpuProcessPendingInterrupts(m->cpu) && (m->psr() & 31) == 0x11);
            cpuIrq(m->cpu, true, false); m->step(); m->step();
            assert(m->psr() == 0x13);
            assert(cpuProcessPendingInterrupts(m->cpu) && (m->psr() & 31) == 0x12);
            cpuIrq(m->cpu, false, false); m->step(); m->step();
            assert(m->psr() == 0x13 && cpuExceptionSequence(m->cpu) == 2);
        }
        same(exact, fast);
        for (auto* m : {&exact, &fast}) {
            m->reset(0x13);
            cpuIrq(m->cpu, false, true);
            assert(cpuProcessPendingInterrupts(m->cpu) && m->psr() == 0x92);
            cpuIrq(m->cpu, true, true);
            assert(cpuProcessPendingInterrupts(m->cpu) && m->psr() == 0xd1);
            assert(cpuGetRegExternal(m->cpu, ARM_REG_NUM_SPSR) == 0x92);
            cpuIrq(m->cpu, true, false); m->step(); m->step();
            assert(m->psr() == 0x92 && cpuGetRegisters(m->cpu)[15] == 0x18);
            assert(cpuGetRegExternal(m->cpu, ARM_REG_NUM_SPSR) == 0x13);
            cpuIrq(m->cpu, false, false); m->step(); m->step();
            assert(m->psr() == 0x13 && cpuGetRegisters(m->cpu)[15] == code);
            assert(cpuExceptionSequence(m->cpu) == 2);
        }
        same(exact, fast);
    }
    std::puts("PASS: ARM920/ARMv5 ARM/Thumb IRQ/FIQ masks, priority, banked context, vectors and fetch-free entry");
}
void batchedRequests() {
    for (auto model : {ArmCpuModel::arm920t, ArmCpuModel::armv5te}) for (bool single : {false, true}) {
        Machine exact(false, model, single), fast(true, model, single);
        for (bool fiq : {false, true}) for (uint32_t masks : {0u, 0x40u, 0x80u, 0xc0u}) {
            const bool taken = !(masks & (fiq ? ARM_SR_F : ARM_SR_I));
            // The ARMv5TE core keeps the pre-Zodiac behaviour on purpose (patch 0786): the request
            // stops a batch only while F is clear, whichever line it is on. The ARM920T checks are
            // unchanged. A request that does not stop the batch is still taken afterwards.
            const bool stops = model == ArmCpuModel::armv5te ? !(masks & ARM_SR_F) : taken;
            for (auto* m : {&exact, &fast}) {
                m->reset(masks | 0x13);
                m->word(code, 0xe5810000); m->word(code + 4, 0xe3a04055); m->word(code + 8, 0xeafffffe);
                cpuSetReg(m->cpu, 1, trigger); cpuSetReg(m->cpu, 4, 0);
                m->raiseOnStore = true; m->storeFiq = fiq;
                uint64_t blocks = 0, before = 0, after = 0;
                if (m == &fast) assert(cpuJitExecutionCounts(m->cpu, &blocks, &before));
                const auto cycles = cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(m->cpu, 8);
                const bool blockLatency = m == &fast && !single && stops;
                const uint32_t expectedCycles = stops ? (blockLatency ? 3 : 1) : 8;
                if (m == &fast) {
                    assert(cpuJitExecutionCounts(m->cpu, &blocks, &after));
                    if (!(stops ? after == before + expectedCycles : after >= before + 3))
                        std::fprintf(stderr, "batch model=%u fiq=%u masks=%02x cycles=%u generated=%llu\n",
                            unsigned(model), fiq, masks, cycles, (unsigned long long)(after - before));
                    assert(stops ? after == before + expectedCycles : after >= before + 3);
                }
                assert(cycles == expectedCycles);
                const uint32_t next = code + (stops && !blockLatency ? 4 : 8);
                assert(cpuGetRegisters(m->cpu)[15] == next);
                assert(cpuGetRegisters(m->cpu)[4] == (stops && !blockLatency ? 0u : 0x55u));
                assert(cpuExceptionSequence(m->cpu) == 0);
                assert(cpuProcessPendingInterrupts(m->cpu) == taken);
                cpuIrq(m->cpu, fiq, false);
                if (taken) {
                    assert(cpuGetRegisters(m->cpu)[14] == next + 4);
                    m->step(); m->step();
                    assert(cpuGetRegisters(m->cpu)[15] == next);
                    if (next == code + 4) m->step();
                    assert(cpuGetRegisters(m->cpu)[15] == code + 8 && cpuGetRegisters(m->cpu)[4] == 0x55);
                }
            }
            // Exact stops after the store and Fast after its block, so Exact finishes the block here.
            if (stops && !taken && !single && model == ArmCpuModel::armv5te) {
                assert(cpuGetRegisters(exact.cpu)[15] == code + 4 && cpuGetRegisters(fast.cpu)[15] == code + 8);
                exact.step();
            }
            same(exact, fast);
        }
        uint64_t blocks = 0, instructions = 0;
        assert(cpuJitExecutionCounts(fast.cpu, &blocks, &instructions) && instructions);
    }
    std::puts("PASS: generated STR, bounded generic Fast block latency, exact probe boundaries and all I/F combinations");
}
void maskedSleep() {
    for (auto model : {ArmCpuModel::arm920t, ArmCpuModel::armv5te}) {
        Machine m(false, model);
        for (bool fiq : {false, true}) {
            m.reset(0xd3);
            cpuSetSleeping(m.cpu);
            cpuSetSlowPath(m.cpu, SLOW_PATH_REASON_RESCHEDULE);
            cpuIrq(m.cpu, fiq, true);
            assert(cpuHasPendingInterrupt(m.cpu) && !cpuProcessPendingInterrupts(m.cpu));
            auto reasons = cpuGetSlowPathReason(m.cpu);
            assert((reasons & (SLOW_PATH_REASON_SLEEP | SLOW_PATH_REASON_RESCHEDULE)) ==
                   (SLOW_PATH_REASON_SLEEP | SLOW_PATH_REASON_RESCHEDULE));
            assert(!(reasons & SLOW_PATH_REASON_EVENTS));
            cpuWakeup(m.cpu);
            assert(cpuHasPendingInterrupt(m.cpu) && !(cpuGetSlowPathReason(m.cpu) & SLOW_PATH_REASON_SLEEP));
            assert(cpuGetSlowPathReason(m.cpu) & SLOW_PATH_REASON_RESCHEDULE);
            cpuIrq(m.cpu, fiq, false);
            assert(!cpuHasPendingInterrupt(m.cpu));
            cpuClearSlowPath(m.cpu, SLOW_PATH_REASON_RESCHEDULE);
        }
    }
    std::puts("PASS: masked pending queries, sleep/wakeup and unrelated slow-path reasons remain independent");
}
void controllerGates() {
    Imx1Bootstrap soc;
    soc.connectCpuInterrupts();
    constexpr uint32_t base = 0x00223000;
    auto write = [&](uint32_t offset, uint32_t value, Imx1BootstrapAccess expected = Imx1BootstrapAccess::ok) {
        assert(soc.access(base + offset, 4, true, value) == expected);
    };
    write(8, 31); write(0x54, 1u << 31);
    assert(soc.interruptLines().irq == Imx1AitcLine::asserted);
    write(4, 0);
    assert(soc.interruptPending().normal == uint64_t(1) << 31);
    assert(soc.interruptLines().irq == Imx1AitcLine::clear);
    // Source 1 is a known reset-state source now, so source 6, which nothing models, stands in as the unknown one.
    write(8, 6); // Unknown source 6 is ineligible at its reset priority zero.
    assert(soc.clockStatus() == Imx1BootstrapAccess::ok);
    write(4, 31, Imx1BootstrapAccess::externalInput);
    assert(soc.interruptLines().irq == Imx1AitcLine::clear);
    write(0x30, 0x10000000); // Source 31 priority 1, source 6 still masked.
    assert(soc.interruptLines().irq == Imx1AitcLine::asserted);
    write(4, 31, Imx1BootstrapAccess::externalInput); // A known request does not hide eligible uncertainty.
    write(0x1c, 1u << 6, Imx1BootstrapAccess::externalInput); // FIQ bypasses NIMASK.
    write(0x0c, 6); write(4, 31);
    write(0x1c, 1u << 31);
    assert(soc.interruptLines().irq == Imx1AitcLine::clear && soc.interruptLines().fiq == Imx1AitcLine::asserted);
    write(0x54, 0);
    assert(soc.interruptLines().fiq == Imx1AitcLine::clear);
    std::puts("PASS: NIMASK eligibility, force clearing and transactional eligible-unknown guards");
}
void continuation() {
    Machine exact(false, ArmCpuModel::arm920t), fast(true, ArmCpuModel::arm920t);
    for (bool cached : {false, true}) {
        for (auto* m : {&exact, &fast}) {
            m->reset(0x53);
            m->word(0x4000, 0xc0a);
            auto cp = [&](uint32_t address, uint32_t instruction, uint32_t value) {
                m->word(address, instruction); cpuSetReg(m->cpu, 0, value);
                cpuSetReg(m->cpu, 15, address); m->step();
            };
            cp(0x3000, 0xee020f10, 0x4000); cp(0x3020, 0xee030f10, 1);
            m->word(code + 4, 0xe3a03055); m->word(code + 8, 0xe1a00000);
            cp(code, 0xee010f10, cached ? 0x1079 : 0x79);
            const uint32_t resume = cached ? 0x1800 : code + 4;
            if (cached) {
                m->word(resume, 0xe3a03055); cpuSetReg(m->cpu, 15, resume);
                uint32_t instruction = 0; uint_fast8_t fsr = 0;
                assert(cpuFetchInstruction(m->cpu, resume, &instruction, &fsr) && instruction == 0xe3a03055);
            }
            m->word(resume, 0xe3a03077);
            cpuIrq(m->cpu, false, true);
            assert(cpuProcessPendingInterrupts(m->cpu)); cpuIrq(m->cpu, false, false);
            m->step(); m->step();
            assert(cpuGetRegisters(m->cpu)[15] == resume && m->psr() == 0x53);
            m->step(); assert(cpuGetRegisters(m->cpu)[3] == (cached ? 0x55u : 0x77u));
        }
        same(exact, fast);
    }
    for (auto* m : {&exact, &fast}) {
        m->reset(0x13); cpuSetReg(m->cpu, 15, 0x30000);
        cpuIrq(m->cpu, false, true);
        assert(cpuProcessPendingInterrupts(m->cpu)); cpuIrq(m->cpu, false, false);
        assert(cpuExceptionSequence(m->cpu) == 1 && cpuGetRegisters(m->cpu)[15] == 0x18);
        assert(cpuGetRegisters(m->cpu)[14] == 0x30004);
    }
    same(exact, fast);
    std::puts("PASS: interrupt entry flushes MMU continuation, retains cached words and precedes pending fetch faults");
}
struct Trace {
    std::vector<Imx1ProbeTrace> instructions;
    std::vector<Imx1ProbeAccess> bus;
    std::vector<Imx1ProbeInterrupt> interrupts;
};
void load(std::vector<uint32_t>& words, unsigned reg, uint32_t value) {
    words.push_back(0xe59f0000 | reg << 12); words.push_back(0xea000000); words.push_back(value);
}
void store(std::vector<uint32_t>& words, unsigned base, uint32_t offset, uint32_t value) {
    load(words, 1, value); words.push_back(0xe5801000 | base << 16 | offset);
}
Imx1ProbeResult probe(const std::vector<uint32_t>& words, Trace& trace, bool fast, uint32_t steps = 500,
                     bool settled = false, bool gpio = false) {
    Imx1ProbeOptions options{};
    options.bootstrapPeripherals = true;
    options.rtcInitialTimeKnown = true;
    options.gpioAInputKnown = gpio;
    options.gpioAInput = 0;
    options.assumeMpllSettled = options.assumeSpllSettled = settled;
    options.busObserver = [](void* p, const Imx1ProbeAccess& a) { static_cast<Trace*>(p)->bus.push_back(a); };
    options.busContext = &trace;
    options.interruptObserver = [](void* p, const Imx1ProbeInterrupt& a) { static_cast<Trace*>(p)->interrupts.push_back(a); };
    options.interruptContext = &trace;
    return imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4,
        Imx1ProbeEntry::resetAlias, steps, [](void* p, const Imx1ProbeTrace& t) {
            static_cast<Trace*>(p)->instructions.push_back(t);
        }, &trace, fast, &options);
}
void probeParity(const std::vector<uint32_t>& words, Imx1ProbeResult& result, Trace& trace, uint32_t steps = 500,
                 bool settled = false, bool gpio = false) {
    result = probe(words, trace, false, steps, settled, gpio);
    Trace other;
    const auto fast = probe(words, other, true, steps, settled, gpio);
    assert(result.stop == fast.stop && result.trace.step == fast.trace.step && result.cpsr == fast.cpsr);
    assert(result.spsr == fast.spsr && result.irqEntries == fast.irqEntries && result.fiqEntries == fast.fiqEntries);
    assert(!std::memcmp(result.registers, fast.registers, sizeof(result.registers)));
    assert(trace.instructions.size() == other.instructions.size() && trace.bus.size() == other.bus.size());
    for (size_t n = 0; n < trace.instructions.size(); ++n) {
        const auto& a = trace.instructions[n]; const auto& b = other.instructions[n];
        assert(a.step == b.step && a.pc == b.pc && a.cpsr == b.cpsr && a.instruction == b.instruction);
    }
    for (size_t n = 0; n < trace.bus.size(); ++n) {
        const auto& a = trace.bus[n]; const auto& b = other.bus[n];
        assert(a.step == b.step && a.pc == b.pc && a.address == b.address && a.value == b.value &&
               a.write == b.write && a.status == b.status);
    }
    assert(trace.interrupts.size() == other.interrupts.size());
    for (size_t n = 0; n < trace.interrupts.size(); ++n) {
        const auto& a = trace.interrupts[n]; const auto& b = other.interrupts[n];
        assert(a.step == b.step && a.interruptedPc == b.interruptedPc && a.vector == b.vector && a.link == b.link);
        assert(a.cpsr == b.cpsr && a.fiq == b.fiq && a.pending.normal == b.pending.normal &&
               a.pending.fast == b.pending.fast && a.pending.unknownNormal == b.pending.unknownNormal &&
               a.pending.unknownFast == b.pending.unknownFast);
        assert(trace.instructions[a.step].pc == a.vector && trace.instructions[a.step].instructionSize == 4);
        assert(a.link == a.interruptedPc + 4);
    }
    assert(fast.jitBlocks && fast.jitInstructions);
}
std::vector<uint32_t> setup() {
    std::vector<uint32_t> words(0x100 / 4, 0xe1a00000);
    words[0] = 0xea00003e; words[0x18 / 4] = 0xea0000f8; words[0x1c / 4] = 0xea0000f7; // Both vectors branch to 0x400.
    load(words, 0, 0x0021b000); store(words, 0, 0, 0x57000003); store(words, 0, 0x20, 0);
    return words;
}
void installTimerHandler(std::vector<uint32_t>& words) {
    words.resize(0x400 / 4, 0xe1a00000);
    load(words, 9, 0x00203000);
    words.push_back(0xe3a0a000); words.push_back(0xe589a000); // Disable IRQEN and counting without acknowledging COMP.
    words.push_back(0xe599b014); words.push_back(0xe589a014); // Read COMP, then write zero to clear it.
    words.push_back(0xe2877001); words.push_back(0xe25ef004);
}
void timerDelivery() {
    for (bool fiq : {false, true}) for (bool pendingAtEnable : {false, true}) for (bool thumb : {false, true}) {
        auto words = setup();
        load(words, 0, 0x00223000);
        if (fiq) store(words, 0, 0x18, 1u << 26);
        if (!pendingAtEnable) store(words, 0, 8, 58);
        load(words, 0, 0x00203000); store(words, 0, 8, 20); store(words, 0, 0, 0x13);
        words.push_back(0xe3a07000);
        if (pendingAtEnable) {
            for (unsigned n = 0; n < 25; ++n) words.push_back(0xe1a00000);
            load(words, 0, 0x00223000); store(words, 0, 8, 58);
        }
        words.push_back(0xe321f013); // Enable IRQ and FIQ.
        if (thumb) {
            load(words, 2, uint32_t(words.size() * 4 + 16 + 1)); words.push_back(0xe12fff12);
            for (unsigned n = 0; n < 40; ++n) words.push_back(0x46c046c0);
            words.push_back(0xdefe46c0);
        } else {
            for (unsigned n = 0; n < 40; ++n) words.push_back(0xe1a00000);
            words.push_back(0xe7f000f0);
        }
        installTimerHandler(words);
        Trace trace; Imx1ProbeResult result{}; probeParity(words, result, trace);
        assert(result.stop == Imx1ProbeStop::unsupportedInstruction && result.irqEntries == !fiq && result.fiqEntries == fiq);
        assert(result.registers[7] == 1 && trace.interrupts.size() == 1);
        const auto& entry = trace.interrupts.front();
        assert(entry.pending.normal == (fiq ? 0 : uint64_t(1) << 58));
        assert(entry.pending.fast == (fiq ? uint64_t(1) << 58 : 0));
        bool readPending = false, cleared = false;
        for (const auto& a : trace.bus) {
            if (a.address == 0x00203014 && !a.write) { assert(a.value == 1); readPending = true; }
            if (a.address == 0x00203014 && a.write) { assert(readPending && a.value == 0); cleared = true; }
        }
        assert(cleared);
    }
    std::puts("PASS: elapsed and MMIO-pending Timer 2 IRQ/FIQ, vector traces and read-before-W0C acknowledgement");
}
void gpioDelivery() {
    constexpr uint32_t pins = 0x0021c000, bit = 1u << 13;
    for (bool fiq : {false, true}) {
        auto words = setup();
        load(words, 0, pins); store(words, 0, 0x28, 3u << 26);
        load(words, 0, 0x00223000);
        if (fiq) store(words, 0, 0x1c, 1u << 11);
        store(words, 0, 8, 11);
        words.push_back(0xe3a07000); words.push_back(0xe321f013);
        load(words, 0, pins);
        const auto triggerStepPc = uint32_t(words.size() * 4 + 12);
        store(words, 0, 0x30, bit);
        words.push_back(0xe1a00000); words.push_back(0xe7f000f0);
        words.resize(0x400 / 4, 0xe1a00000);
        load(words, 9, pins);
        words.push_back(0xe599a034); // Read the latched GPIO status.
        words.push_back(0xe3a0b000); words.push_back(0xe589b030); // Mask before W1C of a held level.
        words.push_back(0xe589a034); words.push_back(0xe599b034);
        words.push_back(0xe2877001); words.push_back(0xe25ef004);
        Trace trace; Imx1ProbeResult result{};
        probeParity(words, result, trace, 500, false, true);
        assert(result.stop == Imx1ProbeStop::unsupportedInstruction);
        assert(result.irqEntries == !fiq && result.fiqEntries == fiq && result.registers[7] == 1);
        assert(trace.interrupts.size() == 1 && trace.interrupts[0].interruptedPc == triggerStepPc + 4);
        bool pendingRead = false, masked = false, acknowledged = false, clearRead = false;
        for (const auto& access : trace.bus) {
            if (access.address == pins + 0x34 && !access.write) {
                if (!pendingRead) { assert(access.value == bit); pendingRead = true; }
                else { assert(acknowledged && access.value == 0); clearRead = true; }
            }
            if (access.address == pins + 0x30 && access.write && access.value == 0) masked = true;
            if (access.address == pins + 0x34 && access.write) {
                assert(masked && pendingRead && access.value == bit); acknowledged = true;
            }
        }
        assert(clearRead);
    }
    std::puts("PASS: generated GPIO unmask, next-instruction IRQ/FIQ entry, ISR read and mask/W1C acknowledgement");
}
void thumbPrefix() {
    for (bool fiq : {false, true}) for (bool changedReturn : {false, true})
    for (bool ldmReturn : {false, true}) {
        auto words = setup();
        load(words, 0, 0x00223000);
        if (fiq) store(words, 0, 0x18, 1u << 26);
        store(words, 0, 8, 58);
        load(words, 0, 0x00203000); store(words, 0, 8, 6); store(words, 0, 0, 0x13);
        words.push_back(0xe321f013);
        const uint32_t thumbPc = words.size() * 4 + 16;
        load(words, 2, thumbPc | 1); words.push_back(0xe12fff12);
        words.push_back(0xf806f000); words.push_back(0xde00de00);
        words.push_back(0xde00f800); words.push_back(0xde00de00);
        words.push_back(0x47702655);
        installTimerHandler(words);
        if (changedReturn) {
            words.back() = 0xe28ee006;
            words.push_back(0xe25ef004);
        }
        if (ldmReturn) {
            words.back() = 0xe24ee004; // Store the interrupted PC in SRAM before returning.
            load(words, 8, 0x00300000);
            words.push_back(0xe588e000);
            words.push_back(0xe8d88000); // LDMIA r8,{pc}^.
        }
        Trace trace; Imx1ProbeResult result{}; probeParity(words, result, trace);
        assert(result.stop == Imx1ProbeStop::unsupportedInstruction && trace.interrupts.size() == 1);
        assert(trace.interrupts[0].interruptedPc == thumbPc + 2 && (trace.interrupts[0].cpsr & ARM_SR_T));
        assert(result.trace.pc == thumbPc + (changedReturn ? 8 : 4));
        assert(result.trace.instruction == (changedReturn ? 0xf800u : 0xde00u));
        assert(result.registers[6] == (changedReturn ? 0u : 0x55u));
    }
    std::puts("PASS: IRQ/FIQ SUBS/LDM returns between Thumb BL halves restore prefix context and changed returns drop it");
}
void suspendedThumbPrefix() {
    for (bool fiq : {false, true}) for (unsigned switches : {1u, 3u})
    for (unsigned damaged : {0u, 1u, 2u}) {
        auto words = setup();
        load(words, 0, 0x00223000);
        if (fiq) store(words, 0, 0x18, 1u << 26);
        store(words, 0, 8, 58);
        load(words, 0, 0x00203000); store(words, 0, 8, 6); store(words, 0, 0, 0x13);
        words.push_back(0xe321f013);
        const uint32_t thumbPc = words.size() * 4 + 16;
        load(words, 2, thumbPc | 1); words.push_back(0xe12fff12);
        words.push_back(0xf806f000); words.push_back(0xde00de00);
        words.push_back(0xde00de00); words.push_back(0xde00de00);
        words.push_back(0x47702655);
        words.resize(0x300 / 4, 0xe1a00000);
        if (damaged == 1) load(words, 14, 0x200);
        load(words, 0, 0x00203000); store(words, 0, 8, 6); store(words, 0, 0, 0x13);
        for (unsigned n = 0; n < 30; ++n) words.push_back(0xe1a00000);
        words.push_back(0xe7f000f0);
        installTimerHandler(words);
        words.pop_back(); // Replace the handler's direct SUBS return.
        words.push_back(0xe3570001);
        const size_t skipSave = words.size(); words.push_back(0);
        load(words, 9, 0x00300000);
        words.push_back(0xe14f8000); words.push_back(0xe5898000);
        words.push_back(0xe24ee004); words.push_back(0xe589e004);
        const uint32_t choose = words.size() * 4;
        words[skipSave] = 0x1a000000 | ((choose - (skipSave * 4 + 8)) / 4);
        words.push_back(0xe3570000 | (switches + 1));
        const size_t finish = words.size(); words.push_back(0);
        load(words, 9, 0x00300000); words.push_back(0xe5998000);
        words.push_back(0xe3c88020); words.push_back(0xe169f008);
        load(words, 14, 0x300); words.push_back(0xe1b0f00e);
        const uint32_t resume = words.size() * 4;
        words[finish] = 0x0a000000 | ((resume - (finish * 4 + 8)) / 4);
        load(words, 9, 0x00300000); words.push_back(0xe5998000);
        if (damaged == 2) { load(words, 10, 0x80000000); words.push_back(0xe028800a); }
        words.push_back(0xe169f008); words.push_back(0xe599e004); words.push_back(0xe1b0f00e);
        Trace trace; Imx1ProbeResult result{}; probeParity(words, result, trace, 1000);
        assert(result.stop == Imx1ProbeStop::unsupportedInstruction);
        assert(trace.interrupts.size() == switches + 1);
        assert(result.irqEntries == (fiq ? 0 : switches + 1));
        assert(result.fiqEntries == (fiq ? switches + 1 : 0));
        assert(trace.interrupts[0].interruptedPc == thumbPc + 2);
        assert(result.trace.pc == thumbPc + (damaged ? 2 : 4));
        assert(result.trace.instruction == (damaged ? 0xf806u : 0xde00u));
        assert(result.registers[6] == (damaged ? 0u : 0x55u));
        assert(result.registers[7] == switches + 1);
    }
    std::puts("PASS: suspended Thumb BL resumes after IRQ/FIQ task switches; wrong LR/flags remain guarded");
}
void sharedThumbPrefix() {
    for (bool fiq : {false, true}) {
        auto words = setup();
        load(words, 13, 0x00300100); load(words, 3, 0x380);
        load(words, 0, 0x00223000);
        if (fiq) store(words, 0, 0x18, 1u << 26);
        store(words, 0, 8, 58);
        load(words, 0, 0x00203000); store(words, 0, 8, 6); store(words, 0, 0, 0x13);
        words.push_back(0xe321f01f);
        const uint32_t thumbPc = words.size() * 4 + 16;
        load(words, 2, thumbPc | 1); words.push_back(0xe12fff12);
        words.push_back(0xf806f000); words.push_back(0xde004718);
        words.push_back(0xde00de00); words.push_back(0xde00de00);
        words.push_back(0x47702655);
        words.resize(0x300 / 4, 0xe1a00000);
        load(words, 13, 0x00300200);
        load(words, 0, 0x00203000); store(words, 0, 8, 6); store(words, 0, 0, 0x13);
        words.push_back(0xe321f01f);
        load(words, 2, thumbPc | 1); words.push_back(0xe12fff12);
        words.resize(0x380 / 4, 0xe1a00000);
        words.push_back(0xe2855001); words.push_back(0xe3550002);
        const size_t completed = words.size(); words.push_back(0);
        load(words, 0, 0x00203000); store(words, 0, 8, 6); store(words, 0, 0, 0x13);
        for (unsigned n = 0; n < 12; ++n) words.push_back(0xe1a00000);
        const uint32_t end = words.size() * 4;
        words[completed] = 0x0a000000 | ((end - (completed * 4 + 8)) / 4);
        words.push_back(0xe7f000f0);
        installTimerHandler(words); words.pop_back();
        words.push_back(0xe3570001);
        const size_t later = words.size(); words.push_back(0);
        load(words, 9, 0x00300000); words.push_back(0xe14f8000); words.push_back(0xe5898000);
        words.push_back(0xe24ee004); words.push_back(0xe589e004);
        words.push_back(0xe289a008); words.push_back(0xe8ca6000);
        words.push_back(0xe3c88020); words.push_back(0xe169f008);
        load(words, 14, 0x300); words.push_back(0xe1b0f00e);
        const uint32_t select = words.size() * 4;
        words[later] = 0x1a000000 | ((select - (later * 4 + 8)) / 4);
        load(words, 9, 0x00300000); words.push_back(0xe3570002);
        const size_t third = words.size(); words.push_back(0);
        words.push_back(0xe289a010); words.push_back(0xe8ca2000);
        words.push_back(0xe289a008);
        const size_t restore = words.size(); words.push_back(0);
        const uint32_t secondTask = words.size() * 4;
        words[third] = 0x1a000000 | ((secondTask - (third * 4 + 8)) / 4);
        words.push_back(0xe289a010);
        const uint32_t loadTask = words.size() * 4;
        words[restore] = 0xea000000 | ((loadTask - (restore * 4 + 8)) / 4);
        words.push_back(0xe8da2000); words.push_back(0xe289a00c); words.push_back(0xe8da4000);
        words.push_back(0xe5998000); words.push_back(0xe169f008);
        words.push_back(0xe599e004); words.push_back(0xe1b0f00e);
        Trace trace; Imx1ProbeResult result{}; probeParity(words, result, trace, 1000);
        assert(result.stop == Imx1ProbeStop::unsupportedInstruction && result.trace.pc == end);
        assert(trace.interrupts.size() == 3 && result.registers[5] == 2 && result.registers[6] == 0x55);
        assert(trace.interrupts[0].interruptedPc == thumbPc + 2);
        assert(trace.interrupts[1].interruptedPc == thumbPc + 2);
        assert(trace.interrupts[0].cpsr == trace.interrupts[1].cpsr);
        assert(result.registers[13] == 0x00300200);
    }
    std::puts("PASS: two tasks sharing a Thumb BL pair retain separate interrupted stacks through IRQ/FIQ resumption");
}
void returnGuards() {
    for (uint32_t mode : {0x10u, 0x11u, 0x12u, 0x13u, 0x17u, 0x1bu, 0x1fu})
    for (uint32_t saved : {0u, 0xd3u, 0xf3u, 0xd4u, 0x10000d3u})
    for (uint32_t target : {0x100u, 0x101u, 0x102u}) {
        std::vector<uint32_t> words;
        load(words, 0, saved); words.push_back(0xe16ff000);
        words.push_back(0xe321f000 | mode);
        load(words, 14, target + 4);
        const uint32_t returnPc = words.size() * 4; words.push_back(0xe25ef004);
        words.resize(0x108 / 4, 0xe7f000f0);
        if (saved & ARM_SR_T) words[target / 4] = 0xde00de00;
        Trace trace; Imx1ProbeResult result{}; probeParity(words, result, trace);
        const bool valid = mode != 0x10 && mode != 0x1f && (saved == 0xd3 || saved == 0xf3) &&
                           !(target & (saved & ARM_SR_T ? 1u : 3u));
        assert(result.stop == Imx1ProbeStop::unsupportedInstruction);
        // SPSR was initialised in SVC; other exception banks retain reset zero.
        const bool accepted = valid && mode == 0x13;
        assert(result.trace.pc == (accepted ? target : returnPc));
        if (accepted) assert(result.cpsr == saved);
        else assert(result.trace.instruction == 0xe25ef004 && result.registers[14] == target + 4);
    }
    std::puts("PASS: SUBS pc,lr,#4 source mode, saved status and ARM/Thumb alignment guards");
}
void stalledDelivery() {
    std::vector<uint32_t> words(0x100 / 4, 0xe1a00000);
    words[0] = 0xea00003e; words[0x18 / 4] = 0xe7f000f0;
    load(words, 0, 0x00223000); store(words, 0, 8, 18);
    load(words, 0, 0x00204000); store(words, 0, 0x18, 0x8000);
    for (unsigned n = 0; n < 15200; ++n) words.push_back(0xe1a00000);
    load(words, 0, 0x0021b000); store(words, 0, 0, 0x57000003);
    store(words, 0, 4, 0x03e717bf); store(words, 0, 0, 0x57200003);
    for (unsigned n = 0; n < 6000; ++n) words.push_back(0xe1a00000);
    load(words, 1, 0xc0000078); words.push_back(0xee011f10);
    store(words, 0, 0x0c, 0x123638ad); load(words, 1, 0x57400003);
    words.push_back(0xe321f013);
    const uint32_t restart = words.size() * 4; words.push_back(0xe5801000);
    words.push_back(0xe3a07055); words.push_back(0xe7f000f0);
    Trace trace; Imx1ProbeResult result{}; probeParity(words, result, trace, 30000, true);
    assert(result.stop == Imx1ProbeStop::unsupportedInstruction && result.trace.pc == 0x18 && result.irqEntries == 1);
    assert(result.registers[7] == 0 && trace.interrupts.size() == 1);
    assert(trace.interrupts[0].interruptedPc == restart + 4);
    assert(trace.bus.back().pc == restart && trace.bus.back().value == 0x57400003);
    assert(result.trace.step == trace.bus.back().step + 1);
    std::puts("PASS: elapsed RTC request during SPLL stall enters before preflight and vector qualification");
}
} // namespace

int main() {
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT", "1", 1);
    entryMatrix();
    batchedRequests();
    maskedSleep();
    controllerGates();
    continuation();
    timerDelivery();
    gpioDelivery();
    thumbPrefix();
    suspendedThumbPrefix();
    sharedThumbPrefix();
    returnGuards();
    stalledDelivery();
    return 0;
}
