// Faster speed on the Zodiac: in clock mode 3 the CPU's FCLK runs the multiplier times the MCU PLL's, so the
// guest retires that many times the instructions per guest second, with HCLK, the timers and bus waits as at
// Original. FastBus and a pending System PLL restart keep the PLL's own rate, and 1 / 1 changes nothing.
#include <algorithm>
#include <utility>
#include "zodiac-machine-parity.h"
#define main originalIrqMain
#include "zodiac-irq.cpp"
#undef main

namespace {
using T = Imx1ReferenceTime;
constexpr uint32_t cgmBase = 0x0021b000, timerCount = 0x00202010;
constexpr std::pair<uint32_t, uint32_t> multipliers[] = {{3, 2}, {2, 1}, {3, 1}, {4, 1}, {8, 1}};

void boardWrite(Imx1Bootstrap& b, uint32_t address, uint32_t value, uint32_t mode = 0) {
    assert(b.access(address, 4, true, value, mode) == Imx1BootstrapAccess::ok);
}
uint32_t boardRead(Imx1Bootstrap& b, uint32_t address) {
    uint32_t value = 0;
    assert(b.access(address, 4, false, value) == Imx1BootstrapAccess::ok);
    return value;
}
// The MCU PLL at its nominal ratio, the CPU able to go asynchronous, timer 1 counting PERCLK1 / 32.
Imx1Bootstrap configured() {
    Imx1Bootstrap b;
    b.setSystemPllScenario(true);
    boardWrite(b, cgmBase, 0x57000003);
    boardWrite(b, cgmBase + 4, 0x03e717bf);
    boardWrite(b, cgmBase, 0x57200003);
    b.advanceCpuCycles(6000, 0, true);
    b.advanceCpuCycles(56, 3, true);
    boardWrite(b, 0x00202004, 31, 3);
    boardWrite(b, 0x00202000, 0x103, 3);
    b.advanceCpuCycles(3, 3, true);
    return b;
}
bool samePeriod(Imx1PllRatio a, Imx1PllRatio b) {
    return __uint128_t(a.numerator) * b.denominator == __uint128_t(b.numerator) * a.denominator;
}

void board() {
    const auto base = configured();
    assert(base.cpuClockModeSupported(3, true));
    for (const auto& m : multipliers) {
        auto a = base, f = base;
        f.setCpuClockMultiplier(m.first, m.second);
        assert(f.cpuClockModeSupported(3, true));
        // FCLK's period over the multiplier; FastBus and HCLK as they were.
        const auto pa = a.cpuReferencePeriod(3), pf = f.cpuReferencePeriod(3);
        assert(__uint128_t(pf.numerator) * pa.denominator * m.first == __uint128_t(pa.numerator) * pf.denominator * m.second);
        assert(samePeriod(a.cpuReferencePeriod(0), f.cpuReferencePeriod(0)));
        assert(samePeriod(a.busReferencePeriod(), f.busReferencePeriod()));
        // N times the cycles take the same guest time and give the same timer counts, charged whole or in pieces.
        for (unsigned n = 0; n < 40; ++n) {
            const uint32_t cycles = 997 + n * 131;
            uint32_t predictedA = 0, predictedF = 0;
            assert(a.timerCountAfter(timerCount, 4, uint64_t(cycles) * m.second, 3, predictedA));
            assert(f.timerCountAfter(timerCount, 4, uint64_t(cycles) * m.first, 3, predictedF));
            assert(predictedA == predictedF);
            a.advanceCpuCycles(cycles * m.second, 3, true);
            if (n % 2) f.advanceCpuCycles(cycles * m.first, 3, true);
            else for (uint32_t left = cycles * m.first; left;) {
                const uint32_t piece = std::min(left, 333u);
                f.advanceCpuCycles(piece, 3, true);
                left -= piece;
            }
            assert(a.referenceTime().compare(f.referenceTime()) == 0);
            assert(boardRead(a, timerCount) == predictedA && boardRead(f, timerCount) == predictedF);
        }
        // Bus waits (HCLK) and FastBus cycles take the same time at any multiplier.
        a.advanceHclkCycles(5000); f.advanceHclkCycles(5000);
        assert(a.referenceTime().compare(f.referenceTime()) == 0);
        a.advanceCpuCycles(777, 0, true); f.advanceCpuCycles(777, 0, true);
        assert(a.referenceTime().compare(f.referenceTime()) == 0 && boardRead(a, timerCount) == boardRead(f, timerCount));
        // A pending System PLL restart is charged at the MCU PLL's own rate, then the multiplier is back.
        boardWrite(a, cgmBase + 12, 0x123638ad, 3); boardWrite(a, cgmBase, 0x57400003, 3);
        boardWrite(f, cgmBase + 12, 0x123638ad, 3); boardWrite(f, cgmBase, 0x57400003, 3);
        assert(samePeriod(a.cpuReferencePeriod(3), f.cpuReferencePeriod(3)));
        a.advanceCpuCycles(56, 3, true); f.advanceCpuCycles(56, 3, true);
        assert(a.referenceTime().compare(f.referenceTime()) == 0);
        const auto stallA = a.finishSystemPllBusStall(), stallF = f.finishSystemPllBusStall();
        assert(stallA && stallA == stallF && a.referenceTime().compare(f.referenceTime()) == 0);
        a.advanceCpuCycles(1000 * m.second, 3, true); f.advanceCpuCycles(1000 * m.first, 3, true);
        assert(a.referenceTime().compare(f.referenceTime()) == 0);
        // Back to 1 / 1: the same rate as a board that never had one.
        f.setCpuClockMultiplier(2, 2);
        assert(f.cpuClockMultiplierNum() == 1 && f.cpuClockMultiplierDen() == 1);
        assert(samePeriod(a.cpuReferencePeriod(3), f.cpuReferencePeriod(3)));
        a.advanceCpuCycles(4321, 3, true); f.advanceCpuCycles(4321, 3, true);
        assert(a.referenceTime().compare(f.referenceTime()) == 0 && boardRead(a, timerCount) == boardRead(f, timerCount));
    }
}

// The PLL program from zodiac-bounded's pll case up to the switch to asynchronous mode, then a loop of
// 1000 nops and a branch back for ever.
std::vector<uint32_t> loopProgram(uint32_t& loopAddress) {
    std::vector<uint32_t> words(0x100 / 4, 0xe1a00000); words[0] = 0xea00003e; words[6] = 0xe7f000f0;
    load(words, 0, 0x00223000); store(words, 0, 8, 18); load(words, 0, 0x00204000); store(words, 0, 0x18, 0x8000);
    for (unsigned n = 0; n < 15200; ++n) words.push_back(0xe1a00000);
    load(words, 0, 0x0021b000); store(words, 0, 0, 0x57000003); store(words, 0, 4, 0x03e717bf); store(words, 0, 0, 0x57200003);
    for (unsigned n = 0; n < 6000; ++n) words.push_back(0xe1a00000);
    load(words, 1, 0xc0000078); words.push_back(0xee011f10);
    loopAddress = uint32_t(words.size() * 4);
    for (unsigned n = 0; n < 1000; ++n) words.push_back(0xe1a00000);
    const int32_t back = (int32_t(loopAddress) - int32_t(words.size() * 4) - 8) / 4;
    words.push_back(0xea000000 | (uint32_t(back) & 0xffffff));
    return words;
}

struct Window { uint64_t retired; T elapsed; };

// Runs the program into its loop, sets the multiplier (none with num 0), then runs ticks of guest time.
Window window(bool fast, uint32_t num, uint32_t den, uint64_t ticks) {
    uint32_t loopAddress = 0;
    const auto words = loopProgram(loopAddress);
    Imx1ProbeOptions o; o.bootstrapPeripherals = true; o.rtcInitialTimeKnown = true;
    o.assumeMpllSettled = o.assumeSpllSettled = true;
    auto* m = imx1MachineCreate(reinterpret_cast<const uint8_t*>(words.data()), uint32_t(words.size() * 4),
                                Imx1ProbeEntry::resetAlias, fast, o);
    assert(m);
    for (unsigned n = 0;; ++n) {
        assert(n < 1000);
        const auto run = imx1MachineRunBounded(m, {50000, {UINT64_MAX, 0, 1}, 10000000});
        assert(run.reason == Imx1MachineYield::retired || run.reason == Imx1MachineYield::work);
        if (run.total.retired > 21300 && run.total.pc >= loopAddress && run.total.pc <= loopAddress + 4000) break;
    }
    if (num) assert(imx1MachineSetCpuClockMultiplier(m, num, den));
    const auto start = imx1MachineGetStatus(m);
    const T deadline{start.time.ticks + ticks, 0, 1};
    for (;;) {
        const auto run = imx1MachineRunBounded(m, {1000000, deadline, UINT64_MAX / 4});
        assert(run.reason == Imx1MachineYield::retired || run.reason == Imx1MachineYield::deadline ||
               run.reason == Imx1MachineYield::work);
        if (run.reason == Imx1MachineYield::deadline) break;
    }
    const auto end = imx1MachineGetStatus(m);
    assert(end.pc >= loopAddress && end.pc <= loopAddress + 4000);
    imx1MachineStop(m);
    return {end.retired - start.retired, end.time.since(start.time)};
}

double seconds(const T& t) {
    return (double(t.ticks) + double(t.numerator) / double(t.denominator)) / 32768.0;
}

void machine(bool fast) {
    constexpr uint64_t ticks = 1024;  // 31.25 ms
    const auto base = window(fast, 0, 0, ticks), original = window(fast, 1, 1, ticks);
    assert(base.retired == original.retired && base.elapsed.compare(original.elapsed) == 0);
    const double baseRate = base.retired / seconds(base.elapsed);
    std::printf("%s original: %llu instructions in %.6f s, %.0f a second\n", fast ? "Fast" : "Exact",
                (unsigned long long)base.retired, seconds(base.elapsed), baseRate);
    for (const auto& m : multipliers) {
        const auto w = window(fast, m.first, m.second, ticks);
        const double ratio = w.retired / seconds(w.elapsed) / baseRate, want = double(m.first) / m.second;
        std::printf("  %u/%u: %llu instructions in %.6f s, %.4fx\n", m.first, m.second,
                    (unsigned long long)w.retired, seconds(w.elapsed), ratio);
        // Guest time is the window asked for (the deadline is a whole tick, the start isn't), to a slice's overshoot.
        assert(seconds(w.elapsed) >= (ticks - 1) / 32768.0 && seconds(w.elapsed) < (ticks + 2) / 32768.0);
        assert(ratio > want * 0.995 && ratio < want * 1.005);
    }
}
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT", "1", 1);
    board();
    machine(false);
    machine(true);
    std::puts("PASS: Zodiac Faster runs FCLK times the multiplier, guest time, HCLK, timers and FastBus as at Original");
}
