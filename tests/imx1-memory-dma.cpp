// i.MX1 memory-to-memory DMA (REN clear): the data moves when CEN sets, the DISR bit and interrupt source 61
// follow after the bursts' time, the guarded set-ups, abort, DMA reset and a state saved mid-transfer.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "imx1_bootstrap.h"
#include "machine_state_archive.h"

// Stands in for the graphics memory on CS5: 1 MiB at 0x16800000, a fixed wait a beat.
struct FakeGraphics final : Imx1DmaExternal {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x100000);
    Imx1BootstrapAccess refuse = Imx1BootstrapAccess::ok;
    uint32_t perBeat = 5, moves = 0;
    bool owns(uint32_t address) const override { return address >= 0x16800000 && address < 0x16900000; }
    Imx1BootstrapAccess check(uint32_t address, uint32_t size, bool) const override {
        assert(owns(address) && owns(address + size - 1));
        return refuse;
    }
    void move(uint32_t address, uint32_t size, bool write, uint32_t& value) override {
        assert(refuse == Imx1BootstrapAccess::ok);
        ++moves;
        if (write) std::memcpy(bytes.data() + (address - 0x16800000), &value, size);
        else std::memcpy(&value, bytes.data() + (address - 0x16800000), size);
    }
    bool clocks(uint32_t& clocks) const override { clocks = perBeat; return true; }
};

