// ARMv4 SWP and SWPB on the ARM920T: Exact and Fast give the same registers and memory, a misaligned SWP
// rotates what it reads and writes the aligned word, the probe admits the valid forms (Rd may be Rm) and stops
// at the unpredictable ones, and a Fast block that reaches a swap carries on past it with Exact's result.
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
constexpr uint32_t code = 0x1000, data = 0x2000, romBase = 0x10000000;
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
    explicit Machine(bool fast, bool singleStep = true) {
        assert(cpuSetFastEngine(fast));
        auto* mem = memInit();
        assert(memRegionAdd(mem, 0, bytes.size(), memory, this));
        auto* patches = initPatchDispatch();
        cpu = cpuInit(code, mem, ARM_MEMORY_SYSTEM_MMU, false, false, -1, 0, 0, patches, new PacePatch{},
                      createSystemState(), ArmCpuModel::arm920t, singleStep);
        patchDispatchSetCpu(patches, cpu);
        // The swap's cache check asks which memory is plain RAM: all of it here.
        cpuArm920SetMemoryCheck(cpu, [](void* context, uint32_t address, uint32_t size, bool) {
            return uint64_t(address) + size <= static_cast<Machine*>(context)->bytes.size();
        }, this);
    }
    void word(uint32_t address, uint32_t value) { std::memcpy(bytes.data() + address, &value, 4); }
    uint32_t word(uint32_t address) const { uint32_t v; std::memcpy(&v, bytes.data() + address, 4); return v; }
    void setup(const std::vector<uint32_t>& program, const std::array<uint32_t, 15>& regs) {
        std::fill(bytes.begin(), bytes.end(), 0);
        cpuReset(cpu, code);
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, 0xd3);
        for (unsigned i = 0; i < regs.size(); ++i) cpuSetReg(cpu, i, regs[i]);
        cpuSetReg(cpu, 15, code);
        for (size_t n = 0; n < program.size(); ++n) word(code + 4 * n, program[n]);
        word(data, 0x11223344); word(data + 4, 0x55667788);
    }
    void step() { assert((cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, 1) == 1)); }
};
void same(const Machine& a, const Machine& b) {
    assert(!std::memcmp(cpuGetRegisters(a.cpu), cpuGetRegisters(b.cpu), 16 * sizeof(uint32_t)));
    assert(cpuGetRegExternal(a.cpu, ARM_REG_NUM_CPSR) == cpuGetRegExternal(b.cpu, ARM_REG_NUM_CPSR));
    assert(a.bytes == b.bytes);
}
uint32_t swp(unsigned rd, unsigned rm, unsigned rn, bool byte) {
    return 0xe1000090 | uint32_t(byte) << 22 | rn << 16 | rd << 12 | rm;
}
std::array<uint32_t, 15> registers() {
    std::array<uint32_t, 15> r{};
    for (unsigned i = 0; i < r.size(); ++i) r[i] = 0x12340000 + i;
    return r;
}

void directed() {
    Machine exact(false), fast(true);
    struct Case { uint32_t instruction, address, rm; uint32_t loaded, stored0, stored4; unsigned rd; };
    const Case cases[] = {
        // SWP r0, r1, [r2]: the old word to r0, r1 to memory.
        {swp(0, 1, 2, false), data, 0xaabbccdd, 0x11223344, 0xaabbccdd, 0x55667788, 0},
        // SWPB r0, r0, [r3] (the games' form): the old byte to r0, r0's low byte to memory.
        {swp(0, 0, 3, true), data + 1, 0xaabbccdd, 0x33, 0x1122dd44, 0x55667788, 0},
        {swp(5, 6, 7, true), data + 7, 0x000000ee, 0x55, 0x11223344, 0xee667788, 5},
        // A misaligned SWP reads the aligned word rotated and writes Rm to the aligned word.
        {swp(0, 1, 2, false), data + 6, 0xaabbccdd, 0x77885566, 0x11223344, 0xaabbccdd, 0},
    };
    for (const auto& c : cases) {
        auto r = registers();
        const unsigned rn = (c.instruction >> 16) & 15, rm = c.instruction & 15;
        r[rn] = c.address; r[rm] = c.rm;
        for (Machine* m : {&exact, &fast}) m->setup({c.instruction}, r);
        exact.step(); fast.step(); same(exact, fast);
        for (unsigned n = 0; n < 15; ++n) {
            const uint32_t expected = n == c.rd ? c.loaded : r[n];
            if (cpuGetRegisters(exact.cpu)[n] != expected)
                fprintf(stderr, "%08x r%u: %08x, expected %08x\n", c.instruction, n, cpuGetRegisters(exact.cpu)[n], expected);
            assert(cpuGetRegisters(exact.cpu)[n] == expected);
        }
        assert(exact.word(data) == c.stored0 && exact.word(data + 4) == c.stored4);
        assert(cpuGetRegisters(exact.cpu)[15] == code + 4 && cpuGetRegExternal(exact.cpu, ARM_REG_NUM_CPSR) == 0xd3);
    }
    // A failed condition changes nothing.
    auto r = registers(); r[2] = data;
    for (Machine* m : {&exact, &fast}) m->setup({swp(0, 1, 2, false) & 0x0fffffffu}, r);
    exact.step(); fast.step(); same(exact, fast);
    assert(cpuGetRegisters(exact.cpu)[0] == r[0] && exact.word(data) == 0x11223344);
    std::puts("PASS: SWP and SWPB, aligned, misaligned and Rd = Rm, Exact and Fast agree");
}

