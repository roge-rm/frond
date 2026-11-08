#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <utility>
#include "CPU.h"
#include "imx1_boot_probe.h"
#include "imx1_bootstrap.h"

using A = Imx1BootstrapAccess;
constexpr uint32_t ssi = 0x218000, dma = 0x209000, ch = 0x209300;
void write(Imx1Bootstrap& soc, uint32_t address, uint32_t value, A expected = A::ok) {
    assert(soc.access(address, 4, true, value) == expected);
}
uint32_t read(Imx1Bootstrap& soc, uint32_t address) {
    uint32_t value = 0;
    assert(soc.access(address, 4, false, value) == A::ok);
    return value;
}
Imx1Bootstrap configured(bool repeat = false) {
    Imx1Bootstrap soc;
    soc.connectCpuInterrupts();
    write(soc, dma, 1);
    write(soc, dma + 8, 0x3ff);
    write(soc, ssi + 8, 0x9900);
    write(soc, ssi + 0x14, 0x6100);
    write(soc, ssi + 0x20, 4);
    write(soc, ssi + 0xc, 0xe8);
    for (unsigned i = 0; i < 8; ++i) write(soc, ssi, 0);
    for (unsigned i = 0; i < 4; ++i) write(soc, 0x300000 + i * 4, 0x12340001 + i * 0x10002);
    write(soc, ch, 0x300000); write(soc, ch + 4, ssi); write(soc, ch + 8, 16);
    write(soc, ch + 0xc, repeat ? 0x208c : 0x2088);
    write(soc, ch + 0x10, 16); write(soc, ch + 0x14, 8);
    write(soc, ch + 0xc, repeat ? 0x208d : 0x2089);
    return soc;
}
int main() {
    auto soc = configured();
    assert(soc.pcmDmaRemaining() == 16);
    soc.advanceHclkCycles(10240);
    assert(soc.pcmDmaRemaining() == 16 && read(soc, ssi + 0x20) == 0x804);
    // The full native programming order, including the 4096-byte count, runs in both engines.
    std::vector<uint32_t> program;
    std::vector<std::pair<size_t,uint32_t>> literals;
    auto load = [&](unsigned reg, uint32_t value) {
        literals.emplace_back(program.size(), value);
        program.push_back(0xe59f0000 | (reg << 12));
    };
    auto store = [&](uint32_t address, uint32_t value) {
        load(0, address); load(1, value); program.push_back(0xe5801000);
    };
    store(dma, 2); store(dma, 1); store(dma + 8, 0x3ff); store(dma + 0x1c, 0xffff);
    store(ch + 0xc, 8); store(ch + 0x10, 16); store(ch + 0xc, 0x2088);
    store(ch + 0x14, 8); store(ch + 0xc, 0x208c); store(ch + 0x18, 0);
    store(ch + 8, 0x1000); store(ch + 4, ssi);
    store(ssi + 8, 0); store(ssi + 0x28, 0); store(ssi + 0x10, 0); store(ssi + 0x18, 0);
    store(ssi + 8, 0x9900); store(ssi + 0x14, 0x6100); store(ssi + 0x20, 4); store(ssi + 0xc, 0xe8);
    for (unsigned i = 0; i < 8; ++i) store(ssi, 0);
    store(ch, 0x300000); store(ch + 0xc, 0x208c); store(ch + 0xc, 0x208d);
    store(dma + 4, 0x400); store(ssi + 0xc, 0x2e8); store(ssi + 8, 0x9b00);
    load(0, dma);
    program.push_back(0xe5902004);  // LDR r2,[r0,#4]
    program.push_back(0xe3120b01);  // TST r2,#0x400
    program.push_back(0x0afffffc);  // BEQ polling read
    store(dma + 4, 0x400);
    load(0, ch); program.push_back(0xe5903008); program.push_back(0xe590400c);
    store(ssi + 8, 0x9d00);
    for (const auto& literal : literals) {
        program[literal.first] |= uint32_t((program.size() - literal.first - 2) * 4);
        program.push_back(literal.second);
    }
    std::vector<uint8_t> rom(program.size() * 4);
    std::memcpy(rom.data(), program.data(), rom.size());
    Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT", "1", 1);
    Imx1ProbeResult results[2];
    for (unsigned fast = 0; fast < 2; ++fast) {
        results[fast] = imx1BootProbe(rom.data(), rom.size(), Imx1ProbeEntry::physicalFlash,
                                    500000, nullptr, nullptr, fast, &options);
        assert(results[fast].stop == Imx1ProbeStop::unsupportedPeripheral &&
               results[fast].address == ssi + 8 && results[fast].peripheralStatus == A::sideEffect);
        assert(results[fast].registers[2] == 0x400 && results[fast].registers[3] == 4096 &&
               results[fast].registers[4] == 0x208d);
    }
    assert(results[0].busAccesses == results[1].busAccesses && results[0].cpsr == results[1].cpsr);
    assert(!std::memcmp(results[0].registers, results[1].registers, sizeof(results[0].registers)));
    write(soc, ssi + 0xc, 0x2e8);
    soc.advanceHclkCycles(10240);
    assert(soc.pcmDmaRemaining() == 16);
    write(soc, ssi + 8, 0x9b00);
    // Reset dividers are both 12. Eq30-4 gives 4*16 PERCLK3 per slot, 128 per frame.
    soc.advanceHclkCycles(63); assert(read(soc, ssi + 0x20) == 0x804);
    soc.advanceHclkCycles(1); assert(read(soc, ssi + 0x20) == 0x704);
    assert(!(read(soc, ssi + 8) & 8));
    assert(soc.pcmTransmittedWords() == 0);
    soc.advanceHclkCycles(192); assert(read(soc, ssi + 0x20) == 0x404);
    assert(soc.pcmDmaRemaining() == 16);
    soc.advanceHclkCycles(1); assert(soc.pcmDmaRemaining() == 16);
    soc.advanceHclkCycles(1); assert(soc.pcmDmaRemaining() == 14);
    soc.advanceHclkCycles(6); assert(soc.pcmDmaRemaining() == 8);
    assert(read(soc, dma + 4) == 0 && read(soc, ch + 8) == 16);
    auto copy = soc;
    soc.advanceHclkCycles(255); assert(read(soc, dma + 4) == 0);
    soc.advanceHclkCycles(1); assert(read(soc, dma + 4) == 0x400);
    assert(soc.pcmDmaRemaining() == 0 && read(soc, ch + 0xc) == 0x2089);
    write(soc, 0x223004, 31); write(soc, 0x223008, 61);
    assert(soc.interruptLines().irq == Imx1AitcLine::asserted);
    write(soc, dma + 8, 0x7ff); assert(soc.interruptLines().irq == Imx1AitcLine::clear);
    write(soc, dma + 8, 0x3ff); assert(soc.interruptLines().irq == Imx1AitcLine::asserted);
    assert(copy.pcmDmaRemaining() == 8);
    write(soc, dma + 4, 0x400); assert(read(soc, dma + 4) == 0);
    assert(soc.interruptLines().irq == Imx1AitcLine::clear);
    // Eight priming words precede the packed SRAM samples, even after DMA completes.
    soc.advanceHclkCycles(120);
    assert(soc.pcmTransmittedWords() == 9 && soc.pcmLastTransmittedWord() == 1);
    for (uint16_t expected : {uint16_t(0x1234),uint16_t(3),uint16_t(0x1235),uint16_t(5),uint16_t(0x1236),uint16_t(7),uint16_t(0x1237)}) {
        soc.advanceHclkCycles(64); assert(soc.pcmLastTransmittedWord() == expected);
    }
    soc.advanceHclkCycles(64); assert(read(soc, ssi + 8) & 0x10);
    write(soc, ssi, 0xbeef); assert(!(read(soc, ssi + 8) & 0x10));
    // TE clear finishes the loaded shift word and holds the FIFO.
    write(soc, ssi + 8, 0x9900);
    soc.advanceHclkCycles(64); const auto words = soc.pcmTransmittedWords();
    soc.advanceHclkCycles(10000); assert(soc.pcmTransmittedWords() == words);
    assert((read(soc, ssi + 0x20) >> 8) == 1);
    // A gated DMAC stalls while the independent serial clock continues.
    soc = configured(); write(soc, ssi + 0xc, 0x2e8); write(soc, ssi + 8, 0x9b00);
    write(soc, 0x21b810, 0x27); soc.advanceHclkCycles(512);
    assert(soc.pcmDmaRemaining() == 16 && read(soc, dma + 4) == 0);
    write(soc, 0x21b810, 0x2f); soc.advanceHclkCycles(8);
    assert(soc.pcmDmaRemaining() == 8);
    // Abort discards internal progress; restart reads the new source and count.
    write(soc, ch + 0xc, 0x2088); assert(soc.pcmDmaRemaining() == 0);
    write(soc, ch + 8, 4); write(soc, ch + 0xc, 0x2089);
    soc.advanceHclkCycles(4); assert(read(soc, dma + 4) == 0x400);
    write(soc, ch + 0xc, 0x2088); write(soc, dma + 4, 0x400);
    assert(read(soc, dma + 4) == 0x400);
    write(soc, dma, 2); assert(read(soc, dma + 4) == 0 && soc.pcmDmaRemaining() == 0);
    // Repeat reloads the newly published SAR, retaining the programmed count and CEN.
    soc = configured(true); write(soc, ssi + 0xc, 0x2e8); write(soc, ssi + 8, 0x9b00);
    write(soc, 0x301000, 0xfacecafe); write(soc, ch, 0x301000);
    soc.advanceHclkCycles(520);
    assert(read(soc, dma + 4) == 0x400 && soc.pcmDmaRemaining() == 16);
    assert(read(soc, ch + 8) == 16 && read(soc, ch + 0xc) == 0x208d);
    write(soc, dma + 4, 0x400); soc.advanceHclkCycles(512);
    assert(read(soc, dma + 4) == 0x400);
    // Unsupported request/format/memory changes are rejected before state changes.
    soc = configured();
    write(soc, ch, 0x400000, A::memoryUnavailable); assert(read(soc, ch) == 0x300000);
    write(soc, ch, 0x31fff4, A::memoryUnavailable); assert(read(soc, ch) == 0x300000);
    write(soc, ch, 0xfffffffcu, A::memoryUnavailable); assert(read(soc, ch) == 0x300000);
    write(soc, 0x221000, 0x81120200, A::sideEffect);
    write(soc, 0x21b020, 0x000a00bb, A::sideEffect);
    write(soc, ch + 8, 20, A::sideEffect); assert(read(soc, ch + 8) == 16);
    write(soc, ch + 0xc, 0x208b, A::sideEffect); assert(read(soc, ch + 0xc) == 0x2089);
    write(soc, ssi + 0xc, 0x2c8, A::sideEffect);
    write(soc, ssi + 8, 0x9d00, A::sideEffect);
    write(soc, ssi + 0x14, 0x6000); write(soc, ssi + 8, 0x9b00, A::sideEffect);
    assert(soc.pcmDmaRemaining() == 16 && read(soc, ssi + 0x20) == 0x804);
    // A large charge and single-clock charges cross the same serial/DMA deadlines.
    for (bool repeat : {false, true}) {
        auto large = configured(repeat), small = large;
        write(large, ssi + 0xc, 0x2e8); write(small, ssi + 0xc, 0x2e8);
        write(large, ssi + 8, 0x9b00); write(small, ssi + 8, 0x9b00);
        for (unsigned interval : {520u, 512u, 64u}) {
            large.advanceHclkCycles(interval);
            for (unsigned i = 0; i < interval; ++i) small.advanceHclkCycles(1);
            for (auto address : {ssi + 8, ssi + 0x20, dma + 4, ch + 8, ch + 0xc})
                assert(read(large, address) == read(small, address));
            assert(large.pcmDmaRemaining() == small.pcmDmaRemaining());
            assert(large.pcmTransmittedWords() == small.pcmTransmittedWords());
            assert(large.pcmLastTransmittedWord() == small.pcmLastTransmittedWord());
            write(large, dma + 4, 0x400); write(small, dma + 4, 0x400);
        }
    }
    // TFS marks the first slot, starting at SSI_EN's initial frame boundary.
    soc = configured(); write(soc, ssi + 8, 0x9b00);
    assert(read(soc, ssi + 8) & 8);
    soc.advanceHclkCycles(63); assert(read(soc, ssi + 8) & 8);
    soc.advanceHclkCycles(1); assert(!(read(soc, ssi + 8) & 8));
    soc.advanceHclkCycles(64); assert(read(soc, ssi + 8) & 8);
    write(soc, ssi + 8, 0x9900); assert(!(read(soc, ssi + 8) & 8));
    soc = configured(); soc.advanceHclkCycles(31); write(soc, ssi + 8, 0x9b00);
    soc.advanceHclkCycles(32); assert(read(soc, ssi + 0x20) == 0x804);
    soc.advanceHclkCycles(1); assert(read(soc, ssi + 0x20) == 0x704);
    assert(!(read(soc, ssi + 8) & 8));
    soc = configured(); write(soc, ssi + 0xc, 0x88); write(soc, ssi + 8, 0x9b00, A::externalInput);
    for (auto size : {1u, 2u, 8u}) {
        uint32_t value = 0xdeadbeef;
        assert(soc.access(ch, size, true, value) == A::width);
        assert(soc.access(ch, size, false, value) == A::width && value == 0xdeadbeef);
    }
    std::puts("PASS: timed SSI/DMA transport, packing, FIFO, completion, repeat, gate, abort, reset, copy and guards");
}
