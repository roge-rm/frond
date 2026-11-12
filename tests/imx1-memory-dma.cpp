// i.MX1 memory-to-memory DMA (REN clear): the data moves when CEN sets, the DISR bit and interrupt source 61
// follow after the bursts' time, the guarded set-ups, abort, DMA reset, a state saved mid-transfer and the wake
// from a WFI on its interrupt.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "imx1_bootstrap.h"
#include "imx1_boot_probe.h"
#include "imx1_mmcsd.h"
#include "machine_state_archive.h"
#include "vSD.h"

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

static bool readSector(uint32_t, void* buf) { std::memset(buf, 0, 512); return true; }
static bool writeSector(uint32_t, const void*) { return true; }

// A guest that starts a memory-to-memory transfer on channel 3 with source 61 enabled and DIMR as given, then
// waits with nothing else to wake it. Code from literals: ldr from the pool after the program.
struct Guest {
    std::vector<uint32_t> code;
    std::vector<std::pair<size_t, uint32_t>> literals;
    size_t beforeStart = 0, beforeWait = 0;
    explicit Guest(uint32_t dimr) {
        constexpr uint32_t ch3 = 0x00209140, sram = 0x00300000;
        write(0x00209000, 1); write(0x00223008, 61); write(0x00209008, dimr);
        write(ch3, sram); write(ch3 + 4, sram + 0x400); write(ch3 + 8, 0x80);
        write(ch3 + 0x10, 0); write(ch3 + 0x14, 0); write(ch3 + 0x18, 0);
        beforeStart = code.size();
        write(ch3 + 0xc, 1);
        beforeWait = code.size();
        code.push_back(0xe3a00000);   // mov r0, #0
        code.push_back(0xee070f90);   // wait for interrupt
        code.push_back(0xe3a06055);   // mov r6, #0x55
        code.push_back(0xe7f000f0);   // undefined, ends the run
        for (size_t n = 0; n < literals.size(); ++n) {
            code[literals[n].first] |= uint32_t((code.size() + n) * 4 - (literals[n].first * 4 + 8));
        }
        for (auto& literal : literals) code.push_back(literal.second);
    }
    void load(unsigned reg, uint32_t value) { literals.push_back({code.size(), value}); code.push_back(0xe59f0000 | reg << 12); }
    void write(uint32_t address, uint32_t value) { load(2, address); load(1, value); code.push_back(0xe5821000); }
    const uint8_t* rom() const { return reinterpret_cast<const uint8_t*>(code.data()); }
    uint32_t size() const { return uint32_t(code.size() * 4); }
};
static Imx1ProbeOptions guestOptions() { Imx1ProbeOptions o{}; o.bootstrapPeripherals = true; return o; }

