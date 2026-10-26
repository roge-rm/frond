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
static bool passes(unsigned cond, unsigned flags) {
    const bool n = flags & 8, z = flags & 4, c = flags & 2, v = flags & 1;
    switch (cond) {
        case 0: return z;
        case 1: return !z;
        case 2: return c;
        case 3: return !c;
        case 4: return n;
        case 5: return !n;
        case 6: return v;
        case 7: return !v;
        case 8: return c && !z;
        case 9: return !c || z;
        case 10: return n == v;
        case 11: return n != v;
        case 12: return !z && n == v;
        case 13: return z || n != v;
        case 14: return true;
        default: assert(false); return false;
    }
}
int main() {
    auto mem = memInit(); assert(memRegionAdd(mem, base, memory.size(), access, nullptr));
    PacePatch pace{}; auto pd = initPatchDispatch();
    auto cpu = cpuInit(base, mem, ARM_MEMORY_SYSTEM_MMU, true, false, -1, 0x69054114,
                       0x0b16a16a, pd, &pace, createSystemState());
    patchDispatchSetCpu(pd, cpu);
    auto run = [&](uint32_t instr, unsigned flags, bool thumb = false) {
        put(base, instr); cpuInvalidateCodeCache(cpu);
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | (uint32_t(flags) << 28));
        cpuSetReg(cpu, 15, base | unsigned(thumb));
        cpuCyclePure(cpu);
    };
    auto flagsNow = [&]() { return cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR) & 0xf0000000u; };
    // ARMv5 Thumb translations predicate only conditional branches.
    for (unsigned opcode = 0; opcode < 65536; ++opcode) {
        uint32_t translated;
        cpuDecodeThumb<ARM_MEMORY_SYSTEM_MMU>(opcode, translated);
        if (translated > 0xffff && (translated >> 28) != 14) {
            assert((translated & 0x0e000000) == 0x0a000000);
            assert((opcode & 0xf000) == 0xd000 && ((opcode >> 8) & 15) < 14);
        }
    }
    for (unsigned flags = 0; flags < 16; ++flags) {
        for (unsigned cond = 0; cond < 15; ++cond) {
            const uint32_t prefix = cond << 28;
            const bool yes = passes(cond, flags);
            cpuSetReg(cpu, 0, 123);
            run(prefix | 0x03a0002a, flags); // MOV r0,#42
            assert(cpuGetRegExternal(cpu, 0) == (yes ? 42u : 123u));
            assert(flagsNow() == flags << 28);
            cpuSetReg(cpu, 1, 7); cpuSetReg(cpu, 2, 9); cpuSetReg(cpu, 0, 123);
            run(prefix | 0x00000291, flags); // MUL r0,r1,r2
            assert(cpuGetRegExternal(cpu, 0) == (yes ? 63u : 123u));
            cpuSetReg(cpu, 0, 123);
            run(prefix | 0x02810001, flags); // ADD r0,r1,#1
            assert(cpuGetRegExternal(cpu, 0) == (yes ? 8u : 123u));
            cpuSetReg(cpu, 1, base + 0x100); cpuSetReg(cpu, 0, 123); put(base + 0x100, 456);
            run(prefix | 0x05910000, flags); // LDR r0,[r1]
            assert(cpuGetRegExternal(cpu, 0) == (yes ? 456u : 123u));
            cpuSetReg(cpu, 0, 789);
            run(prefix | 0x05810000, flags); // STR r0,[r1]
            assert(get(base + 0x100) == (yes ? 789u : 456u));
            run(prefix | 0x0a000000, flags); // B .+8
            assert(cpuGetRegisters(cpu)[15] == base + (yes ? 8 : 4));
            run(prefix | 0x03b00000, flags); // MOVS r0,#0 preserves C and V.
            const auto expected = yes ? ((flags << 28) & (ARM_SR_C | ARM_SR_V)) | ARM_SR_Z : flags << 28;
            assert(flagsNow() == expected);
            if (!yes) {
                cpuSetReg(cpu, 1, base + memory.size());
                run(prefix | 0x05910000, flags); // Failed predicates must not touch unmapped data.
                assert(cpuGetRegisters(cpu)[15] == base + 4);
            }
        }
        for (unsigned cond = 0; cond < 14; ++cond) {
            run(0xd000 | (cond << 8), flags, true); // Thumb conditional branch .+4.
            assert(cpuGetRegisters(cpu)[15] == base + (passes(cond, flags) ? 4 : 2));
        }
        run(0x202a, flags, true); // MOVS r0,#42 is unconditional.
        assert(cpuGetRegExternal(cpu, 0) == 42);
        assert(flagsNow() == ((flags << 28) & (ARM_SR_C | ARM_SR_V)));
        cpuSetReg(cpu, 1, base + 0x100); put(base + 0x100, 456);
        run(0x6808, flags, true); // LDR r0,[r1]
        assert(cpuGetRegExternal(cpu, 0) == 456 && flagsNow() == flags << 28);
        cpuSetReg(cpu, 0, 789);
        run(0x6008, flags, true); // STR r0,[r1]
        assert(get(base + 0x100) == 789 && flagsNow() == flags << 28);
        cpuSetReg(cpu, 0, 0xffffffff);
        run(0x3001, flags, true); // ADDS r0,#1
        assert(cpuGetRegExternal(cpu, 0) == 0 && flagsNow() == (ARM_SR_C | ARM_SR_Z));
        cpuSetReg(cpu, 0, 7); cpuSetReg(cpu, 1, 9);
        run(0x4348, flags, true); // MULS r0,r1
        assert(cpuGetRegExternal(cpu, 0) == 63);
        assert(flagsNow() == ((flags << 28) & (ARM_SR_C | ARM_SR_V)));
        run(0xdf00, flags, true); // SWI and undefined Thumb instructions also ignore NZCV.
        assert(cpuGetRegisters(cpu)[15] == 8);
        run(0xde00, flags, true);
        assert(cpuGetRegisters(cpu)[15] == 4);
    }
    puts("PASS: ARM predicates across all NZCV values, memory side effects, flag writes and Thumb branches");
}
