// Firmware-free real CPU target for the TCP RSP integration tests.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "MMU.h"
#include "patch_dispatch.h"
#include "system_state.h"
#include "savestate/savestateAll.h"

static std::vector<uint8_t> memory(1024 * 1024);
static uint32_t reads = 0, writes = 0;
static ArmCpu* irqCpu = nullptr;
static bool access(void*, uint32_t addr, uint_fast8_t size, bool write, void* buf) {
    if (uint64_t(addr) + size > memory.size()) return false;
    if (addr == 0x21000 && size == 4 && !write) { memcpy(buf, &reads, 4); return true; }
    if (addr == 0x21004 && size == 4 && !write) { memcpy(buf, &writes, 4); return true; }
    if ((addr >= 0x20000 && addr < 0x20008) || (addr >= 0xffff8 && addr < 0x100000)) { if (write) ++writes; else ++reads; }
    if (irqCpu && addr == 0x22000 && size == 4 && write)
        cpuSetSlowPath(irqCpu, SLOW_PATH_REASON_RESCHEDULE);
    if (irqCpu && addr == 0x22008 && size == 4 && write) cpuIrq(irqCpu, false, true);
    if (write) memcpy(memory.data() + addr, buf, size);
    else memcpy(buf, memory.data() + addr, size);
    return true;
}
static void word(uint32_t addr, uint32_t val) { memcpy(memory.data() + addr, &val, 4); }
struct CpuState {
    ArmCpu* cpu;
    template <typename T> void Save(T& state) { cpuSave(cpu, state); }
    void Load(SavestateLoader<ChunkType>& state) { cpuLoad(cpu, state); }
};
int main(int argc, char** argv) {
    // Leave an unmapped virtual section and a mapped section without backing RAM.
    word(0x4000, 0xc0e);
    word(0x4004, 0x100c0e);
    word(0x10000, 0xe3a00001); // MOV r0,#1
    word(0x10004, 0xe2800001); // ADD r0,r0,#1
    word(0x10008, 0xeafffffd); // B 0x10004
    word(0x11000, 0x30012005); // Thumb MOV r0,#5; ADD r0,#1
    word(0x11004, 0xe7fde7fd); // B 0x11002
    word(0x12000, 0xe5821000); // STR r1,[r2]
    word(0x12004, 0xeafffffe); // B .
    word(0x13000, 0xe7fe6011); // Thumb STR r1,[r2]; B .
    auto mem = memInit();
    assert(memRegionAdd(mem, 0, memory.size(), access, nullptr));
    PacePatch pace{};
    const bool paceFixture = argc == 2 && std::strcmp(argv[1], "--pace") == 0;
    if (paceFixture) {
        // Synthetic ARM -> optimized PACE entry, state block and exception callout.
        // These are interpreter ABI fixtures, not Palm firmware or app addresses.
        word(0x14000, INSTR_PACE_ENTER);
        word(0x14004, INSTR_PACE_RESUME);
        word(0x14008, INSTR_PACE_RETURN_FROM_CALLOUT);
        word(0x15000, 0xeafffffe); // Native exception handler: B .
        word(0x18, 0xe3a06055);    // IRQ handler sentinel: MOV r6,#0x55
        word(0x1c, 0xeafffffe);    // B .
        word(0x12004, 0xe5923004); // Watchpoint-contract probe: LDR r3,[r2,#4]
        word(0x20004, 0x11223344); // Distinct sentinel for an unwanted extra load
        word(0x12008, 0xeafffffe); // B .
        word(0x10, 0xeafffffe);    // Native data-abort vector: B .
        word(0x30000 + 17 * 4, 0x31000); // 68k PC
        word(0x30000 + 16 * 4, 0x34000); // A7
        word(0x30000 + 18 * 4, 0x2010);  // SR (X set)
        const uint8_t program[] = {0x70, 7, 0x52, 0x80, 0x60, 0xfc};
        memcpy(memory.data() + 0x31000, program, sizeof(program));
        pace.enterPace = 0x14000;
        pace.resumePace = 0x14004;
        pace.returnFromCallout = 0x14008;
        pace.calloutIllegalInstr = pace.calloutDivu = pace.calloutDivs = 0x15000;
        pace.calloutSyscall = pace.calloutTrap0 = pace.calloutTrap8 = 0x15000;
        pace.calloutLine1010 = pace.calloutLine1111 = pace.calloutUnimplementedInstr = 0x15000;
    }
    auto patches = initPatchDispatch();
    auto cpu = cpuInit(0x10000, mem, ARM_MEMORY_SYSTEM_MMU, true, false, 0,
                       0x69054114, 0x0b16a16a, patches, &pace, createSystemState());
    patchDispatchSetCpu(patches, cpu);
    mmuSetDomainCfg(cpuGetMMU(cpu), 3);
    mmuSetTTP(cpuGetMMU(cpu), 0x4000);
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC);
    if (paceFixture) irqCpu = cpu;
    CpuState target{cpu};
    Savestate<ChunkType> saved;
    if (paceFixture) assert(saved.Save(target));
    // The Python driver terminates only this child after each test.
    for (;;) {
        cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, 1024);
        // Test-only host control, serviced after a batch, never inside an access.
        // Exercise the actual reset/load APIs with an attached debugger.
        if (paceFixture) {
            uint32_t control;
            memcpy(&control, memory.data() + 0x22000, 4);
            word(0x22000, 0);
            if (control == 1) cpuReset(cpu, 0x10000);
            if (control == 2) {
                SavestateLoader<ChunkType> loader;
                assert(loader.Load(saved.GetBuffer(), saved.GetSize(), target));
            }
        }
    }
}
