#include <algorithm>
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
constexpr uint32_t code = 0x1000, romBase = 0x10000000;
struct Machine {
    ArmCpu* cpu;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x10000);
    static bool memory(void* context, uint32_t address, uint_fast8_t size, bool write, void* value) {
        auto& m = *static_cast<Machine*>(context);
        if (uint64_t(address) + size > m.bytes.size()) return false;
        if (write) std::memcpy(m.bytes.data() + address, value, size);
        else std::memcpy(value, m.bytes.data() + address, size);
        return true;
    }
    explicit Machine(bool fast, bool singleStep = true, ArmCpuModel model = ArmCpuModel::arm920t) {
        assert(cpuSetFastEngine(fast));
        auto* mem = memInit();
        assert(memRegionAdd(mem, 0, bytes.size(), memory, this));
        auto* patches = initPatchDispatch();
        cpu = cpuInit(code, mem, ARM_MEMORY_SYSTEM_MMU, model == ArmCpuModel::armv5te, false,
                      -1, 0, 0, patches, new PacePatch{}, createSystemState(), model, singleStep);
        patchDispatchSetCpu(patches, cpu);
    }
    void word(uint32_t address, uint32_t value) { std::memcpy(bytes.data() + address, &value, 4); }
    void half(uint32_t address, uint16_t value) { std::memcpy(bytes.data() + address, &value, 2); }
    void setup(uint32_t instruction, bool thumb, uint32_t flags,
               const std::array<uint32_t, 15>& regs) {
        std::fill(bytes.begin(), bytes.end(), 0);
        cpuReset(cpu, code);
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, flags | 0xd3 | (thumb ? 0x20 : 0));
        for (unsigned i = 0; i < regs.size(); ++i) cpuSetReg(cpu, i, regs[i]);
        cpuSetReg(cpu, 15, code | (thumb ? 1 : 0));
        word(code, instruction);
    }
    void step() { assert((cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, 1) == 1)); }
};
std::array<uint32_t, 15> initial() {
    std::array<uint32_t, 15> r{};
    for (unsigned i = 0; i < r.size(); ++i) r[i] = 0x12340000 + i;
    return r;
}
void same(const Machine& a, const Machine& b) {
    assert(!std::memcmp(cpuGetRegisters(a.cpu), cpuGetRegisters(b.cpu), 16 * sizeof(uint32_t)));
    assert(cpuGetRegExternal(a.cpu, ARM_REG_NUM_CPSR) == cpuGetRegExternal(b.cpu, ARM_REG_NUM_CPSR));
    assert(cpuGetRegExternal(a.cpu, ARM_REG_NUM_SPSR) == cpuGetRegExternal(b.cpu, ARM_REG_NUM_SPSR));
    assert(a.bytes == b.bytes);
}
uint32_t nz(uint32_t result) { return (result & 0x80000000) | (result == 0 ? 0x40000000 : 0); }
uint32_t arm(unsigned d, unsigned rm, unsigned rs, unsigned add, bool accumulate, bool flags) {
    return 0xe0000090 | d << 16 | add << 12 | rs << 8 | rm |
           uint32_t(accumulate) << 21 | uint32_t(flags) << 20;
}
void directed() {
    struct Vector { uint32_t a, b, add, product, accumulated; };
    constexpr Vector vectors[] = {
        {0x1c, 0, 7, 0, 7},
        {0xfffffffe, 3, 7, 0xfffffffa, 1},
        {0x80000000, 1, 0x80000000, 0x80000000, 0},
        {0x10000, 0x10000, 0xffffffff, 0, 0xffffffff},
        {0xffffffff, 0xffffffff, 0x7fffffff, 1, 0x80000000},
    };
    for (auto model : {ArmCpuModel::arm920t, ArmCpuModel::armv5te}) {
        Machine exact(false, true, model), fast(true, true, model);
        for (const auto& v : vectors) for (uint32_t cv : {0u, 0x10000000u, 0x20000000u, 0x30000000u})
            for (uint32_t oldNz : {0u, 0xc0000000u}) {
                // Thumb's destination is also one multiplicand, with distinct encoded registers.
                for (unsigned d : {0u, 2u, 7u}) {
                    const unsigned rm = (d + 1) & 7;
                    auto r = initial(); r[d] = v.a; r[rm] = v.b;
                    const uint16_t instruction = 0x4340 | rm << 3 | d;
                    for (Machine* m : {&exact, &fast}) m->setup(instruction, true, cv | oldNz, r);
                    exact.step(); fast.step(); same(exact, fast);
                    for (unsigned n = 0; n < r.size(); ++n) {
                        if (cpuGetRegisters(exact.cpu)[n] != (n == d ? v.product : r[n]))
                            std::fprintf(stderr, "model=%u thumb=%04x a=%08x b=%08x r%u=%08x expected=%08x\n",
                                         unsigned(model), instruction, v.a, v.b, n, cpuGetRegisters(exact.cpu)[n],
                                         n == d ? v.product : r[n]);
                        assert(cpuGetRegisters(exact.cpu)[n] == (n == d ? v.product : r[n]));
                    }
                    assert(cpuGetRegisters(exact.cpu)[15] == code + 2);
                    assert(cpuGetRegExternal(exact.cpu, ARM_REG_NUM_CPSR) == (cv | nz(v.product) | 0xf3));
                }
                // Rd may equal Rs or the accumulator; Rd==Rm stays outside the probe subset.
                for (bool accumulate : {false, true}) for (bool flags : {false, true})
                    for (unsigned d : {2u, 1u, 3u}) {
                        auto r = initial(); r[0] = v.a; r[1] = v.b; r[3] = v.add;
                        const uint32_t instruction = arm(d, 0, 1, accumulate ? 3 : 0, accumulate, flags);
                        for (Machine* m : {&exact, &fast}) m->setup(instruction, false, cv | oldNz, r);
                        exact.step(); fast.step(); same(exact, fast);
                        const uint32_t result = accumulate ? v.accumulated : v.product;
                        for (unsigned n = 0; n < r.size(); ++n)
                            assert(cpuGetRegisters(exact.cpu)[n] == (n == d ? result : r[n]));
                        assert(cpuGetRegisters(exact.cpu)[15] == code + 4);
                        assert(cpuGetRegExternal(exact.cpu, ARM_REG_NUM_CPSR) ==
                               (cv | (flags ? nz(result) : oldNz) | 0xd3));
                    }
            }
    }
}
void literal(std::vector<uint32_t>& words, unsigned reg, uint32_t value) {
    words.push_back(0xe59f0000 | reg << 12); words.push_back(0xea000000); words.push_back(value);
}
Imx1ProbeResult probe(std::vector<uint32_t> words, bool fast) {
    Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
    return imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4,
                         Imx1ProbeEntry::physicalFlash, 100, nullptr, nullptr, fast, &options);
}
void guards() {
    for (bool fast : {false, true}) {
        for (uint32_t instruction : {0xe0121190u, 0xe0100190u, 0xe01f0190u,
                                     0xe0120f90u, 0xe012019fu, 0xe032f190u,
                                     0xe0902190u, 0xe1600281u}) {
            std::vector<uint32_t> p; literal(p, 0, 0x12345678); literal(p, 1, 0x76543210);
            p.push_back(instruction); p.push_back(0xe7f000f0);
            const auto r = probe(p, fast);
            assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == instruction &&
                   r.trace.step == 4 && !r.busAccesses && r.cpsr == 0xd3 &&
                   r.registers[0] == 0x12345678 && r.registers[1] == 0x76543210 &&
                   r.registers[15] == romBase + 24);
        }
        for (unsigned reg = 0; reg < 8; ++reg) {
            std::vector<uint32_t> p; literal(p, reg, 7); literal(p, 12, romBase + 28 + 1);
            p.push_back(0xe12fff1c);
            p.push_back(0xde000000 | (0x4340 | reg << 3 | reg));
            const auto r = probe(p, fast);
            assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instructionSize == 2 &&
                   r.trace.instruction == (0x4340 | reg << 3 | reg) && !r.busAccesses &&
                   r.registers[reg] == 7 && r.cpsr == 0xf3 && r.registers[15] == romBase + 28);
        }
        // Failed conditions skip valid flag-setting instructions and even rejected register forms.
        for (uint32_t instruction : {0x00120190u, 0x00323190u, 0x00100190u, 0x001f0190u}) {
            const auto r = probe({0xe3a00007, 0xe3a01003, instruction, 0xe7f000f0}, fast);
            assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == 0xe7f000f0 &&
                   r.registers[0] == 7 && r.registers[1] == 3 && !r.registers[2] &&
                   r.cpsr == 0xd3 && !r.busAccesses && r.registers[15] == romBase + 12);
        }
        // The probe admits ordinary scalar S forms and the reached Thumb encoding.
        std::vector<uint32_t> p; literal(p, 0, 0xfffffffe); literal(p, 1, 3);
        p.push_back(0xe0120190); p.push_back(0xe3a03007); p.push_back(0xe0343190);
        p.push_back(0xe7f000f0);
        auto r = probe(p, fast);
        assert(r.trace.instruction == 0xe7f000f0 && r.registers[2] == 0xfffffffa &&
               r.registers[4] == 1 && r.cpsr == 0xd3 && !r.busAccesses);
        p.clear(); literal(p, 2, 0x1c); literal(p, 4, 0); literal(p, 12, romBase + 40 + 1);
        p.push_back(0xe12fff1c); p.push_back(0xde004362);
        r = probe(p, fast);
        assert(r.trace.instruction == 0xde00 && !r.registers[2] && !r.registers[4] &&
               r.cpsr == 0x400000f3 && !r.busAccesses && r.registers[15] == romBase + 42);
    }
}
void normalBlocks() {
    Machine exact(false), fast(true, false);
    for (bool thumb : {false, true}) for (bool accumulate : {false, true}) {
        if (thumb && accumulate) continue;
        auto r = initial(); r[0] = 0xfffffffe; r[1] = 3; r[3] = 7;
        const uint32_t instruction = thumb ? 0x4348 : arm(2, 0, 1, accumulate ? 3 : 0, accumulate, true);
        for (Machine* m : {&exact, &fast}) {
            m->setup(instruction, thumb, 0x30000000, r);
            if (thumb) {
                for (unsigned offset = 2; offset < 32; offset += 2) m->half(code + offset, 0x46c0);
                m->half(code + 32, 0xe7fe);
            } else {
                for (unsigned offset = 4; offset < 32; offset += 4) m->word(code + offset, 0xe1a0a00a);
                m->word(code + 32, 0xeafffffe);
            }
        }
        uint64_t blocks = 0, instructions = 0;
        assert(cpuJitExecutionCounts(fast.cpu, &blocks, &instructions));
        const auto before = instructions, beforeBlocks = blocks;
        const unsigned count = thumb ? 16 : 8;
        for (unsigned i = 0; i < count; ++i) exact.step();
        assert((cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(fast.cpu, count) == count));
        same(exact, fast);
        assert(cpuGetRegisters(fast.cpu)[15] == code + 32);
        assert(cpuGetRegisters(fast.cpu)[thumb ? 0 : 2] == (accumulate ? 1u : 0xfffffffau));
        assert(cpuGetRegExternal(fast.cpu, ARM_REG_NUM_CPSR) ==
               (0x30000000 | (accumulate ? 0 : 0x80000000) | (thumb ? 0xf3 : 0xd3)));
        assert(cpuJitExecutionCounts(fast.cpu, &blocks, &instructions));
        assert(blocks > beforeBlocks && instructions - before == count);
        std::printf("PASS: generated %s multiply block, instructions=%u\n", thumb ? "Thumb" : accumulate ? "ARM MLAS" : "ARM MULS", count);
    }
}

