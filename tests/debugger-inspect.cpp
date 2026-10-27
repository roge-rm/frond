#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "MMU_internal.h"
#include "RAM.h"
#include "memory_buffer.h"
#include "patch_dispatch.h"
#include "system_state.h"

static MemoryBuffer backing;
static unsigned mmio = 0;
static bool device(void*, uint32_t, uint_fast8_t size, bool, void* buffer) {
    ++mmio;
    std::memset(buffer, 0, size);
    return true;
}
static void put(uint32_t address, uint32_t value) {
    std::memcpy(backing.buffer + address, &value, 4);
}

int main() {
    auto memory = memInit();
    assert(memoryBufferAllocate(&backing, 65536));
    ramInit(memory, nullptr, 0, 65536, &backing, true);
    assert(memRegionAdd(memory, 0x40000000, 0x10000, device, nullptr));
    auto mmu = mmuInit(memory, true);
    mmuSetTTP(mmu, 0x4000);
    mmuSetDomainCfg(mmu, 3);
    std::vector<uint8_t> before(sizeof(*mmu));
    auto inspect = [&](uint32_t address) {
        std::memcpy(before.data(), mmu, sizeof(*mmu));
        auto result = mmuTranslateInspect(mmu, address, false);
        assert(!std::memcmp(before.data(), mmu, sizeof(*mmu)));
        assert(mmio == 0);
        return result;
    };
    put(0x4000, 0xc02);
    assert(MMU_TRANSLATE_RESULT_OK(inspect(0x100)));
    assert(mmu->tlb[0].revision != mmu->revision);
    put(0x4000, 0x40008001);  // Device-backed second-level table.
    assert(!MMU_TRANSLATE_RESULT_OK(inspect(0)));
    put(0x4000, 0x8001);
    put(0x8000, 0xff2);
    assert(MMU_TRANSLATE_RESULT_OK(inspect(0x100)));
    put(0x8000, 0);
    assert(!MMU_TRANSLATE_RESULT_OK(inspect(0)));
    mmuSetTTP(mmu, 0x40004000);  // Device-backed first-level table.
    assert(!MMU_TRANSLATE_RESULT_OK(inspect(0)));

    // Inspection honors an existing guest TLB entry without replacing it.
    mmuSetTTP(mmu, 0x4000);
    put(0x4000, 0xc02);
    mmuTranslate(mmu, 0, true, false);
    auto cached = mmu->tlb[0];
    put(0x4000, 0);
    assert(MMU_TRANSLATE_RESULT_OK(inspect(0)));
    assert(!std::memcmp(&cached, &mmu->tlb[0], sizeof(cached)));

    auto dispatch = initPatchDispatch();
    PacePatch pace{};
    auto cpu = cpuInit(0x1000, memory, ARM_MEMORY_SYSTEM_MMU, true, false, -1,
                       0x69054114, 0x0b16a16a, dispatch, &pace, createSystemState());
    patchDispatchSetCpu(dispatch, cpu);
    uint8_t buffer[16];
    assert(cpuInspectRead(cpu, 0x100, buffer, sizeof(buffer)));
    assert(!cpuInspectRead(cpu, 0x40000000, buffer, sizeof(buffer)));
    assert(!cpuInspectRead(cpu, 0xfffffff8, buffer, sizeof(buffer)));
    assert(!cpuInspectRead(cpu, 0, buffer, 1048577));
    std::memset(buffer, 0x5a, sizeof(buffer));
    assert(!cpuInspectRead(cpu, 0xfff8, buffer, sizeof(buffer)));
    for (auto value : buffer) assert(value == 0x5a);

    // Read-only guest metadata is still physically RAM, despite denied writes.
    auto cpuMmu = cpuGetMMU(cpu);
    mmuSetTTP(cpuMmu, 0x4000);
    mmuSetDomainCfg(cpuMmu, 1);
    put(0x4000, 2);
    assert(cpuInspectRamRange(cpu, 0x100, sizeof(buffer)));
    assert(!MMU_TRANSLATE_RESULT_OK(mmuTranslateInspect(cpuMmu, 0x100, true)));
    mmuSetTTP(cpuMmu, 0x40004000);
    mmuSetDomainCfg(cpuMmu, 3);
    std::vector<uint8_t> state(sizeof(*cpuMmu));
    std::memcpy(state.data(), cpuMmu, sizeof(*cpuMmu));
    std::vector<uint8_t> savedRam(backing.buffer, backing.buffer + backing.size);
    auto* dirty = reinterpret_cast<uint8_t*>(backing.dirtyPages);
    std::vector<uint8_t> savedDirty(dirty, dirty + backing.dirtyPagesSize);
    assert(!cpuInspectRead(cpu, 0, buffer, 4));
    assert(mmio == 0);
    assert(!std::memcmp(state.data(), cpuMmu, sizeof(*cpuMmu)));
    assert(!std::memcmp(savedRam.data(), backing.buffer, backing.size));
    assert(!std::memcmp(savedDirty.data(), dirty, backing.dirtyPagesSize));
    std::puts("PASS debugger inspection: bounded RAM/ROM reads, no MMIO, TLB or dirty-page changes");
}
