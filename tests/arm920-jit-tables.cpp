// The ARM920T in Fast against Exact when the guest edits a page table entry under translated code. First the
// TLB is flushed and then the entry for a hot code page is changed with no flush after it, so the next fetch
// walks the new entry. Then a data load fills the TLB from the old entry and the table is changed back with
// no flush, so the stale TLB entry keeps the old page. Last the entry is made a fault one and flushed, then
// set to a third page with no flush (as WinCE maps a slot back). Fast runs the same code as Exact each time.
// Data pages are kept across PID changes and TLB invalidations too: loads at 32 MB and up and below it go on
// with the mapping each change leaves.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "CPU.h"
#include "RAM.h"
#include "mem.h"
#include "memory_buffer.h"
#include "patch_dispatch.h"
#include "system_state.h"

namespace {
constexpr uint32_t ramSize = 4u << 20, ttb = 0x300000, l2 = 0x310000;
// Small pages for VA 0x1000, uncached, read/write for all: A at PA 0x101000, B at PA 0x201000, C at 0x281000.
constexpr uint32_t pageA = 0x101ff2, pageB = 0x201ff2, pageC = 0x281ff2;

constexpr uint32_t mcr(unsigned crn, unsigned rd) { return 0xee000f10u | crn << 16 | rd << 12; }
constexpr uint32_t movPc(unsigned rm) { return 0xe1a0f000u | rm; }
constexpr uint32_t movReg(unsigned rd, unsigned rm) { return 0xe1a00000u | rd << 12 | rm; }
constexpr uint32_t ldr(unsigned rd, unsigned rn) { return 0xe5900000u | rn << 16 | rd << 12; }
constexpr uint32_t str(unsigned rd, unsigned rn) { return 0xe5800000u | rn << 16 | rd << 12; }
constexpr uint32_t loop = 0xeafffffe, nop = 0xe1a00000;
constexpr uint32_t tlbFlush = 0xee080f17, icacheInvalidate = 0xee070f15;

struct Machine {
    ArmCpu* cpu;
    uint8_t* bytes;
    Machine() {
        auto* mem = memInit();
        MemoryBuffer buffer{};
        assert(memoryBufferAllocate(&buffer, ramSize));
        bytes = buffer.buffer;
        std::memset(bytes, 0, ramSize);
        ramSetNoFramebuffer(ramInit(mem, nullptr, 0, ramSize, &buffer, true));
        auto* patches = initPatchDispatch();
        cpu = cpuInit(0x100, mem, ARM_MEMORY_SYSTEM_MMU, false, false, -1, 0, 0, patches, new PacePatch{},
                      createSystemState(), ArmCpuModel::arm920t, false);
        patchDispatchSetCpu(patches, cpu);
    }
    void word(uint32_t pa, uint32_t value) { std::memcpy(bytes + pa, &value, 4); }
    uint32_t reg(unsigned r) const { return cpuGetRegisters(cpu)[r]; }
    void step() { assert(cpuCycleAnyKind(cpu, 1) == 1); }
    void setup() {
        // Kernel sections at 0xc0000000 and 0xfff00000 (cached), the tables uncached at 0xd0300000, and VA 0
        // through a coarse table.
        word(ttb + (0xc0000000 >> 20) * 4, 0x000000 | 0xc1e);
        word(ttb + (0xfff00000 >> 20) * 4, 0x000000 | 0xc1e);
        word(ttb + (0xd0300000 >> 20) * 4, 0x300000 | 0xc12);
        word(ttb, l2 | 0x11);
        word(l2 + 4, pageA);
        word(0x101000, 0xe2822001);  // ADD r2, r2, #1
        word(0x101004, movPc(8));
        word(0x201000, 0xe2822801);  // ADD r2, r2, #0x10000
        word(0x201004, movPc(8));
        word(0x281000, 0xe2822c01);  // ADD r2, r2, #0x100
        word(0x281004, movPc(8));
        // Bootstrap: TTB, domain 0 manager, MMU with caches and high vectors, then 0xc0000200.
        word(0x100, mcr(2, 4));
        word(0x104, mcr(3, 5));
        word(0x108, mcr(1, 6));
        word(0x10c, movPc(7));
        word(0x200, 0xea00003e);  // B 0x300
        // Forty calls to VA 0x1000 (A).
        word(0x300, movPc(10));
        word(0x304, 0xe25bb001);  // SUBS r11, r11, #1
        word(0x308, 0x1afffffc);  // BNE 0x300
        // Flush, then map B with no flush after: the next fetch walks B.
        word(0x30c, tlbFlush);
        word(0x310, str(12, 13));
        word(0x314, icacheInvalidate);
        word(0x318, mcr(13, 3));
        word(0x31c, movReg(8, 9));
        word(0x320, movPc(10));
        // Flush, map A, load through VA 0x1000 (the TLB holds A), map B with no flush: the fetch still gets A.
        word(0x340, tlbFlush);
        word(0x344, nop);
        word(0x348, str(7, 13));
        word(0x34c, ldr(6, 10));
        word(0x350, str(12, 13));
        word(0x354, icacheInvalidate);
        word(0x358, mcr(13, 3));
        word(0x35c, movReg(8, 1));
        word(0x360, movPc(10));
        // Fault entry and flush, then C over the fault entry with no flush: the fetch walks C.
        word(0x3a0, str(3, 13));
        word(0x3a4, tlbFlush);
        word(0x3a8, nop);
        word(0x3ac, str(14, 13));
        word(0x3b0, icacheInvalidate);
        word(0x3b4, movReg(8, 4));
        word(0x3b8, movPc(10));
        word(0x3e0, loop);
        cpuReset(cpu, 0x100);
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, 0xd3);
        for (unsigned i = 0; i < 15; ++i) cpuSetReg(cpu, i, 0);
        cpuSetReg(cpu, 15, 0x100);
        cpuSetReg(cpu, 4, ttb);
        cpuSetReg(cpu, 5, 3);
        cpuSetReg(cpu, 6, 0x78 | 0x1 | 0x4 | 0x1000 | 0x2000);
        cpuSetReg(cpu, 7, 0xc0000200);
        for (unsigned i = 0; i < 4; ++i) step();
        assert(reg(15) == 0xc0000200);
        cpuSetReg(cpu, 1, 0xc00003a0);
        cpuSetReg(cpu, 4, 0xc00003e0);
        cpuSetReg(cpu, 14, pageC);
        cpuSetReg(cpu, 3, 0);
        cpuSetReg(cpu, 7, pageA);
        cpuSetReg(cpu, 8, 0xc0000304);
        cpuSetReg(cpu, 9, 0xc0000340);
        cpuSetReg(cpu, 10, 0x1000);
        cpuSetReg(cpu, 11, 40);
        cpuSetReg(cpu, 12, pageB);
        cpuSetReg(cpu, 13, 0xd0300000 + (l2 - ttb) + 4);
    }
};