// A Fast block with a swap in the middle carries on after it as Exact does.
void blocks() {
    Machine exact(false), fast(true, false);
    auto r = registers(); r[3] = data; r[1] = 0xcafef00d;
    std::vector<uint32_t> program;
    for (unsigned n = 0; n < 3; ++n) program.push_back(0xe2844001);  // add r4, r4, #1
    program.push_back(swp(0, 1, 3, false));
    program.push_back(swp(1, 1, 3, true));
    for (unsigned n = 0; n < 3; ++n) program.push_back(0xe2855001);  // add r5, r5, #1
    program.push_back(0xeafffffe);
    for (Machine* m : {&exact, &fast}) m->setup(program, r);
    // A Fast run goes on to the end of its block, so it may also take the closing branch to itself. Exact
    // steps as many.
    uint32_t retired = 0;
    for (unsigned runs = 0; runs < 16 && retired < 8; ++runs)
        retired += cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(fast.cpu, 8 - retired);
    assert(retired >= 8 && retired < 16);
    for (unsigned i = 0; i < retired; ++i) exact.step();
    same(exact, fast);
    assert(cpuGetRegisters(fast.cpu)[0] == 0x11223344 && cpuGetRegisters(fast.cpu)[1] == 0x0d);
    assert(fast.word(data) == 0xcafef00d && cpuGetRegisters(fast.cpu)[15] == code + 32);
    std::puts("PASS: Fast blocks run through SWP and SWPB with Exact's result");
}

void literal(std::vector<uint32_t>& words, unsigned reg, uint32_t value) {
    words.push_back(0xe59f0000 | reg << 12); words.push_back(0xea000000); words.push_back(value);
}
Imx1ProbeResult probe(std::vector<uint32_t> words, bool fast) {
    Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
    return imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4,
                         Imx1ProbeEntry::physicalFlash, 100, nullptr, nullptr, fast, &options);
}
void probeForms() {
    constexpr uint32_t sram = 0x00300010;
    for (bool fast : {false, true}) {
        // The swaps run in internal SRAM and the probe goes on to the undefined word after them.
        std::vector<uint32_t> p;
        literal(p, 3, sram); literal(p, 0, 0x11223344); literal(p, 1, 0xaabbccdd);
        p.push_back(0xe5830000);           // str r0, [r3]
        p.push_back(swp(2, 1, 3, false));  // r2 = 11223344, memory aabbccdd
        p.push_back(0xe5934000);           // ldr r4, [r3]
        p.push_back(swp(0, 0, 3, true));   // r0 = dd, memory aabbcc44
        p.push_back(0xe5936000);           // ldr r6, [r3]
        p.push_back(0xe7f000f0);
        auto result = probe(p, fast);
        assert(result.stop == Imx1ProbeStop::unsupportedInstruction && result.trace.instruction == 0xe7f000f0);
        assert(result.registers[2] == 0x11223344 && result.registers[4] == 0xaabbccdd &&
               result.registers[0] == 0xdd && result.registers[6] == 0xaabbcc44);
        // Unpredictable forms stop at the swap with nothing changed.
        for (uint32_t instruction : {swp(3, 1, 3, false), swp(0, 3, 3, true), swp(0, 1, 15, false),
                                     swp(15, 1, 3, false), swp(0, 15, 3, true)}) {
            std::vector<uint32_t> q; literal(q, 3, sram); literal(q, 1, 0xaabbccdd);
            q.push_back(instruction); q.push_back(0xe7f000f0);
            result = probe(q, fast);
            assert(result.stop == Imx1ProbeStop::unsupportedInstruction && result.trace.instruction == instruction &&
                   result.registers[15] == romBase + 24 && result.registers[3] == sram && !result.registers[0]);
        }
    }
    std::puts("PASS: the probe admits SWP and SWPB and stops at their unpredictable forms, Exact and Fast");
}
}  // namespace

int main() {
    unsetenv("FROND_JIT");
    cpuSetJitSync(true);
    setenv("FROND_JIT_HOT", "1", 1);
    directed(); blocks(); probeForms();
    std::puts("PASS: ARMv4 swap");
}
