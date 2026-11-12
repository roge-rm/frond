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
    void advance() { assert((cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, 1) == 1)); }
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
uint32_t instruction(unsigned addressMode, unsigned base, unsigned list, bool writeback = false) {
    return 0xe8400000 | ((addressMode & 1) ? 0x00800000 : 0) |
           ((addressMode & 2) ? 0x01000000 : 0) | 0x00100000 | (writeback ? 0x00200000 : 0) | (base << 16) | list;
}
uint32_t first(unsigned addressMode, unsigned count, uint32_t base = data) {
    return (addressMode & 1) ? base + ((addressMode & 2) ? 4 : 0) :
                              base - 4 * count + ((addressMode & 2) ? 0 : 4);
}
constexpr std::array<unsigned, 6> banks{0x1f, 0x11, 0x12, 0x13, 0x17, 0x1b};
using Registers = std::array<std::array<uint32_t, 15>, banks.size()>;
struct State { Registers registers; std::array<uint32_t, banks.size()> saved; };
unsigned bank(unsigned mode, unsigned r) {
    if (r < 8) return 0;
    if (r < 13) return mode == 0x11;
    return mode == 0x10 ? 0x1f : mode;
}
State snapshot(Machine& m) {
    State state{}; const auto old = m.psr();
    for (unsigned n = 0; n < banks.size(); ++n) {
        m.mode(banks[n]); std::copy_n(cpuGetRegisters(m.cpu), 15, state.registers[n].begin());
        state.saved[n] = m.spsr();
    }
    cpuSetReg(m.cpu, ARM_REG_NUM_CPSR, old); return state;
}
void setExpected(State& state, unsigned mode, unsigned r, uint32_t value) {
    for (unsigned n = 0; n < banks.size(); ++n)
        if (bank(banks[n], r) == bank(mode, r)) state.registers[n][r] = value;
}
void check(Machine& m, const State& expected) {
    const auto actual = snapshot(m);
    assert(actual.registers == expected.registers && actual.saved == expected.saved);
}
void returns(Machine& exact, Machine& fast) {
    unsigned cases = 0;
    for (unsigned mode : modes) for (unsigned next : {0x10u, 0x11u, 0x12u, 0x13u, 0x17u, 0x1bu, 0x1fu})
    for (unsigned am = 0; am < 4; ++am) for (unsigned base : {0u, 8u, 12u, 13u, 14u})
    for (unsigned list : {0x8000u, 0x900fu, 0xff00u, 0xffffu}) for (bool wb : {false, true})
    for (bool thumb : {false, true}) for (unsigned low = 0; low < 4; ++low) {
        if (wb && (list & (1u << base))) continue;
        const unsigned count = __builtin_popcount(list), start = first(am, count);
        const uint32_t saved = ((cases & 15u) << 28) | 0xc0 | next | (thumb ? 0x20 : 0);
        const uint32_t target = 0x6000 | low, aligned = target & (thumb ? ~1u : ~3u);
        for (Machine* m : {&exact, &fast}) {
            m->reset(mode); cpuSetReg(m->cpu, base, data); m->saved(saved);
            auto expected = snapshot(*m);
            unsigned n = 0;
            for (unsigned r = 0; r < 16; ++r) if (list & (1u << r)) {
                const uint32_t value = r == 15 ? target : 0x34560000 + r;
                m->word(start + n++ * 4, value);
                if (r < 15) setExpected(expected, mode, r, value);
            }
            if (wb) setExpected(expected, mode, base, (am & 1) ? data + count * 4 : data - count * 4);
            m->word(code, instruction(am, base, list, wb));
            m->word(0x6000, thumb ? 0x46c046c0 : 0xe1a00000);
            uint64_t blocks = 0, before = 0, after = 0;
            if (m == &fast) assert(cpuJitExecutionCounts(m->cpu, &blocks, &before));
            m->step();
            if (m == &fast) assert(cpuJitExecutionCounts(m->cpu, &blocks, &after) && after == before);
            assert(m->psr() == saved && m->reg(15) == aligned && m->accesses.size() == count);
            check(*m, expected);
            for (unsigned access = 0; access < count; ++access) {
                const unsigned index = (am & 1) ? access : count - 1 - access;
                assert(m->accesses[access].address == start + index * 4 && !m->accesses[access].write);
            }
            m->advance();
            assert(m->reg(15) == aligned + (thumb ? 2 : 4) && m->psr() == saved);
            if (m == &fast) assert(cpuJitExecutionCounts(m->cpu, &blocks, &after) && after == before + 1);
        }
        same(exact, fast); ++cases;
    }
    std::printf("PASS: %u LDM returns, all banks/address modes, writeback before restored mode, low PC bits and Exact fallback\n", cases);
}
void faults(Machine& exact, Machine& fast) {
    unsigned cases = 0;
    for (unsigned mode : modes) for (unsigned am = 0; am < 4; ++am)
    for (unsigned base : {0u, 8u, 12u, 13u, 14u}) for (unsigned list : {0x8000u, 0x900fu, 0xffffu})
    for (bool wb : {false, true}) for (unsigned fail = 0; fail < unsigned(__builtin_popcount(list)); ++fail) {
        if (wb && (list & (1u << base))) continue;
        const unsigned count = __builtin_popcount(list), start = first(am, count);
        const uint32_t saved = 0x50000030;
        for (Machine* m : {&exact, &fast}) {
            m->reset(mode); cpuSetReg(m->cpu, base, data); m->saved(saved);
            auto expected = snapshot(*m);
            std::vector<unsigned> selected;
            for (unsigned r = 0; r < 16; ++r) if (list & (1u << r)) {
                m->word(start + selected.size() * 4, r == 15 ? 0x6003 : 0x34560000 + r);
                selected.push_back(r);
            }
            if (!(am & 1)) std::reverse(selected.begin(), selected.end());
            for (unsigned n = 0; n < fail; ++n) if (selected[n] < 15)
                setExpected(expected, mode, selected[n], 0x34560000 + selected[n]);
            setExpected(expected, mode, base, data);
            setExpected(expected, 0x17, 14, code + 8);
            expected.saved[4] = 0xa00000c0 | mode;
            m->word(code, instruction(am, base, list, wb));
            m->fault = start + ((am & 1) ? fail : count - 1 - fail) * 4;
            m->step();
            assert(m->psr() == 0xa00000d7 && m->spsr() == (0xa00000c0 | mode));
            assert(m->reg(15) == 0x10 && m->reg(14) == code + 8 && m->accesses.size() == fail);
            check(*m, expected);
        }
        same(exact, fast); ++cases;
    }
    std::printf("PASS: %u partial faults, independent destination/bank expectations, restored base and original CPSR on abort\n", cases);
}
void continuation(Machine& exact, Machine& fast) {
    for (bool thumb : {false, true}) for (bool cached : {false, true}) {
        for (Machine* m : {&exact, &fast}) {
            m->reset(0x13); m->word(0x4000, 0xc0a);
            m->cp(0x2000, 0xee020f10, 0x4000); m->cp(0x2020, 0xee030f10, 1);
            m->cp(0x2040, 0xee010f10, cached ? 0x1079 : 0x79);
            m->word(0x6000, thumb ? 0x46c02755 : 0xe3a07055);
            uint32_t insn = 0; uint_fast8_t fsr = 0;
            cpuSetReg(m->cpu, ARM_REG_NUM_CPSR, thumb ? 0xf3 : 0xd3);
            assert(cpuFetchInstruction(m->cpu, 0x6000, &insn, &fsr));
            cpuSetReg(m->cpu, ARM_REG_NUM_CPSR, 0xd3);
            m->word(0x6000, thumb ? 0x46c02777 : 0xe3a07077);
            m->saved(thumb ? 0x50000030 : 0x50000010);
            cpuSetReg(m->cpu, 13, data); m->word(data, 0x6000);
            m->word(code, instruction(1, 13, 0x8000, true)); m->step();
            assert(m->reg(15) == 0x6000 && m->psr() == (thumb ? 0x50000030u : 0x50000010u));
            m->advance(); assert(m->reg(7) == (cached ? 0x55u : 0x77u));
            m->mode(0x13); assert(m->reg(13) == data + 4);
        }
        same(exact, fast);
    }
    for (bool thumb : {false, true}) for (unsigned mapping = 0; mapping < 3; ++mapping) {
        for (Machine* m : {&exact, &fast}) {
            m->reset(0x13); m->word(0x4000, 0xc02);
            m->word(0x4004, mapping == 0 ? 0 : mapping == 1 ? 0x402 : 0xc02);
            m->cp(0x2000, 0xee020f10, 0x4000); m->cp(0x2020, 0xee030f10, 1);
            m->cp(0x2040, 0xee010f10, 0x79);
            m->word(0x6000, thumb ? 0x46c02755 : 0xe3a07055);
            const uint32_t saved = thumb ? 0x50000030 : 0x50000010;
            m->saved(saved); cpuSetReg(m->cpu, 13, data); m->word(data, 0x106000);
            m->word(code, instruction(1, 13, 0x8000, true)); m->step();
            assert(m->psr() == saved && m->reg(15) == 0x106000);
            m->advance();
            if (mapping == 2) assert(m->reg(7) == 0x55 && (m->psr() & 31) == 0x10);
            else assert(m->reg(15) == 0xc && m->spsr() == saved && m->psr() == 0x50000097 && m->reg(14) == 0x106004);
        }
        same(exact, fast);
    }
    for (Machine* m : {&exact, &fast}) {
        m->reset(0x13); m->saved(0xd3); m->word(0x4000, 0xc02);
        m->cp(0x2800, 0xee020f10, 0x4000); m->cp(0x2820, 0xee030f10, 1);
        cpuSetReg(m->cpu, 13, data); m->word(data, code + 8);
        m->word(code + 4, instruction(1, 13, 0x8000, true));
        m->word(code + 8, 0xe3a07055);
        m->cp(code, 0xee010f10, 0x79);
        m->word(code + 8, 0xe3a07077);
        m->advance(); assert(m->reg(15) == code + 8);
        m->advance(); assert(m->reg(7) == 0x77);
    }
    same(exact, fast);
    std::puts("PASS: LDM return flushes old-map continuation, retains I-cache words and checks restored User permissions");
}
void dirtyStack(Machine& exact, Machine& fast) {
    for (Machine* m : {&exact, &fast}) {
        m->reset(0x12); m->saved(0x5000003f); m->word(0x4000, 0xc0e);
        cpuArm920SetMemoryCheck(m->cpu, [](void* context, uint32_t address, uint32_t size, bool) {
            return uint64_t(address) + size <= static_cast<Machine*>(context)->bytes.size();
        }, m);
        m->cp(0x2000, 0xee020f10, 0x4000); m->cp(0x2020, 0xee030f10, 1);
        m->cp(0x2040, 0xee010f10, 0x407d);
        m->word(data, 0x7000); cpuSetReg(m->cpu, 0, data); cpuSetReg(m->cpu, 1, 0x6003);
        m->word(0x2120, 0xe5902000); m->step(0x2120); // Allocate the write-back line by reading.
        m->word(0x2100, 0xe5801000); m->step(0x2100);
        assert(m->word(data) == 0x7000);
        cpuSetReg(m->cpu, 13, data); m->word(code, instruction(1, 13, 0x8000, true));
        m->step(); assert(m->psr() == 0x5000003f && m->reg(15) == 0x6002);
        assert(m->word(data) == 0x7000);
        m->mode(0x12); assert(m->reg(13) == data + 4);
    }
    same(exact, fast);
    std::puts("PASS: dirty cached stack PC wins over stale backing RAM without preflight data reads");
}
void permissions(Machine& exact, Machine& fast) {
    for (bool allowed : {false, true}) for (bool alignment : {false, true}) for (unsigned low = 0; low < 4; ++low) {
        for (Machine* m : {&exact, &fast}) {
            m->reset(0x13); m->saved(0x50000030);
            m->word(0x4000, 0xc02); m->word(0x4004, allowed ? 0x402 : 0x002);
            m->cp(0x2000, 0xee020f10, 0x4000); m->cp(0x2020, 0xee030f10, 1);
            m->cp(0x2040, 0xee010f10, alignment ? 0x7b : 0x79);
            cpuSetReg(m->cpu, 13, 0x109000 | low); m->word(data, 0x6003);
            m->word(code, instruction(1, 13, 0x8000, true)); m->step();
            if (allowed && (!alignment || !low)) assert(m->psr() == 0x50000030 && m->reg(15) == 0x6002);
            else {
                assert(m->psr() == 0xa00000d7 && m->spsr() == 0xa00000d3 && m->reg(15) == 0x10);
                m->mode(0x13); assert(m->reg(13) == (0x109000 | low));
            }
        }
        same(exact, fast);
    }
    std::puts("PASS: LDM memory accesses retain source privilege and alignment/permission faults preserve source SP");
}
void probeGuards() {
    for (bool fast : {false, true}) for (unsigned mode : {0x10u, 0x11u, 0x12u, 0x13u, 0x17u, 0x1bu, 0x1fu})
    for (uint32_t saved : {0x1fu, 0x3fu, 0u, 0xd4u, 0x100001fu, 0x0800001fu})
    for (unsigned am = 0; am < 4; ++am) {
        // Probe CPUs have process lifetime; each child retains at most 16 fixtures.
        const pid_t child = fork(); assert(child >= 0);
        if (!child) {
            for (bool wb : {false, true})
            for (unsigned list : {0x8000u, 0x900fu, 0xa000u, 0u}) for (unsigned base : {13u, 15u}) {
                std::vector<uint32_t> words(0x200 / 4, 0xe7f000f0);
                words[0] = 0xe321f000 | mode;
                words[1] = 0xe59f0034; words[0x40 / 4] = saved;
                words[2] = 0xe16ff000; words[3] = 0xe3a0d080;
                const uint32_t insn = instruction(am, base, list, wb); words[4] = insn;
                const unsigned count = __builtin_popcount(list), start = first(am, count, 0x80);
                for (unsigned n = 0; n < count; ++n) words[(start + n * 4) / 4] = n + 1 == count ? 0x180 : 0x34560000 + n;
                words[0x180 / 4] = saved == 0x3f ? 0xde002709 : 0xe3a07009;
                Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
                const auto result = imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4,
                    Imx1ProbeEntry::resetAlias, 100, nullptr, nullptr, fast, &options);
                const bool accepted = mode != 0x10 && mode != 0x1f && (saved == 0x1f || saved == 0x3f) &&
                    base != 15 && (list & 0x8000) && !(wb && (list & (1u << base)));
                assert(result.stop == Imx1ProbeStop::unsupportedInstruction && !result.busAccesses);
                if (accepted) assert(result.cpsr == saved && result.registers[7] == 9 && result.trace.pc == 0x180 + (saved == 0x3f ? 2 : 4));
                else assert(result.trace.pc == 16 && result.trace.instruction == insn);
            }
            _exit(0);
        }
        int status = 0; assert(waitpid(child, &status, 0) == child); assert(WIFEXITED(status) && !WEXITSTATUS(status));
    }
    for (bool fast : {false, true}) {
        const pid_t child = fork(); assert(child >= 0);
        if (!child) {
            for (uint32_t insn : {0xe8ed8001u, 0x08fd900fu, 0x08ff900fu}) {
                std::vector<uint32_t> words(0x100 / 4, 0xe7f000f0);
                words[0] = 0xe3a0003f; words[1] = 0xe16ff000;
                words[2] = 0xe3a0d080; words[3] = insn;
                Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
                const auto result = imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4,
                    Imx1ProbeEntry::resetAlias, 100, nullptr, nullptr, fast, &options);
                assert(result.stop == Imx1ProbeStop::unsupportedInstruction && !result.busAccesses);
                assert(result.trace.pc == (insn >> 28 ? 12u : 16u));
                assert(result.registers[13] == 0x80 && result.cpsr == 0xd3 && result.spsr == 0x3f);
            }
            _exit(0);
        }
        int status = 0; assert(waitpid(child, &status, 0) == child); assert(WIFEXITED(status) && !WEXITSTATUS(status));
    }
    std::puts("PASS: LDM3 probe forms, saved status/base/list/writeback guards, STM-PC rejection and failed conditions");
}
}
int main() {
    unsetenv("FROND_JIT");
    Machine exact(false), fast(true);
    returns(exact, fast); faults(exact, fast); continuation(exact, fast); dirtyStack(exact, fast); permissions(exact, fast); probeGuards();
}
