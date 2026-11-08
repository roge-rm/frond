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
    constexpr uint32_t base = 0x00218000, fmcr = 0x0021b808;
    Imx1Bootstrap soc;
    uint32_t value = 0;
    auto read = [&](uint32_t offset, uint32_t expected) {
        value = 0xdeadbeef;
        assert(soc.access(base + offset,4,false,value) == A::ok && value == expected);
    };
    auto write = [&](uint32_t offset, uint32_t setting, A expected = A::ok) {
        value = setting;
        assert(soc.access(base + offset,4,true,value) == expected);
    };
    read(8,0x41); read(0x20,0x81);
    for (auto offset : {0xcu,0x10u,0x14u,0x18u,0x28u}) read(offset,0);
    // The complete native idle sequence never enables a transmitter, receiver or request.
    for (auto offset : {8u,0x28u,0x10u,0x18u}) write(offset,0);
    write(8,0x9900); write(0x14,0x6100); write(0x20,4); write(0xc,0xe8);
    read(8,0x9941); read(0x14,0x6100); read(0x20,4); read(0xc,0xe8);
    // Read-only status and counts are not supplied by written values.
    write(8,0x99ff); read(8,0x9941);
    write(0x20,0xff84); read(0x20,0x84);
    for (auto setting : {0x9b00u,0x9d00u,0xf900u}) write(8,setting,A::sideEffect);
    write(8,0x10000,A::reservedBits); read(8,0x9941);
    for (auto offset : {0xcu,0x10u}) {
        write(offset,0x100,A::sideEffect); write(offset,0x200,A::sideEffect);
        write(offset,0x800,A::reservedBits);
    }
    write(0x20,0x85,A::ok); read(0x20,0x85);
    for (auto setting : {0u,0x89u,0x94u}) write(0x20,setting,A::sideEffect);
    write(0x20,0x10000,A::reservedBits); read(0x20,0x85);
    write(0x20,4);
    write(0x10,0x80,A::sideEffect); read(0x10,0);
    write(0x20,0x84); write(0x10,0x80);
    write(0x20,4,A::sideEffect); read(0x20,0x84);
    write(0x10,0); write(0x20,4);
    write(0x28,0x71); read(0x28,0x71);
    write(0x28,2,A::reservedBits); read(0x28,0x71);
    write(8,0); read(8,0x41);
    read(0xc,0xe8); read(0x14,0x6100); read(0x20,4);
    for (auto offset : {0u,4u,0x1cu,0x24u}) {
        value = 0xdeadbeef;
        assert(soc.access(base + offset,4,false,value) == A::unmapped && value == 0xdeadbeef);
        assert(soc.access(base + offset,4,true,value) == A::unmapped);
    }
    for (auto size : {1u,2u,8u}) {
        value = 0xdeadbeef;
        assert(soc.access(base + 8,size,false,value) == A::width && value == 0xdeadbeef);
        assert(soc.access(base + 8,size,true,value) == A::width);
    }
    assert(soc.access(base + 9,4,false,value) == A::width);
    assert(!soc.instructionWord(base + 8,value));
    assert(soc.access(0x0021d008,4,false,value) == A::unmapped);
    auto copied = soc;
    soc = Imx1Bootstrap{};
    read(8,0x41); read(0xc,0); read(0x14,0); read(0x20,0x81);
    assert(copied.access(base + 0xc,4,false,value) == A::ok && value == 0xe8);
    soc.connectCpuInterrupts();
    value = 0xffffffff;
    assert(soc.access(0x00223004,4,true,value) == A::ok);
    for (uint32_t source : {42u,43u,44u,45u}) {
        value = source;
        assert(soc.access(0x00223008,4,true,value) == A::ok);
        const auto lines = soc.interruptLines();
        assert(lines.irq == Imx1AitcLine::clear && lines.fiq == Imx1AitcLine::clear);
    }
    // Native GPIO/mux and SSI configuration run through the timed probe in both engines.
    std::vector<uint32_t> program;
    std::vector<std::pair<size_t,uint32_t>> literals;
    auto load = [&](unsigned reg, uint32_t literal) {
        literals.emplace_back(program.size(),literal);
        program.push_back(0xe59f0000 | (reg << 12));
    };
    load(0,0x0021c000);
    for (unsigned offset : {0x220u,0x238u,0x240u}) {
        program.push_back(0xe5901000 | offset);
        program.push_back(0xe3c11f7e);
        program.push_back(0xe5801000 | offset);
    }
    load(0,fmcr);
    program.push_back(0xe5901000);
    program.push_back(0xe3c110f8);
    program.push_back(0xe5801000);
    program.push_back(0xe5908000);
    load(4,base);
    for (auto offset : {8u,0x28u,0x10u,0x18u}) {
        load(1,0); program.push_back(0xe5841000 | offset);
    }
    for (auto setting : {std::pair<uint32_t,uint32_t>{8,0x9900},{0x14,0x6100},{0x20,4},{0xc,0xe8}}) {
        load(1,setting.second); program.push_back(0xe5841000 | setting.first);
    }
    program.push_back(0xe5945008); program.push_back(0xe5946020); program.push_back(0xe594700c);
    program.push_back(0xe5841000);
    for (const auto& item : literals) {
        const auto offset = (program.size() - item.first - 2) * 4;
        assert(offset <= 0xfff);
        program[item.first] |= uint32_t(offset);
        program.push_back(item.second);
    }
    std::vector<uint8_t> rom(program.size() * 4);
    std::memcpy(rom.data(),program.data(),rom.size());
    Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT","1",1);
    Imx1ProbeResult results[2];
    for (unsigned fast = 0; fast < 2; ++fast) {
        results[fast] = imx1BootProbe(rom.data(),rom.size(),Imx1ProbeEntry::physicalFlash,100,
                                    nullptr,nullptr,fast,&options);
        assert(results[fast].stop == Imx1ProbeStop::unsupportedWrite && results[fast].address == base);
        assert(results[fast].registers[5] == 0x9941 && results[fast].registers[6] == 4 &&
               results[fast].registers[7] == 0xe8 && results[fast].registers[8] == 3);
    }
    assert(results[0].cpsr == results[1].cpsr && results[0].busAccesses == results[1].busAccesses);
    assert(!std::memcmp(results[0].registers,results[1].registers,sizeof(results[0].registers)));
    std::puts("PASS: native SSI mux/idle startup, empty state, transactional active guards and Exact/Fast execution");
}
