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
    write(b, cgm + 32, 0x000f0012, A::sideEffect);
    compare(b, untouched);
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
    literal(1, 0xc0000078); words.push_back(0xee011f10); words.push_back(0xee117f10);
    words.push_back(0xe7f000f0);
    const auto divide = words.size();
    literal(1, cgm); words.push_back(0xe5910000); words.push_back(0xe3c00b0f);
    words.push_back(0xe3800b2b); words.push_back(0xe5810000); words.push_back(0xe12fff1e);
    const auto restore = words.size();
    literal(0, cgm); words.push_back(0xe5901000); words.push_back(0xe3c11b2f);
    words.push_back(0xe5801000); words.push_back(0xe12fff1e);
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

}
int main() {
    serialDomains(); guards(); wholePair();
    std::puts("PASS: independent SSI/BCLK domains, buffered words, underrun, continuous phase, DMA guards and equal-time state");
}
