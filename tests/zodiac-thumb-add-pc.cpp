#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "CPU.h"
#include "imx1_boot_probe.h"
#include "patch_dispatch.h"
#include "system_state.h"

namespace {
struct Machine {
    ArmCpu* cpu;
    ArmMem* mem;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x10000);
    static bool memory(void* context, uint32_t address, uint_fast8_t size, bool write, void* value) {
        auto& m = *static_cast<Machine*>(context);
        if (address >= 0x20080000) address -= 0x20080000;
        if (uint64_t(address) + size > m.bytes.size()) return false;
        if (write) std::memcpy(m.bytes.data() + address, value, size);
        else std::memcpy(value, m.bytes.data() + address, size);
        return true;
    }
    explicit Machine(bool fast) {
        assert(cpuSetFastEngine(fast));
        mem = memInit();
        assert(memRegionAdd(mem, 0, bytes.size(), memory, this));
        assert(memRegionAdd(mem, 0x20080000, bytes.size(), memory, this));
        auto* patches = initPatchDispatch();
        cpu = cpuInit(0x1000, mem, ARM_MEMORY_SYSTEM_MMU, false, false, -1, 0, 0,
                      patches, new PacePatch{}, createSystemState(), ArmCpuModel::arm920t, true);
        patchDispatchSetCpu(patches, cpu);
    }
    void word(uint32_t at, uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); }
    void half(uint32_t at, uint16_t value) {
        if (at >= 0x20080000) at -= 0x20080000;
        std::memcpy(bytes.data() + at, &value, 2);
    }
    void setup(uint32_t pc, uint16_t instruction, uint32_t flags) {
        cpuReset(cpu, 0x1000);
        cpuDropTranslatedCode(cpu);
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, flags | 0xf3);
        for (unsigned n = 0; n < 15; ++n) cpuSetReg(cpu, n, 0x12340000 + n);
        half(pc, instruction);
        half(pc + 2, 0x46c0);
        cpuSetReg(cpu, 15, pc);
    }
    void cp(uint32_t instruction, uint32_t value) {
        word(0x1000, instruction);
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, 0xd3);
        cpuSetReg(cpu, 0, value); cpuSetReg(cpu, 15, 0x1000);
        step();
    }
    void step() { assert((cpuCycleAnyKind(cpu, 1) == 1)); }
};
void same(const Machine& a, const Machine& b) {
    assert(!std::memcmp(cpuGetRegisters(a.cpu), cpuGetRegisters(b.cpu), 16 * sizeof(uint32_t)));
    assert(cpuGetRegExternal(a.cpu, ARM_REG_NUM_CPSR) == cpuGetRegExternal(b.cpu, ARM_REG_NUM_CPSR));
    assert(cpuGetRegExternal(a.cpu, ARM_REG_NUM_SPSR) == cpuGetRegExternal(b.cpu, ARM_REG_NUM_SPSR));
    assert(a.bytes == b.bytes);
}
void generatedStep(Machine& exact, Machine& fast) {
    uint64_t beforeBlocks = 0, beforeInstructions = 0, afterBlocks = 0, afterInstructions = 0;
    assert(cpuJitExecutionCounts(fast.cpu, &beforeBlocks, &beforeInstructions));
    exact.step(); fast.step(); same(exact, fast);
    assert(cpuJitExecutionCounts(fast.cpu, &afterBlocks, &afterInstructions));
    assert(afterBlocks > beforeBlocks && afterInstructions == beforeInstructions + 1);
}
void directed() {
    Machine exact(false), fast(true);
    // DDI0100I A2.4.3 and A7.1.6: PC+4, bit 0 ignored on PC writes, flags unchanged.
    struct Target { uint32_t pc, operand, result; };
    constexpr Target cases[] = {
        {0x20089004, 0x2c4, 0x200892cc},
        {0x9000, 0, 0x9004}, {0x9000, 1, 0x9004},
        {0x9000, 2, 0x9006}, {0x9000, 3, 0x9006},
        {0x9002, 0, 0x9006}, {0x9002, 1, 0x9006},
        {0x9002, 2, 0x9008}, {0x9002, 3, 0x9008},
        {0x9000, 0xfffffffc, 0x9000}, {0x9002, 0xfffffffc, 0x9002},
    };
    for (const auto& v : cases) for (unsigned source = 0; source < 15; ++source)
        for (uint32_t nzcv = 0; nzcv < 16; ++nzcv) {
            const uint16_t instruction = 0x4487 | source << 3;
            for (auto* m : {&exact, &fast}) {
                m->setup(v.pc, instruction, nzcv << 28);
                cpuSetReg(m->cpu, source, v.operand);
            }
            generatedStep(exact, fast);
            assert(cpuGetRegisters(exact.cpu)[15] == v.result);
            assert(cpuGetRegExternal(exact.cpu, ARM_REG_NUM_CPSR) == ((nzcv << 28) | 0xf3));
            for (unsigned n = 0; n < 15; ++n)
                assert(cpuGetRegisters(exact.cpu)[n] == (n == source ? v.operand : 0x12340000 + n));
        }
    // Reading PC as the source also retains bit 1, including an unaligned arithmetic result.
    for (uint32_t pc : {0x9000u, 0x9002u}) for (unsigned dest : {0u, 7u, 8u, 13u, 14u})
        for (uint32_t nzcv = 0; nzcv < 16; ++nzcv) {
            const uint16_t instruction = 0x4478 | (dest & 7) | ((dest & 8) << 4);
            for (auto* m : {&exact, &fast}) {
                m->setup(pc, instruction, nzcv << 28);
                cpuSetReg(m->cpu, dest, 1);
            }
            generatedStep(exact, fast);
            assert(cpuGetRegisters(exact.cpu)[dest] == (pc == 0x9000 ? 0x9005u : 0x9007u));
            assert(cpuGetRegisters(exact.cpu)[15] == pc + 2);
            assert(cpuGetRegExternal(exact.cpu, ARM_REG_NUM_CPSR) == ((nzcv << 28) | 0xf3));
            for (unsigned n = 0; n < 15; ++n) if (n != dest)
                assert(cpuGetRegisters(exact.cpu)[n] == 0x12340000 + n);
        }
}
void fetchBoundary() {
    for (bool translated : {false, true}) {
        Machine exact(false), fast(true);
        const uint32_t pc = translated ? 0x100ffcu : 0xfffcu;
        for (auto* m : {&exact, &fast}) {
            m->setup(0x9ffc, 0x449f, 0xb0000000);
            if (translated) {
                m->word(0x4000, 0xc02); // Configuration code and table data.
                m->word(0x4004, 0x8001); m->word(0x8000, 0x9ff2); // One small page.
                m->cp(0xee020f10, 0x4000); m->cp(0xee030f10, 1); m->cp(0xee010f10, 0x79);
            } else m->half(0xfffc, 0x449f);
            cpuSetReg(m->cpu, ARM_REG_NUM_CPSR, 0xb00000f3);
            cpuSetReg(m->cpu, 3, 1); cpuSetReg(m->cpu, 15, pc);
        }
        generatedStep(exact, fast);
        assert(cpuGetRegisters(exact.cpu)[15] == pc + 4);
        for (auto* m : {&exact, &fast}) {
            uint32_t instruction = 0; uint_fast8_t fsr = 0;
            assert(!cpuFetchInstruction(m->cpu, pc + 4, &instruction, &fsr));
            assert(fsr == (translated ? 7 : 8));
            m->step();
            assert(cpuGetRegisters(m->cpu)[15] == 0x0c && cpuGetRegisters(m->cpu)[14] == pc + 8);
            assert(cpuGetRegExternal(m->cpu, ARM_REG_NUM_CPSR) == 0xb00000d7);
            assert(cpuGetRegExternal(m->cpu, ARM_REG_NUM_SPSR) == 0xb00000f3);
        }
        same(exact, fast);
    }
}
void literal(std::vector<uint32_t>& words, unsigned reg, uint32_t value) {
    words.push_back(0xe59f0000 | reg << 12); words.push_back(0xea000000); words.push_back(value);
}
void probeGuards() {
    constexpr uint32_t base = 0x10000000, pc = base + 28;
    Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
    for (bool fast : {false, true}) {
        for (unsigned source = 0; source < 15; ++source) for (uint32_t operand : {0u, 1u, 2u, 3u}) {
            std::vector<uint32_t> words;
            literal(words, source, operand);
            const unsigned entry = source == 12 ? 11 : 12;
            literal(words, entry, pc | 1);
            words.push_back(0xe12fff10 | entry);
            // Branch skips a store, preserving the memory-access count and all other registers.
            words.push_back(0x60014487 | source << 3);
            words.push_back(0xde00de00);
            const auto r = imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4,
                                        Imx1ProbeEntry::physicalFlash, 100, nullptr, nullptr, fast, &options);
            assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == 0xde00);
            assert(r.trace.pc == pc + (operand < 2 ? 4 : 6) && !r.busAccesses && r.cpsr == 0xf3);
            assert(r.registers[source] == operand && r.registers[entry] == (pc | 1));
            for (unsigned n = 0; n < 15; ++n) if (n != source && n != entry) assert(!r.registers[n]);
        }
        // Low/low runs as ADD Rd, Rm (lowRegisters in zodiac-thumb-probe). PC/PC remains an unqualified backend gap.
        for (uint16_t instruction : {0x44ff, 0x45ff, 0x4780, 0x4781}) {
            std::vector<uint32_t> words;
            literal(words, 12, (base + 16) | 1); words.push_back(0xe12fff1c);
            words.push_back(0x60010000 | instruction);
            const auto r = imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4,
                                        Imx1ProbeEntry::physicalFlash, 100, nullptr, nullptr, fast, &options);
            assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == instruction);
            assert(r.trace.pc == base + 16 && !r.busAccesses && r.cpsr == 0xf3);
            assert(r.registers[15] == base + 16 && r.registers[12] == ((base + 16) | 1));
        }
    }
}

}
int main() {
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT", "1", 1);
    directed(); fetchBoundary(); probeGuards();
    std::puts("PASS: ARM920T Thumb high ADD PC operands, targets, flags and fetch aborts, each admitted instruction generated in Fast");
}
