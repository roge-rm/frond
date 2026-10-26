// Synthetic programs only: exercises the actual interpreters, not Palm OS services.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "MMU.h"
#include "pace.h"
#include "patch_dispatch.h"
#include "system_state.h"

static constexpr uint32_t base = 0x10000, stack = base + 0x3000;
static std::vector<uint8_t> memory(0x4000);
static unsigned memoryAccesses = 0;
static bool access(void*, uint32_t addr, uint_fast8_t size, bool write, void* buf) {
    ++memoryAccesses;
    if (addr < base || uint64_t(addr) + size > base + memory.size()) return false;
    if (write) std::memcpy(memory.data() + addr - base, buf, size);
    else std::memcpy(buf, memory.data() + addr - base, size);
    return true;
}
static void armWord(uint32_t addr, uint32_t word) { assert(access(nullptr, addr, 4, true, &word)); }
static void m68kProgram(std::initializer_list<uint16_t> words) {
    uint32_t addr = base;
    for (auto word : words) { paceSet16(addr, word); addr += 2; }
    paceSetPC(base); paceSetAreg(7, stack);
}
int main() {
    auto mem = memInit(); assert(memRegionAdd(mem, base, memory.size(), access, nullptr));
    PacePatch patch{}; auto dispatch = initPatchDispatch();
    auto cpu = cpuInit(base, mem, ARM_MEMORY_SYSTEM_MMU, true, false, -1,
                       0x69054114, 0x0b16a16a, dispatch, &patch, createSystemState());
    patchDispatchSetCpu(dispatch, cpu);
    paceInit(mem, cpuGetMMU(cpu)); paceSetPriviledged(true);
    // Mixed-width big-endian accesses, including a long aligned to two bytes.
    paceSet32(base + 0x202, 0x12345678);
    assert(memory[0x202] == 0x12 && memory[0x205] == 0x78);
    assert(paceGet16(base + 0x204) == 0x5678);
    paceSet8(base + 0x203, 0xab); assert(paceGet32(base + 0x202) == 0x12ab5678);
    // MOVEQ, ADD, taken BNE, BSR/RTS and balanced stack; final RTS exits PACE.
    m68kProgram({0x7007, 0x7209, 0xd081, 0x6602, 0x7000,
                 0x6102, 0x4e75, 0x5280, 0x4e75});
    pacePush32(PACE_RETURN_MAGIC_PC);
    unsigned instructions = 0;
    paceStatus status;
    do { status = paceExecute(); assert(++instructions < 20); } while (status == pace_status_ok);
    assert(status == pace_status_return && paceGetDreg(0) == 17);
    assert(paceGetAreg(7) == stack);
    // Boundary reporting must stop at the trap without pretending to run an OS.
    m68kProgram({0x4e4f, 0xa0c7}); // TRAP #15, StrLen selector
    assert(paceExecute() == pace_status_syscall && paceReadTrapWord() == 0xa0c7);
    m68kProgram({0x4afc}); assert(paceExecute() == pace_status_illegal_instr);
    m68kProgram({0x80fc, 0x0000}); assert(paceExecute() == pace_status_division_by_zero);
    paceSetPC(base + memory.size());
    assert(paceExecute() == pace_status_memory_fault);
    uint32_t addr; bool write; uint_fast8_t fsr;
    paceGetMemeryFault(&addr, &write, &fsr);
    assert(addr == base + memory.size() && !write && fsr);
    // Debug inspection must not read guest memory, clear the fault latch, or
    // mutate the interpreter. Repeated snapshots preserve all these invariants.
    const unsigned beforeInspection = memoryAccesses;
    const auto snapshot = paceDebugState();
    assert(snapshot.pc == base + memory.size());
    assert(snapshot.d[0] == paceGetDreg(0) && snapshot.a[7] == paceGetAreg(7));
    assert(paceDebugState().sr == snapshot.sr);
    uint32_t addrAfter; bool writeAfter; uint_fast8_t fsrAfter;
    paceGetMemeryFault(&addrAfter, &writeAfter, &fsrAfter);
    assert(addrAfter == addr && writeAfter == write && fsrAfter == fsr);
    assert(memoryAccesses == beforeInspection);
    puts("PASS pace-execution: endian memory, arithmetic, branch, stack, traps and faults");

    // A real ARM -> Thumb -> ARM call chain, with stack and callee registers.
    armWord(base, 0xe92d4010);       // PUSH {r4,lr}
    armWord(base + 4, 0xe3a04055);   // MOV r4,#0x55
    armWord(base + 8, 0xe1a0e00f);   // MOV lr,pc
    armWord(base + 12, 0xe12fff13);  // BX r3
    armWord(base + 16, 0xe0800004);  // ADD r0,r0,r4
    armWord(base + 20, 0xe8bd4010);  // POP {r4,lr}
    armWord(base + 24, 0xe12fff1e);  // BX lr
    armWord(base + 0x100, 0x3007b510); // Thumb PUSH {r4,lr}; ADD r0,#7
    armWord(base + 0x104, 0xbc08bc10); // POP {r4}; POP {r3}
    armWord(base + 0x108, 0x46c04718); // BX r3; NOP
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC);
    cpuSetReg(cpu, 0, 10); cpuSetReg(cpu, 3, base + 0x101);
    cpuSetReg(cpu, 4, 0xdeadbeef); cpuSetReg(cpu, 13, stack);
    cpuSetReg(cpu, 14, base + 0x200); cpuSetReg(cpu, 15, base);
    for (instructions = 0; cpuGetRegisters(cpu)[15] != base + 0x200; ++instructions) {
        assert(instructions < 30); cpuCyclePure(cpu);
    }
    assert(cpuGetRegExternal(cpu, 0) == 102);
    assert(cpuGetRegExternal(cpu, 4) == 0xdeadbeef && cpuGetRegExternal(cpu, 13) == stack);
    assert(!(cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR) & ARM_SR_T));
    puts("PASS native-interworking: ARM/Thumb calls, return state, callee registers and stack");
}
