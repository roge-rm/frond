#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "CPU.h"
#include "MMU.h"
#include "patch_dispatch.h"
#include "system_state.h"
#include "savestate/savestateAll.h"

static std::vector<uint8_t> memory(2 * 1024 * 1024);
static unsigned fetches;
static bool access(void*, uint32_t addr, uint_fast8_t size, bool write, void* data) {
    if (uint64_t(addr) + size > memory.size()) return false;
    if (write) std::memcpy(memory.data() + addr, data, size);
    else { ++fetches; std::memcpy(data, memory.data() + addr, size); }
    return true;
}
static void put(uint32_t addr, uint32_t value) {
    std::memcpy(memory.data() + addr, &value, 4);
}
struct State {
    ArmCpu* cpu;
    template <class T> void Save(T& state) { cpuSave(cpu, state); }
    void Load(SavestateLoader<ChunkType>& loader) { cpuLoad(cpu, loader); }
};
static ArmCpu* makeCpu(bool xscale) {
    auto mem = memInit();
    assert(memRegionAdd(mem, 0, memory.size(), access, nullptr));
    static PacePatch pace{};
    auto patches = initPatchDispatch();
    auto cpu = cpuInit(0x10000, mem, ARM_MEMORY_SYSTEM_MMU, xscale, false, -1,
                       0x69054114, 0x0b16a16a, patches, &pace, createSystemState());
    patchDispatchSetCpu(patches, cpu);
    return cpu;
}
int main() {
    auto cpu = makeCpu(true);
    auto step = [&](uint32_t pc) {
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
        cpuSetReg(cpu, 15, pc);
        cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, 1);
    };
    auto mmu = cpuGetMMU(cpu);
    // The physical continuation has no mapping. Only the virtual destination does.
    put(0x6000, 0x00000c02); // VA 0x80000000 -> PA 0, uncacheable section.
    put(0x10000, 0xee010f10); // MCR p15,0,r0,c1,c0,0 (enable MMU)
    put(0x10004, 0xe1a00000); // NOP
    put(0x10008, 0xe1a00000); // NOP, now with translation enabled
    put(0x1000c, 0xe1a0f001); // MOV pc,r1, still in the fetched physical line
    put(0x11000, 0xe3a0202a); // MOV r2,#42 at the mapped continuation
    mmuSetDomainCfg(mmu, 3);
    // Set TTBR through a real CP15 instruction while translation is off.
    put(0x12000, 0xee023f10);
    cpuSetReg(cpu, 3, 0x4000); step(0x12000);
    cpuSetReg(cpu, 0, 0x71); cpuSetReg(cpu, 1, 0x80011000);
    step(0x10000); step(0x10004); step(0x10008);
    assert(mmuIsOn(mmu));
    assert(cpuGetRegisters(cpu)[15] == 0x1000c);
    assert(!MMU_TRANSLATE_RESULT_OK(mmuTranslate(mmu, 0x1000c, true, false)));
    State state{cpu}; Savestate<ChunkType> saved;
    assert(saved.Save(state));
    std::vector<uint32_t> snapshot(saved.GetSize() / 4);
    std::memcpy(snapshot.data(), saved.GetBuffer(), saved.GetSize());
    step(0x1000c);
    assert(cpuGetRegisters(cpu)[15] == 0x80011000);
    step(0x80011000); assert(cpuGetRegExternal(cpu, 2) == 42);
    // A fresh CPU must recover the already-fetched physical continuation.
    auto fresh = makeCpu(true); State restored{fresh}; SavestateLoader<ChunkType> loader;
    assert(loader.Load(snapshot.data(), snapshot.size() * 4, restored));
    cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(fresh, 1);
    assert(cpuGetRegisters(fresh)[15] == 0x80011000);
    cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(fresh, 1);
    assert(cpuGetRegExternal(fresh, 2) == 42);

    // Invalidating the fetched line exposes the real first-level translation fault.
    put(0x11020, 0xee071f35); // invalidate I-cache line selected by r1
    cpuSetReg(cpu, 1, 0x10000); step(0x80011020); step(0x1000c);
    assert(cpuGetRegisters(cpu)[15] == 0x0c);
    assert(cpuGetRegExternal(cpu, 14) == 0x10010);
    // Reading data never uses the instruction buffers, including physical SRAM aliases.
    auto result = mmuTranslate(mmu, 0x5c03ffe4, true, true);
    assert(!MMU_TRANSLATE_RESULT_OK(result) && MMU_TRANSLATE_RESULT_FSR(result) == 5);

    // Only two lines survive. A miss on a third line evicts the oldest one.
    mmuSetTTP(mmu, MMU_DISABLED_TTP); cpuInvalidateCodeCache(cpu);
    put(0x13000, 0xe3a00001); put(0x13020, 0xe3a00002); put(0x13040, 0xe3a00003);
    step(0x13000); step(0x13020); put(0x13000, 0xe3a00009);
    fetches = 0; step(0x13000);
    assert(cpuGetRegExternal(cpu, 0) == 1 && fetches == 0);
    step(0x13040); step(0x13000); assert(cpuGetRegExternal(cpu, 0) == 9);
    // Thumb halves share the same fetched word, including already-buffered writes.
    cpuInvalidateCodeCache(cpu); put(0x14000, 0x21072006);
    step(0x14001); assert(cpuGetRegExternal(cpu, 0) == 6);
    put(0x14000, 0x21092008); step(0x14003); assert(cpuGetRegExternal(cpu, 1) == 7);
    cpuInvalidateCodeCache(cpu); step(0x14003); assert(cpuGetRegExternal(cpu, 1) == 9);

    // Old CP15 chunks have no buffers; loading one discards current buffer contents.
    size_t offset = 1 + 2 * snapshot[0], cp15Offset = 0, cp15Toc = 0;
    for (unsigned i = 0; i < snapshot[0]; ++i) {
        if (snapshot[1 + i * 2] == unsigned(ChunkType::cp15mmu)) {
            cp15Offset = offset; cp15Toc = 2 + i * 2; break;
        }
        offset += snapshot[2 + i * 2];
    }
    assert(cp15Offset && snapshot[cp15Offset] == 1);
    auto corrupt = snapshot; corrupt[cp15Offset + 8] = 2; // invalid buffer replacement index
    assert(!loader.Load(corrupt.data(), corrupt.size() * 4, restored));
    corrupt = snapshot; corrupt[cp15Offset + 9] = 0x10001; // misaligned buffer base
    assert(!loader.Load(corrupt.data(), corrupt.size() * 4, restored));
    auto legacy = snapshot;
    legacy[cp15Offset] = 0; legacy[cp15Toc] = 8;
    legacy.erase(legacy.begin() + cp15Offset + 8, legacy.begin() + cp15Offset + snapshot[cp15Toc]);
    assert(loader.Load(legacy.data(), legacy.size() * 4, restored));
    cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(fresh, 1);
    assert(cpuGetRegisters(fresh)[15] == 0x0c);

    // Non-XScale MMU CPUs keep their unbuffered fetch path.
    auto generic = makeCpu(false);
    put(0x15000, 0xe3a00001); cpuSetReg(generic, 15, 0x15000); cpuCyclePure(generic);
    put(0x15000, 0xe3a00002); cpuSetReg(generic, 15, 0x15000); cpuCyclePure(generic);
    assert(cpuGetRegExternal(generic, 0) == 2);
    puts("PASS: XScale buffered MMU continuation, two-line eviction, ARM/Thumb invalidation, fresh restore, legacy/corrupt states and precise translation faults");
}