uint32_t longArm(unsigned hi, unsigned lo, unsigned rm, unsigned rs,
                 bool signedProduct, bool accumulate, bool flags) {
    return 0xe0800090 | hi << 16 | lo << 12 | rs << 8 | rm |
           uint32_t(signedProduct) << 22 | uint32_t(accumulate) << 21 | uint32_t(flags) << 20;
}
uint32_t longNz(uint64_t result) {
    return (uint32_t(result >> 32) & 0x80000000u) | (result == 0 ? 0x40000000u : 0);
}
void longDirected() {
    struct Vector { uint32_t a, b; uint64_t unsignedProduct, signedProduct, add; };
    constexpr Vector vectors[] = {
        {0x12345678, 0, 0, 0, 0},
        {0xfffffffe, 3, 0x00000002fffffffauLL, 0xfffffffffffffffauLL, 7},
        {0x80000000, 1, 0x80000000uLL, 0xffffffff80000000uLL, 0x80000000uLL},
        {0x10000, 0x10000, 0x100000000uLL, 0x100000000uLL, 0xffffffffffffffffuLL},
        {0xffffffff, 0xffffffff, 0xfffffffe00000001uLL, 1, 0x1ffffffffuLL},
        {0x80000000, 0x80000000, 0x4000000000000000uLL, 0x4000000000000000uLL,
         0xc000000000000000uLL},
        {0x81a4e269, 0xd61829d1, 0x6c6c11e63fb7a8b9uLL, 0x14af05ac3fb7a8b9uLL, 0},
    };
    Machine exact(false), fast(true);
    for (const auto& v : vectors) for (bool signedProduct : {false, true})
        for (bool accumulate : {false, true}) for (bool flags : {false, true})
            for (uint32_t cv : {0u, 0x10000000u, 0x20000000u, 0x30000000u})
                for (uint32_t oldNz : {0u, 0x40000000u, 0x80000000u, 0xc0000000u})
                    for (unsigned alias : {0u, 1u, 2u}) {
                        // Rs may alias either destination. Rm remains distinct from both.
                        const unsigned hi = alias == 1 ? 1 : 4, lo = alias == 2 ? 1 : 3;
                        auto r = initial();
                        r[hi] = v.add >> 32; r[lo] = v.add; r[0] = v.a; r[1] = v.b;
                        const uint64_t add = uint64_t(r[hi]) << 32 | r[lo];
                        const uint64_t result = (signedProduct ? v.signedProduct : v.unsignedProduct) +
                                                (accumulate ? add : 0);
                        const uint32_t instruction = longArm(hi, lo, 0, 1, signedProduct, accumulate, flags);
                        for (Machine* m : {&exact, &fast}) m->setup(instruction, false, cv | oldNz, r);
                        exact.step(); fast.step(); same(exact, fast);
                        for (unsigned n = 0; n < r.size(); ++n)
                            assert(cpuGetRegisters(exact.cpu)[n] ==
                                   (n == hi ? uint32_t(result >> 32) : n == lo ? uint32_t(result) : r[n]));
                        assert(cpuGetRegisters(exact.cpu)[15] == code + 4);
                        assert(cpuGetRegExternal(exact.cpu, ARM_REG_NUM_CPSR) ==
                               (cv | (flags ? longNz(result) : oldNz) | 0xd3));
                    }
    // Captured UMULL r0,r12,r2,lr operands from the original-ROM stop at 203b84b8.
    auto r = initial();
    r[0] = 0xd61829d1; r[12] = 0xa536f0c7; r[2] = 0x81a4e269; r[14] = 0xd61829d1;
    for (Machine* m : {&exact, &fast}) m->setup(0xe08c0e92, false, 0x40000000, r);
    exact.step(); fast.step(); same(exact, fast);
    assert(cpuGetRegisters(exact.cpu)[0] == 0x3fb7a8b9 && cpuGetRegisters(exact.cpu)[12] == 0x6c6c11e6);
    assert(cpuGetRegExternal(exact.cpu, ARM_REG_NUM_CPSR) == 0x400000d3);
    // Equal multiplicands are valid when neither is a destination.
    for (Machine* m : {&exact, &fast}) m->setup(longArm(4, 3, 2, 2, false, false, true), false, 0, r);
    exact.step(); fast.step(); same(exact, fast);
    assert(cpuGetRegisters(exact.cpu)[3] == 0xb0c58f11 && cpuGetRegisters(exact.cpu)[4] == 0x41a79660);
}
void longGuards() {
    for (bool fast : {false, true}) {
        for (bool signedProduct : {false, true}) for (bool accumulate : {false, true})
            for (bool flags : {false, true}) {
                const auto encode = [&](unsigned hi, unsigned lo, unsigned rm, unsigned rs) {
                    return longArm(hi, lo, rm, rs, signedProduct, accumulate, flags);
                };
                for (uint32_t instruction : {encode(15, 3, 0, 1), encode(4, 15, 0, 1),
                        encode(4, 3, 15, 1), encode(4, 3, 0, 15), encode(3, 3, 0, 1),
                        encode(0, 3, 0, 1), encode(4, 0, 0, 1), 0xe0443190u, 0xe1643180u,
                        encode(4, 3, 0, 1) | 0xf0000000u}) {
                    const auto r = probe({instruction, 0xe7f000f0}, fast);
                    assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.step == 0 &&
                           r.trace.instruction == instruction && r.registers[15] == romBase &&
                           !r.busAccesses && r.cpsr == 0xd3);
                    for (unsigned reg = 0; reg < 15; ++reg) assert(!r.registers[reg]);
                }
                // Failed conditions skip both valid and conservatively excluded long forms.
                for (uint32_t instruction : {encode(4, 3, 0, 1), encode(0, 3, 0, 1), encode(3, 3, 0, 1)}) {
                    const auto r = probe({0xe3a00007, 0xe3a01003, instruction & 0x0fffffffu, 0xe7f000f0}, fast);
                    assert(r.trace.instruction == 0xe7f000f0 && r.registers[0] == 7 &&
                           r.registers[1] == 3 && !r.registers[3] && !r.registers[4] &&
                           r.registers[15] == romBase + 12 && !r.busAccesses && r.cpsr == 0xd3);
                }
                std::vector<uint32_t> p; literal(p, 0, 0xfffffffe); literal(p, 1, 3);
                literal(p, 3, 7); literal(p, 4, 0);
                p.push_back(encode(4, 3, 0, 1)); p.push_back(0xe7f000f0);
                const auto r = probe(p, fast);
                const uint64_t expected = (signedProduct ? 0xfffffffffffffffauLL : 0x2fffffffauLL) +
                                          (accumulate ? 7 : 0);
                assert(r.trace.instruction == 0xe7f000f0 && r.registers[3] == uint32_t(expected) &&
                       r.registers[4] == uint32_t(expected >> 32) && !r.busAccesses &&
                       r.cpsr == ((flags ? longNz(expected) : 0) | 0xd3));
            }
        std::vector<uint32_t> p;
        literal(p, 0, 0xd61829d1); literal(p, 12, 0xa536f0c7);
        literal(p, 2, 0x81a4e269); literal(p, 14, 0xd61829d1);
        p.push_back(0xe08c0e92); p.push_back(0xe7f000f0);
        const auto r = probe(p, fast);
        assert(r.trace.instruction == 0xe7f000f0 && r.registers[0] == 0x3fb7a8b9 &&
               r.registers[12] == 0x6c6c11e6 && r.registers[2] == 0x81a4e269 &&
               r.registers[14] == 0xd61829d1 && r.cpsr == 0xd3 && !r.busAccesses);
    }
}
void longNormalBlocks() {
    Machine exact(false), fast(true, false);
    for (bool signedProduct : {false, true}) for (bool accumulate : {false, true})
        for (bool flags : {false, true}) {
            auto r = initial(); r[0] = 0xfffffffe; r[1] = 3; r[3] = 7; r[4] = 0;
            for (Machine* m : {&exact, &fast}) {
                m->setup(longArm(4, 3, 0, 1, signedProduct, accumulate, flags), false, 0x70000000, r);
                for (unsigned offset = 4; offset < 32; offset += 4) m->word(code + offset, 0xe1a0a00a);
                m->word(code + 32, 0xeafffffe);
            }
            uint64_t blocks = 0, instructions = 0;
            assert(cpuJitExecutionCounts(fast.cpu, &blocks, &instructions));
            const auto before = instructions, beforeBlocks = blocks;
            for (unsigned i = 0; i < 8; ++i) exact.step();
            assert((cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(fast.cpu, 8) == 8));
            same(exact, fast);
            const uint64_t result = (signedProduct ? 0xfffffffffffffffauLL : 0x2fffffffauLL) +
                                    (accumulate ? 7 : 0);
            assert(cpuGetRegisters(fast.cpu)[3] == uint32_t(result) &&
                   cpuGetRegisters(fast.cpu)[4] == uint32_t(result >> 32) &&
                   cpuGetRegisters(fast.cpu)[15] == code + 32);
            assert(cpuGetRegExternal(fast.cpu, ARM_REG_NUM_CPSR) ==
                   (0x300000d3 | (flags ? longNz(result) : 0x40000000)));
            assert(cpuJitExecutionCounts(fast.cpu, &blocks, &instructions));
            assert(blocks > beforeBlocks && instructions - before == 8);
        }
    std::puts("PASS: all eight long multiply forms execute in generated ARM blocks");
}
}  // namespace
int main() {
    unsetenv("FROND_JIT");
    cpuSetJitSync(true);
    setenv("FROND_JIT_HOT", "1", 1);
    directed(); guards(); normalBlocks();
    longDirected(); longGuards(); longNormalBlocks();
    std::puts("PASS: ARMv4T scalar and long multiply, C/V preservation policy, operands and Exact/Fast parity");
}
