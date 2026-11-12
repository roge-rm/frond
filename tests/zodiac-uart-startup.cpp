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
    for (uint32_t address : {0x00206080u, 0x00207080u}) {
        assert(state.access(address,4,false,value) == A::ok && value == 4);
        for (uint32_t setting : {0u, 2u, 4u, 0xc06u}) {
            value = setting;
            assert(state.access(address,4,true,value) == A::ok);
            value = 0;
            assert(state.access(address,4,false,value) == A::ok && value == setting);
        }
        // DMA requests, BREAK and automatic baud detection are refused, and DOZE with the UART on.
        for (unsigned bit : {3u,4u,8u,14u}) {
            value = 1u << bit;
            assert(state.access(address,4,true,value) == A::sideEffect);
        }
        value = 3;
        assert(state.access(address,4,true,value) == A::sideEffect);
        value = 0x10000;
        assert(state.access(address,4,true,value) == A::reservedBits);
        for (uint32_t width : {1u,2u,8u})
            assert(state.access(address,width,true,value) == A::width);
        assert(state.access(address+1,4,true,value) == A::width);
        assert(state.access(address,4,false,value) == A::ok && value == 0xc06);
        value = 0xdeadbeef;
        assert(state.access(address+0x54,4,false,value) == A::unmapped && value == 0xdeadbeef);
    }
    value = 0;
    assert(state.access(0x00206080,4,true,value) == A::ok);
    assert(state.access(0x00207080,4,false,value) == A::ok && value == 0xc06);
    state = Imx1Bootstrap{};
    assert(state.access(0x00206080,4,false,value) == A::ok && value == 4);
    assert(state.access(0x00207080,4,false,value) == A::ok && value == 4);
    const uint32_t program[]{0xe59f0018,0xe3a01000,0xe5801080,0xe59f0010,
        0xe5801080,0xe5902080,0xe3a01901,0xe5801080,0x00206000,0x00207000};
    std::vector<uint8_t> rom(sizeof(program));
    std::memcpy(rom.data(),program,sizeof(program));
    Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT","1",1);
    Imx1ProbeResult results[2];
    for (unsigned fast = 0; fast < 2; ++fast) {
        results[fast] = imx1BootProbe(rom.data(),rom.size(),Imx1ProbeEntry::physicalFlash,
                                      32,nullptr,nullptr,fast,&options);
        const auto& result = results[fast];
        assert(result.stop == Imx1ProbeStop::unsupportedPeripheral && result.address == 0x00207080 &&
               result.value == 0x4000 && result.size == 4 && result.peripheralStatus == A::sideEffect);
        assert(result.registers[2] == 0);
    }
    assert(results[0].cpsr == results[1].cpsr && results[0].busAccesses == results[1].busAccesses);
    assert(!std::memcmp(results[0].registers,results[1].registers,sizeof(results[0].registers)));
    std::puts("PASS: independent UART control, reset/readback, refused modes and Exact/Fast startup");
}
