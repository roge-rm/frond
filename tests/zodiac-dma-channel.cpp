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
    uint32_t value = 0;
    auto read = [&](unsigned channel, unsigned word) {
        value = 0xdeadbeef;
        assert(state.access(0x00209080 + channel * 0x40 + word * 4,4,false,value) == A::ok);
        return value;
    };
    auto write = [&](unsigned channel, unsigned word, uint32_t setting, A status = A::ok) {
        value = setting;
        assert(state.access(0x00209080 + channel * 0x40 + word * 4,4,true,value) == status);
    };
    for (unsigned channel = 0; channel < 11; ++channel) {
        for (unsigned word = 0; word < 7; ++word) assert(read(channel,word) == 0);
        // Native allocation marks REN without CEN, then selects the request source.
        write(channel,3,read(channel,3) | 8);
        write(channel,4,16);
        assert(read(channel,3) == 8 && read(channel,4) == 16);
        write(channel,0,0x08000123 + channel * 4);
        write(channel,1,0x08001003 + channel * 4);
        write(channel,2,0x123456);
        write(channel,5,0x3f);
        write(channel,6,0xabcd);
        assert(read(channel,0) == 0x08000120 + channel * 4);
        assert(read(channel,1) == 0x08001000 + channel * 4);
        assert(read(channel,2) == 0x123456 && read(channel,5) == 0x3f && read(channel,6) == 0xabcd);
        for (uint32_t active : {1u,2u,9u,10u}) write(channel,3,active,A::sideEffect);
        for (uint32_t invalid : {0x30u,0xc0u,0xc00u,0x3000u}) write(channel,3,invalid,A::sideEffect);
        write(channel,3,0x4000,A::reservedBits);
        assert(read(channel,3) == 8);
        for (unsigned word : {2u,4u,5u,6u}) {
            const uint32_t old = read(channel,word);
            const uint32_t invalid = word == 2 ? 0x1000000 : word == 4 ? 0x20 : word == 5 ? 0x40 : 0x10000;
            write(channel,word,invalid,A::reservedBits);
            assert(read(channel,word) == old);
        }
    }
    auto saved = state;
    state.advanceHclkCycles(1000);
    assert(read(10,3) == 8 && read(9,4) == 16);
    // Releasing and reconfiguring one channel does not change another channel.
    write(10,3,0); write(10,4,0);
    assert(read(10,3) == 0 && read(9,3) == 8);
    write(10,3,0x20a8); write(10,4,17);
    assert(read(10,3) == 0x20a8 && read(10,4) == 17);
    state = saved;
    assert(read(10,3) == 8 && read(10,4) == 16);
    state.connectCpuInterrupts();
    value = 0xffffffff;
    assert(state.access(0x00223004,4,true,value) == A::ok);
    for (uint32_t source : {60u,61u}) {
        value = source;
        assert(state.access(0x00223008,4,true,value) == A::ok);
        assert(state.interruptLines().irq == Imx1AitcLine::clear);
    }
    value = 2;
    assert(state.access(0x00209000,4,true,value) == A::ok);
    for (unsigned channel = 0; channel < 11; ++channel)
        for (unsigned word = 0; word < 7; ++word) assert(read(channel,word) == 0);
    write(10,3,8); write(10,4,16);
    assert(state.dmaResetRemainingHclk() == 3 && read(10,4) == 16);
    value = 0xdeadbeef;
    for (auto address : {0x0020909cu,0x0020933cu,0x00209340u}) {
        assert(state.access(address,4,false,value) == A::unmapped && value == 0xdeadbeef);
        assert(state.access(address,4,true,value) == A::unmapped);
    }
    for (uint32_t size : {1u,2u,8u})
        assert(state.access(0x0020930c,size,true,value) == A::width);
    assert(state.access(0x0020930d,4,false,value) == A::width);
    const uint32_t program[]{0xe59f0024,0xe5902000,0xe3822008,0xe5802000,
        0xe3a01010,0xe5801004,0xe5903000,0xe5904004,0xe3a01009,0xe5801000,
        0xeafffffe,0x0020930c};
    std::vector<uint8_t> rom(sizeof(program));
    std::memcpy(rom.data(),program,sizeof(program));
    Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT","1",1);
    Imx1ProbeResult results[2];
    for (unsigned fast = 0; fast < 2; ++fast) {
        results[fast] = imx1BootProbe(rom.data(),rom.size(),Imx1ProbeEntry::physicalFlash,
                                     32,nullptr,nullptr,fast,&options);
        const auto& result = results[fast];
        assert(result.stop == Imx1ProbeStop::unsupportedPeripheral && result.address == 0x0020930c);
        assert(result.registers[2] == 8 && result.registers[3] == 8 && result.registers[4] == 16);
    }
    assert(results[0].cpsr == results[1].cpsr && results[0].busAccesses == results[1].busAccesses);
    assert(!std::memcmp(results[0].registers,results[1].registers,sizeof(results[0].registers)));
    std::puts("PASS: disabled DMA channel allocation, configuration, reset, guards and Exact/Fast native sequence");
}
