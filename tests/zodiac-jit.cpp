#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <fstream>
#include <vector>

#include "imx1_boot_probe.h"

#include "CPU.h"
#include "RAM.h"
#include "mem.h"
#include "memory_buffer.h"
#include "patch_dispatch.h"
#include "system_state.h"

namespace {
constexpr uint32_t code = 0x1000, data = 0x2000, target = 0x3000, size = 0x10000;
struct Machine {
    ArmCpu* cpu;
    uint8_t* bytes;
    explicit Machine(bool fast, bool singleStep = true) {
        assert(cpuSetFastEngine(fast));
        auto* mem = memInit();
        MemoryBuffer buffer{};
        assert(memoryBufferAllocate(&buffer, size));
        bytes = buffer.buffer;
        ramSetNoFramebuffer(ramInit(mem, nullptr, 0, size, &buffer, true));
        auto* patches = initPatchDispatch();
        cpu = cpuInit(code, mem, ARM_MEMORY_SYSTEM_MMU, false, false, -1, 0, 0,
                      patches, new PacePatch{}, createSystemState(), ArmCpuModel::arm920t, singleStep);
        patchDispatchSetCpu(patches, cpu);
    }
    void word(uint32_t address, uint32_t value) { std::memcpy(bytes + address, &value, 4); }
    void reset(uint32_t instruction, bool thumb) {
        std::memset(bytes, 0, size);
        cpuReset(cpu, code);
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, 0xd3 | (thumb ? 0x20 : 0));
        for (unsigned i = 0; i < 15; ++i) cpuSetReg(cpu, i, 0);
        cpuSetReg(cpu, 0, data);
        cpuSetReg(cpu, 1, target | 1);
        cpuSetReg(cpu, 13, data);
        cpuSetReg(cpu, 15, code);
        word(code, instruction);
        word(data, target | 1);
        word(target, 0xeafffffe);
    }
    void step() { assert((cpuCycleAnyKind(cpu, 1) == 1)); }
};
void same(Machine& exact, Machine& fast) {
    for (unsigned i = 0; i < 16; ++i) {
        if (cpuGetRegisters(exact.cpu)[i] != cpuGetRegisters(fast.cpu)[i]) {
            std::fprintf(stderr, "r%u: exact=%08x Fast=%08x\n", i,
                         cpuGetRegisters(exact.cpu)[i], cpuGetRegisters(fast.cpu)[i]);
            std::abort();
        }
    }
    assert(cpuGetRegExternal(exact.cpu, ARM_REG_NUM_CPSR) ==
           cpuGetRegExternal(fast.cpu, ARM_REG_NUM_CPSR));
    assert(cpuGetRegExternal(exact.cpu, ARM_REG_NUM_SPSR) ==
           cpuGetRegExternal(fast.cpu, ARM_REG_NUM_SPSR));
    assert(!std::memcmp(exact.bytes, fast.bytes, size));
}
}
static void collect(void* context, const Imx1ProbeTrace& trace) {
    static_cast<std::vector<Imx1ProbeTrace>*>(context)->push_back(trace);
}
static void compareRom(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    assert(input && input.tellg() == 0x7d0000);
    input.seekg(0);
    std::vector<uint8_t> rom(0x7d0000);
    assert(input.read(reinterpret_cast<char*>(rom.data()), rom.size()));
    for (auto entry : {Imx1ProbeEntry::physicalFlash, Imx1ProbeEntry::resetAlias, Imx1ProbeEntry::linkedAlias}) {
        std::vector<Imx1ProbeTrace> exact, fast;
        auto a = imx1BootProbe(rom.data(), rom.size(), entry, 100000, collect, &exact, false);
        auto b = imx1BootProbe(rom.data(), rom.size(), entry, 100000, collect, &fast, true);
        assert(a.stop == b.stop && a.address == b.address && a.value == b.value && a.size == b.size);
        assert(a.cpsr == b.cpsr && a.spsr == b.spsr);
        assert(!std::memcmp(a.registers, b.registers, sizeof(a.registers)));
        assert(exact.size() == fast.size());
        for (size_t i = 0; i < exact.size(); ++i) {
            if (exact[i].pc != fast[i].pc || exact[i].instruction != fast[i].instruction ||
                exact[i].cpsr != fast[i].cpsr || exact[i].operand != fast[i].operand || exact[i].instructionSize != fast[i].instructionSize)
                std::fprintf(stderr, "trace[%zu] exact pc=%08x instr=%08x cpsr=%08x operand=%08x; Fast pc=%08x instr=%08x cpsr=%08x operand=%08x\n",
                             i, exact[i].pc, exact[i].instruction, exact[i].cpsr, exact[i].operand,
                             fast[i].pc, fast[i].instruction, fast[i].cpsr, fast[i].operand);
            assert(exact[i].step == fast[i].step && exact[i].pc == fast[i].pc &&
                   exact[i].instruction == fast[i].instruction && exact[i].cpsr == fast[i].cpsr &&
                   exact[i].operand == fast[i].operand && exact[i].instructionSize == fast[i].instructionSize);
        }
        assert(!a.jitBlocks && !a.jitInstructions && b.jitBlocks && b.jitInstructions);
        std::printf("PASS: private SmallROM entry=%u trace=%zu stop=%s address=%08x blocks=%llu instructions=%llu\n",
                    unsigned(entry), exact.size(), imx1ProbeStopName(b.stop), b.address,
                    (unsigned long long)b.jitBlocks, (unsigned long long)b.jitInstructions);
    }
}
int main(int argc, char** argv) {
    unsetenv("FROND_JIT");
    Machine exact(false), fast(true);
    auto check = [&](uint32_t instruction, bool thumb = false) {
        exact.reset(instruction, thumb);
        fast.reset(instruction, thumb);
        exact.step(); fast.step(); same(exact, fast);
    };
    // ARMv4T ordinary PC writes preserve state, BX exchanges it.
    for (uint32_t i : {0xe590f000u, 0xe8908000u, 0xe1a0f001u}) {
        check(i);
        assert(!cpuIsThumb(fast.cpu) && cpuGetRegisters(fast.cpu)[15] == target);
    }
    check(0xe12fff11);
    assert(cpuIsThumb(fast.cpu) && cpuGetRegisters(fast.cpu)[15] == target);
    check(0x0000bd00, true); // POP {pc} preserves Thumb.
    assert(cpuIsThumb(fast.cpu) && cpuGetRegisters(fast.cpu)[15] == target);
    check(0x00004700, true); // BX r0 enters ARM.
    assert(!cpuIsThumb(fast.cpu) && cpuGetRegisters(fast.cpu)[15] == data);
    // ARMv5-only instructions take undefined without executing their operation.
    for (uint32_t i : {0xe12fff31u, 0xfa000000u, 0xe16f1f10u, 0xe1200070u,
                       0xe1000051u, 0xe1600281u, 0xe1c020d0u, 0xe1c020f0u,
                       0xf5d0f000u, 0xfe000f10u, 0xec410f02u}) {
        check(i);
        assert((cpuGetRegExternal(fast.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_UND);
        assert(cpuGetRegisters(fast.cpu)[15] == 4);
    }
    for (uint32_t i : {0x00004780u, 0x0000e800u, 0x0000be00u}) {
        check(i, true);
        assert((cpuGetRegExternal(fast.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_UND);
    }
    check(0x016f1f10); // CLZEQ does not execute with Z clear.
    assert(cpuGetRegisters(fast.cpu)[15] == code + 4);
    // Ordinary integer, memory, multiply and Thumb forms still compile.
    for (uint32_t i : {0xe2812003u, 0xe0902001u, 0xe0020190u, 0xe5801004u, 0xe5902000u,
                       0xe1d020b0u, 0xe1d020d0u, 0xe1d020f0u, 0xe1002091u}) check(i);
    check(0x00002242, true);
    assert(cpuGetRegisters(fast.cpu)[2] == 0x42);
    // A guest write to uncached RAM drops translated code before it runs again.
    for (Machine* m : {&exact, &fast}) {
        m->reset(0xe3a02001, false);
        m->word(code + 4, 0xe5801000); // STR r1, [r0]
        m->word(code + 8, 0xe12fff10); // BX r0
        cpuSetReg(m->cpu, 0, code);
        cpuSetReg(m->cpu, 1, 0xe3a02002);
    }
    for (unsigned i = 0; i < 4; ++i) { exact.step(); fast.step(); same(exact, fast); }
    assert(cpuGetRegisters(fast.cpu)[2] == 2);
    // Normal blocks stop at line boundaries and preserve final state across linked PCs.
    Machine normal(true, false), normalExact(false);
    normalExact.reset(0xe3a02020, false);
    normal.reset(0xe3a02020, false);
    for (Machine* m : {&normalExact, &normal}) {
        m->word(code + 4, 0xe2522001); // SUBS r2, r2, #1
        m->word(code + 8, 0x1afffffd); // BNE code+4
        m->word(code + 12, 0xeafffffe);
    }
    for (unsigned i = 0; i < 1000 && cpuGetRegisters(normalExact.cpu)[15] != code + 12; ++i) normalExact.step();
    for (unsigned i = 0; i < 1000 && cpuGetRegisters(normal.cpu)[15] != code + 12; ++i)
        cpuCycleAnyKind(normal.cpu, 8);
    same(normalExact, normal);
    assert(cpuGetRegisters(normal.cpu)[2] == 0);
    for (Machine* m : {&normalExact, &normal}) {
        m->reset(0xe3a02000, false);
        for (unsigned i = 1; i < 16; ++i) m->word(code + i * 4, 0xe2822001);
        m->word(code + 64, 0xeafffffe);
    }
    for (unsigned i = 0; i < 16; ++i) normalExact.step();
    for (unsigned i = 0; i < 1000 && cpuGetRegisters(normal.cpu)[15] != code + 64; ++i)
        cpuCycleAnyKind(normal.cpu, 8);
    same(normalExact, normal);
    assert(cpuGetRegisters(normal.cpu)[2] == 15);
    // Misaligned word loads retain the interpreter's rotation.
    for (Machine* m : {&exact, &fast}) {
        m->reset(0xe5902001, false);
        m->word(data, 0x12345678);
    }
    exact.step(); fast.step(); same(exact, fast);
    assert(cpuGetRegisters(fast.cpu)[2] == 0x78123456);
    // CP15 bootstrap reset control and rejected cache enable use the same interpreter path.
    check(0xee112f10);
    assert(cpuGetRegisters(fast.cpu)[2] == 0x78);
    check(0xee010f10);
    assert((cpuGetRegExternal(fast.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_UND);
    if (argc == 2) compareRom(argv[1]);
    uint64_t blocks = 0, instructions = 0;
    assert(cpuJitExecutionCounts(fast.cpu, &blocks, &instructions));
    assert(blocks && instructions);
    assert(!cpuJitExecutionCounts(exact.cpu, &blocks, &instructions));
    std::printf("PASS: ARM920 Exact/Fast instruction and exception parity, compiled blocks=%llu instructions=%llu\n",
                (unsigned long long)blocks, (unsigned long long)instructions);
}
