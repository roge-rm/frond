#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "imx1_bootstrap.h"
#include "imx1_boot_probe.h"

int main() {
    using A = Imx1BootstrapAccess;
    Imx1Bootstrap state;
    uint32_t value = 99;
    auto read = [&](uint32_t offset) {
        value = 0xdeadbeef;
        assert(state.access(0x00209000 + offset,4,false,value) == A::ok);
        return value;
    };
    auto write = [&](uint32_t offset, uint32_t setting) {
        value = setting;
        assert(state.access(0x00209000 + offset,4,true,value) == A::ok);
    };
    assert(read(0) == 0 && read(8) == 0x7ff && read(0x1c) == 0);
    write(0,1); write(8,0); write(0x1c,0xffff);
    assert(read(0) == 1 && read(8) == 0 && read(0x1c) == 0xffff);
    for (auto offset : {4u,0xcu,0x10u,0x14u,0x18u}) {
        assert(read(offset) == 0);
        write(offset,0x7ff);
        assert(read(offset) == 0);
    }
    value = 3;
    assert(state.access(0x00209000,4,true,value) == A::sideEffect && read(0) == 1);
    value = 4;
    assert(state.access(0x00209000,4,true,value) == A::reservedBits && read(0) == 1);
    for (auto offset : {8u,0x1cu}) {
        value = offset == 8 ? 0x800 : 0x10000;
        assert(state.access(0x00209000+offset,4,true,value) == A::reservedBits);
    }
    assert(read(8) == 0 && read(0x1c) == 0xffff);
    write(0,2);
    assert(read(0) == 0 && state.dmaResetRemainingHclk() == 3);
    for (unsigned i = 0; i < 8; ++i) assert(read(0) == 0);
    assert(state.dmaResetRemainingHclk() == 3);
    // The native driver reads and configures global words while the pulse is pending.
    assert(read(8) == 0x7ff && read(0x1c) == 0);
    write(8,0x7ff); write(0x1c,0xffff);
    assert(read(8) == 0x7ff && read(0x1c) == 0xffff);
    assert(state.dmaResetRemainingHclk() == 3);
    write(0,1);
    value = 0x27;
    assert(state.access(0x0021b810,4,true,value) == A::ok);
    state.advanceHclkCycles(100);
    assert(state.dmaResetRemainingHclk() == 3);
    value = 0x2f;
    assert(state.access(0x0021b810,4,true,value) == A::ok);
    state.advanceHclkCycles(2);
    assert(state.dmaResetRemainingHclk() == 1 && read(0) == 1);
    state.advanceHclkCycles(1);
    assert(state.dmaResetRemainingHclk() == 0 && read(8) == 0x7ff && read(0x1c) == 0xffff);
    // 2D surfaces and unimplemented per-channel words remain guarded.
    for (auto offset : {0x20u,0x40u,0x9cu,0x1dcu}) {
        value = 0xdeadbeef;
        assert(state.access(0x00209000+offset,4,false,value) == A::unmapped && value == 0xdeadbeef);
        assert(state.access(0x00209000+offset,4,true,value) == A::unmapped);
    }
    for (uint32_t size : {1u,2u,8u})
        assert(state.access(0x00209000,size,true,value) == A::width);
    assert(state.access(0x00209001,4,false,value) == A::width);
    state = Imx1Bootstrap{};
    assert(state.dmaResetRemainingHclk() == 0 && read(0) == 0 && read(8) == 0x7ff);
    // Unmasking either DMA source observes a known inactive line, not unknown host input.
    state.connectCpuInterrupts();
    value = 0xffffffff;
    assert(state.access(0x00223004,4,true,value) == A::ok);
    for (uint32_t source : {60u,61u}) {
        value = source;
        assert(state.access(0x00223008,4,true,value) == A::ok);
        const auto lines = state.interruptLines();
        assert(lines.irq == Imx1AitcLine::clear && lines.fiq == Imx1AitcLine::clear);
    }
    const uint32_t program[]{0xe59f0038,0xe3a01002,0xe5801000,0xe3a01001,0xe5801000,
        0xe5902008,0xe59f1024,0xe1822001,0xe5802008,0xe590301c,0xe59f1018,
        0xe1833001,0xe580301c,0xe5904000,0xe5905008,0xe580109c,
        0x00209000,0x7ff,0xffff};
    std::vector<uint8_t> rom(sizeof(program));
    std::memcpy(rom.data(),program,sizeof(program));
    Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT","1",1);
    Imx1ProbeResult results[2];
    for (unsigned fast = 0; fast < 2; ++fast) {
        results[fast] = imx1BootProbe(rom.data(),rom.size(),Imx1ProbeEntry::physicalFlash,
                                      32,nullptr,nullptr,fast,&options);
        const auto& result = results[fast];
        assert(result.stop == Imx1ProbeStop::unsupportedWrite && result.address == 0x0020909c);
        assert(result.registers[2] == 0x7ff && result.registers[3] == 0xffff &&
               result.registers[4] == 1 && result.registers[5] == 0x7ff);
    }
    assert(results[0].cpsr == results[1].cpsr && results[0].busAccesses == results[1].busAccesses);
    assert(!std::memcmp(results[0].registers,results[1].registers,sizeof(results[0].registers)));
    std::puts("PASS: DMA reset pulse, idle global state, guarded transfers and Exact/Fast startup");
}
