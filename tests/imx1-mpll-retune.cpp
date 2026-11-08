#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "imx1_boot_probe.h"

namespace {
using A = Imx1BootstrapAccess;
constexpr uint32_t cgm = 0x0021b000, timer = 0x00202000;
uint32_t read(Imx1Bootstrap& b, uint32_t address) {
    uint32_t v = 0;
    assert(b.access(address, 4, false, v) == A::ok);
    return v;
}
void write(Imx1Bootstrap& b, uint32_t address, uint32_t v, A status = A::ok, uint32_t mode = 3) {
    const auto actual = b.access(address, 4, true, v, mode);
    if (actual != status) std::fprintf(stderr, "write %08x=%08x mode%u status%s expected%s\n", address, v, mode, imx1BootstrapAccessName(actual), imx1BootstrapAccessName(status));
    assert(actual == status);
}
void configure(Imx1Bootstrap& b, uint32_t target = 0x03e717bf) {
    b.setMcuRetuneScenario(true);
    b.setSystemPllScenario(true);
    assert(b.setRtcInitialTime({1, 12, 34, 56}));
    write(b, cgm, 0x57000003, A::ok, 0);
    write(b, cgm + 4, target, A::ok, 0);
    write(b, cgm, 0x57200003, A::ok, 0);
    b.advanceFastBusCycles(6000);
    b.advanceCpuCycles(56, 3, true);
    write(b, cgm + 12, 0x123638ad);
    write(b, cgm, 0x57400003);
    assert(b.finishSystemPllBusStall());
    write(b, cgm, 0x5700ac03);
    write(b, cgm + 32, 2, A::ok, 0);
    write(b, timer + 4, 31); write(b, timer, 0x103);
    write(b, 0x00203004, 6); write(b, 0x00203000, 0x103);
    b.advanceCpuCycles(3, 3, true);
}
void compare(Imx1Bootstrap& a, Imx1Bootstrap& b) {
    for (auto address : {cgm, cgm + 4, cgm + 12, cgm + 32, timer + 16, 0x00203010u,
                         0x00204000u, 0x00204004u, 0x00204020u, 0x00208000u, 0x00201000u})
        assert(read(a, address) == read(b, address));
    const auto x = a.pcmState(), y = b.pcmState();
    assert(x.remaining == y.remaining && x.source == y.source && x.pendingSource == y.pendingSource &&
           x.fifoWords == y.fifoWords && x.status == y.status && x.transmittedWords == y.transmittedWords &&
           x.dmaActive == y.dmaActive);
    assert(a.clockStatus() == b.clockStatus());
    assert(a.pcmLastTransmittedWord() == b.pcmLastTransmittedWord());
}
void spacingAndAliases() {
    Imx1Bootstrap resetAsync;
    resetAsync.setMcuRetuneScenario(true);
    resetAsync.advanceCpuCycles(1, 3, true);
    write(resetAsync, cgm + 4, 0x03e717bf, A::sideEffect);
    write(resetAsync, cgm, 0x0f20ac03, A::sideEffect);
    assert(resetAsync.cpuClockModeSupported(3, true));
    Imx1Cgm c;
    c.setMcuRetuneScenario(true);
    auto w = [&](uint32_t address, uint32_t v, Imx1CgmAccess status = Imx1CgmAccess::ok) {
        assert(c.access(address, 4, true, v) == status);
    };
    w(cgm + 4, 0x03e717bf);
    w(cgm, 0x57200003);
    c.advanceSystemPllCycles(6000);
    c.qualifyMcuOutput();
    c.advanceMcuReference(55 * 125, 762752);
    w(cgm + 4, 0x03e703bf, Imx1CgmAccess::sideEffect);
    c.advanceMcuReference(124, 762752);
    w(cgm + 4, 0x03e703bf, Imx1CgmAccess::sideEffect);
    c.advanceMcuReference(1, 762752);
    w(cgm + 4, 0x03e703bf);
    assert(c.mcuClockReady(true) && !c.mcuClockReady(false));
    w(cgm + 4, 0x03e717bf, Imx1CgmAccess::sideEffect);
    w(cgm, 0x57200003);
    for (unsigned i = 0; i < 100; ++i) {
        uint32_t v = 0; assert(c.access(cgm, 4, false, v) == Imx1CgmAccess::ok && (v & 0x200000));
    }
    w(cgm, 0x57200003, Imx1CgmAccess::sideEffect);
    w(cgm + 4, 0x03e717bf, Imx1CgmAccess::sideEffect);
    w(cgm, 0x57000003); // Zero cannot cancel the reference deadline.
    auto copied = c;
    c.advanceMcuReference(2 * 762752 - 1, 762752);
    uint32_t v = 0; assert(c.access(cgm, 4, false, v) == Imx1CgmAccess::ok && (v & 0x200000));
    c.advanceMcuReference(1, 762752);
    copied.advanceMcuReference(2, 1);
    assert(c.access(cgm, 4, false, v) == Imx1CgmAccess::ok && !(v & 0x200000));
    w(cgm + 4, 0x03e717be, Imx1CgmAccess::sideEffect);
    w(cgm + 4, 0x03e717bf);
    uint32_t same = 0x03e717bf;
    assert(copied.access(cgm + 4, 4, true, same) == Imx1CgmAccess::ok);
    // A restart can be reused without programming, and reset removes every qualification.
    w(cgm, 0x57200003); c.advanceMcuReference(UINT64_MAX, 762752);
    c = {};
    w(cgm + 4, 0x03e717bf); c.advanceMcuReference(UINT64_MAX, 1);
    w(cgm + 4, 0x03e717bf, Imx1CgmAccess::externalInput);
}
void helperClocks() {
    Imx1Bootstrap b; configure(b);
    // Queue packed SRAM samples behind a partly elapsed SSI word before reprogramming.
    constexpr uint32_t ssi = 0x218000, dma = 0x209000, ch = 0x209300;
    write(b, dma, 1); write(b, dma + 8, 0x3ff);
    write(b, ssi + 8, 0x9900); write(b, ssi + 0x14, 0x6100);
    write(b, ssi + 0x20, 4); write(b, ssi + 0xc, 0xe8);
    for (unsigned i = 0; i < 8; ++i) write(b, ssi, 0);
    for (unsigned i = 0; i < 4; ++i) write(b, 0x300000 + i * 4, 0x12340001 + i * 0x10002);
    write(b, ch, 0x300000); write(b, ch + 4, ssi); write(b, ch + 8, 16);
    write(b, ch + 0xc, 0x2088); write(b, ch + 0x10, 16); write(b, ch + 0x14, 8);
    write(b, ch + 0xc, 0x2089); write(b, ssi + 0xc, 0x2e8); write(b, ssi + 8, 0x9b00);
    b.advanceCpuCycles(31, 3, true);
    assert(b.pcmDmaRemaining() == 16 && b.pcmTransmittedWords() == 0);
    auto baseline = b, chunks = b;
    // The native helper switches to FastBus, programs, changes PRESC, restarts, polls, returns async.
    write(b, cgm + 4, 0x03e717bf, A::ok, 0);
    write(chunks, cgm + 4, 0x03e717bf, A::ok, 0);
    for (auto* board : {&b, &baseline, &chunks}) {
        board->advanceFastBusCycles(5);
        write(*board, cgm, 0x57002c03, A::ok, 0);
        board->advanceFastBusCycles(3);
    }
    write(b, cgm, 0x57202c03, A::ok, 0);
    write(chunks, cgm, 0x57202c03, A::ok, 0);
    auto rejected = b;
    write(b, cgm + 4, 0x03e717be, A::sideEffect, 0);
    write(b, cgm, 0x57202c03, A::sideEffect, 0);
    b.advanceFastBusCycles(488);
    rejected.advanceFastBusCycles(488);
    assert(read(b, cgm) & 0x200000); compare(b, rejected);
    b.advanceFastBusCycles(1);
    baseline.advanceFastBusCycles(489);
    for (unsigned i = 0; i < 489; ++i) chunks.advanceFastBusCycles(1);
    assert(!(read(b, cgm) & 0x200000));
    compare(b, chunks); compare(b, baseline);
    assert(b.pcmDmaRemaining() == 0 && b.pcmTransmittedWords() >= 16);
    // Subsequent single-cycle charges expose any hidden SSI or DMA phase difference.
    for (unsigned i = 0; i < 200; ++i) {
        b.advanceCpuCycles(1, 3, true); chunks.advanceCpuCycles(1, 3, true); baseline.advanceCpuCycles(1, 3, true);
        compare(b, baseline); compare(b, chunks);
    }
    b.advanceCpuCycles(UINT32_MAX, 3, true);
    chunks.advanceCpuCycles(UINT32_MAX - 71, 3, true); chunks.advanceCpuCycles(71, 3, true);
    compare(b, chunks);
    // Repeating the full restart preserves the fractional timer/reference phase again.
    write(b, cgm + 4, 0x03e717bf); write(chunks, cgm + 4, 0x03e717bf);
    write(b, cgm, 0x57202c03); write(chunks, cgm, 0x57202c03);
    b.advanceCpuCycles(12205, 3, true);
    for (unsigned i = 0; i < 12205; ++i) chunks.advanceCpuCycles(1, 3, true);
    compare(b, chunks); assert(!(read(b, cgm) & 0x200000));
    // The opt-in cannot admit an unequal ratio or relax legacy strict behaviour.
    write(b, cgm + 4, 0x03e717be, A::sideEffect);
    b.setMcuRetuneScenario(false);
    write(b, cgm + 4, 0x03e717bf, A::sideEffect);
}
struct Trace { std::vector<Imx1ProbeAccess> bus; };
void observe(void* p, const Imx1ProbeAccess& e) { static_cast<Trace*>(p)->bus.push_back(e); }
void literal(std::vector<uint32_t>& words, unsigned reg, uint32_t v) {
    words.push_back(0xe59f0000 | reg << 12); words.push_back(0xea000000); words.push_back(v);
}
void wholeSequence() {
    for (unsigned missing = 0; missing < 2; ++missing) {
        Imx1ProbeOptions invalid{};
        invalid.virtualMpllRetune = true;
        invalid.bootstrapPeripherals = missing != 0;
        invalid.assumeMpllSettled = missing == 0;
        const auto result = imx1BootProbe(nullptr, 0, Imx1ProbeEntry::physicalFlash, 1, nullptr, nullptr, false, &invalid);
        assert(result.stop == Imx1ProbeStop::unsupportedPeripheral && result.peripheralStatus == A::sequence);
    }
    std::vector<uint32_t> words;
    literal(words, 0, cgm); literal(words, 1, 0x57000003); words.push_back(0xe5801000);
    literal(words, 1, 0x03e717bf); words.push_back(0xe5801004);
    literal(words, 1, 0x57200003); words.push_back(0xe5801000);
    auto poll = [&] { words.push_back(0xe5902000); words.push_back(0xe3120602); words.push_back(0x1afffffc); };
    poll();
    literal(words, 1, 0xc0000078); words.push_back(0xee011f10);
    for (unsigned i = 0; i < 60; ++i) words.push_back(0xe1a03003);
    literal(words, 1, 0x78); words.push_back(0xee011f10);
    literal(words, 1, 0x03e717bf); words.push_back(0xe5801004);
    words.push_back(0xe5901000); words.push_back(0xe3c11c80); words.push_back(0xe5801000);
    words.push_back(0xe5901000); words.push_back(0xe3811602); words.push_back(0xe5801000);
    poll();
    literal(words, 1, 0xc0000078); words.push_back(0xee011f10);
    words.push_back(0xee114f10); words.push_back(0xe7f000f0);
    auto run = [&](bool fast, bool enabled, Trace& trace) {
        Imx1ProbeOptions o{}; o.bootstrapPeripherals = true; o.assumeMpllSettled = true;
        o.virtualMpllRetune = enabled; o.busObserver = observe; o.busContext = &trace;
        return imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4,
            Imx1ProbeEntry::physicalFlash, 30000, nullptr, nullptr, fast, &o);
    };
    Trace strict, exact, fast;
    assert(run(false, false, strict).stop == Imx1ProbeStop::unsupportedPeripheral);
    const auto a = run(false, true, exact);
    assert(a.stop == Imx1ProbeStop::unsupportedInstruction && a.registers[4] == 0xc0000078);
    unsigned writes = 0, pending = 0, complete = 0;
    for (const auto& e : exact.bus) {
        assert(e.status == A::ok);
        if (e.address == cgm + 4 && e.write) ++writes;
        if (e.address == cgm && !e.write) { if (e.value & 0x200000) ++pending; else ++complete; }
    }
    assert(writes == 2 && pending > 1 && complete > 1);
#if FROND_TEST_FAST
    const auto b = run(true, true, fast);
    assert(a.stop == b.stop && a.trace.step == b.trace.step && a.cpsr == b.cpsr);
    assert(!std::memcmp(a.registers, b.registers, sizeof(a.registers)) && exact.bus.size() == fast.bus.size());
    for (size_t i = 0; i < exact.bus.size(); ++i) {
        const auto& x = exact.bus[i]; const auto& y = fast.bus[i];
        assert(x.step == y.step && x.pc == y.pc && x.address == y.address && x.value == y.value &&
               x.write == y.write && x.status == y.status);
    }
    assert(b.jitInstructions);
#endif
}
}
int main() {
    spacingAndAliases(); helperClocks(); wholeSequence();
    std::puts("i.MX1 equal-frequency MCU retune: PASS");
}