void enableJit(Machine& machine) {
    auto leanPage = [](void* context, uint32_t pa, bool, bool* counted) -> uint8_t* {
        *counted = true;
        return pa < ramSize ? static_cast<Machine*>(context)->bytes + (pa & ~0xfffu) : nullptr;
    };
    cpuArm920SetLean(machine.cpu, leanPage, [](uint32_t, const uint32_t*, uint32_t, uint32_t) { return true; },
                    &machine);
    assert(cpuArm920SetJit(machine.cpu, true));
    cpuArm920LeanReset(machine.cpu);
}

void testSuspendedSlot() {
    constexpr uint32_t slot = 0x02000000, otherSlot = 0x04000000;
    constexpr uint32_t slotTable = l2 + 0x400, slotPte = slotTable + 8;
    constexpr uint32_t original = 0x102ff2, replacement = 0x202ff2;
    constexpr uint32_t entry = 0x1ffc, kernel = 0xc0000400, park = 0xc0000440;
    Machine exact;
    Machine fast;
    for (auto* machine : {&exact, &fast}) {
        machine->setup();
        machine->word(ttb + (slot >> 20) * 4, slotTable | 0x11);
        machine->word(ttb + (0xd0200000 >> 20) * 4, 0x100c12);
        machine->word(slotTable + 4, pageA);
        machine->word(slotPte, original);
        machine->word(0x101ffc, 0xe2822001);  // ADD r2, r2, #1
        machine->word(0x102000, 0xe2822c01);  // ADD r2, r2, #0x100
        machine->word(0x102004, movPc(8));
        machine->word(0x202000, 0xe2822801);  // ADD r2, r2, #0x10000
        machine->word(0x202004, movPc(8));
        machine->word(0x400, mcr(13, 0));
        machine->word(0x404, str(0, 13));
        machine->word(0x408, tlbFlush);
        machine->word(0x40c, icacheInvalidate);
        machine->word(0x410, ldr(1, 10));
        machine->word(0x414, str(0, 11));
        machine->word(0x440, loop);
        cpuSetReg(machine->cpu, 8, park);
        cpuSetReg(machine->cpu, 10, slot + 0x2000);
        cpuSetReg(machine->cpu, 11, 0xd0202000);
        cpuSetReg(machine->cpu, 13, 0xd0300000 + slotPte - ttb);
    }
    enableJit(fast);

    auto step = [&](uint32_t pc, uint32_t value = 0) {
        for (auto* machine : {&exact, &fast}) {
            cpuSetReg(machine->cpu, 0, value);
            cpuSetReg(machine->cpu, 15, pc);
            machine->step();
        }
    };
    auto run = [&](uint32_t pc, int expectedStop) {
        cpuSetReg(exact.cpu, 15, pc);
        cpuSetReg(fast.cpu, 15, pc);
        uint32_t ran = 0;
        int stop = arm920JitExit;
        cpuArm920RunJit(fast.cpu, 16, &stop, 16, &ran);
        assert(stop == expectedStop);
        assert((expectedStop == arm920JitBudget) == (ran != 0));
        for (uint32_t i = 0; i < ran; ++i) exact.step();
        for (unsigned r = 0; r < 16; ++r) assert(exact.reg(r) == fast.reg(r));
    };
    auto suspend = [&] {
        step(kernel, otherSlot);
        step(kernel + 4);
        step(kernel + 8);
        step(kernel + 12);
        run(park, arm920JitBudget);
    };

    // Use the high FPSCR tag bits for the slot whose page will be guarded.
    for (uint32_t pid = 3; pid < 10; ++pid) {
        step(kernel, pid * slot);
        run(park, arm920JitBudget);
    }
    step(kernel, slot);
    run(entry, arm920JitBudget);
    assert(fast.reg(2) == 0x101);

    // The block starts on a mapped page and crosses into a suspended page.
    suspend();
    step(kernel, slot);
    run(entry, arm920JitCold);
    step(kernel + 4, original);
    run(entry, arm920JitBudget);
    assert(fast.reg(2) == 0x202);

    // The guest fills the old translation through the MVA while running a different PID.
    suspend();
    step(kernel + 4, original);
    step(kernel + 16);
    run(slot + 0x2000, arm920JitBudget);
    step(kernel + 4, replacement);
    step(kernel, slot);
    run(entry, arm920JitBudget);
    assert(fast.reg(2) == 0x403);

    // Once the guest flushes it, the replacement mapping takes effect.
    step(kernel + 8);
    step(kernel + 12);
    run(entry, arm920JitBudget);
    assert(fast.reg(2) == 0x10404);

    // An entry with no compiled block revalidates the guarded page before compiling it.
    suspend();
    step(kernel + 4, original);
    step(kernel, slot);
    run(0x2000, arm920JitBudget);
    assert(fast.reg(2) == 0x10504);

    // A physical alias write invalidates the suspended block before the mapping comes back.
    suspend();
    constexpr uint32_t changedAdd = 0xe2822002;  // ADD r2, r2, #2
    for (auto* machine : {&exact, &fast}) {
        cpuSetReg(machine->cpu, 0, changedAdd);
        cpuSetReg(machine->cpu, 15, kernel + 20);
    }
    exact.step();
    uint32_t blNext;
    uint64_t bus;
    assert(cpuArm920RunLeanFor(fast.cpu, 1, 1, &blNext, &bus) == 1);
    step(kernel + 4, original);
    step(kernel, slot);
    run(0x2000, arm920JitBudget);
    assert(fast.reg(2) == 0x10506);
}

