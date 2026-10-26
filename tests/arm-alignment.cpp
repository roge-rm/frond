#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "patch_dispatch.h"
#include "system_state.h"

static constexpr uint32_t base = 0x10000;
static std::vector<uint8_t> memory(0x4000);
static bool access(void*, uint32_t addr, uint_fast8_t size, bool write, void* buf) {
    if (addr < base || uint64_t(addr) + size > base + memory.size()) return false;
    if (write) std::memcpy(memory.data() + addr - base, buf, size);
    else std::memcpy(buf, memory.data() + addr - base, size);
    return true;
}
static void put(uint32_t addr, uint32_t value) { assert(access(nullptr, addr, 4, true, &value)); }
static uint32_t get(uint32_t addr) { uint32_t v; assert(access(nullptr, addr, 4, false, &v)); return v; }
static ArmCpu* cpu;
static void step(uint32_t instr, bool thumb = false) {
    put(base, instr);
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC);
    cpuSetReg(cpu, 15, base | unsigned(thumb));
    cpuCyclePure(cpu);
}
static void control(uint32_t value) {
    cpuSetReg(cpu, 0, value);
    step(0xee010f10); // MCR p15,0,r0,c1,c0,0
}
static uint32_t readControl() { step(0xee113f10); return cpuGetRegExternal(cpu, 3); }
static void fault(uint32_t addr) {
    assert(cpuGetRegisters(cpu)[15] == 0x10);
    assert(cpuGetRegExternal(cpu, 14) == base + 8);
    assert((cpuGetRegExternal(cpu, ARM_REG_NUM_SPSR) & 0x1f) == ARM_SR_MODE_SVC);
    step(0xee153f10); // MRC p15,0,r3,c5,c0,0
    assert(cpuGetRegExternal(cpu, 3) == 1);
    step(0xee163f10); // MRC p15,0,r3,c6,c0,0
    assert(cpuGetRegExternal(cpu, 3) == addr);
}
int main() {
    ArmMem* mem = memInit();
    assert(memRegionAdd(mem, base, memory.size(), access, nullptr));
    PacePatch pace{};
    auto pd = initPatchDispatch();
    cpu = cpuInit(base, mem, ARM_MEMORY_SYSTEM_MMU, true, false, -1, 0x69054114,
                  0x0b16a16a, pd, &pace, createSystemState());
    patchDispatchSetCpu(pd, cpu);
    assert(readControl() & 2);
    control(0x78);
    assert(!(readControl() & 2));
    control(readControl() | 0x1800);
    assert(!(readControl() & 2));
    const uint32_t data = base + 0x100;
    const uint32_t rotated[] = {0x44332211, 0x11443322, 0x22114433, 0x33221144};
    for (unsigned off = 0; off != 4; ++off) {
        put(data, 0x44332211);
        put(data + 4, 0xaabbccdd);
        cpuSetReg(cpu, 0, data + off);
        step(0xe4901004); // LDR r1,[r0],#4
        assert(cpuGetRegExternal(cpu, 1) == rotated[off]);
        assert(cpuGetRegExternal(cpu, 0) == data + off + 4);
        cpuSetReg(cpu, 0, data + off);
        cpuSetReg(cpu, 2, 0x76543210);
        step(0xe4802004); // STR r2,[r0],#4
        assert(get(data) == 0x76543210 && get(data + 4) == 0xaabbccdd);
        assert(cpuGetRegExternal(cpu, 0) == data + off + 4);
        put(data, 0x44332211);
        cpuSetReg(cpu, 0, data + off);
        step(0xe1001092); // SWP r1,r2,[r0]
        assert(cpuGetRegExternal(cpu, 1) == rotated[off]);
        assert(get(data) == 0x76543210);
        put(data, 0x44332211);
        cpuSetReg(cpu, 0, data + off);
        step(0x6801, true); // Thumb LDR r1,[r0]
        assert(cpuGetRegExternal(cpu, 1) == rotated[off]);
    }
    put(data, 0x44332211);
    put(data + 4, 0xaabbccdd);
    cpuSetReg(cpu, 0, data + 2);
    step(0xe8b00006); // LDMIA r0!,{r1,r2}
    assert(cpuGetRegExternal(cpu, 1) == 0x44332211);
    assert(cpuGetRegExternal(cpu, 2) == 0xaabbccdd);
    assert(cpuGetRegExternal(cpu, 0) == data + 10);
    cpuSetReg(cpu, 0, data + 2);
    cpuSetReg(cpu, 1, 0x12345678);
    cpuSetReg(cpu, 2, 0x87654321);
    step(0xe8a00006); // STMIA r0!,{r1,r2}
    assert(get(data) == 0x12345678 && get(data + 4) == 0x87654321);
    control(readControl() | 2);
    assert(readControl() & 2);
    cpuSetReg(cpu, 0, data + 2);
    cpuSetReg(cpu, 1, 0xdeadbeef);
    step(0xe4901004);
    assert(cpuGetRegExternal(cpu, 1) == 0xdeadbeef);
    assert(cpuGetRegExternal(cpu, 0) == data + 2);
    fault(data + 2);
    cpuSetReg(cpu, 0, data + 1);
    step(0xe4802004);
    assert(get(data) == 0x12345678);
    assert(cpuGetRegExternal(cpu, 0) == data + 1);
    fault(data + 1);
    control(readControl() & ~2u);
    cpuSetReg(cpu, 0, data + 1);
    step(0xe1d010b0); // odd LDRH remains unsupported
    fault(data + 1);
    puts("PASS: CP15 A read/write, ARM/Thumb LDR rotation, aligned STR/SWP/LDM/STM, precise alignment aborts");
}
