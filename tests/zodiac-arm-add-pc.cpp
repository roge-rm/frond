#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

#include "CPU.h"
#include "imx1_boot_probe.h"
#include "patch_dispatch.h"
#include "system_state.h"

namespace {
constexpr uint32_t code = 0x1000;
struct Machine {
    ArmCpu* cpu;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x20000);
    static bool memory(void* context, uint32_t address, uint_fast8_t size, bool write, void* value) {
        auto& m = *static_cast<Machine*>(context);
        if (uint64_t(address) + size > m.bytes.size()) return false;
        if (write) std::memcpy(m.bytes.data() + address, value, size);
        else std::memcpy(value, m.bytes.data() + address, size);
        return true;
    }
    explicit Machine(bool fast) {
        assert(cpuSetFastEngine(fast));
        auto* mem = memInit(); assert(memRegionAdd(mem, 0, 0xffffffff, memory, this));
        auto* patches = initPatchDispatch();
        cpu = cpuInit(code, mem, ARM_MEMORY_SYSTEM_MMU, false, false, -1, 0, 0,
                      patches, new PacePatch{}, createSystemState(), ArmCpuModel::arm920t, true);
        patchDispatchSetCpu(patches, cpu);
    }
    void word(uint32_t address, uint32_t value) { std::memcpy(bytes.data() + address, &value, 4); }
    uint32_t reg(unsigned r) const { return cpuGetRegisters(cpu)[r]; }
    uint32_t psr() const { return cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR); }
    uint32_t spsr() const { return cpuGetRegExternal(cpu, ARM_REG_NUM_SPSR); }
    void advance() { assert((cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, 1) == 1)); }
    void at(uint32_t pc) { cpuSetReg(cpu, 15, pc); advance(); }
    void reset(uint32_t status = 0xd3) {
        std::fill(bytes.begin(), bytes.end(), 0); cpuReset(cpu, code);
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, status);
        for (unsigned r = 0; r < 15; ++r) cpuSetReg(cpu, r, 0x12340000 + r);
    }
    void cp(uint32_t pc, uint32_t instruction, uint32_t operand) {
        word(pc, instruction); cpuSetReg(cpu, 0, operand); at(pc);
    }
};
void same(Machine& a, Machine& b) {
    assert(a.psr() == b.psr() && a.spsr() == b.spsr() && a.bytes == b.bytes);
    assert(!std::memcmp(cpuGetRegisters(a.cpu), cpuGetRegisters(b.cpu), 16 * sizeof(uint32_t)));
}
void generated(Machine& exact, Machine& fast, uint32_t pc) {
    uint64_t blocks = 0, before = 0, after = 0;
    assert(cpuJitExecutionCounts(fast.cpu, &blocks, &before));
    exact.at(pc); fast.at(pc); same(exact, fast);
    assert(cpuJitExecutionCounts(fast.cpu, &blocks, &after) && after == before + 1);
}
void directed(Machine& exact, Machine& fast) {
    unsigned count = 0;
    for (unsigned mode : {0x10u, 0x11u, 0x12u, 0x13u, 0x17u, 0x1bu, 0x1fu})
    for (unsigned source = 0; source < 15; ++source) for (unsigned flags = 0; flags < 16; ++flags)
    for (uint32_t pc : {0x1000u, 0x1004u})
    for (uint32_t offset : {0u, 0x100u, 0xffffffc0u, 0x7ffffff0u, 0x80000000u, 0xffffe000u}) {
        const uint32_t status = (flags << 28) | 0xc0 | mode;
        for (Machine* m : {&exact, &fast}) {
            m->reset(status); cpuSetReg(m->cpu, source, offset);
            m->word(pc, 0xe08ff000 | source);
        }
        const auto saved = exact.spsr();
        generated(exact, fast, pc);
        assert(exact.psr() == status && exact.spsr() == saved && exact.reg(15) == pc + 8 + offset);
        for (unsigned r = 0; r < 15; ++r) assert(exact.reg(r) == (r == source ? offset : 0x12340000 + r));
        ++count;
    }
    for (unsigned flags = 0; flags < 16; ++flags) {
        for (Machine* m : {&exact, &fast}) {
            m->reset((flags << 28) | 0xd3); cpuSetReg(m->cpu, 12, 0x20);
            m->word(code, 0x008ff00c); // ADDEQ pc,pc,r12.
        }
        generated(exact, fast, code);
        assert(exact.reg(15) == code + ((flags & 4) ? 0x28 : 4));
        assert(exact.psr() == ((flags << 28) | 0xd3));
    }
    std::printf("PASS: %u aligned ARM ADD pc,pc,Rm cases, modes, flags, source registers and wraparound\n", count);
}
void continuation(Machine& exact, Machine& fast) {
    for (bool cached : {false, true}) for (unsigned mapping = 0; mapping < 3; ++mapping) {
        for (Machine* m : {&exact, &fast}) {
            m->reset(); m->word(0x4000, 0xc0a);
            m->word(0x4004, mapping == 0 ? 0 : mapping == 1 ? 0x40a : 0xc0a);
            m->cp(0x2000, 0xee020f10, 0x4000); m->cp(0x2020, 0xee030f10, 1);
            m->cp(0x2040, 0xee010f10, cached ? 0x1079 : 0x79);
            m->word(code, 0xe08ff00c); m->word(0x6000, 0xe3a07055);
            uint32_t instruction = 0; uint_fast8_t fsr = 0;
            if (mapping) assert(cpuFetchInstruction(m->cpu, 0x106000, &instruction, &fsr));
            m->word(0x6000, 0xe3a07077);
            cpuSetReg(m->cpu, ARM_REG_NUM_CPSR, 0x50000010);
            cpuSetReg(m->cpu, 12, 0x106000 - code - 8);
        }
        generated(exact, fast, code);
        assert(exact.psr() == 0x50000010 && exact.reg(15) == 0x106000);
        exact.advance(); fast.advance(); same(exact, fast);
        if (mapping == 2) assert(exact.reg(7) == (cached ? 0x55u : 0x77u) && exact.reg(15) == 0x106004);
        else assert(exact.reg(15) == 0xc && exact.reg(14) == 0x106004 && exact.spsr() == 0x50000010 && exact.psr() == 0x50000097);
    }
    std::puts("PASS: ARM ADD pc continuation, User fetch permissions, prefetch abort and retained I-cache words");
}
void probeCases(bool fast, unsigned source) {
    for (uint32_t target : {0x40u, 0xc0u}) for (unsigned low = 0; low < 4; ++low) {
        std::vector<uint32_t> p(0x100 / 4, 0xe7f000f0);
        p[0] = 0xea00001e;
        p[0x80 / 4] = 0xe59f0040 | (source << 12); // LDR Rm,[pc,#64].
        p[0xc8 / 4] = target + low - 0x8c;
        p[0x84 / 4] = 0xe08ff000 | source;
        p[target / 4] = 0xe3a07009;
        Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
        const auto r = imx1BootProbe(reinterpret_cast<const uint8_t*>(p.data()), p.size() * 4,
                                    Imx1ProbeEntry::resetAlias, 100, nullptr, nullptr, fast, &options);
        assert(r.stop == Imx1ProbeStop::unsupportedInstruction && !r.busAccesses);
        if (!low) assert(r.registers[15] == target + 4 && r.registers[7] == 9 && r.cpsr == 0xd3);
        else assert(r.registers[15] == 0x84 && r.trace.instruction == (0xe08ff000 | source));
    }
}
void guards() {
    for (bool fast : {false, true}) for (unsigned source = 0; source < 15; ++source) {
        const pid_t child = fork(); assert(child >= 0);
        if (!child) { probeCases(fast, source); _exit(0); }
        int status = 0; assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    for (bool fast : {false, true}) for (uint32_t word : {0xe08ff00fu, 0xe09ff00cu, 0xe08ff08cu}) {
        Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
        const auto r = imx1BootProbe(reinterpret_cast<const uint8_t*>(&word), 4,
                                    Imx1ProbeEntry::resetAlias, 100, nullptr, nullptr, fast, &options);
        assert(r.stop == Imx1ProbeStop::unsupportedInstruction && !r.trace.step && !r.busAccesses);
    }
    std::puts("PASS: aligned ARM linkage stubs admitted, unaligned/PC-source/flags/other-shift forms guarded");
}
}
int main() {
    unsetenv("FROND_JIT"); Machine exact(false), fast(true);
    directed(exact, fast); continuation(exact, fast); guards();
}