void testKeptData() {
    constexpr uint32_t slot = 0x04000000, slotTable = l2 + 0x800, slotPte = slotTable + 8;
    constexpr uint32_t kernel = 0xc0000400, park = 0xc0000440, loads = 0xc0000500;
    constexpr uint32_t dtlbInvalidate = 0xee080f16;  // MCR p15, 0, r0, c8, c6, 0
    Machine exact;
    Machine fast;
    for (auto* machine : {&exact, &fast}) {
        machine->setup();
        machine->word(ttb + (slot >> 20) * 4, slotTable | 0x11);
        machine->word(slotPte, 0x102ff2);
        machine->word(l2 + 8, 0x281ff2);
        machine->word(0x102010, 0x11111111);
        machine->word(0x202010, 0x22222222);
        machine->word(0x281010, 0x33333333);
        machine->word(0x400, mcr(13, 0));
        machine->word(0x404, str(0, 13));
        machine->word(0x408, tlbFlush);
        machine->word(0x40c, dtlbInvalidate);
        machine->word(0x440, loop);
        machine->word(0x500, ldr(1, 10));
        machine->word(0x504, ldr(2, 11));
        machine->word(0x508, movPc(8));
        cpuSetReg(machine->cpu, 8, park);
        cpuSetReg(machine->cpu, 10, slot + 0x2010);
        cpuSetReg(machine->cpu, 11, 0x2010);
        cpuSetReg(machine->cpu, 13, 0xd0300000 + slotPte - ttb);
    }
    enableJit(fast);
    auto step = [&](uint32_t pc, uint32_t value = 0) {
        for (auto* machine : {&exact, &fast}) {
            cpuSetReg(machine->cpu, 0, value);
            cpuSetReg(machine->cpu, 15, pc);
            machine->step();
        }
    };
    auto run = [&] {
        for (auto* machine : {&exact, &fast}) {
            cpuSetReg(machine->cpu, 1, 0);
            cpuSetReg(machine->cpu, 2, 0);
            cpuSetReg(machine->cpu, 15, loads);
        }
        uint32_t ran = 0;
        int stop = arm920JitExit;
        cpuArm920RunJit(fast.cpu, 16, &stop, 16, &ran);
        assert(stop == arm920JitBudget && ran);
        for (uint32_t i = 0; i < ran; ++i) exact.step();
        for (unsigned r = 0; r < 16; ++r) assert(exact.reg(r) == fast.reg(r));
    };

    step(kernel, 0);
    run();
    run();
    assert(fast.reg(1) == 0x11111111 && fast.reg(2) == 0x33333333);
    // PID 2: VA 0x2010 is MVA 0x04002010, the page the slot load reads.
    step(kernel, slot);
    run();
    assert(fast.reg(1) == 0x11111111 && fast.reg(2) == 0x11111111);
    // The slot entry moves to B and the D TLB is invalidated: both loads read B.
    step(kernel + 4, 0x202ff2);
    step(kernel + 12);
    run();
    assert(fast.reg(1) == 0x22222222 && fast.reg(2) == 0x22222222);
    // PID 0 again: the low load goes back to its own page, the slot load stays on B.
    step(kernel, 0);
    run();
    assert(fast.reg(1) == 0x22222222 && fast.reg(2) == 0x33333333);
    // An invalidation with no table change keeps both, then a flush after moving the slot entry back to A.
    step(kernel + 12);
    run();
    assert(fast.reg(1) == 0x22222222 && fast.reg(2) == 0x33333333);
    step(kernel + 4, 0x102ff2);
    step(kernel + 8);
    run();
    assert(fast.reg(1) == 0x11111111 && fast.reg(2) == 0x33333333);
}

}

