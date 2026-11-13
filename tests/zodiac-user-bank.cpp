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
constexpr uint32_t code = 0x1000, data = 0x9000;
constexpr std::array<unsigned, 5> modes{0x11, 0x12, 0x13, 0x17, 0x1b};
uint32_t seed(unsigned mode, unsigned reg) { return 0x12000000 | (mode << 16) | reg; }
struct Access { uint32_t address, value; bool write; };
struct Machine {
    ArmCpu* cpu;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x20000);
    std::vector<Access> accesses;
    uint32_t fault = 0xffffffff;
    static bool memory(void* context, uint32_t address, uint_fast8_t size, bool write, void* value) {
        auto& m = *static_cast<Machine*>(context);
        if (address == m.fault || uint64_t(address) + size > m.bytes.size()) return false;
        if (write) std::memcpy(m.bytes.data() + address, value, size);
        else std::memcpy(value, m.bytes.data() + address, size);
        if (size == 4 && address >= 0x8000 && address < 0xa000)
            m.accesses.push_back({address, *static_cast<uint32_t*>(value), write});
        return true;
    }
    explicit Machine(bool fast) {
        assert(cpuSetFastEngine(fast));
        auto* mem = memInit();
        assert(memRegionAdd(mem, 0, 0xffffffff, memory, this));
        auto* patches = initPatchDispatch();
        cpu = cpuInit(code, mem, ARM_MEMORY_SYSTEM_MMU, false, false, -1, 0, 0,
                      patches, new PacePatch{}, createSystemState(), ArmCpuModel::arm920t, true);
        patchDispatchSetCpu(patches, cpu);
    }
    void word(uint32_t address, uint32_t value) { std::memcpy(bytes.data() + address, &value, 4); }
    uint32_t word(uint32_t address) const { uint32_t v; std::memcpy(&v, bytes.data() + address, 4); return v; }
    void saved(uint32_t value) {
        const uint32_t old = reg(0);
        word(0x3000, 0xe16ff000); cpuSetReg(cpu, 0, value); step(0x3000);
        cpuSetReg(cpu, 0, old);
    }
    void mode(unsigned mode) { cpuSetReg(cpu, ARM_REG_NUM_CPSR, 0xa00000c0 | mode); }
    uint32_t reg(unsigned r) const { return cpuGetRegisters(cpu)[r]; }
    uint32_t psr() const { return cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR); }
    uint32_t spsr() const { return cpuGetRegExternal(cpu, ARM_REG_NUM_SPSR); }
    void reset(unsigned current) {
        std::fill(bytes.begin(), bytes.end(), 0); accesses.clear(); fault = 0xffffffff;
        cpuReset(cpu, code);
        mode(0x1f);
        for (unsigned r = 0; r < 15; ++r) cpuSetReg(cpu, r, seed(0x10, r));
        for (unsigned m : modes) {
            mode(m);
            for (unsigned r = m == 0x11 ? 8 : 13; r < 15; ++r) cpuSetReg(cpu, r, seed(m, r));
            saved(0x5000001f);
        }
        mode(current);
        cpuSetReg(cpu, 15, code);
    }
    void advance() { assert((cpuCycleAnyKind(cpu, 1) == 1)); }
    void step(uint32_t pc = code) { cpuSetReg(cpu, 15, pc); advance(); }
    void cp(uint32_t pc, uint32_t instruction, uint32_t operand) {
        word(pc, instruction); cpuSetReg(cpu, 0, operand); step(pc);
    }
};
void same(Machine& a, Machine& b) {
    assert(a.psr() == b.psr() && a.spsr() == b.spsr());
    assert(!std::memcmp(cpuGetRegisters(a.cpu), cpuGetRegisters(b.cpu), 16 * sizeof(uint32_t)));
    assert(a.bytes == b.bytes && a.accesses.size() == b.accesses.size());
    for (size_t n = 0; n < a.accesses.size(); ++n) {
        assert(a.accesses[n].address == b.accesses[n].address);
        assert(a.accesses[n].value == b.accesses[n].value && a.accesses[n].write == b.accesses[n].write);
    }
    const uint32_t old = a.psr();
    for (unsigned mode : {0x1fu, 0x11u, 0x12u, 0x13u, 0x17u, 0x1bu}) {
        a.mode(mode); b.mode(mode);
        assert(!std::memcmp(cpuGetRegisters(a.cpu), cpuGetRegisters(b.cpu), 15 * sizeof(uint32_t)));
        assert(a.spsr() == b.spsr());
    }
    cpuSetReg(a.cpu, ARM_REG_NUM_CPSR, old); cpuSetReg(b.cpu, ARM_REG_NUM_CPSR, old);
}
uint32_t instruction(bool load, unsigned addressMode, unsigned base, unsigned list) {
    return 0xe8400000 | ((addressMode & 1) ? 0x00800000 : 0) |
           ((addressMode & 2) ? 0x01000000 : 0) | (load ? 0x00100000 : 0) | (base << 16) | list;
}
uint32_t first(unsigned addressMode, unsigned count, uint32_t base = data) {
    return (addressMode & 1) ? base + ((addressMode & 2) ? 4 : 0) :
                              base - 4 * count + ((addressMode & 2) ? 0 : 4);
}
void transfers(Machine& exact, Machine& fast) {
    unsigned cases = 0;
    for (unsigned mode : modes) for (bool load : {false, true}) for (unsigned am = 0; am < 4; ++am)
    for (unsigned base : {2u, 8u, 13u, 14u}) for (unsigned list : {0x2ff1u, 0x7fffu, 0x6000u, 0x1f00u}) {
        const unsigned count = __builtin_popcount(list), start = first(am, count);
        for (Machine* m : {&exact, &fast}) {
            m->reset(mode); cpuSetReg(m->cpu, base, data);
            m->word(code, instruction(load, am, base, list));
            for (unsigned n = 0; n < count; ++n) m->word(start + 4 * n, 0x34560000 + n);
            m->step();
            assert(m->psr() == (0xa00000c0 | mode) && m->spsr() == 0x5000001f && m->reg(15) == code + 4);
            assert(m->accesses.size() == count);
            // The address base belongs to the current mode; transferred SP/LR belong to User.
            unsigned n = 0;
            for (unsigned r = 0; r < 15; ++r) if (list & (1u << r)) {
                const bool shared = r < 8 || (r < 13 && mode != 0x11);
                const uint32_t old = r == base && shared ? data : seed(0x10, r);
                if (!load) assert(m->word(start + 4 * n) == old);
                ++n;
            }
            m->mode(0x1f); n = 0;
            for (unsigned r = 0; r < 15; ++r) {
                const bool selected = list & (1u << r);
                const bool shared = r < 8 || (r < 13 && mode != 0x11);
                const uint32_t old = r == base && shared ? data : seed(0x10, r);
                assert(m->reg(r) == (load && selected ? 0x34560000 + n : old));
                if (selected) ++n;
            }
            m->mode(mode);
            if (mode == 0x11) for (unsigned r = 8; r < 13; ++r)
                assert(m->reg(r) == (r == base ? data : seed(mode, r)));
            for (unsigned r = 13; r < 15; ++r) assert(m->reg(r) == (r == base ? data : seed(mode, r)));
        }
        same(exact, fast); ++cases;
    }
    std::printf("PASS: %u user-bank transfer cases, all exception modes/address modes and banked bases\n", cases);
}
void faults(Machine& exact, Machine& fast) {
    for (unsigned mode : {0x11u, 0x13u}) for (bool load : {false, true})
    for (unsigned am = 0; am < 4; ++am) for (unsigned base : {0u, 8u, 13u}) for (unsigned fail : {0u, 1u, 14u}) {
        const unsigned list = 0x7fff, start = first(am, 15);
        for (Machine* m : {&exact, &fast}) {
            m->reset(mode); cpuSetReg(m->cpu, base, data);
            m->word(code, instruction(load, am, base, list));
            for (unsigned n = 0; n < 15; ++n) m->word(start + n * 4, 0x34560000 + n);
            // The shared interpreter visits decreasing address modes from high to low.
            m->fault = start + ((am & 1) ? fail : 14 - fail) * 4;
            m->step();
            assert(m->psr() == 0xa00000d7 && m->spsr() == (0xa00000c0 | mode));
            assert(m->reg(15) == 0x10 && m->reg(14) == code + 8 && m->accesses.size() == fail);
            m->mode(mode);
            assert(m->reg(base) == data);
            m->mode(0x17);
        }
        same(exact, fast);
    }
    std::puts("PASS: partial transfer faults preserve the current-mode base and enter Abort");
}
void permissions(Machine& exact, Machine& fast) {
    for (bool load : {false, true}) for (bool allowed : {false, true}) for (bool alignment : {false, true}) {
        for (Machine* m : {&exact, &fast}) {
            m->reset(0x13);
            m->word(0x4000, 0xc02); // AP3 identity map for code and tables.
            m->word(0x4004, allowed ? 0x402 : 0x002); // AP1 or AP0, domain0, physical0.
            m->cp(0x2000, 0xee020f10, 0x4000);
            m->cp(0x2020, 0xee030f10, 1);
            m->cp(0x2040, 0xee010f10, alignment ? 0x7b : 0x79);
            cpuSetReg(m->cpu, 2, 0x109001);
            m->word(data, 0x12345678);
            m->word(code, instruction(load, 1, 2, 1u << 13));
            m->step();
            if (allowed && !alignment) {
                assert(m->psr() == 0xa00000d3 && m->reg(2) == 0x109001);
                assert(m->word(data) == (load ? 0x12345678 : seed(0x10, 13)));
                m->mode(0x1f); assert(m->reg(13) == (load ? 0x12345678 : seed(0x10, 13))); m->mode(0x13);
            } else {
                assert(m->psr() == 0xa00000d7 && m->reg(2) == 0x109001 && m->reg(15) == 0x10);
                m->word(0x2060, 0xee155f10); m->step(0x2060); assert(m->reg(5) == (alignment ? 1u : 13u));
                m->word(0x2080, 0xee166f10); m->step(0x2080); assert(m->reg(6) == 0x109001);
                m->mode(0x1f); assert(m->reg(13) == seed(0x10, 13)); m->mode(0x17);
            }
        }
        same(exact, fast);
    }
    std::puts("PASS: user-bank accesses use privileged permissions and preserve alignment/permission faults");
}
Imx1ProbeResult probe(uint32_t transfer, bool fast, unsigned mode = 0x13) {
    std::vector<uint32_t> words(0x100 / 4, 0xe7f000f0);
    words[0] = 0xe3a02080; // ROM-backed data at 0x80.
    words[1] = 0xe321f000 | (0xc0 | mode);
    words[2] = transfer;
    words[3] = 0xe1a00000; // NOP after the User register transfer.
    words[4] = 0xe7f000f0;
    for (unsigned n = 0; n < 15; ++n) words[0x80 / 4 + n] = 0x34560000 + n;
    Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
    return imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4,
                         Imx1ProbeEntry::resetAlias, 100, nullptr, nullptr, fast, &options);
}
void followers(Machine& exact, Machine& fast) {
    for (unsigned mode : modes) for (bool load : {false, true}) {
        for (Machine* m : {&exact, &fast}) {
            m->reset(mode); cpuSetReg(m->cpu, 1, data);
            m->word(code, instruction(load, 1, 1, 0x2ff9));
            m->word(code + 4, 0xe581e080); // STR current LR,[r1,#128].
            m->word(code + 8, 0xe581d084); // STR current SP,[r1,#132].
            for (unsigned n = 0; n < 11; ++n) m->word(data + n * 4, 0x34560000 + n);
            m->step(); m->advance(); m->advance();
            assert(m->word(data + 128) == seed(mode, 14));
            assert(m->word(data + 132) == seed(mode, 13));
            assert(m->psr() == (0xa00000c0 | mode) && m->spsr() == 0x5000001f);
            m->mode(0x1f);
            assert(m->reg(13) == (load ? 0x3456000au : seed(0x10, 13)));
            assert(m->reg(14) == seed(0x10, 14));
            m->mode(mode);
        }
        same(exact, fast);
    }
    std::puts("PASS: chosen immediate bank visibility for User-bank transfer followers");
}
void returns(Machine& exact, Machine& fast) {
    unsigned cases = 0;
    for (unsigned mode : modes) for (unsigned next : {0x10u, 0x11u, 0x12u, 0x13u, 0x17u, 0x1bu, 0x1fu})
    for (unsigned source = 0; source < 15; ++source) for (unsigned state = 0; state < 3; ++state)
    for (unsigned flags : {0u, 5u, 10u, 15u}) {
        const uint32_t target = 0x6000 + (state == 2 ? 2 : 0);
        const uint32_t saved = (flags << 28) | 0xc0 | next | (state ? 0x20 : 0);
        for (Machine* m : {&exact, &fast}) {
            m->reset(mode); cpuSetReg(m->cpu, source, target);
            m->saved(saved);
            m->mode(next);
            std::array<uint32_t, 15> expected{};
            std::copy_n(cpuGetRegisters(m->cpu), 15, expected.begin());
            const uint32_t expectedSpsr = m->spsr();
            m->mode(mode);
            if (state) { const uint16_t mov = 0x2709; std::memcpy(m->bytes.data() + target, &mov, 2); }
            else m->word(target, 0xe3a07009);
            m->word(code, 0xe1b0f000 | source);
            uint64_t blocks = 0, before = 0, after = 0;
            if (m == &fast) assert(cpuJitExecutionCounts(m->cpu, &blocks, &before));
            m->step();
            if (m == &fast) assert(cpuJitExecutionCounts(m->cpu, &blocks, &after) && after == before + 1);
            assert(m->psr() == saved && m->spsr() == expectedSpsr && m->reg(15) == target);
            assert(std::equal(expected.begin(), expected.end(), cpuGetRegisters(m->cpu)));
            m->advance(); assert(m->reg(7) == 9 && m->psr() == (state ? saved & ~0xc0000000u : saved));
        }
        same(exact, fast); ++cases;
    }
    // All NZCV combinations, including independent incoming and restored flags.
    for (unsigned incoming = 0; incoming < 16; ++incoming) for (unsigned outgoing = 0; outgoing < 16; ++outgoing) {
        for (Machine* m : {&exact, &fast}) {
            m->reset(0x13); m->saved((outgoing << 28) | 0x1f);
            cpuSetReg(m->cpu, ARM_REG_NUM_CPSR, (incoming << 28) | 0xd3);
            cpuSetReg(m->cpu, 1, 0x6000); m->word(code, 0xe1b0f001); m->step();
            assert(m->psr() == ((outgoing << 28) | 0x1f) && m->reg(15) == 0x6000);
        }
        same(exact, fast);
    }
    std::printf("PASS: %u MOVS pc,Rm return cases, source banks, destination modes, ARM/Thumb targets and NZCV\n", cases);
}
void returnContinuation(Machine& exact, Machine& fast) {
    for (bool thumb : {false, true}) for (bool cached : {false, true}) {
        for (Machine* m : {&exact, &fast}) {
            m->reset(0x13);
            m->word(0x4000, 0xc0a);
            m->cp(0x2000, 0xee020f10, 0x4000);
            m->cp(0x2020, 0xee030f10, 1);
            m->cp(0x2040, 0xee010f10, cached ? 0x1079 : 0x79);
            m->word(0x6000, thumb ? 0x46c02755 : 0xe3a07055);
            uint32_t instruction = 0; uint_fast8_t fsr = 0;
            cpuSetReg(m->cpu, ARM_REG_NUM_CPSR, thumb ? 0xf3 : 0xd3);
            assert(cpuFetchInstruction(m->cpu, 0x6000, &instruction, &fsr));
            cpuSetReg(m->cpu, ARM_REG_NUM_CPSR, 0xd3);
            m->word(0x6000, thumb ? 0x46c02777 : 0xe3a07077);
            m->saved(thumb ? 0x50000030 : 0x50000010);
            cpuSetReg(m->cpu, 1, 0x6000);
            m->word(code, 0xe1b0f001); m->step();
            assert(m->reg(15) == 0x6000 && m->psr() == (thumb ? 0x50000030u : 0x50000010u));
            m->advance(); assert(m->reg(7) == (cached ? 0x55u : 0x77u));
        }
        same(exact, fast);
    }
    for (bool thumb : {false, true}) for (unsigned mapping = 0; mapping < 3; ++mapping) {
        for (Machine* m : {&exact, &fast}) {
            m->reset(0x13); m->word(0x4000, 0xc02);
            m->word(0x4004, mapping == 0 ? 0 : mapping == 1 ? 0x402 : 0xc02);
            m->cp(0x2000, 0xee020f10, 0x4000);
            m->cp(0x2020, 0xee030f10, 1);
            m->cp(0x2040, 0xee010f10, 0x79);
            m->word(0x6000, thumb ? 0x46c02755 : 0xe3a07055);
            const uint32_t saved = thumb ? 0x50000030 : 0x50000010;
            m->saved(saved); cpuSetReg(m->cpu, 1, 0x106000);
            m->word(code, 0xe1b0f001); m->step();
            assert(m->psr() == saved && m->reg(15) == 0x106000);
            m->advance();
            if (mapping == 2) assert(m->reg(7) == 0x55 && (m->psr() & 31) == 0x10);
            else {
                assert(m->reg(15) == 0xc && m->spsr() == saved && m->psr() == 0x50000097);
                assert(m->reg(14) == 0x106004);
            }
        }
        same(exact, fast);
    }
    std::puts("PASS: MOVS pc,r1 continuation retains I-cache words and checks restored User MMU permissions");
}
void returnGuardCases(bool fast, unsigned source, unsigned mode) {
    for (uint32_t saved : {0x1fu, 0x3fu, 0u, 0xd4u, 0x100001fu, 0x0800001fu})
    for (uint32_t target : {0x80u, 0x81u, 0x82u, 0x83u}) {
        std::vector<uint32_t> p(0x100 / 4, 0xe7f000f0);
        p[0] = 0xe321f000 | mode;
        p[1] = 0xe59f005c; p[0x68 / 4] = saved;
        p[2] = 0xe16ff000;
        p[3] = 0xe59f0058 | (source << 12); p[0x6c / 4] = target;
        p[4] = 0xe1b0f000 | source;
        if (saved == 0x3f && !(target & 1)) { p[target / 4] = (target & 2) ? 0x2709de00 : 0xde002709; if (target & 2) p[target / 4 + 1] = 0xde00de00; }
        else if (saved == 0x1f && !(target & 3)) p[target / 4] = 0xe3a07009;
        Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
        const auto r = imx1BootProbe(reinterpret_cast<const uint8_t*>(p.data()), p.size() * 4,
                                    Imx1ProbeEntry::resetAlias, 100, nullptr, nullptr, fast, &options);
        const bool allowed = mode != 0x10 && mode != 0x1f &&
                             (saved == 0x1f || saved == 0x3f) && !(target & (saved & 0x20 ? 1 : 3));
        assert(r.stop == Imx1ProbeStop::unsupportedInstruction && !r.busAccesses);
        if (allowed) assert(r.registers[15] == target + (saved & 0x20 ? 2 : 4) && r.registers[7] == 9 && r.cpsr == saved);
        else assert(r.registers[15] == 16 && r.trace.instruction == (0xe1b0f000 | source));
    }
}
void returnGuards() {
    // Probe CPUs have process lifetime. Bound the memory retained by each fixture group.
    for (bool fast : {false, true}) for (unsigned source = 0; source < 15; ++source)
    for (unsigned mode : {0x10u, 0x11u, 0x12u, 0x13u, 0x17u, 0x1bu, 0x1fu}) {
        if (mode != 0x13 && source != 1) continue;
        const pid_t child = fork(); assert(child >= 0);
        if (!child) { returnGuardCases(fast, source, mode); _exit(0); }
        int status = 0; assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    std::puts("PASS: MOVS pc,Rm mode/SPSR/target guards and all ordinary source registers");
}
void guards() {
    for (bool fast : {false, true}) {
        for (unsigned mode : modes) {
            auto r = probe(0xe8d22ff1, fast, mode);
            assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.registers[15] == 16);
            assert(r.registers[0] == 0x34560000 && r.registers[4] == 0x34560001 && r.registers[2] == 0x80);
        }
        for (unsigned mode : {0x10u, 0x1fu}) {
            auto r = probe(0xe8d22ff1, fast, mode);
            assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.registers[15] == 8 && !r.busAccesses);
        }
        // The LDM exception return below has an uninitialised SPSR.
        for (uint32_t bad : {0xe8f22ff1u, 0xe8e22ff1u, 0xe8d28001u, 0xe8c28001u, 0xe8df0001u, 0xe8d20000u}) {
            auto r = probe(bad, fast);
            assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.registers[15] == 8 && !r.busAccesses);
        }
        // A failed condition must skip even an otherwise guarded form.
        auto r = probe(0x08f22ff1, fast);
        assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.registers[15] == 16 && !r.busAccesses);
    }
    std::puts("PASS: reached LDM admitted, PC/writeback/empty/base-PC/User/System forms guarded");
}
}
int main() {
    unsetenv("FROND_JIT");
    Machine exact(false), fast(true);
    transfers(exact, fast); faults(exact, fast); permissions(exact, fast); guards(); followers(exact, fast); returns(exact, fast); returnGuards(); returnContinuation(exact, fast);
}
