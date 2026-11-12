#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include "CPU.h"
#include "imx1_bootstrap.h"
#include "imx1_boot_probe.h"

int main() {
    using A = Imx1BootstrapAccess;
    constexpr uint32_t pwm = 0x0020502c, refresh = 0x00205034;
    Imx1Bootstrap soc;
    uint32_t value = 0x5ff;
    assert(soc.access(pwm,4,true,value) == A::unmapped);
    value = 2;
    assert(soc.access(refresh,4,true,value) == A::sideEffect);
    soc.setLcdcContrastOnly(true);
    auto read = [&](uint32_t address, uint32_t expected) {
        value = 0xdeadbeef;
        assert(soc.access(address,4,false,value) == A::ok && value == expected);
    };
    auto write = [&](uint32_t address, uint32_t setting, A expected = A::ok) {
        value = setting;
        assert(soc.access(address,4,true,value) == expected);
    };
    read(pwm,0); read(refresh,0);
    write(refresh,2,A::sequence);
    // The complete native brightness transaction disables, configures and enables contrast.
    for (uint32_t duty : {0xffu,0x80u,0u}) {
        read(refresh,duty == 0xff ? 0 : 2);
        write(refresh,0);
        write(pwm,0x500 | duty);
        write(refresh,2);
        read(pwm,0x500 | duty); read(refresh,2);
    }
    soc.advanceHclkCycles(5);
    assert(soc.lcdcContrastPhase() == 5);
    for (unsigned i = 0; i < 8; ++i) { read(pwm,0x500); read(refresh,2); }
    assert(soc.lcdcContrastPhase() == 5);
    auto copied = soc;
    soc.advanceHclkCycles(256);
    assert(soc.lcdcContrastPhase() == 5 && copied.lcdcContrastPhase() == 5);
    write(pwm,0x580,A::sequence); read(pwm,0x500);
    // CC_EN alone may go off with the LCDC on, the ROM's sound power-down order. The counter stops.
    write(pwm,0x400); read(pwm,0x400); read(refresh,2); soc.advanceHclkCycles(99);
    assert(soc.lcdcContrastPhase() == 5);
    write(pwm,0x500,A::sequence); write(pwm,0x401,A::sequence);
    write(refresh,0); soc.advanceHclkCycles(99);
    assert(soc.lcdcContrastPhase() == 5);
    write(pwm,0x400); soc.advanceHclkCycles(99);
    assert(soc.lcdcContrastPhase() == 5);
    write(refresh,2,A::sequence);
    for (uint32_t bad : {0x600u,0x800u,0x2000000u}) write(pwm,bad,A::reservedBits);
    for (uint32_t bad : {0x100u,0x300u}) write(pwm,bad,A::sideEffect);
    read(pwm,0x400);
    for (uint32_t bad : {1u,3u}) write(refresh,bad,A::sideEffect);
    write(refresh,4,A::reservedBits); read(refresh,0);
    write(pwm,0x580); write(refresh,2);
    // PERCLK2 is System PLL divided by PCDR bits 7..4, independent of HCLK division.
    write(0x0021b020,0x000b002b);
    soc.advanceHclkCycles(1);
    assert(soc.lcdcContrastPhase() == 9);
    for (uint32_t width : {1u,2u,8u}) {
        value = 0xdeadbeef;
        assert(soc.access(pwm,width,false,value) == A::width && value == 0xdeadbeef);
        assert(soc.access(pwm,width,true,value) == A::width);
    }
    assert(soc.access(pwm + 1,4,false,value) == A::width);
    assert(!soc.instructionWord(pwm,value));
    for (uint32_t address : {0x00205000u,0x00205004u,0x00205018u,0x00205030u,0x00205040u}) {
        value = 0xdeadbeef;
        assert(soc.access(address,4,false,value) == A::unmapped && value == 0xdeadbeef);
        assert(soc.access(address,4,true,value) == A::unmapped);
    }
    soc = Imx1Bootstrap{};
    assert(soc.lcdcContrastPhase() == 0);
    assert(soc.access(pwm,4,false,value) == A::unmapped);
    assert(copied.access(pwm,4,false,value) == A::ok && value == 0x500);

    std::vector<uint32_t> program;
    std::vector<std::pair<size_t,uint32_t>> literals;
    auto load = [&](unsigned reg, uint32_t literal) {
        literals.emplace_back(program.size(),literal);
        program.push_back(0xe59f0000 | (reg << 12));
    };
    load(5,0x00205000);
    program.push_back(0xe5950034); // Native read, clear enable, write back.
    program.push_back(0xe3c00002);
    program.push_back(0xe5850034);
    load(0,0x5ff);
    program.push_back(0xe585002c);
    program.push_back(0xe3a00002);
    program.push_back(0xe5850034);
    program.push_back(0xe595602c);
    program.push_back(0xe5957034);
    load(0,0x00205030);
    program.push_back(0xe5801000); // Internal panel DMA remains guarded.
    for (auto literal : literals) {
        const size_t at = program.size(); program.push_back(literal.second);
        program[literal.first] |= uint32_t((at-literal.first-2)*4);
    }
    std::vector<uint8_t> rom(program.size()*4);
    std::memcpy(rom.data(),program.data(),rom.size());
    Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT","1",1);
    for (unsigned fast = 0; fast < 2; ++fast) {
        auto strict = imx1BootProbe(rom.data(),rom.size(),Imx1ProbeEntry::physicalFlash,100,
                                    nullptr,nullptr,fast,&options);
        assert(strict.address == pwm && strict.stop == Imx1ProbeStop::unsupportedWrite);
        options.lcdcContrastOnly = true;
        auto result = imx1BootProbe(rom.data(),rom.size(),Imx1ProbeEntry::physicalFlash,100,
                                    nullptr,nullptr,fast,&options);
        assert(result.stop == Imx1ProbeStop::unsupportedWrite && result.address == 0x00205030);
        assert(result.registers[6] == 0x5ff && result.registers[7] == 2);
        options.lcdcContrastOnly = false;
    }
    options.lcdcContrastOnly = true; options.bootstrapPeripherals = false;
    auto invalid = imx1BootProbe(rom.data(),rom.size(),Imx1ProbeEntry::physicalFlash,100,
                                nullptr,nullptr,false,&options);
    assert(invalid.stop == Imx1ProbeStop::unsupportedPeripheral && invalid.peripheralStatus == A::sequence);
    std::puts("PASS: explicit LCD-clock contrast configuration, guest-time phase, native transaction and guarded panel DMA");
}
