#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "imx1_boot_probe.h"

namespace {
using A = Imx1BootstrapAccess;
constexpr uint32_t cgm = 0x21b000, ssi = 0x218000, dma = 0x209000, ch = 0x209300;
void write(Imx1Bootstrap& b, uint32_t a, uint32_t v, A expected = A::ok, unsigned mode = 3) {
    const auto result = b.access(a, 4, true, v, mode);
    if (result != expected) std::fprintf(stderr, "%08x=%08x mode%u: %s expected %s\n", a, v, mode,
                                       imx1BootstrapAccessName(result), imx1BootstrapAccessName(expected));
    assert(result == expected);
}
uint32_t read(Imx1Bootstrap& b, uint32_t a) {
    uint32_t v = 0; const auto status = b.access(a, 4, false, v);
    if (status != A::ok) std::fprintf(stderr, "read %08x: %s\n", a, imx1BootstrapAccessName(status));
    assert(status == A::ok); return v;
}
void configure(Imx1Bootstrap& b) {
    b.setSystemPllScenario(true);
    assert(b.setRtcInitialTime({1,12,34,56}));
    write(b, cgm, 0x57000003, A::ok, 0);
    write(b, cgm + 4, 0x03e717bf, A::ok, 0);
    write(b, cgm, 0x57200003, A::ok, 0);
    b.advanceFastBusCycles(6000);
    b.advanceCpuCycles(56, 3, true);
    write(b, cgm + 12, 0x123638ad); write(b, cgm, 0x57400003);
    assert(b.finishSystemPllBusStall());
    write(b, cgm + 32, 0x00100012, A::ok, 0);
    write(b, 0x202004, 31); write(b, 0x202000, 0x103);
    write(b, 0x203004, 6); write(b, 0x203000, 0x103);
    write(b, 0x201000, 0x7f02); write(b, 0x201000, 0x7f03);
    write(b, ssi + 8, 0x9900); write(b, ssi + 0x14, 0x6100);
    write(b, ssi + 0x20, 4); write(b, ssi + 0xc, 0x2e8);
    for (unsigned i = 0; i < 8; ++i) write(b, ssi, 0x3100 + i);
    write(b, ssi + 8, 0x9b00);
    b.advanceCpuCycles(37, 3, true);
}
void compare(Imx1Bootstrap& a, Imx1Bootstrap& b, bool sameCgm = true) {
    if (sameCgm) assert(read(a, cgm) == read(b, cgm));
    for (auto address : {cgm + 4, cgm + 12, cgm + 32, 0x202010u, 0x203010u,
                         0x204000u, 0x204004u, 0x204014u, 0x204020u, 0x201000u,
                         0x201008u, 0x208000u, ssi + 8, ssi + 0x20, dma + 4, ch + 8, ch + 12})
        assert(read(a, address) == read(b, address));
    const auto x = a.pcmState(), y = b.pcmState();
    assert(x.remaining == y.remaining && x.source == y.source && x.pendingSource == y.pendingSource &&
           x.fifoWords == y.fifoWords && x.status == y.status && x.transmittedWords == y.transmittedWords &&
           x.dmaActive == y.dmaActive && a.pcmLastTransmittedWord() == b.pcmLastTransmittedWord());
    assert(a.clockStatus() == b.clockStatus());
}
void serialDomains() {
    Imx1Bootstrap b; configure(b);
    auto unchanged = b;
    write(b, cgm, 0x5700ac03);
    assert(b.cpuClockModeSupported(3, true));
    // PRESC /2 charges twice the reference duration of the unchanged CPU cycle.
    // BCLK /12 leaves the serial and timer paths at the same System PLL time.
    for (unsigned i = 0; i < 18000; ++i) {
        b.advanceCpuCycles(1, 3, true); unchanged.advanceCpuCycles(2, 3, true);
        compare(b, unchanged, false);
    }
    // External bus waits charge HCLK rather than CPU cycles, preserving equal System PLL time.
    b.advanceHclkCycles(17); unchanged.advanceHclkCycles(17 * 12);
    compare(b, unchanged, false);
    assert(b.pcmTransmittedWords() > 8 && b.pcmState().fifoWords == 0);
    assert(read(b, ssi + 8) & 0x10);
    assert(b.pcmLastTransmittedWord() == 0x3107);
    // Restore the dividers during an underrun word without resetting its phase.
    write(b, cgm, 0x57000003);
    for (unsigned i = 0; i < 2500; ++i) {
        b.advanceCpuCycles(1, 3, true); unchanged.advanceCpuCycles(1, 3, true); compare(b, unchanged);
    }
    write(b, cgm, 0x5700ac03); write(unchanged, cgm, 0x5700ac03);
    auto chunks = b;
    b.advanceCpuCycles(1000000, 3, true);
    for (unsigned i = 0; i < 1000; ++i) chunks.advanceCpuCycles(1000, 3, true);
    compare(b, chunks);
    // TE clear still leaves the internal continuous serial clock running.
    write(b, ssi + 8, 0x9900); write(chunks, ssi + 8, 0x9900);
    b.advanceCpuCycles(3000, 3, true); chunks.advanceCpuCycles(3000, 3, true);
    write(b, cgm + 32, 0x000f0012, A::sideEffect);
    compare(b, chunks);
    write(b, ssi, 0xabcd); write(chunks, ssi, 0xabcd);
}
void fastBusDomains() {
    Imx1Bootstrap b; configure(b);
    b.setLcdcContrastOnly(true);
    write(b, 0x20502c, 0x580); write(b, 0x205034, 2);
    write(b, 0x208008, 47); write(b, 0x208004, 17); write(b, 0x208000, 0x10);
    // Establish nonzero independent fractions before the native FastBus handoff.
    b.advanceCpuCycles(17, 3, true);
    auto unchanged = b;
    write(b, cgm, 0x57003c03, A::ok, 0);
    for (unsigned i = 0; i < 4000; ++i) {
        b.advanceCpuCycles(1, 0, true); unchanged.advanceCpuCycles(16, 0, true);
        compare(b, unchanged, false);
        assert(b.lcdcContrastPhase() == unchanged.lcdcContrastPhase());
        assert(read(b, 0x20800c) == read(unchanged, 0x20800c));
    }
    b.advanceHclkCycles(11); unchanged.advanceHclkCycles(11 * 16);
    compare(b, unchanged, false);
    assert(b.lcdcContrastPhase() == unchanged.lcdcContrastPhase());
    assert(read(b, 0x20800c) == read(unchanged, 0x20800c));
    write(b, cgm, 0x57000003, A::ok, 0);
    // Returning to asynchronous operation must retain the pre-handoff PLL fraction.
    for (unsigned i = 0; i < 3000; ++i) {
        b.advanceCpuCycles(1, 3, true); unchanged.advanceCpuCycles(1, 3, true);
        compare(b, unchanged);
        assert(b.lcdcContrastPhase() == unchanged.lcdcContrastPhase());
        assert(read(b, 0x20800c) == read(unchanged, 0x20800c));
    }
    write(b, cgm, 0x57003c03, A::ok, 0);
    auto chunks = b;
    b.advanceCpuCycles(100000, 0, true);
    for (unsigned i = 0; i < 1000; ++i) chunks.advanceCpuCycles(100, 0, true);
    compare(b, chunks);
    assert(b.lcdcContrastPhase() == chunks.lcdcContrastPhase());
    assert(read(b, 0x20800c) == read(chunks, 0x20800c));
    // An actual PERCLK1 divider change remains guarded with live consumers.
    write(b, cgm + 32, 0x00100013, A::sideEffect, 0);
    compare(b, chunks);
    b.advanceCpuCycles(19, 0, true); chunks.advanceCpuCycles(19, 0, true);
    compare(b, chunks);
}
void latentPeripheralPhase() {
    Imx1Bootstrap b; configure(b);
    write(b, 0x202000, 0); write(b, 0x203000, 0);
    write(b, 0x202004, 0); write(b, 0x203004, 0);
    // Disabled consumers do not stop the independent PERCLK1 divider.
    b.advanceCpuCycles(1, 0, true);
    auto unchanged = b;
    write(b, cgm, 0x57003c03, A::ok, 0);
    for (auto* state : {&b, &unchanged}) {
        write(*state, 0x202000, 0x103); write(*state, 0x203000, 0x103);
        write(*state, 0x208008, 29); write(*state, 0x208004, 7);
        write(*state, 0x208000, 0x10);
    }
    for (unsigned i = 0; i < 200; ++i) {
        b.advanceCpuCycles(1, 0, true); unchanged.advanceCpuCycles(16, 0, true);
        compare(b, unchanged, false);
        assert(read(b, 0x20800c) == read(unchanged, 0x20800c));
    }
    assert(read(b, 0x202010) > 0);
}
void guards() {
    Imx1Bootstrap b; configure(b);
    auto untouched = b;
    write(b, cgm + 32, 0x000f0012, A::sideEffect);
    compare(b, untouched);
    // Active DMA and a partly elapsed beat still block HCLK retiming.
    write(b, ssi + 8, 0); write(b, ssi + 8, 0x9b00);
    write(b, dma, 1); write(b, dma + 8, 0x3ff);
    write(b, 0x300000, 0x12345678);
    write(b, ch, 0x300000); write(b, ch + 4, ssi); write(b, ch + 8, 4);
    write(b, ch + 12, 0x2088); write(b, ch + 16, 16); write(b, ch + 20, 8);
    write(b, ch + 12, 0x2089);
    // Stop within the first two-HCLK beat with an empty FIFO.
    b.advanceCpuCycles(1, 3, true);
    assert(b.pcmState().dmaActive && b.pcmDmaRemaining() == 4);
    untouched = b;
    write(b, cgm, 0x5700ac03, A::sideEffect);
    write(b, cgm, 0x57003c03, A::sideEffect, 0);
    write(b, cgm + 32, 0x000f0012, A::sideEffect);
    compare(b, untouched);
    {
        // The ROM's sound power-down (2009f9a8..20084608): SSI1 off with channel 10 left set up,
        // then PERCLK3 and HCLK slowed. Neither clock has a user left.
        auto off = b;
        write(off, ssi + 8, 0x9900); write(off, dma + 8, 0x7ff); write(off, ssi + 0xc, 0xe8);
        write(off, cgm + 32, 0x007f0012, A::sideEffect);
        write(off, cgm, 0x57003c03, A::sideEffect, 0);
        write(off, ssi + 8, 0x9800); write(off, ssi + 0x28, 0x40);
        assert(off.pcmState().dmaActive);
        write(off, cgm + 32, 0x007f0012);
        write(off, cgm, 0x57003c03, A::ok, 0);
        assert(off.pcmState().dmaActive && off.pcmDmaRemaining() == b.pcmDmaRemaining());
        off.advanceCpuCycles(64, 0, true);
        assert(off.pcmDmaRemaining() == b.pcmDmaRemaining());
    }
    for (unsigned i = 0; i < 3000; ++i) {
        b.advanceCpuCycles(1, 3, true); untouched.advanceCpuCycles(1, 3, true); compare(b, untouched);
    }
    assert(!b.pcmState().dmaActive);
    write(b, cgm, 0x5700ac03);
    // A candidate that makes FCLK <= BCLK remains unsafe before any mutation.
    Imx1Bootstrap equal;
    write(equal, cgm, 0x57000403, A::ok, 0);
    equal.advanceCpuCycles(1, 3, false);
    auto copy = equal;
    write(equal, cgm, 0x57008003, A::sideEffect);
    assert(read(equal, cgm) == read(copy, cgm));
}
struct Trace { std::vector<Imx1ProbeAccess> bus; };
void observe(void* p, const Imx1ProbeAccess& e) { static_cast<Trace*>(p)->bus.push_back(e); }
void wholePair() {
    std::vector<uint32_t> words;
    auto literal = [&](unsigned reg, uint32_t value) {
        words.push_back(0xe59f0000 | reg << 12); words.push_back(0xea000000); words.push_back(value);
    };
    auto store = [&](uint32_t address, uint32_t value) {
        literal(0, address); literal(1, value); words.push_back(0xe5801000);
    };
    literal(13, 0x31fff0);
    store(cgm, 0x57000003); store(cgm + 4, 0x03e717bf); store(cgm, 0x57200003);
    literal(0, cgm);
    words.push_back(0xe5902000); words.push_back(0xe3120602); words.push_back(0x1afffffc);
    literal(1, 0xc0000078); words.push_back(0xee011f10);
    store(cgm + 12, 0x123638ad); store(cgm, 0x57400003);
    // The next architectural access stalls until System PLL output resumes.
    store(ssi + 8, 0x9900); store(ssi + 0x14, 0x6100);
    store(ssi + 0x20, 4); store(ssi + 0xc, 0x2e8);
    store(ssi, 0x1234); store(ssi, 0x5678); store(ssi + 8, 0x9b00);
    store(0x202004, 31); store(0x202000, 0x103);
    const auto callDivide = words.size(); words.push_back(0);
    literal(0, cgm); words.push_back(0xe5904000);
    // Read-only timer polling supplies real elapsed peripheral time between the pair.
    literal(0, 0x202010);
    words.push_back(0xe5906000); words.push_back(0xe3560005); words.push_back(0x3afffffc);
    const auto callRestore = words.size(); words.push_back(0);
    literal(0, cgm); words.push_back(0xe5905000);
    literal(1, 0x78); words.push_back(0xee011f10);
    literal(0, 6);
    const auto callSlow = words.size(); words.push_back(0);
    literal(0, cgm); words.push_back(0xe5908000);
    literal(1, 0xc0000078); words.push_back(0xee011f10);
    literal(0, 96);
    const auto callNormal = words.size(); words.push_back(0);
    literal(0, cgm); words.push_back(0xe590a000); words.push_back(0xee117f10);
    words.push_back(0xe7f000f0);
    const auto divide = words.size();
    literal(1, cgm); words.push_back(0xe5910000); words.push_back(0xe3c00b0f);
    words.push_back(0xe3800b2b); words.push_back(0xe5810000); words.push_back(0xe12fff1e);
    const auto restore = words.size();
    literal(0, cgm); words.push_back(0xe5901000); words.push_back(0xe3c11b2f);
    words.push_back(0xe5801000); words.push_back(0xe12fff1e);
    // Native rate clamp/divide/two-write helper, with fixture-relative BL and literal.
    const auto later = words.size();
    const uint32_t laterWords[] = {
        0xe3500006, 0x33a00006, 0xe92d4008, 0x3a000001, 0xe3500060, 0x83a00060,
        0xe3a01060, 0xeb010cb1, 0xe2401001, 0xe51f02e4, 0xe5902000, 0xe3c22df0,
        0xe5802000, 0xe5902000, 0xe1821501, 0xe5801000, 0xe8bd4008, 0xe12fff1e,
    };
    words.insert(words.end(), std::begin(laterWords), std::end(laterWords));
    const auto cgmLiteral = words.size(); words.push_back(cgm);
    const auto divide96 = words.size();
    const uint32_t divideWords[] = {
        0xe3a02000, 0xe070c1a1, 0x3a000020, 0xe070c421, 0x3a00000f, 0xe1a00400,
        0xe38224ff, 0xe070c221, 0x3a000017, 0xe070c421, 0x3a000009, 0xe1a00400,
        0xe38228ff, 0xe070c421, 0x21a00400, 0x23822cff, 0xe070c221, 0x3a00000e,
        0xe270c000, 0x2a0002d1, 0x21a00420, 0xe070c3a1, 0x20411380, 0xe0a22002,
        0xe070c321, 0x20411300, 0xe0a22002, 0xe070c2a1, 0x20411280, 0xe0a22002,
        0xe070c221, 0x20411200, 0xe0a22002, 0xe070c1a1, 0x20411180, 0xe0a22002,
        0xe070c121, 0x20411100, 0xe0a22002, 0xe070c0a1, 0x20411080, 0xe0a22002,
        0xe070c001, 0x20411000, 0xe0b22002, 0x2affffe5, 0xe1a00002, 0xe12fff1e,
    };
    words.insert(words.end(), std::begin(divideWords), std::end(divideWords));
    const auto zeroDivisor = words.size(); words.push_back(0xe7f000f0);
    words[later + 7] = 0xeb000000 | uint32_t(divide96 - later - 7 - 2);
    words[later + 9] = 0xe59f0000 | uint32_t((cgmLiteral - later - 9 - 2) * 4);
    words[divide96 + 18] = 0x2a000000 | uint32_t(zeroDivisor - divide96 - 18 - 2);
    words[callSlow] = 0xeb000000 | uint32_t(later - callSlow - 2);
    words[callNormal] = 0xeb000000 | uint32_t(later - callNormal - 2);
    words[callDivide] = 0xeb000000 | uint32_t(divide - callDivide - 2);
    words[callRestore] = 0xeb000000 | uint32_t(restore - callRestore - 2);
    std::vector<uint8_t> rom(words.size() * 4); std::memcpy(rom.data(), words.data(), rom.size());
    Imx1ProbeOptions options{}; options.bootstrapPeripherals = true;
    options.assumeMpllSettled = options.assumeSpllSettled = true;
    Imx1ProbeResult results[2]; Trace traces[2];
    constexpr unsigned engines = FROND_TEST_FAST ? 2 : 1;
    for (unsigned fast = 0; fast < engines; ++fast) {
        options.busObserver = observe; options.busContext = &traces[fast];
        results[fast] = imx1BootProbe(rom.data(), rom.size(), Imx1ProbeEntry::physicalFlash,
                                    100000, nullptr, nullptr, fast, &options);
        assert(results[fast].registers[4] == 0x5700ac03 && results[fast].registers[5] == 0x57000003);
        assert(results[fast].registers[8] == 0x57003c03 && results[fast].registers[10] == 0x57000003);
        assert(results[fast].registers[6] >= 5 && (results[fast].registers[7] & 0xc0000000) == 0xc0000000);
        assert(results[fast].stop == Imx1ProbeStop::unsupportedInstruction);
    }
#if FROND_TEST_FAST
    assert(results[0].stop == results[1].stop && results[0].cpsr == results[1].cpsr);
    assert(results[0].busAccesses == results[1].busAccesses && results[1].jitInstructions);
    assert(!std::memcmp(results[0].registers, results[1].registers, sizeof(results[0].registers)));
    assert(traces[0].bus.size() == traces[1].bus.size());
    for (size_t i = 0; i < traces[0].bus.size(); ++i) {
        const auto& a = traces[0].bus[i]; const auto& b = traces[1].bus[i];
        assert(a.step == b.step && a.pc == b.pc && a.address == b.address && a.value == b.value &&
               a.size == b.size && a.write == b.write && a.status == b.status);
    }
#endif
}

// The ASP converts on PERCLK2 (manual table 12-6). With the ROM's clocks, System PLL 96 MHz, HCLK /1,
// PERCLK2 /2 and ASP_CLKDIV 3, a 6-count slot is 1260 * 6 * 4 PERCLK2 cycles, 60480 System PLL clocks.
// The idle code's HCLK /16 write (20084608) lands mid-conversion and doesn't retime it.
void aspOnPerclk2() {
    constexpr uint32_t asp = 0x215000;
    Imx1Bootstrap b(true, true, 0x8000); configure(b);
    write(b, asp + 0x2c, 3); write(b, asp + 0x14, 0x1814); write(b, asp + 0x10, 0x02003001);
    write(b, 0x223008, 33);
    write(b, asp + 0x10, 0x02003003);
    // Past the 30240 clocks an HCLK-timed slot would take.
    b.advanceHclkCycles(30300);
    assert(read(b, asp + 0x1c) == 0);
    // A PERCLK2 divider change would retime the conversion and stays refused.
    write(b, cgm + 32, 0x00100032, A::sideEffect, 0);
    write(b, cgm, 0x57003c03, A::ok, 0);
    assert(read(b, cgm) == 0x57003c03);
    // 30180 System PLL clocks are left: 1886.25 HCLK periods at /16.
    b.advanceHclkCycles(1885);
    assert(read(b, asp + 0x1c) == 0);
    b.advanceHclkCycles(2);
    assert(read(b, asp + 0x1c) == 1 && b.clockStatus() == A::ok);
    write(b, cgm + 32, 0x00100032, A::ok, 0);
}

}
int main() {
    serialDomains(); fastBusDomains(); latentPeripheralPhase(); guards(); wholePair(); aspOnPerclk2();
    std::puts("PASS: independent SSI/PERCLK/BCLK domains, FastBus live dividers, retained fractions, DMA guards, equal-time state and the ASP on PERCLK2");
}