int main() {
    using A = Imx1BootstrapAccess;
    constexpr uint32_t sram = 0x00300000, dma = 0x00209000, graphics = 0x16800000;
    constexpr uint32_t ch7 = dma + 0x80 + 7 * 0x40, ch3 = dma + 0x80 + 3 * 0x40;
    FakeGraphics gpu;
    Imx1Bootstrap soc;
    soc.setDmaExternal(&gpu);
    auto read = [&](Imx1Bootstrap& b, uint32_t address) {
        uint32_t value = 0xdeadbeef;
        assert(b.access(address, 4, false, value) == A::ok);
        return value;
    };
    auto write = [&](Imx1Bootstrap& b, uint32_t address, uint32_t value, A expected = A::ok) {
        const auto got = b.access(address, 4, true, value);
        if (got != expected) fprintf(stderr, "write %08x: %d, expected %d\n", address, int(got), int(expected));
        assert(got == expected);
    };
    auto channel = [&](Imx1Bootstrap& b, uint32_t base, uint32_t source, uint32_t destination, uint32_t count,
                       uint32_t burst, uint32_t bucr) {
        write(b, base, source); write(b, base + 4, destination); write(b, base + 8, count);
        write(b, base + 0x10, 0); write(b, base + 0x14, burst); write(b, base + 0x18, bucr);
    };
    auto done = [&](Imx1Bootstrap& b, unsigned number) { return (read(b, dma + 4) >> number) & 1; };
    auto pending61 = [&](Imx1Bootstrap& b) { return (b.interruptPending().normal >> 61) & 1; };
    uint8_t* source = soc.plainPage(sram, true);
    for (unsigned i = 0; i < 0x1000; ++i) source[i] = uint8_t(i * 13 + 7);
    write(soc, dma, 1);
    // DIMR as the ROM leaves it: channels 7 and 10 unmasked. AITC source 61 enabled.
    write(soc, dma + 8, 0x37f);
    write(soc, 0x00223008, 61);

    // SRAM to graphics memory as the games set it up: 32-bit ports, BLR 0 (64 bytes), BUCR 0x18.
    // 3612 bytes: 903 beats a side, 57 bursts. 903 + 903 * 5 + 57 * 3 + 56 * 24 = 6933 HCLK.
    channel(soc, ch7, sram, graphics + 0x100, 0xe1c, 0, 0x18);
    write(soc, ch7 + 0xc, 1);
    assert(!std::memcmp(gpu.bytes.data() + 0x100, source, 0xe1c));
    assert(gpu.bytes[0xff] == 0 && gpu.bytes[0x100 + 0xe1c] == 0 && gpu.moves == 903);
    assert(soc.memoryDmaBusy() && soc.idleQuietHclk() <= 6933);
    // The set-up can't change while it runs, nor can it start again.
    write(soc, ch7, sram, A::sideEffect); write(soc, ch7 + 8, 4, A::sideEffect);
    write(soc, ch7 + 0xc, 1, A::sideEffect);
    soc.advanceHclkCycles(6932);
    assert(!done(soc, 7) && !pending61(soc) && soc.idleQuietHclk() == 1);
    soc.advanceHclkCycles(1);
    assert(done(soc, 7) && pending61(soc) && !soc.memoryDmaBusy());
    write(soc, dma + 4, 1u << 7);
    assert(!done(soc, 7) && !pending61(soc));
    // A masked channel sets DISR with no interrupt.
    write(soc, ch7 + 0xc, 0);
    channel(soc, ch3, graphics + 0x100, sram + 0x800, 0x40, 0x20, 0);
    write(soc, ch3 + 0xc, 1);
    assert(!std::memcmp(source + 0x800, source, 0x40));
    // 16 beats a side, 2 bursts: 16 * 5 + 16 + 2 * 3 = 102.
    soc.advanceHclkCycles(101); assert(!done(soc, 3));
    soc.advanceHclkCycles(1); assert(done(soc, 3) && !pending61(soc));
    write(soc, dma + 4, 1u << 3); write(soc, ch3 + 0xc, 0);

    // SRAM to SRAM, 16-bit source and 8-bit destination ports, BLR 16: 64 + 128 beats, 8 bursts.
    // 64 + 128 + 8 * 6 + 7 * 2 = 254.
    channel(soc, ch3, sram, sram + 0x400, 0x80, 16, 2);
    write(soc, ch3 + 0xc, 0x61);
    assert(!std::memcmp(source + 0x400, source, 0x80));
    soc.advanceHclkCycles(253); assert(!done(soc, 3));
    soc.advanceHclkCycles(1); assert(done(soc, 3));
    write(soc, dma + 4, 1u << 3); write(soc, ch3 + 0xc, 0);
    // Overlapping ends copy a burst at a time, as the DMA reads each burst before writing it.
    for (unsigned i = 0; i < 0x80; ++i) source[0xc00 + i] = uint8_t(i);
    uint8_t expected[0x80];
    std::memcpy(expected, source + 0xc00, 0x80);
    for (unsigned burst = 0; burst < 0x40; burst += 8) {
        uint8_t held[8];
        std::memcpy(held, expected + burst, 8);
        std::memcpy(expected + 4 + burst, held, 8);
    }
    channel(soc, ch3, sram + 0xc00, sram + 0xc04, 0x40, 8, 0);
    write(soc, ch3 + 0xc, 1);
    assert(!std::memcmp(source + 0xc00, expected, 0x80));
    assert(source[0xc0c] == 4 && source[0xc10] == 12);
    soc.advanceHclkCycles(1000); assert(done(soc, 3));
    write(soc, dma + 4, 1u << 3); write(soc, ch3 + 0xc, 0);

    // Clearing CEN abandons it: the data has moved, the DISR bit never sets.
    channel(soc, ch7, sram, graphics + 0x2000, 0x100, 0, 0);
    write(soc, ch7 + 0xc, 1);
    write(soc, ch7 + 0xc, 0);
    assert(!soc.memoryDmaBusy() && !std::memcmp(gpu.bytes.data() + 0x2000, source, 0x100));
    soc.advanceHclkCycles(10000); assert(!done(soc, 7));

    // Guarded set-ups, each with nothing moved.
    const uint32_t moves = gpu.moves;
    channel(soc, ch7, sram, graphics + 0x3000, 0x100, 0, 0);
    write(soc, ch7 + 0xc, 0x5, A::sideEffect);        // repeat
    write(soc, ch7 + 0xc, 0x401, A::sideEffect);      // 2D source
    write(soc, ch7 + 0xc, 0x201, A::sideEffect);      // MDIR
    write(soc, ch7 + 8, 0); write(soc, ch7 + 0xc, 1, A::sideEffect);
    write(soc, ch7 + 8, 0x101); write(soc, ch7 + 0xc, 1, A::sideEffect);  // not whole 32-bit beats
    write(soc, ch7 + 8, 0x100);
    write(soc, ch7 + 4, 0x00500000); write(soc, ch7 + 0xc, 1, A::memoryUnavailable);
    write(soc, ch7 + 4, 0x168fff80); write(soc, ch7 + 0xc, 1, A::memoryUnavailable);  // runs past the end
    write(soc, ch7 + 4, graphics + 0x3000);
    gpu.refuse = A::sequence; write(soc, ch7 + 0xc, 1, A::sequence); gpu.refuse = A::ok;
    write(soc, 0x00209000, 0); write(soc, ch7 + 0xc, 1, A::sideEffect); write(soc, 0x00209000, 1);
    assert(gpu.moves == moves && !soc.memoryDmaBusy() && gpu.bytes[0x3000] == 0);
    // A board with no graphics memory reaches none.
    {
        Imx1Bootstrap bare;
        write(bare, dma, 1);
        channel(bare, ch7, sram, graphics, 0x100, 0, 0);
        write(bare, ch7 + 0xc, 1, A::memoryUnavailable);
    }

    // DMA reset ends a transfer under way.
    write(soc, ch7 + 0xc, 1);
    assert(soc.memoryDmaBusy());
    write(soc, dma, 2);
    assert(!soc.memoryDmaBusy());
    soc.advanceHclkCycles(10000);
    assert(!done(soc, 7));

    // A state saved half way goes into a new board and finishes at the same HCLK.
    write(soc, dma, 1); write(soc, dma + 8, 0x37f);
    channel(soc, ch7, sram, graphics + 0x4000, 0x400, 0, 0);
    write(soc, ch7 + 0xc, 1);
    // 256 + 256 * 5 + 16 * 3 = 1584.
    soc.advanceHclkCycles(1000);
    std::vector<uint8_t> out[3];
    {
        StateWriter board(out[0]); soc.state(board);
        StateWriter memory(out[1]); soc.stateSram(memory);
        StateWriter transfer(out[2]); soc.stateMemoryDma(transfer);
    }
    Imx1Bootstrap next;
    next.setDmaExternal(&gpu);
    {
        StateReader board(out[0].data(), out[0].size()); next.state(board); assert(board.complete());
        StateReader memory(out[1].data(), out[1].size()); next.stateSram(memory); assert(memory.complete());
        StateReader transfer(out[2].data(), out[2].size()); next.stateMemoryDma(transfer); assert(transfer.complete());
    }
    for (auto* b : {&soc, &next}) {
        assert(b->memoryDmaBusy());
        b->advanceHclkCycles(583); assert(!done(*b, 7));
        b->advanceHclkCycles(1); assert(done(*b, 7) && !b->memoryDmaBusy());
    }
    // A section that claims a channel with no memory transfer set up is refused.
    {
        std::vector<uint8_t> bad;
        { StateWriter w(bad); uint32_t active = 1u << 5, remaining[11] = {}; remaining[5] = 9;
          w.Do32(active).DoBuffer32(remaining, 11); }
        Imx1Bootstrap other;
        StateReader r(bad.data(), bad.size()); other.stateMemoryDma(r);
        assert(!r.ok());
    }
    std::puts("PASS: memory-to-memory DMA moves at CEN, sets DISR and source 61 after its bursts, guards, abort, reset, state");
}
