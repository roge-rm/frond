#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "CPU.h"
#include "MMU.h"
#include "patch_dispatch.h"
#include "system_state.h"

static std::vector<uint8_t> memory(8 * 1024 * 1024);
static unsigned accesses;
static bool access(void*, uint32_t addr, uint_fast8_t size, bool write, void* buf) {
    if (uint64_t(addr) + size > memory.size()) return false;
    ++accesses;
    if (write) std::memcpy(memory.data() + addr, buf, size);
    else std::memcpy(buf, memory.data() + addr, size);
    return true;
}
static void put(uint32_t addr, uint32_t value) {
    std::memcpy(memory.data() + addr, &value, sizeof(value));
}

int main(int argc, char**) {
    auto mem = memInit();
    assert(memRegionAdd(mem, 0, memory.size(), access, nullptr));
    PacePatch pace{};
    auto patches = initPatchDispatch();
    auto cpu = cpuInit(0x10000, mem, ARM_MEMORY_SYSTEM_MMU, true, false, -1,
                       0x69054114, 0x0b16a16a, patches, &pace, createSystemState());
    patchDispatchSetCpu(patches, cpu);
    auto mmu = cpuGetMMU(cpu);
    put(0x4000, 0xc0e);          // Cacheable identity-mapped section, domain zero.
    put(0x4010, 0x400c0e);      // Same cache index, a different tag.
    put(0x6000, 0x400c0e);      // High virtual tag, mapped to physical section four.
    put(0x4014, 0x500c02);      // Uncacheable section five.
    mmuSetDomainCfg(mmu, 3);
    mmuSetTTP(mmu, 0x4000);

    auto step = [&](uint32_t pc, bool thumb = false) {
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC);
        cpuSetReg(cpu, 15, pc | unsigned(thumb));
        cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, 1);
    };
    put(0x10000, 0xe3a00001);   // MOV r0,#1
    step(0x10000);
    assert(cpuGetRegExternal(cpu, 0) == 1);
    put(0x10000, 0xe3a00002);
    accesses = 0;
    step(0x10000);
    assert(cpuGetRegExternal(cpu, 0) == 1 && accesses == 0);
    cpuInvalidateCodeCache(cpu);
    step(0x10000);
    assert(cpuGetRegExternal(cpu, 0) == 2);
    put(0x410000, 0xe3a00003);
    step(0x80010000);
    assert(cpuGetRegExternal(cpu, 0) == 3);
    step(0x10000);
    assert(cpuGetRegExternal(cpu, 0) == 2);

    step(0x410000);
    assert(cpuGetRegExternal(cpu, 0) == 3);
    step(0x10000);
    assert(cpuGetRegExternal(cpu, 0) == 2);

    put(0x10000, 0xe3a00007);
    put(0x10040, 0xee071f35);   // MCR p15,0,r1,c7,c5,1: invalidate one instruction line.
    cpuSetReg(cpu, 1, 0x10000);
    step(0x10040);
    step(0x10000);
    assert(cpuGetRegExternal(cpu, 0) == 7);

    // ARM and Thumb decodes overlap within each cached line.
    put(0x10000, 0xe3a02004);   // ARM MOV r2,#4; low half is Thumb MOV r0,#4.
    cpuInvalidateCodeCache(cpu);
    step(0x10000);
    assert(cpuGetRegExternal(cpu, 2) == 4);
    step(0x10000, true);
    assert(cpuGetRegExternal(cpu, 0) == 4);
    accesses = 0;
    step(0x10000, true);
    assert(cpuGetRegExternal(cpu, 0) == 4 && accesses == 0);
    step(0x10000);
    assert(cpuGetRegExternal(cpu, 2) == 4);

    cpuSetReg(cpu, 2, 0);
    cpuSetReg(cpu, 15, 0x10000);
    cpuCycle<ARM_MEMORY_SYSTEM_MMU, true>(cpu, 1);
    assert(cpuGetRegExternal(cpu, 2) == 4);

    put(0x510000, 0xe3a00005);
    step(0x510000);
    assert(cpuGetRegExternal(cpu, 0) == 5);
    put(0x510000, 0xe3a00006);
    step(0x510000);
    assert(cpuGetRegExternal(cpu, 0) == 6);

    step(0x10002);
    assert(cpuGetRegisters(cpu)[15] == 0x0c);
    assert(cpuGetRegExternal(cpu, 14) == 0x10006);

    // Cached addresses must not hide a tag collision with an unmapped section.
    step(0x810000);
    assert(cpuGetRegisters(cpu)[15] == 0x0c);
    assert(cpuGetRegExternal(cpu, 14) == 0x810004);

    // Predicated cached decodes must read current flags; AL/conditional replacements
    // must select a new execution path after code invalidation.
    auto conditionStep = [&](uint32_t flags) {
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | flags);
        cpuSetReg(cpu, 15, 0x10000);
        cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, 1);
    };
    put(0x10000, 0x03a00063); // MOVEQ r0,#99
    cpuInvalidateCodeCache(cpu);
    conditionStep(ARM_SR_Z);
    assert(cpuGetRegExternal(cpu, 0) == 99);
    cpuSetReg(cpu, 0, 123); accesses = 0;
    conditionStep(0);
    assert(cpuGetRegExternal(cpu, 0) == 123 && accesses == 0);
    put(0x10000, 0xe3a00063); // MOV r0,#99
    cpuInvalidateCodeCache(cpu);
    conditionStep(0);
    assert(cpuGetRegExternal(cpu, 0) == 99);
    put(0x10000, 0x03a00063);
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, 0, 123);
    conditionStep(0);
    assert(cpuGetRegExternal(cpu, 0) == 123);
    puts("PASS: cached ARM/Thumb execution, tag collisions, invalidation and prefetch abort");

    if (argc > 1) {
        put(0x10000, 0xe2800001);   // ADD r0,r0,#1
        put(0x10004, 0xeafffffd);   // B 0x10000
        cpuInvalidateCodeCache(cpu);
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC);
        cpuSetReg(cpu, 15, 0x10000);
        cpuSetReg(cpu, 0, 0);
        constexpr unsigned cycles = 100000000;
        for (unsigned run = 0; run < 5; ++run) {
            const auto started = std::chrono::steady_clock::now();
            const auto done = cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, cycles);
            const double seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            assert(done == cycles && cpuGetRegExternal(cpu, 0) == (run + 1) * cycles / 2);
            std::printf("BENCH %u instructions %.6f seconds %.3f MIPS\n",
                        done, seconds, done / seconds / 1e6);
        }
    }
}
