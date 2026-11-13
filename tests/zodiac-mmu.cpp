#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "CPU.h"
#include "RAM.h"
#include "mem.h"
#include "memory_buffer.h"
#include "patch_dispatch.h"
#include "system_state.h"

namespace {
constexpr uint32_t code = 0x1000, table = 0x4000, bytes = 0x20000;
constexpr uint32_t control = 0x2020, ttb = 0x2040, domain = 0x2060,
                   lineInval = 0x2080, allInval = 0x20a0, store = 0x20c0, tlbInval = 0x20e0;
constexpr uint32_t cleanIndex = 0x2400, cleanInvalIndex = 0x2420, rejected = 0x2440;
struct Machine {
    ArmCpu* cpu;
    uint8_t* ram;
    explicit Machine(bool fast) {
        assert(cpuSetFastEngine(fast));
        auto* mem = memInit();
        MemoryBuffer buffer{};
        assert(memoryBufferAllocate(&buffer, bytes));
        ram = buffer.buffer;
        ramSetNoFramebuffer(ramInit(mem, nullptr, 0, bytes, &buffer, true));
        auto* patches = initPatchDispatch();
        cpu = cpuInit(code, mem, ARM_MEMORY_SYSTEM_MMU, false, false, -1, 0, 0,
                      patches, new PacePatch{}, createSystemState(), ArmCpuModel::arm920t, true);
        patchDispatchSetCpu(patches, cpu);
    }
    void word(uint32_t address, uint32_t value) { std::memcpy(ram + address, &value, 4); }
    void reset() {
        std::memset(ram, 0, bytes);
        cpuReset(cpu, code);
        cpuArm920SetIcacheIndexCompatibility(cpu, false);
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, 0xd3);
        for (unsigned i = 0; i < 15; ++i) cpuSetReg(cpu, i, 0);
        word(control, 0xee010f10); word(ttb, 0xee020f10); word(domain, 0xee030f10);
        word(lineInval, 0xee070f35); word(allInval, 0xee070f15);
        word(store, 0xe5801000); word(tlbInval, 0xee080f17);
        word(0x2300, 0xee050f10); word(0x2320, 0xee060f10);
        word(cleanIndex, 0xee070f5a); word(cleanInvalIndex, 0xee070f5e);
        word(table, 0x00000c0a);       // AP3, domain0, cacheable section.
        word(table + 4, 0x00000c0a);   // A virtual synonym of the first section.
        word(table + 8, 0x0000044a);   // AP1, domain2, cacheable section.
    }
    void step(uint32_t pc) {
        cpuSetReg(cpu, 15, pc);
        assert((cpuCycleAnyKind(cpu, 1) == 1));
    }
    void cp(uint32_t pc, uint32_t value) { cpuSetReg(cpu, 0, value); step(pc); }
    void setup(uint32_t ctl) { cp(ttb, table); cp(domain, 1); cp(control, ctl); }
    uint32_t read(unsigned crn) {
        // These read instructions are at distinct addresses and do not change while cached.
        const uint32_t pc = 0x2800 + crn * 32;
        word(pc, 0xee100f10 | (crn << 16));
        step(pc);
        return cpuGetRegisters(cpu)[0];
    }
};
void same(Machine& a, Machine& b) {
    assert(!std::memcmp(cpuGetRegisters(a.cpu), cpuGetRegisters(b.cpu), 16 * sizeof(uint32_t)));
    assert(cpuGetRegExternal(a.cpu, ARM_REG_NUM_CPSR) == cpuGetRegExternal(b.cpu, ARM_REG_NUM_CPSR));
    assert(cpuGetRegExternal(a.cpu, ARM_REG_NUM_SPSR) == cpuGetRegExternal(b.cpu, ARM_REG_NUM_SPSR));
    assert(!std::memcmp(a.ram, b.ram, bytes));
}
}
int main() {
    unsetenv("FROND_JIT");
    Machine exact(false), fast(true);
    auto run = [&](auto body) {
        for (Machine* m : {&exact, &fast}) { m->reset(); body(*m); }
        same(exact, fast);
    };
    run([](Machine& m) {
        assert(m.read(1) == 0x78);
        m.word(0x2340, 0xee100f30); m.step(0x2340);
        assert(cpuGetRegisters(m.cpu)[0] == 0x0d172172);
        m.setup(0x79);
        assert(m.read(2) == table && m.read(3) == 1 && m.read(1) == 0x79);
        m.word(code, 0xe3a02001); m.step(0x101000);
        assert(cpuGetRegisters(m.cpu)[2] == 1);
        // Ctt=1 alone must not retain instructions while Icr=0.
        cpuSetReg(m.cpu, 0, code); cpuSetReg(m.cpu, 1, 0xe3a02002); m.step(store); m.step(0x101000);
        assert(cpuGetRegisters(m.cpu)[2] == 2);
        m.cp(control, 0xc000027b); assert(m.read(1) == 0xc000027b);
        cpuReset(m.cpu, code); m.word(code, 0xee111f10); m.step(code);
        assert(cpuGetRegisters(m.cpu)[1] == 0x78);
        m.step(0x101000);
        assert((cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_ABT);
        assert(cpuGetRegisters(m.cpu)[15] == 0x0c);
    });
    run([](Machine& m) {
        m.setup(0x79); m.word(code, 0xe3a02001); m.step(0x101000);
        m.word(0x8000, 0x00000c0a); // New table has no mapping for the synonym.
        m.cp(ttb, 0x8000); m.step(0x101000);
        assert(cpuGetRegisters(m.cpu)[2] == 1);
        m.cp(control, 0x1079); m.step(0x101000);
        assert(cpuGetRegisters(m.cpu)[2] == 1);
        m.cp(tlbInval, 0); m.step(0x101000);
        assert((cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_ABT);
        assert(cpuGetRegisters(m.cpu)[15] == 0x0c);
    });
    run([](Machine& m) {
        m.setup(0x5079);
        m.word(code, 0xe3a02001); m.step(code); m.step(0x101000);
        cpuSetReg(m.cpu, 0, code); cpuSetReg(m.cpu, 1, 0xe3a02002); m.step(store);
        m.step(code); assert(cpuGetRegisters(m.cpu)[2] == 1);
        m.cp(lineInval, code); m.step(code); assert(cpuGetRegisters(m.cpu)[2] == 2);
        m.step(0x101000); assert(cpuGetRegisters(m.cpu)[2] == 1);
        m.cp(allInval, 0); m.step(0x101000); assert(cpuGetRegisters(m.cpu)[2] == 2);
        // Disabling Icr ignores cache contents and preserves them for re-enabling.
        m.cp(control, 0x79);
        cpuSetReg(m.cpu, 0, code); cpuSetReg(m.cpu, 1, 0xe3a02003); m.step(store);
        m.step(0x101000); assert(cpuGetRegisters(m.cpu)[2] == 3);
        m.cp(control, 0x5079); m.step(0x101000); assert(cpuGetRegisters(m.cpu)[2] == 2);
        cpuReset(m.cpu, code); m.step(code);
        assert(cpuGetRegisters(m.cpu)[2] == 3);
    });
    run([](Machine& m) {
        m.setup(0x5079); m.cp(allInval, 0);
        m.word(code, 0xe3a02001); m.step(code);
        cpuSetReg(m.cpu, 0, code); cpuSetReg(m.cpu, 1, 0xe3a02002); m.step(store);
        // Each address selects segment0 with a different virtual tag.
        for (unsigned n = 1; n <= 64; ++n) {
            const uint32_t pc = code + n * 0x100;
            if (pc == table) continue;
            m.word(pc, 0xe3a03007); m.step(pc);
        }
        // The table line was skipped, so one more segment0 fill is needed.
        m.word(code + 65 * 0x100, 0xe3a03007); m.step(code + 65 * 0x100);
        m.step(code); assert(cpuGetRegisters(m.cpu)[2] == 2);
    });
    run([](Machine& m) {
        m.setup(0x79);
        m.word(code, 0xe5902000); cpuSetReg(m.cpu, 0, 0x203000); m.step(code);
        assert((cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_ABT);
        assert(cpuGetRegisters(m.cpu)[15] == 0x10 && cpuGetRegisters(m.cpu)[14] == code + 8);
        assert(m.read(5) == 0x29 && m.read(6) == 0x203000);
    });
    run([](Machine& m) {
        m.setup(0x7b); m.word(code, 0xe5902000);
        cpuSetReg(m.cpu, 0, 0x3001); m.step(code);
        assert((cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_ABT);
        assert(cpuGetRegisters(m.cpu)[15] == 0x10 && cpuGetRegisters(m.cpu)[14] == code + 8);
        assert(m.read(5) == 1 && m.read(6) == 0x3001);
    });
    run([](Machine& m) {
        m.setup(0x1079); m.cp(domain, 0x11);
        m.cp(0x2300, 0x55); m.cp(0x2320, 0x12345678);
        m.word(code, 0xe3a02001); m.step(0x201000); // Privileged AP1 fill.
        cpuSetReg(m.cpu, ARM_REG_NUM_CPSR, 0xd0); m.step(0x201000);
        assert((cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_ABT);
        assert(cpuGetRegisters(m.cpu)[15] == 0x0c && cpuGetRegisters(m.cpu)[14] == 0x201004);
        assert(m.read(5) == 0x55 && m.read(6) == 0x12345678);
    });
    run([](Machine& m) {
        m.setup(0x79); m.cp(0x2300, 0x55); m.cp(0x2320, 0x12345678);
        m.word(code, 0xe3a02001); m.step(0x301000);
        assert((cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_ABT);
        assert(m.read(5) == 0x55 && m.read(6) == 0x12345678);
    });
    run([](Machine& m) {
        m.cp(control, 0x1078); // I-cache can run with the MMU disabled.
        m.word(code, 0xe3a02001); m.step(code);
        cpuSetReg(m.cpu, 0, code); cpuSetReg(m.cpu, 1, 0xe3a02002); m.step(store);
        m.step(code); assert(cpuGetRegisters(m.cpu)[2] == 1);
        m.cp(lineInval, code); m.step(code); assert(cpuGetRegisters(m.cpu)[2] == 2);
    });
    run([](Machine& m) {
        m.setup(0x5079);
        m.word(code, 0xe3a0c001); m.step(0x101000);
        cpuSetReg(m.cpu, 0, code); cpuSetReg(m.cpu, 1, 0xe3a0c002); m.step(store);
        // Retain both the old instruction and the synonym's translation.
        m.word(table + 4, 0);
        uint8_t before[bytes];
        std::memcpy(before, m.ram, bytes);
        for (uint32_t pc : {cleanIndex, cleanInvalIndex}) {
            for (unsigned index = 0; index < 64; ++index) {
                for (unsigned segment = 0; segment < 8; ++segment) {
                    cpuSetReg(m.cpu, 0, (index << 26) | (segment << 5));
                    uint32_t regs[15];
                    std::memcpy(regs, cpuGetRegisters(m.cpu), sizeof(regs));
                    const auto cpsr = cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR);
                    const auto spsr = cpuGetRegExternal(m.cpu, ARM_REG_NUM_SPSR);
                    m.step(pc);
                    assert(cpuGetRegisters(m.cpu)[15] == pc + 4);
                    assert(!std::memcmp(regs, cpuGetRegisters(m.cpu), sizeof(regs)));
                    assert(cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) == cpsr);
                    assert(cpuGetRegExternal(m.cpu, ARM_REG_NUM_SPSR) == spsr);
                    assert(!std::memcmp(before, m.ram, bytes));
                    m.step(0x101000);
                    assert(cpuGetRegisters(m.cpu)[12] == 1);
                }
            }
        }
        assert(m.read(1) == 0x5079 && m.read(2) == table && m.read(3) == 1);
        m.cp(allInval, 0); m.step(0x101000);
        assert(cpuGetRegisters(m.cpu)[12] == 2);
        m.cp(tlbInval, 0); m.step(0x101000);
        assert((cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_ABT);
    });
    // Reserved operand bits and unsupported encodings must reject before cache or MMU changes.
    auto reject = [&](uint32_t instruction, uint32_t operand, bool compatibility = false) {
        run([&](Machine& m) {
            cpuArm920SetIcacheIndexCompatibility(m.cpu, compatibility);
            m.word(rejected, instruction);
            m.setup(0x5079);
            m.cp(0x2300, 0x55); m.cp(0x2320, 0x12345678);
            m.word(code, 0xe3a0c001); m.step(0x101000);
            cpuSetReg(m.cpu, 0, code); cpuSetReg(m.cpu, 1, 0xe3a0c002); m.step(store);
            m.word(table + 4, 0);
            cpuSetReg(m.cpu, 0, operand);
            uint32_t regs[13];
            std::memcpy(regs, cpuGetRegisters(m.cpu), sizeof(regs));
            uint8_t before[bytes];
            std::memcpy(before, m.ram, bytes);
            m.step(rejected);
            assert((cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_UND);
            assert(cpuGetRegisters(m.cpu)[15] == 4);
            assert(cpuGetRegisters(m.cpu)[14] == rejected + 4);
            assert(cpuGetRegExternal(m.cpu, ARM_REG_NUM_SPSR) == 0xd3);
            assert(!std::memcmp(regs, cpuGetRegisters(m.cpu), sizeof(regs)));
            assert(!std::memcmp(before, m.ram, bytes));
            cpuSetReg(m.cpu, ARM_REG_NUM_CPSR, 0xd3);
            assert(m.read(1) == 0x5079 && m.read(2) == table && m.read(3) == 1);
            assert(m.read(5) == 0x55 && m.read(6) == 0x12345678);
            m.step(0x101000); assert(cpuGetRegisters(m.cpu)[12] == 1);
        });
    };
    for (uint32_t instruction : {0xee070f5au, 0xee070f5eu}) {
        for (unsigned bit = 0; bit < 26; ++bit) {
            if (bit >= 5 && bit <= 7) continue;
            reject(instruction, 0xfc0000e0u | (1u << bit));
        }
        reject(instruction | 0x00100000, 0); // MRC
        reject(instruction | 0x00200000, 0); // op1=1
        reject(instruction | 0x10000000, 0); // MCR2
        reject(instruction | 0x0000f000, 0); // Rd=PC
        reject(instruction ^ 0x00010000, 0); // CRn=6
        reject(instruction ^ 0x00000080, 0); // op2=6
    }
    for (uint32_t operand : {0u, 0x20420520u, 0xffffffffu}) reject(0xee070f55, operand);
    for (uint32_t instruction : {0xee170f55u,0xee270f55u,0xfe070f55u,0xee07ff55u,
                                 0xee060f55u,0xee070f57u}) reject(instruction,0,true);
    reject(0xee010f10, 0x507c); // Dcache requires the MMU
    reject(0xee070f52, 0);      // Dcache allocation
    reject(0xee090f10, 0);      // Dcache lockdown
    reject(0xee0f0f10, 0);      // Diagnostic RAM access
    reject(0xee070f56, 0);      // Undocumented Dcache invalidate by index
    run([](Machine& m) {
        // DDI0151C example 4-4, both maintenance forms in a nested cleaning loop.
        const uint32_t program[] = {
            0xe3a030e0, 0xe3a0433f, 0xe1840003, 0xee070f5a, 0xee070f5e,
            0xe2544301, 0x2afffffa, 0xe2533020, 0x2afffff7, 0xe3a05055
        };
        for (unsigned i = 0; i < sizeof(program) / sizeof(*program); ++i)
            m.word(code + i * 4, program[i]);
        unsigned count = 0;
        while (cpuGetRegisters(m.cpu)[15] != code + sizeof(program)) {
            assert(++count < 3000);
            assert((cpuCycleAnyKind(m.cpu, 1) == 1));
            assert((cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_SVC);
        }
        assert(count == 2586 && cpuGetRegisters(m.cpu)[5] == 0x55);
    });
    run([](Machine& m) {
        m.word(table, 0); // No virtual mapping for the physical continuation.
        m.cp(ttb, table); m.cp(domain, 1);
        m.word(code, 0xee010f10); m.word(code + 4, 0xe12fff11); // MCR control; BX r1
        m.word(code + 8, 0xe3a03055);
        m.word(code + 0x20, 0xe3a03077);
        cpuSetReg(m.cpu, 0, 0x79); cpuSetReg(m.cpu, 1, 0x101020);
        m.step(code);
        assert((cpuCycleAnyKind(m.cpu, 1) == 1));
        assert(cpuGetRegisters(m.cpu)[15] == 0x101020);
        assert((cpuCycleAnyKind(m.cpu, 1) == 1));
        assert(cpuGetRegisters(m.cpu)[3] == 0x77);
    });
    run([](Machine& m) {
        m.word(table, 0);
        m.cp(ttb, table); m.cp(domain, 1);
        m.word(code, 0xee010f10);
        m.word(code + 4, 0xe3a03001); m.word(code + 8, 0xe2833002);
        cpuSetReg(m.cpu, 0, 0x79); m.step(code);
        for (unsigned i = 0; i < 2; ++i)
            assert((cpuCycleAnyKind(m.cpu, 1) == 1));
        assert(cpuGetRegisters(m.cpu)[3] == 3);
        cpuCycleAnyKind(m.cpu, 1);
        assert((cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_ABT);
        assert(cpuGetRegisters(m.cpu)[15] == 0x0c);
    });
    run([](Machine& m) {
        m.word(table, 0);
        m.cp(ttb, table); m.cp(domain, 1);
        m.word(code, 0xee010f10);
        m.word(code + 4, 0xeaffffff); // B code+8 must discard its prefetched word.
        m.word(code + 8, 0xe3a03055);
        cpuSetReg(m.cpu, 0, 0x79); m.step(code);
        cpuCycleAnyKind(m.cpu, 1);
        cpuCycleAnyKind(m.cpu, 1);
        assert((cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_ABT);
        assert(cpuGetRegisters(m.cpu)[3] == 0);
    });
    run([](Machine& m) {
        m.word(table, 0x00008001); // Coarse table maps VA 0x20000 to valid RAM.
        m.word(0x8080, 0x00003ff2);
        m.word(0x3000, 0xe3a03077);
        m.cp(ttb, table); m.cp(domain, 1);
        m.word(bytes - 8, 0xee010f10);
        m.word(bytes - 4, 0xe3a03001);
        cpuSetReg(m.cpu, 0, 0x79); m.step(bytes - 8);
        cpuCycleAnyKind(m.cpu, 1);
        assert(cpuGetRegisters(m.cpu)[3] == 1);
        cpuCycleAnyKind(m.cpu, 1);
        assert((cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_ABT);
        assert(cpuGetRegisters(m.cpu)[15] == 0x0c && cpuGetRegisters(m.cpu)[3] == 1);
    });
    run([](Machine& m) {
        // Primary ArmRam backing uses the same data view and write watches.
        m.word(0x2460, 0xe5902000); m.word(0x2480, 0xee070f3a);
        m.word(code, 0xe3a03001); m.word(table, 0xc0e);
        m.setup(0x507d); m.step(code); assert(cpuGetRegisters(m.cpu)[3] == 1);
        cpuSetReg(m.cpu, 0, code); m.step(0x2460);
        cpuSetReg(m.cpu, 0, code); cpuSetReg(m.cpu, 1, 0xe3a03002); m.step(store);
        uint32_t backing; std::memcpy(&backing, m.ram + code, 4); assert(backing == 0xe3a03001);
        m.cp(0x2480, code); std::memcpy(&backing, m.ram + code, 4); assert(backing == 0xe3a03002);
        m.step(code); assert(cpuGetRegisters(m.cpu)[3] == 1);
        m.cp(lineInval, code); m.step(code); assert(cpuGetRegisters(m.cpu)[3] == 2);
    });
    uint64_t blocks = 0, instructions = 0;
    assert(cpuJitExecutionCounts(fast.cpu, &blocks, &instructions) && blocks && instructions);
    std::printf("PASS: ARM920 TTBR/DACR/control, MMU permissions/domain/abort/reset, I-cache SMC/synonyms/eviction, empty Dcache index maintenance and guards, Exact/Fast blocks=%llu instructions=%llu\n",
                (unsigned long long)blocks, (unsigned long long)instructions);
}