int main() {
    if (!cpuSetFastEngine(true)) {
        std::printf("PASS: ARM920T table edits under translated code (no Fast engine)\n");
        return 0;
    }
    // Code compiles after one visit, and runs of a few dozen instructions go to the JIT.
    setenv("FROND_JIT_HOT", "1", 1);
    setenv("FROND_JIT_MAX_UNCHECKED", "4", 1);
    cpuSetJitSync(true);
    // Fast blocks go on a CPU made without the whole-CPU JIT, as in rx1950 Fast.
    assert(cpuSetFastEngine(false));
    Machine exact;
    exact.setup();
    Machine fast;
    fast.setup();

    enableJit(fast);
    uint64_t translated = 0;
    // Runs exactly count instructions in Fast: blocks with a budget they can't pass, then the interpreter for
    // what they leave (the CP15 writes). The lean path is left out so the code after each edit runs translated.
    auto runFast = [&](uint64_t count) {
        const uint32_t slack = cpuArm920JitMaxUnchecked(fast.cpu);
        while (count) {
            if (count > slack && cpuArm920JitCanRun(fast.cpu)) {
                int stop = arm920JitExit;
                uint32_t ran = 0;
                const uint32_t budget = uint32_t(count - slack);
                cpuArm920RunJit(fast.cpu, budget, &stop, budget, &ran);
                translated += ran;
                count -= ran;
                if (ran) continue;
            }
            fast.step();
            --count;
        }
    };
    // Exact steps a round, Fast runs the same count, and the registers match after each.
    for (unsigned round = 0; exact.reg(15) != 0xc00003e0; ++round) {
        assert(round < 200);
        const uint64_t count = 30 + round % 7;
        for (uint64_t i = 0; i < count; ++i) exact.step();
        runFast(count);
        for (unsigned r = 0; r < 16; ++r) assert(exact.reg(r) == fast.reg(r));
    }
    assert(exact.reg(2) == 40 + 0x10000 + 1 + 0x100);
    assert(translated > 0);
    testSuspendedSlot();
    testKeptData();
    std::printf("Fast: %llu of the instructions in translated blocks\n", (unsigned long long)translated);
    std::printf("PASS: ARM920T table edits and suspended FCSE code\n");
}
