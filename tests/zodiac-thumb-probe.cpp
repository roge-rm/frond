#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "CPU.h"
#include "imx1_boot_probe.h"
#include "patch_dispatch.h"
#include "system_state.h"

namespace {
constexpr uint32_t base = 0x10000000;
struct Program {
    std::vector<uint8_t> bytes;
    void half(uint16_t h) { bytes.push_back(h); bytes.push_back(h >> 8); }
    void word(uint32_t w) { half(w); half(w >> 16); }
    void put(unsigned at, uint32_t w) { std::memcpy(bytes.data() + at, &w, 4); }
    void literal(unsigned reg, uint32_t value) {
        word(0xe59f0000 | reg << 12); word(0xea000000); word(value);
    }
    unsigned enter(uint32_t data = 0x00300100) {
        literal(0, data); literal(1, 0x80ff1234); literal(13, 0x00300200);
        const unsigned returnLiteral = bytes.size() + 8;
        literal(4, 0);
        literal(3, base + bytes.size() + 16 + 1);
        word(0xe12fff13); // BX r3
        return returnLiteral;
    }
};
struct Capture {
    std::vector<Imx1ProbeTrace> instructions;
    std::vector<Imx1ProbeAccess> accesses;
};
void instruction(void* context, const Imx1ProbeTrace& t) {
    static_cast<Capture*>(context)->instructions.push_back(t);
}
void access(void* context, const Imx1ProbeAccess& t) {
    static_cast<Capture*>(context)->accesses.push_back(t);
}
Imx1ProbeResult compare(const Program& p, uint32_t steps = 100) {
    Capture a, b;
    Imx1ProbeOptions options{};
    options.bootstrapPeripherals = true;
    options.busObserver = access;
    options.busContext = &a;
    const auto exact = imx1BootProbe(p.bytes.data(), p.bytes.size(), Imx1ProbeEntry::physicalFlash,
                                   steps, instruction, &a, false, &options);
    options.busContext = &b;
    const auto fast = imx1BootProbe(p.bytes.data(), p.bytes.size(), Imx1ProbeEntry::physicalFlash,
                                  steps, instruction, &b, true, &options);
    assert(exact.stop == fast.stop && exact.address == fast.address && exact.value == fast.value &&
           exact.size == fast.size && exact.peripheralStatus == fast.peripheralStatus);
    assert(exact.cpsr == fast.cpsr && exact.spsr == fast.spsr &&
           !std::memcmp(exact.registers, fast.registers, sizeof(exact.registers)));
    assert(a.instructions.size() == b.instructions.size() && a.accesses.size() == b.accesses.size());
    bool thumb = false;
    for (size_t n = 0; n < a.instructions.size(); ++n) {
        const auto& x = a.instructions[n]; const auto& y = b.instructions[n];
        assert(x.step == y.step && x.pc == y.pc && x.instruction == y.instruction &&
               x.cpsr == y.cpsr && x.operand == y.operand && x.instructionSize == y.instructionSize);
        assert(x.instructionSize == ((x.cpsr & ARM_SR_T) ? 2u : 4u));
        if (x.instructionSize == 2) { thumb = true; assert(x.instruction <= 0xffff && !x.operand); }
    }
    for (size_t n = 0; n < a.accesses.size(); ++n) {
        const auto& x = a.accesses[n]; const auto& y = b.accesses[n];
        assert(x.step == y.step && x.pc == y.pc && x.address == y.address &&
               x.size == y.size && x.value == y.value && x.write == y.write && x.status == y.status);
    }
    assert(thumb && !exact.jitInstructions && fast.jitInstructions);
    return exact;
}

struct Machine {
    ArmCpu* cpu;
    ArmMem* mem;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x10000);
    uint32_t begin = 0, end = 0x10000, halfReads = 0, wordReads = 0;
    static bool memory(void* context, uint32_t address, uint_fast8_t size, bool write, void* value) {
        auto& m = *static_cast<Machine*>(context);
        if (uint64_t(address) + size > m.bytes.size()) return false;
        if (memIsInstructionFetch(m.mem)) {
            assert(!write);
            if (address < m.begin || uint64_t(address) + size > m.end) return false;
            if (size == 2) ++m.halfReads;
            else { assert(size == 4 || size == 32); ++m.wordReads; }
        }
        if (write) std::memcpy(m.bytes.data() + address, value, size);
        else std::memcpy(value, m.bytes.data() + address, size);
        return true;
    }
    explicit Machine(bool fast, ArmCpuModel model = ArmCpuModel::arm920t) {
        assert(cpuSetFastEngine(fast));
        mem = memInit();
        assert(memRegionAdd(mem, 0, bytes.size(), memory, this));
        auto* patches = initPatchDispatch();
        cpu = cpuInit(0x1000, mem, ARM_MEMORY_SYSTEM_MMU, model == ArmCpuModel::armv5te, false, -1, 0, 0,
                      patches, new PacePatch{}, createSystemState(), model, true);
        patchDispatchSetCpu(patches, cpu);
    }
    void word(uint32_t at, uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); }
    void half(uint32_t at, uint16_t value) { std::memcpy(bytes.data() + at, &value, 2); }
    void cp(uint32_t instruction, uint32_t value) {
        word(0x1000, instruction); cpuSetReg(cpu, 0, value); cpuSetReg(cpu, 15, 0x1000);
        assert((cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, 1) == 1));
        assert(cpuGetRegisters(cpu)[15] == 0x1004);
    }
    void thumb(uint32_t pc) { cpuSetReg(cpu, ARM_REG_NUM_CPSR, 0xf3); cpuSetReg(cpu, 15, pc); }
    uint32_t fetch(uint32_t pc) {
        uint32_t value = 0xdeadbeef; uint_fast8_t fsr = 0;
        assert(cpuFetchInstruction(cpu, pc, &value, &fsr));
        assert(!memIsInstructionFetch(mem));
        return value;
    }
    void step() { assert((cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, 1) == 1)); }
};
void fetchBoundaries() {
    for (bool fast : {false, true}) for (bool translated : {false, true}) {
        Machine m(fast);
        uint32_t pc = 0x9ffe;
        if (translated) {
            m.word(0x4000, 0xc02); // Section 0 for configuration code and table data.
            m.word(0x4004, 0x8001); // Coarse table for VA 0x100000.
            m.word(0x8000, 0x9ff2); // Only its first 4 KiB page exists.
            m.cp(0xee020f10, 0x4000); m.cp(0xee030f10, 1); m.cp(0xee010f10, 0x79);
            pc = 0x100ffe;
        }
        m.half(0x9ffe, 0x2242); // MOV r2,#0x42 at the final backing halfword.
        m.begin = 0x9ffe; m.end = 0xa000; m.halfReads = m.wordReads = 0;
        m.thumb(pc);
        assert(m.fetch(pc) == 0x2242 && m.fetch(pc) == 0x2242);
        assert(m.halfReads == 2 && !m.wordReads);
        m.step();
        assert(cpuGetRegisters(m.cpu)[2] == 0x42 && cpuGetRegisters(m.cpu)[15] == pc + 2);
        assert(cpuIsThumb(m.cpu) && !memIsInstructionFetch(m.mem));
        uint32_t value = 0; uint_fast8_t fsr = 0;
        assert(!cpuFetchInstruction(m.cpu, pc + 2, &value, &fsr));
        assert(fsr == (translated ? 7 : 8) && !memIsInstructionFetch(m.mem));
        assert(!m.wordReads); // No successful word read was needed for the terminal halfword.
        assert(!cpuFetchInstruction(m.cpu, pc + 1, &value, &fsr) && fsr == 3);
    }
    for (bool fast : {false, true}) {
        Machine m(fast);
        m.half(0x9002, 0x2242);
        m.cp(0xee010f10, 0x1078);
        m.thumb(0x9002);
        assert(m.fetch(0x9002) == 0x2242);
        m.half(0x9002, 0x2277);
        assert(m.fetch(0x9002) == 0x2242); // Retain the guest I-cache halfword.
        m.step(); assert(cpuGetRegisters(m.cpu)[2] == 0x42);
        cpuSetReg(m.cpu, ARM_REG_NUM_CPSR, 0xd3);
        m.cp(0xee070f35, 0x9000);
        m.thumb(0x9002); assert(m.fetch(0x9002) == 0x2277);
        m.step(); assert(cpuGetRegisters(m.cpu)[2] == 0x77);
    }
}
void blSteps() {
    constexpr uint32_t flags = 0xb00000f3, pc = 0x9002;
    for (auto model : {ArmCpuModel::arm920t, ArmCpuModel::armv5te}) for (bool fast : {false, true}) {
        for (uint16_t high : {0x001, 0x7ff, 0x400}) {
            Machine m(fast, model);
            m.half(pc, 0xf000 | high); m.half(pc + 2, 0x2200); // Independent prefix.
            m.thumb(pc); cpuSetReg(m.cpu, ARM_REG_NUM_CPSR, flags);
            m.step();
            const uint32_t offset = uint32_t(high & 0x400 ? int32_t(high) - 0x800 : high) << 12;
            assert(cpuGetRegisters(m.cpu)[14] == pc + 4 + offset && cpuGetRegisters(m.cpu)[15] == pc + 2);
            assert(cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) == flags);
        }
        for (bool exchange : {false, true}) {
            if (exchange && model == ArmCpuModel::arm920t) continue;
            Machine m(fast, model);
            m.half(pc, exchange ? 0xe802 : 0xf802); // Independent suffix, preloaded LR.
            m.thumb(pc); cpuSetReg(m.cpu, ARM_REG_NUM_CPSR, flags); cpuSetReg(m.cpu, 14, 0xa006);
            m.step();
            assert(cpuGetRegisters(m.cpu)[14] == ((pc + 2) | 1u));
            assert(cpuGetRegisters(m.cpu)[15] == (exchange ? 0xa008u : 0xa00au));
            assert(cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) == (exchange ? flags & ~ARM_SR_T : flags));
        }
        for (bool exchange : {false, true}) {
            if (exchange && model == ArmCpuModel::arm920t) continue;
            for (uint16_t high : {0x001, 0x7ff}) {
                Machine m(fast, model);
                m.half(pc, 0xf000 | high); m.half(pc + 2, exchange ? 0xe802 : 0xf802);
                m.thumb(pc); cpuSetReg(m.cpu, ARM_REG_NUM_CPSR, flags);
                const uint32_t offset = uint32_t(high & 0x400 ? int32_t(high) - 0x800 : high) << 12;
                uint32_t target = pc + 4 + offset + 4;
                if (exchange) target &= ~3u;
                unsigned cycles = 0;
                while (cpuGetRegisters(m.cpu)[15] != target && cycles < 3) { m.step(); ++cycles; }
                assert(cpuGetRegisters(m.cpu)[15] == target && cpuGetRegisters(m.cpu)[14] == ((pc + 4) | 1u));
                assert(cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) == (exchange ? flags & ~ARM_SR_T : flags));
                assert(cycles == (fast && model == ArmCpuModel::armv5te ? 1u : 2u));
            }
        }
    }
}
void mixedProgram() {
    Program p; const auto returnLiteral = p.enter();
    p.half(0x2207); p.half(0x3201); p.half(0x0052); // MOV/ADD/LSL r2.
    p.half(0x6001); p.half(0x6805); // STR r1,[r0]; LDR r5,[r0].
    p.half(0x8081); p.half(0x8886); // STRH r1,[r0,#4]; LDRH r6,[r0,#4].
    p.half(0x2203); p.half(0x5687); // LDRSB r7,[r0,r2].
    p.half(0x2202); p.half(0x5e86); // LDRSH r6,[r0,r2].
    p.half(0xb422); p.half(0x2100); p.half(0xbc22); // PUSH/POP {r1,r5}.
    p.half(0x2a02); p.half(0xd101); p.half(0xd000); p.half(0xde00); // BNE skips nothing, BEQ skips undefined.
    p.half(0xf000); p.half(0xf803); // BL subroutine, two separately observed halves.
    p.half(0x3501); p.half(0xe002); p.half(0xde00);
    p.half(0x3502); p.half(0x4770); // ADD r5,#2; BX lr.
    p.half(0x4720); // BX r4 to ARM.
    if (p.bytes.size() & 3) p.half(0xde00);
    p.put(returnLiteral, base + p.bytes.size());
    p.word(0xe2855004); p.word(0xe7f000f0);
    const auto r = compare(p);
    assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == 0xe7f000f0);
    assert(r.registers[1] == 0x80ff1234 && r.registers[2] == 2 &&
           r.registers[5] == 0x80ff123b && r.registers[6] == 0xffff80ff && r.registers[7] == 0xffffff80);
    assert(r.registers[13] == 0x00300200 && !(r.cpsr & ARM_SR_T));
}
void capturedBlVeneer() {
    Program p;
    p.literal(0, 0xd0eabc60); p.literal(1, 0x07f8b2b9); p.literal(2, 0x6e378dac);
    p.literal(3, base + p.bytes.size() + 16 + 1); p.word(0xe12fff13);
    p.half(0x46c0);
    const uint32_t prefix = base + p.bytes.size();
    p.half(0xf000); p.half(0xfd4d); p.half(0xde00);
    const uint32_t target = prefix + 4 + 2 * 0x54d;
    while (base + p.bytes.size() < target) p.half(0xde00);
    p.half(0x4778); p.half(0); // Thumb BX PC enters the aligned ARM helper.
    p.word(0xe0900002); p.word(0xe2a11000); p.word(0xe12fff1e);
    const auto r = compare(p);
    assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.pc == prefix + 4);
    assert(r.registers[0] == 0x3f224a0c && r.registers[1] == 0x07f8b2ba);
    assert(r.registers[14] == ((prefix + 4) | 1) && (r.cpsr & ARM_SR_T));
}
void loadBaseInList() {
    for (unsigned baseReg = 0; baseReg < 8; ++baseReg) {
        for (unsigned mask : {1u << baseReg, 0xffu, (1u << baseReg) | 1u | 0x80u}) {
            Program p;
            p.literal(0, 0x00300100);
            for (unsigned n = 0; n < 8; ++n) {
                p.literal(1, 0x12340000 + n); p.word(0xe5801000 | (n * 4));
            }
            p.literal(baseReg, 0x00300100);
            p.literal(12, base + p.bytes.size() + 16 + 1); p.word(0xe12fff1c);
            p.half(0xc800 | baseReg << 8 | mask); p.half(0xde00);
            const auto r = compare(p);
            assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == 0xde00);
            unsigned index = 0;
            for (unsigned n = 0; n < 8; ++n) if (mask & (1u << n))
                assert(r.registers[n] == 0x12340000 + index++);
            assert(r.cpsr == 0xf3 && !r.spsr && r.busAccesses == 8 + index);
        }
    }
}
void stackReturn() {
    Program p; p.enter();
    const uint32_t first = base + p.bytes.size();
    p.half(0x2209); p.half(0xf000); p.half(0xf802); // MOV r2,#9; BL subroutine.
    p.half(0xde00); p.half(0xde00);
    p.half(0xb504); p.half(0x2200); p.half(0xbd04); // PUSH {r2,lr}; clear; POP {r2,pc}.
    const auto r = compare(p);
    assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == 0xde00 &&
           r.trace.pc == first + 6 && r.registers[2] == 9 && r.registers[13] == 0x00300200 &&
           r.registers[14] == ((first + 6) | 1u) && (r.cpsr & ARM_SR_T));
}
void bxPc() {
    Program p; p.enter();
    p.half(0x4778); p.half(0xde00); // BX pc enters the ARM word at PC+4.
    p.word(0xe2822009); p.word(0xe28f3001); p.word(0xe12fff13); // ADD; ADR Thumb; BX r3.
    p.half(0x1c15); p.half(0xde00); // MOV r5,r2; gated undefined.
    auto r = compare(p);
    assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == 0xde00 &&
           r.trace.instructionSize == 2 && r.registers[2] == 9 && r.registers[5] == 9);
    Program unaligned; unaligned.enter(); unaligned.half(0x2200); unaligned.half(0x4778);
    r = compare(unaligned);
    assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == 0x4778);
}
void guards() {
    for (uint16_t h : {0x4780, 0x4781, 0x4701, 0xe800, 0xbe00, 0xdeab, 0xde00,
                       0xb400, 0xbc00, 0xc000, 0xc001, 0xf800, 0x4340, 0xb200, 0x4400,
                       0x44ff, 0x45ff}) {
        Program p; p.enter(); p.half(h); p.half(0xde00);
        const auto r = compare(p);
        if (r.stop != Imx1ProbeStop::unsupportedInstruction || r.trace.instruction != h)
            std::fprintf(stderr, "Thumb guard %04x stopped at %08x instruction %08x\n", h, r.trace.pc, r.trace.instruction);
        assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == h);
        assert(!r.busAccesses && r.cpsr == 0xf3);
    }
    for (uint16_t h : {0x6802, 0x8001, 0x8802, 0x5e42}) {
        Program p; p.enter(0x00300101); p.half(h); p.half(0xde00);
        const auto r = compare(p);
        assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == h && !r.busAccesses);
    }
    Program skipped; skipped.enter();
    skipped.half(0xf000); skipped.half(0xe000); skipped.half(0xde00); skipped.half(0xf800);
    auto rejected = compare(skipped);
    assert(rejected.stop == Imx1ProbeStop::unsupportedInstruction && rejected.trace.instruction == 0xe000);
    Program store; store.enter();
    const uint32_t prefixPc = base + store.bytes.size();
    store.half(0xf000); store.half(0x6001);
    rejected = compare(store);
    assert(rejected.stop == Imx1ProbeStop::unsupportedInstruction && rejected.trace.instruction == 0x6001 &&
           !rejected.busAccesses && rejected.registers[14] == prefixPc + 4 && rejected.registers[15] == prefixPc + 2);
    Program terminal; terminal.enter();
    const uint32_t terminalPc = base + terminal.bytes.size();
    terminal.half(0xf000);
    rejected = compare(terminal);
    assert(rejected.stop == Imx1ProbeStop::exception && rejected.address == terminalPc + 2 && rejected.size == 2 &&
           rejected.registers[14] == terminalPc + 4 && rejected.registers[15] == terminalPc + 2 && !rejected.busAccesses);
    Program misaligned; misaligned.enter(); misaligned.half(0x2202); misaligned.half(0x4710);
    rejected = compare(misaligned);
    assert(rejected.stop == Imx1ProbeStop::unsupportedInstruction && rejected.trace.instruction == 0x4710);
    for (bool fast : {false, true}) for (uint32_t target : {0x00206081u, 0x08000001u}) {
        Program code; code.literal(3, target); code.word(0xe12fff13);
        Imx1ProbeOptions options{}; options.bootstrapPeripherals = true; options.ramMiB = 32;
        const auto r = imx1BootProbe(code.bytes.data(), code.bytes.size(), Imx1ProbeEntry::physicalFlash,
                                    100, nullptr, nullptr, fast, &options);
        assert(r.stop == Imx1ProbeStop::exception && r.trace.instructionSize == 2 && r.size == 2 &&
               r.address == (target & ~1u) && !r.busAccesses && r.value == 8);
    }
    // A Thumb access uses the same strict peripheral failure path as an ARM access.
    Program p; p.enter(0x00206084); p.half(0x6001); p.half(0xde00);
    const auto r = compare(p);
    assert(r.stop == Imx1ProbeStop::unsupportedWrite && r.address == 0x00206084 && r.size == 4);
}
}
int main() {
    unsetenv("FROND_JIT");
    cpuSetJitSync(true);
    setenv("FROND_JIT_HOT", "1", 1);
    fetchBoundaries(); blSteps(); mixedProgram(); capturedBlVeneer(); stackReturn(); loadBaseInList(); bxPc(); guards();
    std::puts("PASS: Exact/Fast Thumb fetch boundaries, retained cache, BL halfword stepping, mixed ARM/Thumb execution and probe guards");
}
