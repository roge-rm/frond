// The ARM920T's FCSE on the interpreter (Exact): a nonzero PID moves VAs below 32 MB to VA | PID for
// fetches and data, aborts report the MVA in FAR, VAs from 32 MB up stay as they are, and the I-cache and
// D-cache hold MVAs, so a PID change never hits another process's lines. Fast refuses to run while the
// PID is nonzero.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "CPU.h"
#include "RAM.h"
#include "mem.h"
#include "memory_buffer.h"
#include "patch_dispatch.h"
#include "system_state.h"

namespace {
constexpr uint32_t ramSize = 4u << 20, ttb = 0x300000;
constexpr uint32_t slot1 = 0x02000000, slot2 = 0x04000000, slot3 = 0x06000000;

// MCR/MRC p15, 0, Rd, CRn, c0, 0
constexpr uint32_t mcr(unsigned crn, unsigned rd) { return 0xee000f10u | crn << 16 | rd << 12; }
constexpr uint32_t mrc(unsigned crn, unsigned rd) { return 0xee100f10u | crn << 16 | rd << 12; }
constexpr uint32_t movPc(unsigned rm) { return 0xe1a0f000u | rm; }
constexpr uint32_t movImm(unsigned rd, uint32_t imm) { return 0xe3a00000u | rd << 12 | imm; }
constexpr uint32_t ldr(unsigned rd, unsigned rn) { return 0xe5900000u | rn << 16 | rd << 12; }
constexpr uint32_t str(unsigned rd, unsigned rn) { return 0xe5800000u | rn << 16 | rd << 12; }
constexpr uint32_t loop = 0xeafffffe;

struct Machine {
    ArmCpu* cpu;
    uint8_t* bytes;
    explicit Machine(bool fast) {
        assert(cpuSetFastEngine(fast));
        auto* mem = memInit();
        MemoryBuffer buffer{};
        assert(memoryBufferAllocate(&buffer, ramSize));
        bytes = buffer.buffer;
        ramSetNoFramebuffer(ramInit(mem, nullptr, 0, ramSize, &buffer, true));
        auto* patches = initPatchDispatch();
        cpu = cpuInit(0x100, mem, ARM_MEMORY_SYSTEM_MMU, false, false, -1, 0, 0, patches, new PacePatch{},
                      createSystemState(), ArmCpuModel::arm920t, true);
        patchDispatchSetCpu(patches, cpu);
    }
    void word(uint32_t pa, uint32_t value) { std::memcpy(bytes + pa, &value, 4); }
    uint32_t reg(unsigned r) const { return cpuGetRegisters(cpu)[r]; }
    uint32_t mode() const { return cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR) & 31; }
    void step() { assert(cpuCycleAnyKind(cpu, 1) == 1); }
    void runTo(uint32_t pc, unsigned limit = 100) {
        for (unsigned i = 0; i < limit && reg(15) != pc; ++i) step();
        assert(reg(15) == pc);
    }
    // Sections, domain 0 manager, MMU, D-cache, I-cache and high vectors on, then pc at 0xc0000200. The
    // test's code goes in between prepare and start, as the I-cache keeps what it fetched.
    void prepare(uint32_t pid) {
        std::memset(bytes, 0, ramSize);
        auto section = [&](uint32_t va, uint32_t pa) { word(ttb + (va >> 20) * 4, pa | 0xc1e); };
        section(0xc0000000, 0x000000);
        section(0xfff00000, 0x000000);
        section(slot1, 0x100000);
        section(slot2, 0x200000);
        // VA 0 and slot 3 are left unmapped.
        cpuReset(cpu, 0x100);
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, 0xd3);
        for (unsigned i = 0; i < 15; ++i) cpuSetReg(cpu, i, 0);
        cpuSetReg(cpu, 15, 0x100);
        cpuSetReg(cpu, 3, pid);
        cpuSetReg(cpu, 4, ttb);
        cpuSetReg(cpu, 5, 3);
        cpuSetReg(cpu, 6, 0x78 | 0x1 | 0x4 | 0x1000 | 0x2000);
        cpuSetReg(cpu, 7, 0xc0000200);
        word(0x100, mcr(2, 4));
        word(0x104, mcr(3, 5));
        word(0x108, mcr(1, 6));
        word(0x10c, movPc(7));
        word(0x110, loop);
        word(0x200, mcr(13, 3));
        // The data abort vector at 0xffff0010 reads FAR into r1 and FSR into r14.
        word(0xf0010, mrc(6, 1));
        word(0xf0014, mrc(5, 14));
        word(0xf0018, loop);
    }
    void start() {
        const uint32_t pid = reg(3);
        runTo(0xc0000200);
        step();  // the PID write
        assert(cpuGetPid(cpu) == pid);
    }
};
}