int main() {
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT", "1", 1);
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

    // CCR switches: a card channel under way refuses a memory-to-memory start, and a memory transfer under way a
    // card start, each with nothing changed.
    {
        VSD* card = vsdInit(readSector, writeSector, 2048);
        assert(card);
        Imx1Bootstrap other;
        other.setDmaExternal(&gpu);
        other.setMmcsdEmptySlot(true);
        other.attachMmcsdCard(card);
        write(other, dma, 1);
        constexpr uint32_t ch9 = dma + 0x80 + 9 * 0x40, fifo = 0x00214038;
        channel(other, ch9, fifo, sram, 512, 0, 0);
        write(other, ch9 + 0x10, 13);
        write(other, ch9 + 0xc, 0x829);
        write(other, ch9 + 0xc, 0x1, A::sideEffect);
        write(other, ch9 + 0xc, 0x0);
        // The set-up of a memory transfer that has RSSR 13 would pass the card checks if it got that far.
        channel(other, ch3, sram, sram + 0x400, 0x80, 0, 0);
        write(other, ch3 + 0x10, 13);
        write(other, ch3 + 0xc, 0x1);
        assert(other.memoryDmaBusy());
        write(other, ch3 + 0xc, 0x9, A::sideEffect);
        write(other, ch3 + 0xc, 0x1, A::sideEffect);
        assert(other.memoryDmaBusy());
    }

    // With the DMA clock gated off a start is refused, and runs again with the clock back.
    {
        constexpr uint32_t gccr = 0x0021b810;
        channel(soc, ch3, sram, sram + 0x400, 0x40, 0, 0);
        write(soc, gccr, 0x27);
        write(soc, ch3 + 0xc, 0x1, A::sideEffect);
        assert(!soc.memoryDmaBusy());
        write(soc, gccr, 0x2f);
        write(soc, ch3 + 0xc, 0x1);
        assert(soc.memoryDmaBusy());
        write(soc, ch3 + 0xc, 0);
    }

    // A WFI with nothing but the transfer's DISR bit to wake it: Exact and Fast skip to it and reach the same HCLK.
    {
        const Guest unmasked(0x7f7), masked(0x7ff);
        Imx1ProbeResult results[2];
        for (unsigned fast = 0; fast < 2; ++fast) {
            auto options = guestOptions();
            results[fast] = imx1BootProbe(unmasked.rom(), unmasked.size(), Imx1ProbeEntry::physicalFlash, 50000,
                                          nullptr, nullptr, fast, &options);
            const auto& r = results[fast];
            assert(r.stop != Imx1ProbeStop::idleNoWake && r.registers[6] == 0x55);
            assert(r.idleHclk > 0 && r.idleHclk <= 76);
            const auto stuck = imx1BootProbe(masked.rom(), masked.size(), Imx1ProbeEntry::physicalFlash, 50000,
                                             nullptr, nullptr, fast, &options);
            assert(stuck.stop == Imx1ProbeStop::idleNoWake && stuck.registers[6] != 0x55);
        }
        assert(results[0].idleHclk == results[1].idleHclk && results[0].stop == results[1].stop);
        assert(!std::memcmp(results[0].registers, results[1].registers, sizeof(results[0].registers)));

        // A machine saved with the transfer under way (with an MDMA section) and before it started (with none)
        // loads and finishes the wait at the same HCLK as the one that kept running.
        auto finish = [&](Imx1Machine* m) {
            Imx1MachineRun run{};
            for (unsigned i = 0; i < 20; ++i) {
                run = imx1MachineRunSlice(m, 1000, 100000);
                if (run.status != Imx1MachineStatus::yielded && run.status != Imx1MachineStatus::waiting) break;
            }
            return run;
        };
        auto holds = [](const std::vector<uint8_t>& state, const char* name) {
            for (size_t i = 0; i + 4 <= state.size(); ++i) if (!std::memcmp(state.data() + i, name, 4)) return true;
            return false;
        };
        for (unsigned fast = 0; fast < 2; ++fast) {
            const auto options = guestOptions();
            auto create = [&] {
                auto* m = imx1MachineCreate(unmasked.rom(), unmasked.size(), Imx1ProbeEntry::physicalFlash, fast, options);
                assert(m);
                return m;
            };
            std::vector<uint8_t> before, during;
            auto* m = create();
            assert(imx1MachineRunSlice(m, uint32_t(unmasked.beforeStart), 100000).status == Imx1MachineStatus::yielded);
            assert(imx1MachineSaveState(m, before) == Imx1StateStatus::ok);
            assert(imx1MachineRunSlice(m, uint32_t(unmasked.beforeWait - unmasked.beforeStart), 100000).status ==
                   Imx1MachineStatus::yielded);
            assert(imx1MachineSaveState(m, during) == Imx1StateStatus::ok);
            const auto expected = finish(m);
            imx1MachineStop(m);
            assert(expected.status == Imx1MachineStatus::terminal && expected.result.registers[6] == 0x55 &&
                   expected.result.idleHclk > 0);
            assert(holds(during, "MDMA") && !holds(before, "MDMA"));
            for (auto* state : {&during, &before}) {
                m = create();
                const auto check = imx1MachineCheckState(m, state->data(), state->size());
                const auto info = imx1MachineLoadState(m, state->data(), state->size());
                if (info.status != Imx1StateStatus::ok)
                    fprintf(stderr, "load: %s %s\n", imx1StateStatusName(info.status), info.section);
                assert(check.status == Imx1StateStatus::ok && info.status == Imx1StateStatus::ok);
                const auto run = finish(m);
                imx1MachineStop(m);
                assert(run.status == expected.status && run.result.idleHclk == expected.result.idleHclk);
                assert(!std::memcmp(run.result.registers, expected.result.registers, sizeof(run.result.registers)));
            }
        }
    }
    std::puts("PASS: memory-to-memory DMA moves at CEN, sets DISR and source 61 after its bursts, guards, abort, reset, state, REN switches, DMA clock, WFI wake");
}
