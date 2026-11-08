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
uint32_t immediateOperand(uint32_t encoding) {
    const unsigned rotate = ((encoding >> 8) & 15) * 2;
    const uint32_t value = encoding & 255;
    return rotate ? (value >> rotate) | (value << (32 - rotate)) : value;
}
void immediateDirected(Machine& exact, Machine& fast) {
    for (unsigned mode : {0x10u,0x11u,0x12u,0x13u,0x17u,0x1bu,0x1fu})
    for (unsigned flags = 0; flags < 16; ++flags) for (unsigned source : {0u,12u,15u})
    for (uint32_t encoding : {0u,8u,0xc02u,0x102u}) for (bool subtract : {false,true}) {
        const uint32_t status = (flags << 28) | 0xc0 | mode;
        const uint32_t base = source == 15 ? code+8 : 0xfffffff8;
        const uint32_t instruction = (subtract ? 0xe240f000 : 0xe280f000) | source << 16 | encoding;
        for (auto* m : {&exact,&fast}) {
            m->reset(status); if (source != 15) cpuSetReg(m->cpu,source,base);
            m->word(code,instruction);
        }
        const auto saved = exact.spsr();
        generated(exact,fast,code);
        const uint32_t operand = immediateOperand(encoding);
        assert(exact.reg(15) == (subtract ? base-operand : base+operand));
        assert(exact.psr() == status && exact.spsr() == saved);
        for (unsigned r = 0; r < 15; ++r)
            assert(exact.reg(r) == (r == source ? base : 0x12340000+r));
    }
    for (unsigned flags = 0; flags < 16; ++flags) for (bool subtract : {false,true}) {
        for (auto* m : {&exact,&fast}) {
            m->reset((flags << 28) | 0xd3); cpuSetReg(m->cpu,12,0x1800);
            m->word(code,(subtract ? 0x124cf008 : 0x128cf008));
        }
        generated(exact,fast,code);
        assert(exact.reg(15) == ((flags & 4) ? code+4 : subtract ? 0x17f8u : 0x1808u));
        assert(exact.psr() == ((flags << 28) | 0xd3));
    }
    std::puts("PASS: immediate ADD/SUB PC, rotated operands, wraparound, conditions, modes and preserved status");
}
uint32_t shifted(uint32_t value, unsigned type, unsigned amount, bool carry) {
    if (!amount && type == 3) return (value / 2) | (carry ? 0x80000000u : 0u);
    if (!amount && (type == 1 || type == 2)) amount = 32;
    for (unsigned bit = 0; bit < amount; ++bit) {
        if (type == 0) value *= 2;
        else if (type == 1) value /= 2;
        else if (type == 2) value = (value / 2) | (value & 0x80000000u);
        else value = (value / 2) | ((value & 1) ? 0x80000000u : 0u);
    }
    return value;
}
void registerDirected(Machine& exact, Machine& fast) {
    unsigned count = 0;
    for (unsigned type = 0; type < 4; ++type) for (unsigned amount = 0; amount < 32; ++amount)
    for (uint32_t value : {0u,1u,0xffffffffu,0x80000000u,0x80000001u,0x76543210u})
    for (unsigned flags = 0; flags < 16; ++flags) for (bool subtract : {false,true}) {
        const uint32_t status = (flags << 28) | 0xd3;
        const uint32_t operand = shifted(value,type,amount,flags & 2);
        // An aligned target also checks the shift's low bits in the probe below.
        const uint32_t base = subtract ? 0x1800+operand : 0x1800-operand;
        const uint32_t instruction = (subtract ? 0xe04bf000 : 0xe08bf000) | amount << 7 | type << 5;
        for (auto* m : {&exact,&fast}) {
            m->reset(status); cpuSetReg(m->cpu,0,value); cpuSetReg(m->cpu,11,base);
            m->word(code,instruction);
        }
        const auto saved = exact.spsr(); generated(exact,fast,code);
        assert(exact.reg(15) == 0x1800 && exact.psr() == status && exact.spsr() == saved);
        for (unsigned r = 0; r < 15; ++r)
            assert(exact.reg(r) == (r == 0 ? value : r == 11 ? base : 0x12340000+r));
        ++count;
    }
    for (unsigned mode : {0x10u,0x11u,0x12u,0x13u,0x17u,0x1bu,0x1fu})
    for (unsigned rn = 0; rn < 16; ++rn) for (unsigned rm = 0; rm < 16; ++rm)
    for (uint32_t pc : {code,code+4}) for (bool subtract : {false,true}) {
        const uint32_t status = 0xa00000c0 | mode;
        for (auto* m : {&exact,&fast}) {
            m->reset(status);
            for (unsigned r = 0; r < 15; ++r) cpuSetReg(m->cpu,r,0xffffff00+4*r);
            m->word(pc,(subtract ? 0xe040f000 : 0xe080f000) | rn << 16 | rm);
        }
        const uint32_t base = rn == 15 ? pc+8 : exact.reg(rn);
        const uint32_t operand = rm == 15 ? pc+8 : exact.reg(rm);
        generated(exact,fast,pc);
        assert(exact.reg(15) == (subtract ? base-operand : base+operand) && exact.psr() == status);
        for (unsigned r = 0; r < 15; ++r) assert(exact.reg(r) == 0xffffff00+4*r);
    }
    for (unsigned flags = 0; flags < 16; ++flags) for (bool subtract : {false,true}) {
        for (auto* m : {&exact,&fast}) {
            m->reset((flags << 28) | 0xd3); cpuSetReg(m->cpu,11,0x1800); cpuSetReg(m->cpu,0,0x40);
            m->word(code,subtract ? 0x104bf000 : 0x108bf000); // NE
        }
        generated(exact,fast,code);
        assert(exact.reg(15) == ((flags & 4) ? code+4 : subtract ? 0x17c0u : 0x1840u));
        assert(exact.psr() == ((flags << 28) | 0xd3));
    }
    std::printf("PASS: %u generated register ADD/SUB PC shifts, carry, status, all Rn/Rm aliases, modes and wraparound\n",count);
}
void registerProbeCases(bool fast, unsigned source) {
    for (unsigned rm : {0u,14u,15u}) for (unsigned type = 0; type < 4; ++type) {
        const pid_t child = fork(); assert(child >= 0);
        if (!child) {
            for (unsigned amount : {0u,1u,2u,31u}) for (unsigned low = 0; low < 4; ++low)
            for (bool subtract : {false,true}) {
                std::vector<uint32_t> p(0x100/4,0xe7f000f0);
                p[0] = 0xea00001e;
                // Both PC reads at 0x88 yield 0x90. Other operands are loaded unchanged.
                p[0x80/4] = 0xe59f0040 | source << 12;
                p[0x84/4] = 0xe59f0040 | rm << 12;
                const uint32_t value = source == rm ? 0x90+low : 0x80000003+low;
                p[0xcc/4] = value;
                const uint32_t operand = shifted(rm == 15 ? 0x90 : value,type,amount,false);
                const uint32_t base = source == 15 ? 0x90 : source == rm ? value :
                    subtract ? 0x40+low+operand : 0x40+low-operand;
                p[0xc8/4] = base;
                if (source == 15) p[0x80/4] = 0xe1a00000;
                if (rm == 15) p[0x84/4] = 0xe1a00000;
                p[0x88/4] = (subtract ? 0xe040f000 : 0xe080f000) | source << 16 | amount << 7 | type << 5 | rm;
                const uint32_t target = subtract ? base-operand : base+operand;
                Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
                const auto r = imx1BootProbe(reinterpret_cast<const uint8_t*>(p.data()),p.size()*4,
                                            Imx1ProbeEntry::resetAlias,4,nullptr,nullptr,fast,&options);
                assert(!r.busAccesses && r.cpsr == 0xd3);
                if (target & 3) assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.registers[15] == 0x88);
                else assert(r.stop == Imx1ProbeStop::budget && r.registers[15] == target);
            }
            _exit(0);
        }
        int status = 0; assert(waitpid(child,&status,0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
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
void immediateProbeCases(bool fast, unsigned source) {
    for (bool subtract : {false,true}) for (bool skip : {false,true})
    for (unsigned low = 0; low < 4; ++low) {
        std::vector<uint32_t> p(0x100/4,0xe7f000f0);
        p[0] = skip ? 0xe3500000 : 0xe1a00000; // CMP r0,#0 sets Z for skipped NE.
        p[1] = 0xea00001d;
        uint32_t target = 0x40, encoding = 8;
        if (source == 15) { p[0x80/4] = 0xe1a00000; encoding = 0x18 + low; target = subtract ? 0x74 : 0xa4; }
        else {
            p[0x80/4] = 0xe59f0040 | source << 12;
            p[0xc8/4] = subtract ? target+low+8 : target+low-8;
        }
        p[0x84/4] = (subtract ? 0x1240f000 : 0x1280f000) | source << 16 | encoding;
        p[target/4] = 0xe3a09009;
        Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
        const auto r = imx1BootProbe(reinterpret_cast<const uint8_t*>(p.data()),p.size()*4,
                                    Imx1ProbeEntry::resetAlias,100,nullptr,nullptr,fast,&options);
        assert(r.stop == Imx1ProbeStop::unsupportedInstruction && !r.busAccesses);
        if (skip) assert(r.registers[15] == 0x88 && r.cpsr == 0x600000d3);
        else if (!low) assert(r.registers[15] == target+4 && r.registers[9] == 9 && r.cpsr == 0xd3);
        else assert(r.registers[15] == 0x84 && r.trace.instruction == p[0x84/4] && r.cpsr == 0xd3);
    }
}
void guards() {
    for (bool fast : {false, true}) for (unsigned source = 0; source < 16; ++source) {
        const pid_t child = fork(); assert(child >= 0);
        if (!child) { if (source < 15) probeCases(fast, source); immediateProbeCases(fast,source); registerProbeCases(fast,source); _exit(0); }
        int status = 0; assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    for (bool fast : {false, true}) for (uint32_t word : {0xe09ff00cu,0xe05bf000u,0xe08bf010u,0xe04bf010u,0xe08bf090u,0xe29cf008u,0xe25cf008u,0xe2acf008u}) {
        Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
        const auto r = imx1BootProbe(reinterpret_cast<const uint8_t*>(&word), 4,
                                    Imx1ProbeEntry::resetAlias, 100, nullptr, nullptr, fast, &options);
        assert(r.stop == Imx1ProbeStop::unsupportedInstruction && !r.trace.step && !r.busAccesses);
    }
    std::puts("PASS: aligned ADD/SUB linkage admitted, unaligned/flag-setting/register-shift/illegal forms guarded");
}
}
int main() {
    unsetenv("FROND_JIT"); Machine exact(false), fast(true);
    directed(exact, fast); immediateDirected(exact,fast); registerDirected(exact,fast); continuation(exact, fast); guards();
}
