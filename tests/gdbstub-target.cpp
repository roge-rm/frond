// Firmware-free real CPU target for the TCP RSP integration tests.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "MMU.h"
#include "patch_dispatch.h"
#include "system_state.h"

static std::vector<uint8_t> memory(1024 * 1024);
static uint32_t reads = 0, writes = 0;
static bool access(void*, uint32_t addr, uint_fast8_t size, bool write, void* buf) {
    if (uint64_t(addr) + size > memory.size()) return false;
    if (addr == 0x21000 && size == 4 && !write) { memcpy(buf, &reads, 4); return true; }
    if (addr == 0x21004 && size == 4 && !write) { memcpy(buf, &writes, 4); return true; }
    if (addr >= 0x20000 && addr < 0x20008) { if (write) ++writes; else ++reads; }
    if (write) memcpy(memory.data() + addr, buf, size);
    else memcpy(buf, memory.data() + addr, size);
    return true;
}
static void word(uint32_t addr, uint32_t val) { memcpy(memory.data() + addr, &val, 4); }
int main() {
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
    auto patches = initPatchDispatch();
    auto cpu = cpuInit(0x10000, mem, ARM_MEMORY_SYSTEM_MMU, true, false, 0,
                       0x69054114, 0x0b16a16a, patches, &pace, createSystemState());
    patchDispatchSetCpu(patches, cpu);
    mmuSetDomainCfg(cpuGetMMU(cpu), 3);
    mmuSetTTP(cpuGetMMU(cpu), 0x4000);
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC);
    // The Python driver terminates only this child after each test.
    for (;;) cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, 1024);
}
