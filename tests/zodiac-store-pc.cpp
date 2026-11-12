#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "CPU.h"
#include "imx1_boot_probe.h"
#include "patch_dispatch.h"
#include "system_state.h"

namespace {
constexpr uint32_t code = 0x1000, data = 0x9000;
struct Machine {
    ArmCpu* cpu;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x20000);
    uint32_t fault = 0xffffffff;
    unsigned writes = 0;
    static bool memory(void* context, uint32_t address, uint_fast8_t size, bool write, void* value) {
        auto& m = *static_cast<Machine*>(context);
        if (address == m.fault || uint64_t(address) + size > m.bytes.size()) return false;
        if (write) { std::memcpy(m.bytes.data() + address, value, size); ++m.writes; }
        else std::memcpy(value, m.bytes.data() + address, size);
        return true;
    }
    explicit Machine(bool fast) {
        assert(cpuSetFastEngine(fast));
        auto* mem = memInit(); assert(memRegionAdd(mem, 0, 0xffffffff, memory, this));
        auto* patches = initPatchDispatch();
        cpu = cpuInit(code, mem, ARM_MEMORY_SYSTEM_MMU, false, false, -1, 0, 0,
                      patches, new PacePatch{}, createSystemState(), ArmCpuModel::arm920t, true);
        patchDispatchSetCpu(patches, cpu);
    }
    void word(uint32_t a, uint32_t v) { std::memcpy(bytes.data() + a, &v, 4); }
    uint32_t word(uint32_t a) const { uint32_t v; std::memcpy(&v, bytes.data() + a, 4); return v; }
    uint32_t reg(unsigned r) const { return cpuGetRegisters(cpu)[r]; }
    uint32_t status() const { return cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR); }
    void reset(uint32_t instruction) {
        std::fill(bytes.begin(), bytes.end(), 0); fault = 0xffffffff; writes = 0;
        cpuReset(cpu, code); cpuSetReg(cpu, ARM_REG_NUM_CPSR, 0xa00000d3);
        for (unsigned r = 0; r < 15; ++r) cpuSetReg(cpu, r, 0x12340000 + r);
        cpuSetReg(cpu, 0, data); cpuSetReg(cpu, 13, data); word(code, instruction);
    }
    void step() { assert((cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, 1) == 1)); }
};
void same(Machine& exact, Machine& fast) {
    assert(exact.bytes == fast.bytes && exact.writes == fast.writes && exact.status() == fast.status());
    assert(cpuGetRegExternal(exact.cpu, ARM_REG_NUM_SPSR) == cpuGetRegExternal(fast.cpu, ARM_REG_NUM_SPSR));
    assert(!std::memcmp(cpuGetRegisters(exact.cpu), cpuGetRegisters(fast.cpu), 16 * sizeof(uint32_t)));
}
void core(Machine& exact, Machine& fast) {
    for (bool pre : {false,true}) for (bool up : {false,true})
    for (bool writeback : {false,true}) for (uint32_t list : {0x8000u,0x8006u}) {
        const uint32_t count = __builtin_popcount(list);
        const uint32_t first = up ? data + (pre ? 4 : 0) : data - count * 4 + (pre ? 0 : 4);
        const uint32_t instruction = 0xe8000000 | uint32_t(pre) << 24 | uint32_t(up) << 23 |
                                     uint32_t(writeback) << 21 | list;
        uint64_t blocks, before, after;
        for (auto* m : {&exact,&fast}) {
            m->reset(instruction);
        }
        assert(cpuJitExecutionCounts(fast.cpu,&blocks,&before));
        exact.step(); fast.step(); same(exact,fast);
        assert(cpuJitExecutionCounts(fast.cpu,&blocks,&after) && after == before);
        assert(exact.word(first + (count - 1) * 4) == code + 12);
        if (count == 3) assert(exact.word(first) == 0x12340001 && exact.word(first+4) == 0x12340002);
        assert(exact.reg(0) == (writeback ? (up ? data + count*4 : data - count*4) : data));
        assert(exact.reg(15) == code + 4 && exact.status() == 0xa00000d3 && exact.writes == count);
    }
    // The observed stack push uses banked SP and stores the pipeline PC value.
    for (auto* m : {&exact,&fast}) { m->reset(0xe92d8000); m->step(); }
    same(exact,fast);
    assert(exact.word(data-4) == code+12 && exact.reg(13) == data-4);
    // A failed condition performs no transfer or writeback.
    for (auto* m : {&exact,&fast}) { m->reset(0x092d8000); m->step(); }
    same(exact,fast); assert(!exact.writes && exact.reg(13) == data && exact.reg(15) == code+4);
    // Abort restores the base and saves the original status, even after an earlier word.
    for (uint32_t instruction : {0xe9208000u,0xe8a08006u}) {
        for (auto* m : {&exact,&fast}) {
            m->reset(instruction);
            m->fault = instruction == 0xe9208000 ? data-4 : data+8;
            m->step();
        }
        same(exact,fast);
        assert(exact.reg(0) == data && exact.reg(15) == 0x10 && (exact.status() & 31) == 0x17);
        assert(cpuGetRegExternal(exact.cpu,ARM_REG_NUM_SPSR) == 0xa00000d3);
        assert(exact.reg(14) == code+8);
        if (instruction == 0xe8a08006) assert(exact.writes == 2 && exact.word(data) == 0x12340001);
        else assert(!exact.writes);
    }
}
void singleStores(Machine& exact, Machine& fast) {
    for (bool pre : {false,true}) for (bool up : {false,true})
    for (bool writeback : {false,true}) for (bool immediate : {false,true}) {
        if (!pre && writeback) continue;
        const uint32_t instruction = 0xe400f000 | uint32_t(pre) << 24 | uint32_t(up) << 23 |
            uint32_t(!immediate) << 25 | uint32_t(writeback) << 21 | (immediate ? 4u : 0x101u);
        const uint32_t address = pre ? (up ? data+4 : data-4) : data;
        const uint32_t finalBase = writeback || !pre ? (up ? data+4 : data-4) : data;
        for (auto* m : {&exact,&fast}) { m->reset(instruction); cpuSetReg(m->cpu,1,1); }
        uint64_t blocks, before, after;
        assert(cpuJitExecutionCounts(fast.cpu,&blocks,&before));
        exact.step(); fast.step(); same(exact,fast);
        assert(cpuJitExecutionCounts(fast.cpu,&blocks,&after) && after == before);
        assert(exact.word(address) == code+12 && exact.writes == 1);
        assert(exact.reg(0) == finalBase && exact.reg(15) == code+4 && exact.status() == 0xa00000d3);
        for (auto* m : {&exact,&fast}) {
            m->reset(instruction); cpuSetReg(m->cpu,1,1); m->fault = address; m->step();
        }
        same(exact,fast);
        assert(!exact.writes && exact.reg(0) == data && exact.reg(15) == 0x10);
        assert((exact.status() & 31) == 0x17 && exact.reg(14) == code+8);
        assert(cpuGetRegExternal(exact.cpu,ARM_REG_NUM_SPSR) == 0xa00000d3);
    }
    for (auto* m : {&exact,&fast}) { m->reset(0x0580f000); m->step(); }
    same(exact,fast); assert(!exact.writes && exact.reg(0) == data && exact.reg(15) == code+4);
}
void literal(std::vector<uint32_t>& words, unsigned reg, uint32_t value) {
    words.push_back(0xe59f0000 | reg << 12); words.push_back(0xea000000); words.push_back(value);
}
void probe() {
    for (bool fast : {false,true}) {
        std::vector<uint32_t> words; literal(words,13,0x00300140);
        const uint32_t pc = 0x10000000 + words.size()*4;
        words.push_back(0xe92d8000); words.push_back(0xe59d1000); words.push_back(0xe7f000f0);
        Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
        auto r = imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()),words.size()*4,
            Imx1ProbeEntry::physicalFlash,100,nullptr,nullptr,fast,&options);
        assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == 0xe7f000f0);
        assert(r.registers[13] == 0x0030013c && r.registers[1] == pc+12 && r.busAccesses == 2);
        for (uint32_t store : {0xe580f000u,0xe5a0f004u,0xe480f004u,0xe780f101u}) {
            words.clear(); literal(words,0,0x00300140); literal(words,1,1);
            const uint32_t storePc = 0x10000000 + words.size()*4;
            words.push_back(store); literal(words,2,(store == 0xe5a0f004 || store == 0xe780f101) ? 0x00300144 : 0x00300140);
            words.push_back(0xe5923000); words.push_back(0xe7f000f0);
            r = imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()),words.size()*4,
                Imx1ProbeEntry::physicalFlash,100,nullptr,nullptr,fast,&options);
            assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == 0xe7f000f0);
            assert(r.registers[3] == storePc+12 && r.busAccesses == 2);
        }
        for (uint32_t instruction : {0xe9c08000u,0xe9a08001u,0xe98f8000u,0xe9800000u,0xe5c0f000u,0xe4a0f004u,0xe5aff004u,0xe780f00fu,0xe780f111u}) {
            words.clear(); literal(words,0,0x00300140); words.push_back(instruction);
            r = imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()),words.size()*4,
                Imx1ProbeEntry::physicalFlash,100,nullptr,nullptr,fast,&options);
            assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == instruction);
            assert(!r.busAccesses && r.registers[0] == 0x00300140);
        }
    }
}
}
int main() {
    Machine exact(false),fast(true); core(exact,fast); singleStores(exact,fast); probe();
    std::puts("PASS: ARM920T word stores of PC+12, address modes, writeback, condition, abort and Exact/Fast fallback/probe guards");
}
