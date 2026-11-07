#include <algorithm>
#include <array>
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
constexpr uint32_t code = 0x1000, handler = 0x2000;
struct Machine {
    ArmCpu* cpu;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x20000);
    static bool memory(void* context, uint32_t address, uint_fast8_t size, bool write, void* value) {
        auto& m = *static_cast<Machine*>(context);
        if (address >= 0xffff0000) address -= 0xffff0000;
        if (uint64_t(address) + size > m.bytes.size()) return false;
        if (write) std::memcpy(m.bytes.data() + address, value, size);
        else std::memcpy(value, m.bytes.data() + address, size);
        return true;
    }
    explicit Machine(bool fast, bool singleStep = true, ArmCpuModel model = ArmCpuModel::arm920t) {
        assert(cpuSetFastEngine(fast));
        auto* mem = memInit();
        assert(memRegionAdd(mem, 0, 0xffffffff, memory, this));
        auto* patches = initPatchDispatch();
        cpu = cpuInit(code, mem, ARM_MEMORY_SYSTEM_MMU, model == ArmCpuModel::armv5te, false,
                      -1, 0, 0, patches, new PacePatch{}, createSystemState(), model, singleStep);
        patchDispatchSetCpu(patches, cpu);
    }
    void word(uint32_t address, uint32_t value) { std::memcpy(bytes.data() + address, &value, 4); }
    void half(uint32_t address, uint16_t value) { std::memcpy(bytes.data() + address, &value, 2); }
    void reset(uint32_t psr = 0xd3, bool high = false) {
        std::fill(bytes.begin(), bytes.end(), 0);
        cpuReset(cpu, code);
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, 0xd3);
        cpuSetReg(cpu, 13, 0x9000); cpuSetReg(cpu, 14, 0x12345678);
        for (unsigned i = 0; i < 13; ++i) cpuSetReg(cpu, i, 0x12340000 + i);
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, psr);
        cpuSetReg(cpu, 15, code | ((psr & 0x20) ? 1 : 0));
        cpuSetVectorAddr(cpu, high ? 0xffff0000 : 0);
        word(8, 0xe59ff018); word(0x28, handler);
        word(handler, 0xe1b0f00e); // MOVS pc,lr
    }
    void step() { assert((cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, 1) == 1)); }
    void at(uint32_t pc) { cpuSetReg(cpu, 15, pc); step(); }
    uint32_t psr() { return cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR); }
    uint32_t spsr() { return cpuGetRegExternal(cpu, ARM_REG_NUM_SPSR); }
};
void same(const Machine& a, const Machine& b) {
    assert(!std::memcmp(cpuGetRegisters(a.cpu), cpuGetRegisters(b.cpu), 16 * sizeof(uint32_t)));
    assert(cpuGetRegExternal(a.cpu, ARM_REG_NUM_CPSR) == cpuGetRegExternal(b.cpu, ARM_REG_NUM_CPSR));
    assert(cpuGetRegExternal(a.cpu, ARM_REG_NUM_SPSR) == cpuGetRegExternal(b.cpu, ARM_REG_NUM_SPSR));
    assert(cpuExceptionSequence(a.cpu) == cpuExceptionSequence(b.cpu));
    assert(a.bytes == b.bytes);
}
void software() {
    for (auto model : {ArmCpuModel::arm920t, ArmCpuModel::armv5te}) {
        Machine exact(false, true, model), fast(true, true, model);
        for (uint32_t immediate : {0u, 1u, 3u, 0x37u, 0xacu, 0xffu})
            for (bool thumb : {false, true}) for (bool high : {false, true})
            for (unsigned mode : {0x10u, 0x11u, 0x12u, 0x13u, 0x17u, 0x1bu, 0x1fu}) for (unsigned masks : {0u, 0x40u, 0x80u, 0xc0u}) {
                const uint32_t old = 0xb0000000 | (model == ArmCpuModel::armv5te ? 0x08000000 : 0) |
                                     mode | masks | (thumb ? 0x20 : 0);
                const uint32_t next = code + (thumb ? 2 : 4);
                for (Machine* m : {&exact, &fast}) {
                    m->reset(old, high);
                    for (unsigned n = 0; n < 13; ++n) cpuSetReg(m->cpu, n, 0x12340000 + n);
                    cpuSetReg(m->cpu, 13, 0xa000); cpuSetReg(m->cpu, 14, 0x87654320);
                    if (thumb) { m->half(code, 0xdf00 | immediate); m->half(next, 0x2407); }
                    else { m->word(code, 0xef000000 | immediate); m->word(next, 0xe3a04007); }
                }
                exact.step(); fast.step(); same(exact, fast);
                assert(cpuExceptionSequence(exact.cpu) == 1);
                assert(exact.psr() == ((old & ~0xbfu) | 0x93) && exact.spsr() == old);
                assert(cpuGetRegisters(exact.cpu)[14] == next);
                assert(cpuGetRegisters(exact.cpu)[15] == (high ? 0xffff0008u : 8u));
                assert(cpuGetRegisters(exact.cpu)[13] == (mode == 0x13 ? 0xa000u : 0x9000u));
                exact.step(); fast.step(); same(exact, fast);
                assert(cpuGetRegisters(exact.cpu)[15] == handler);
                exact.step(); fast.step(); same(exact, fast);
                assert(exact.psr() == old && cpuGetRegisters(exact.cpu)[15] == next);
                assert(cpuGetRegisters(exact.cpu)[13] == 0xa000);
                for (unsigned n = 0; n < 13; ++n) assert(cpuGetRegisters(exact.cpu)[n] == 0x12340000 + n);
                assert(cpuGetRegisters(exact.cpu)[14] == (mode == 0x13 ? next : 0x87654320));
                exact.step(); fast.step(); same(exact, fast);
                assert(cpuGetRegisters(exact.cpu)[4] == 7 && cpuExceptionSequence(exact.cpu) == 1);
            }
        // Conditional SWIs do not enter or alter the banked exception state.
        for (Machine* m : {&exact, &fast}) { m->reset(0x20000053); m->word(code, 0x0f000003); }
        exact.step(); fast.step(); same(exact, fast);
        assert(cpuExceptionSequence(exact.cpu) == 0 && exact.psr() == 0x20000053 &&
               cpuGetRegisters(exact.cpu)[15] == code + 4);
    }
}
void otherExceptions() {
    for (auto model : {ArmCpuModel::arm920t, ArmCpuModel::armv5te}) {
        Machine exact(false, true, model), fast(true, true, model);
        for (unsigned kind = 0; kind < 5; ++kind) for (uint32_t f : {0u, 0x40u}) {
            const uint32_t old = 0xa0000013 | f;
            const unsigned mode = kind == 0 ? 0x1b : kind < 3 ? 0x17 : kind == 3 ? 0x12 : 0x11;
            const uint32_t vector = kind == 0 ? 4 : kind == 1 ? 0xc : kind == 2 ? 0x10 : kind == 3 ? 0x18 : 0x1c;
            for (Machine* m : {&exact, &fast}) {
                m->reset(old);
                m->word(vector, 0xe1a00000);
                m->word(code, kind == 0 ? 0xe7f000f0 : kind == 2 ? 0xe5901000 : 0xe1a00000);
                cpuSetReg(m->cpu, 0, 0x30000);
                if (kind == 1) cpuSetReg(m->cpu, 15, 0x30000);
                if (kind >= 3) cpuIrq(m->cpu, kind == 4, true);
            }
            exact.step(); fast.step(); same(exact, fast);
            if (kind >= 3) { cpuIrq(exact.cpu, kind == 4, false); cpuIrq(fast.cpu, kind == 4, false); }
            // A masked FIQ remains pending and executes the original instruction.
            if (kind == 4 && f) {
                assert(!cpuExceptionSequence(exact.cpu) && exact.psr() == old &&
                       cpuGetRegisters(exact.cpu)[15] == code + 4);
                continue;
            }
            assert(cpuExceptionSequence(exact.cpu) == 1 && exact.spsr() == old);
            assert(exact.psr() == (0xa0000080 | mode | (kind == 4 ? 0x40 : f)));
            assert(cpuGetRegisters(exact.cpu)[14] == (kind == 1 ? 0x30004u : code + (kind == 2 ? 8 : 4)));
            assert(cpuGetRegisters(exact.cpu)[15] == vector + (kind >= 3 ? 4 : 0));
        }
        cpuSetReg(exact.cpu, ARM_REG_NUM_CPSR, 0x13);
        cpuReset(exact.cpu, code);
        assert((exact.psr() & 0xff) == 0xd3);
    }
}
void normalBlocks() {
    Machine exact(false), fast(true, false);
    for (bool thumb : {false, true}) {
        for (Machine* m : {&exact, &fast}) {
            m->reset(thumb ? 0x300000f3 : 0x300000d3);
            if (thumb) { m->half(code, 0xdf03); m->half(code + 2, 0x2407); m->half(code + 4, 0xe7fe); }
            else { m->word(code, 0xef000003); m->word(code + 4, 0xe3a04007); m->word(code + 8, 0xeafffffe); }
            m->word(handler, 0xe3a05009); m->word(handler + 4, 0xe1b0f00e);
        }
        uint64_t blocks = 0, instructions = 0;
        assert(cpuJitExecutionCounts(fast.cpu, &blocks, &instructions));
        const auto before = instructions;
        for (unsigned i = 0; i < 5; ++i) exact.step();
        // State changes may end a batch early. The terminal self-branch bounds the checkpoint.
        unsigned batches = 0;
        while (cpuGetRegisters(fast.cpu)[15] != code + (thumb ? 4 : 8)) {
            assert(++batches <= 4);
            const auto cycles = cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(fast.cpu, 5);
            assert(cycles && cycles <= 6);
        }
        same(exact, fast);
        assert(cpuGetRegisters(fast.cpu)[4] == 7 && cpuGetRegisters(fast.cpu)[5] == 9);
        assert(cpuJitExecutionCounts(fast.cpu, &blocks, &instructions) && blocks && instructions - before >= 3);
        std::printf("PASS: %s SWI/vector/return generated instructions=%llu\n", thumb ? "Thumb" : "ARM",
                    static_cast<unsigned long long>(instructions - before));
    }
}
void fetchContinuation() {
    Machine exact(false), fast(true);
    for (bool cached : {false, true}) {
        for (Machine* m : {&exact, &fast}) {
            m->reset();
            m->word(0x4000, 0xc0a); // Identity section, AP3 and cacheable.
            m->word(0x3000, 0xee020f10); m->word(0x3020, 0xee030f10);
            cpuSetReg(m->cpu, 0, 0x4000); m->at(0x3000);
            cpuSetReg(m->cpu, 0, 1); m->at(0x3020);
            m->word(code, 0xee010f10);
            m->word(code + 4, 0xef000003); m->word(code + 8, 0xe3a03055);
            m->word(0x1800, 0xef000003); m->word(0x1804, 0xe3a03055);
            cpuSetReg(m->cpu, 0, cached ? 0x1079 : 0x79); m->at(code);
            // The uncached MMU handoff retained the old word at code+8. With I enabled,
            // a diagnostic fetch fills its retained cache line before backing memory changes.
            if (cached) {
                uint32_t instruction = 0; uint_fast8_t fsr = 0;
                cpuSetReg(m->cpu, 15, 0x1800);
                assert(cpuFetchInstruction(m->cpu, 0x1804, &instruction, &fsr));
                assert(instruction == 0xe3a03055);
            }
            m->word(cached ? 0x1804 : code + 8, 0xe3a03077);
        }
        for (unsigned i = 0; i < 4; ++i) { exact.step(); fast.step(); same(exact, fast); }
        assert(cpuGetRegisters(exact.cpu)[15] == (cached ? 0x1808u : code + 12) && cpuExceptionSequence(exact.cpu) == 1);
        assert(cpuGetRegisters(exact.cpu)[3] == (cached ? 0x55u : 0x77u));
        assert(exact.psr() == 0xd3);
    }
    for (Machine* m : {&exact, &fast}) {
        m->reset();
        m->word(0x4000, 0xc0a);
        m->word(0x3000, 0xee020f10); m->word(0x3020, 0xee030f10);
        cpuSetReg(m->cpu, 0, 0x4000); m->at(0x3000);
        cpuSetReg(m->cpu, 0, 1); m->at(0x3020);
        cpuSetReg(m->cpu, 0, 0x79);
        m->word(code, 0xef000003);
        m->word(handler, 0xe59fe014); m->word(handler + 0x1c, handler + 12);
        m->word(handler + 4, 0xee010f10);
        m->word(handler + 8, 0xe1b0f00e); m->word(handler + 12, 0xe3a03055);
        cpuSetReg(m->cpu, 15, code);
    }
    for (unsigned i = 0; i < 4; ++i) { exact.step(); fast.step(); same(exact, fast); }
    for (Machine* m : {&exact, &fast}) m->word(handler + 12, 0xe3a03077);
    // Return targets the second old-map prefetched instruction, which must be discarded.
    for (unsigned i = 0; i < 2; ++i) { exact.step(); fast.step(); same(exact, fast); }
    assert(cpuGetRegisters(exact.cpu)[3] == 0x77 && cpuGetRegisters(exact.cpu)[15] == handler + 16);
    assert(exact.psr() == 0xd3 && cpuExceptionSequence(exact.cpu) == 1);
}
Imx1ProbeResult probe(const std::vector<uint32_t>& words, bool fast) {
    Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
    return imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4,
                         Imx1ProbeEntry::resetAlias, 100, nullptr, nullptr, fast, &options);
}
void probeGuards() {
    for (bool fast : {false, true}) {
        for (uint32_t accepted : {0u, 1u, 3u, 0x37u, 0xabu, 0xacu, 0xffu, 0x123455u, 0xffffffu}) for (bool thumb : {false, true}) {
            if (thumb && (accepted == 0xab || accepted > 0xff)) continue;
            std::vector<uint32_t> p(0x100 / 4, 0xe7f000f0);
            p[0] = 0xea00000e; p[2] = 0xe59ff018; p[0x28 / 4] = 0x80;
            p[0x80 / 4] = 0xe1b0f00e;
            if (thumb) {
                p[0x40 / 4] = 0xe59f0000; p[0x44 / 4] = 0xe12fff10; p[0x48 / 4] = 0x61;
                p[0x60 / 4] = 0x2407df00 | accepted; p[0x64 / 4] = 0xde00de00;
            } else { p[0x40 / 4] = 0xef000000 | accepted; p[0x44 / 4] = 0xe3a04007; }
            const auto r = probe(p, fast);
            assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.registers[4] == 7);
            assert(r.registers[15] == (thumb ? 0x64u : 0x48u));
            assert(r.registers[14] == (thumb ? 0x62u : 0x44u));
            assert(r.cpsr == (thumb ? 0xf3u : 0xd3u) && r.spsr == r.cpsr && !r.busAccesses);
            // The host semihosting values stop before entry.
            for (uint32_t immediate : {thumb ? 0xabu : 0x123456u}) {
                if (thumb) p[0x60 / 4] = 0x2407df00 | immediate;
                else p[0x40 / 4] = 0xef000000 | immediate;
                const auto rejected = probe(p, fast);
                assert(rejected.stop == Imx1ProbeStop::unsupportedInstruction && !rejected.busAccesses);
                assert(rejected.registers[15] == (thumb ? 0x60u : 0x40u) && !rejected.spsr);
            }
        }
        for (uint32_t mode : {0x10u, 0x13u, 0x1fu})
            for (uint32_t spsr : {0u, 0xd3u, 0xf3u, 0xd4u, 0x10000d3u})
                for (uint32_t target : {0x100u, 0x101u, 0x102u}) {
                    std::vector<uint32_t> p = {
                        0xe59f0000, 0xea000000, spsr,
                        0xe16ff000, // MSR spsr_fsxc,r0
                        0xe59fe000, 0xea000000, target,
                        0xe321f000 | mode, // MSR cpsr_c,#mode
                        0xe1b0f00e
                    };
                    // These combinations have an unpredictable source mode, SPSR or alignment.
                    if (mode == 0x13 && ((spsr == 0xd3 && target == 0x100) ||
                        (spsr == 0xf3 && !(target & 1)))) continue;
                    const auto r = probe(p, fast);
                    assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == 0xe1b0f00e);
                    assert(r.registers[15] == 32 && !r.busAccesses);
                }
        for (uint32_t instruction : {0xe1b0f00du, 0xe1b0f00fu, 0xe25ef004u, 0xe8fd8000u}) {
            const auto r = probe({instruction}, fast);
            assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.step == 0 && !r.busAccesses);
        }
        for (uint32_t instruction : {0x0f000000u, 0x0f123456u}) {
            const auto r = probe({instruction, 0xe3a04007, 0xe7f000f0}, fast);
            assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.step == 2 &&
                   r.registers[4] == 7 && r.cpsr == 0xd3 && !r.spsr && !r.busAccesses);
        }
    }
}
} // namespace
int main() {
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT", "1", 1);
    software(); otherExceptions(); normalBlocks(); fetchContinuation(); probeGuards();
    std::puts("PASS: ARM920T and ARMv5 SWI entry, F mask, exception banks and return, Exact/Fast parity");
}