int main() {
    Machine m(false);

    // Data below 32 MB goes to VA | PID; PID 0 leaves VA 0 unmapped.
    m.prepare(slot1);
    m.word(0x103000, 0x11111111);
    m.word(0x204, ldr(2, 0));
    m.word(0x208, loop);
    cpuSetReg(m.cpu, 0, 0x3000);
    m.start();
    m.step();
    assert(m.reg(2) == 0x11111111 && m.mode() == ARM_SR_MODE_SVC);

    m.prepare(0);
    m.word(0x204, ldr(2, 0));
    cpuSetReg(m.cpu, 0, 0x3000);
    m.start();
    m.step();
    assert(m.mode() == ARM_SR_MODE_ABT);

    // An abort's FAR is the MVA: slot 3 is unmapped, a section translation fault (5) at 0x06003000.
    m.prepare(slot3);
    m.word(0x204, ldr(2, 0));
    cpuSetReg(m.cpu, 0, 0x3000);
    m.start();
    m.step();
    assert(m.mode() == ARM_SR_MODE_ABT && m.reg(15) == 0xffff0010);
    m.step();
    m.step();
    assert(m.reg(1) == slot3 + 0x3000 && (m.reg(14) & 0xf) == 5);

    // VAs from 32 MB up aren't moved.
    m.prepare(slot1);
    m.word(0x3000, 0x33333333);
    m.word(0x204, ldr(2, 0));
    cpuSetReg(m.cpu, 0, 0xc0003000);
    m.start();
    m.step();
    assert(m.reg(2) == 0x33333333);

    // Fetches: the same VA 0x1000 runs slot 1's code, then slot 2's after a PID change, with the I-cache on
    // (the first fetch filled a line for MVA 0x02001000).
    m.prepare(slot1);
    m.word(0x204, movPc(10));
    m.word(0x101000, movImm(2, 1));
    m.word(0x101004, movPc(8));
    m.word(0x201000, movImm(2, 2));
    m.word(0x201004, movPc(8));
    m.word(0x300, mcr(13, 9));
    m.word(0x304, movPc(10));
    cpuSetReg(m.cpu, 8, 0xc0000300);
    cpuSetReg(m.cpu, 9, slot2);
    cpuSetReg(m.cpu, 10, 0x1000);
    m.start();
    m.runTo(0xc0000300);
    assert(m.reg(2) == 1);
    m.step();
    m.runTo(0xc0000300);
    assert(m.reg(2) == 2 && cpuGetPid(m.cpu) == slot2);

    // Data through the write-back D-cache: a store under slot 2 and a load of the same VA under slot 1 are
    // different MVAs and different memory; back under slot 2 the store is there. Then PID 0 again reads the
    // CP15 register as 0.
    m.prepare(slot2);
    m.word(0x103000, 0x11111111);
    m.word(0x204, str(11, 12));
    m.word(0x208, mcr(13, 9));
    m.word(0x20c, ldr(2, 12));
    m.word(0x210, mcr(13, 13));
    m.word(0x214, ldr(5, 12));
    m.word(0x218, mcr(13, 0));
    m.word(0x21c, mrc(13, 4));
    m.word(0x220, loop);
    cpuSetReg(m.cpu, 0, 0);
    cpuSetReg(m.cpu, 9, slot1);
    cpuSetReg(m.cpu, 11, 0x22222222);
    cpuSetReg(m.cpu, 12, 0x3000);
    cpuSetReg(m.cpu, 13, slot2);
    m.start();
    cpuSetReg(m.cpu, 4, 1);
    m.runTo(0xc0000220);
    assert(m.reg(2) == 0x11111111 && m.reg(5) == 0x22222222);
    assert(m.reg(4) == 0 && cpuGetPid(m.cpu) == 0);

    // Fast, where it's built, leaves a nonzero PID to the interpreter.
    if (!cpuSetFastEngine(true)) {
        std::printf("PASS: ARM920T FCSE remap, abort FAR, MVA keyed caches across PID changes (no Fast engine)\n");
        return 0;
    }
    Machine fast(true);
    fast.prepare(slot1);
    fast.word(0x101000, movImm(2, 7));
    fast.word(0x101004, loop);
    fast.word(0x204, movPc(10));
    cpuSetReg(fast.cpu, 10, 0x1000);
    fast.start();
    assert(!cpuArm920JitCanRun(fast.cpu));
    fast.runTo(0x1004);
    assert(fast.reg(2) == 7);

    std::printf("PASS: ARM920T FCSE remap, abort FAR, MVA keyed caches across PID changes\n");
}
