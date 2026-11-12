#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "imx1_bootstrap.h"

namespace {
constexpr uint32_t base = 0x00208000, cscr = 0x0021b000, pcdr = cscr + 32;
uint32_t read(Imx1Pwm& pwm, uint32_t offset) {
    uint32_t value = 0xdeadbeef;
    assert(pwm.access(base + offset, 4, false, value) == Imx1PwmAccess::ok);
    return value;
}
void write(Imx1Pwm& pwm, uint32_t offset, uint32_t value,
           Imx1PwmAccess expected = Imx1PwmAccess::ok) {
    const auto argument = value;
    assert(pwm.access(base + offset, 4, true, value) == expected && value == argument);
}
uint32_t read(Imx1Bootstrap& board, uint32_t address) {
    uint32_t value = 0xdeadbeef;
    assert(board.access(address, 4, false, value) == Imx1BootstrapAccess::ok);
    return value;
}
void write(Imx1Bootstrap& board, uint32_t address, uint32_t value,
           Imx1BootstrapAccess expected = Imx1BootstrapAccess::ok, uint32_t mode = 0) {
    assert(board.access(address, 4, true, value, mode) == expected);
}
void resetAndRegisters() {
    Imx1Pwm pwm;
    assert(read(pwm, 0) == 0x20 && read(pwm, 8) == 0xfffe && read(pwm, 12) == 0);
    for (auto offset : {0u, 4u, 8u, 12u}) {
        assert(Imx1Pwm::registerName(base + offset));
        for (auto size : {0u, 1u, 2u, 8u}) {
            uint32_t value = 0xdeadbeef;
            assert(pwm.access(base + offset, size, false, value) == Imx1PwmAccess::width && value == 0xdeadbeef);
            assert(pwm.access(base + offset, size, true, value) == Imx1PwmAccess::width && value == 0xdeadbeef);
        }
        for (unsigned byte = 1; byte < 4; ++byte) {
            uint32_t value = 0xdeadbeef;
            assert(pwm.access(base + offset + byte, 4, false, value) == Imx1PwmAccess::width && value == 0xdeadbeef);
            assert(pwm.access(base + offset + byte, 4, true, value) == Imx1PwmAccess::width && value == 0xdeadbeef);
        }
        write(pwm, offset, 0xfff80000); // Read-only reserved fields, disabled writes ignored.
    }
    uint32_t value = 0xdeadbeef;
    assert(!Imx1Pwm::registerName(base + 16));
    assert(pwm.access(base + 16, 4, true, value) == Imx1PwmAccess::unmapped && value == 0xdeadbeef);
    assert(pwm.access(base + 4, 4, false, value) == Imx1PwmAccess::externalInput && value == 0xdeadbeef);
    write(pwm, 4, 0x12345678); write(pwm, 8, 7); write(pwm, 12, 42);
    assert(read(pwm, 8) == 0xfffe && read(pwm, 12) == 0);
    assert(pwm.access(base + 4, 4, false, value) == Imx1PwmAccess::externalInput && value == 0xdeadbeef);
    // No disable-time configuration can make a FIFO/period event.
    write(pwm, 0, 0x6ffcf);
    assert(read(pwm, 0) == 0x20);
    write(pwm, 0, 0x10);
    assert(read(pwm, 0) == 0xb0 && pwm.output() == -1);
    write(pwm, 0, 0);
    assert(read(pwm, 0) == 0x20 && !pwm.resetting());
    write(pwm, 0, 0xffffffff); // SWR takes priority over all control fields, including EN.
    assert(pwm.resetting() && read(pwm, 0) == 0x10020);
    for (unsigned i = 0; i < 100; ++i) assert(read(pwm, 0) == 0x10020);
    write(pwm, 0, 0x10010, Imx1PwmAccess::sideEffect);
    write(pwm, 0, 0); // A zero cannot cancel or shorten an in-progress reset.
    write(pwm, 0, 0x10, Imx1PwmAccess::sideEffect);
    write(pwm, 4, 0); write(pwm, 8, 0);
    pwm.advance(4);
    assert(read(pwm, 0) == 0x10020 && read(pwm, 8) == 0xfffe && read(pwm, 12) == 0);
    pwm.advance(1);
    assert(read(pwm, 0) == 0x20 && !pwm.resetting());
    write(pwm, 0, 0x10010);
    pwm.advance(5);
    assert(read(pwm, 0) == 0x20);
    Imx1Pwm chunks, whole;
    write(chunks, 0, 0x10010); write(whole, 0, 0x10010);
    for (unsigned i = 0; i < 4; ++i) chunks.advance(1);
    whole.advance(4);
    assert(read(chunks, 0) == read(whole, 0) && read(whole, 0) == 0x10020);
    chunks.advance(1); whole.advance(1);
    assert(read(chunks, 0) == read(whole, 0) && read(whole, 0) == 0x20);
    write(whole, 0, 0x10010); whole.advance(UINT64_MAX);
    assert(read(whole, 0) == 0x20);
}
void bootstrapTimingAndIrq() {
    Imx1Bootstrap board;
    write(board, 0x00223008, 34); // PWM_INT is source 34, table 10-1.
    assert(read(board, 0x00223058) == 0 && board.clockStatus() == Imx1BootstrapAccess::ok);
    write(board, base, 0x10010);
    for (unsigned i = 0; i < 100; ++i) {
        assert(read(board, base) == 0x10020);
        assert(board.clockStatus() == Imx1BootstrapAccess::ok);
        uint32_t value = 0xdeadbeef;
        assert(!board.instructionRead(base, 4, value));
    }
    write(board, cscr, 0x0f00a003, Imx1BootstrapAccess::sideEffect); // Cannot change HCLK / 12 during reset.
    assert(read(board, cscr) == 0x0f00ac03);
    write(board, pcdr, 0); // PERCLK1 speed does not change the five HCLK requirement.
    board.advanceFastBusCycles(4);
    assert(read(board, base) == 0x10020);
    board.advanceFastBusCycles(1);
    assert(read(board, base) == 0x20);
    write(board, cscr, 0x0f00a003); // HCLK / 9 after reset.
    write(board, pcdr, 0xff);
    write(board, base, 0x10010);
    board.advanceFastBusCycles(4);
    assert(read(board, base) == 0x10020);
    board.advanceFastBusCycles(1);
    assert(read(board, base) == 0x20 && read(board, base + 12) == 0);
    write(board, 0x00223050, 4, Imx1BootstrapAccess::sideEffect); // Software force still cannot deliver to CPU.
    assert(read(board, 0x00223050) == 0 && read(board, 0x00223058) == 0);

    // Asynchronous CPU cycles are converted through System PLL and HCLK / 2.
    Imx1Bootstrap whole;
    write(whole, cscr, 0x57000403);
    write(whole, cscr + 4, 0x03e717bf);
    write(whole, cscr, 0x57200403);
    whole.advanceFastBusCycles(3000);
    assert(whole.cpuClockModeSupported(3, true));
    write(whole, pcdr, 0xff);
    write(whole, base, 0x10010);
    auto chunks = whole;
    // floor(20 * 375000 / 762752 / 2) = 4 HCLK cycles, independent of PERCLK1 / 16.
    whole.advanceCpuCycles(20, 3, true);
    for (unsigned i = 0; i < 20; ++i) chunks.advanceCpuCycles(1, 3, true);
    assert(read(whole, base) == 0x10020 && read(chunks, base) == 0x10020);
    whole.advanceCpuCycles(1, 3, true); chunks.advanceCpuCycles(1, 3, true);
    assert(read(whole, base) == 0x20 && read(chunks, base) == 0x20);
    assert(whole.clockStatus() == Imx1BootstrapAccess::ok);

    Imx1Bootstrap stalled;
    stalled.setSystemPllScenario(true);
    write(stalled, cscr, 0x57000403);
    write(stalled, cscr + 4, 0x03e717bf);
    write(stalled, cscr, 0x57200403);
    stalled.advanceFastBusCycles(3000);
    write(stalled, base, 0x10010);
    write(stalled, cscr, 0x57400403, Imx1BootstrapAccess::ok, 3);
    stalled.advanceCpuCycles(100, 3, true); // Independent FCLK progresses while System PLL output is held high.
    assert(read(stalled, base) == 0x10020);
    assert(stalled.finishSystemPllBusStall() > 0 && read(stalled, base) == 0x10020);
    stalled.advanceCpuCycles(20, 3, true);
    assert(read(stalled, base) == 0x10020);
    stalled.advanceCpuCycles(1, 3, true);
    assert(read(stalled, base) == 0x20);
}
void fifoCounterAndOutput() {
    Imx1Pwm pwm;
    write(pwm, 0, 0x50);
    assert(pwm.interruptOutput() && read(pwm, 0) == 0xf0 && !pwm.interruptOutput());
    uint32_t unknown = 0xdeadbeef;
    assert(pwm.access(base + 4, 4, false, unknown) == Imx1PwmAccess::externalInput);
    assert(unknown == 0xdeadbeef && pwm.output() == -1);
    write(pwm, 8, 2); // Four PCLK ticks, with two PERCLK1 edges per tick.
    for (auto sample : {1u, 2u, 0u, 5u}) write(pwm, 4, sample);
    assert(read(pwm, 0) == 0x50 && read(pwm, 4) == 5);
    write(pwm, 4, 0xdead); // Full FIFO writes cannot change the latch or queue.
    assert(read(pwm, 4) == 5);
    pwm.advancePerclk1(7);
    assert(read(pwm, 12) == 3 && pwm.output() == -1);
    pwm.advancePerclk1(1);
    assert(read(pwm, 12) == 0 && read(pwm, 0) == 0x70 && pwm.output() == 1);
    pwm.advancePerclk1(2);
    assert(read(pwm, 12) == 1 && pwm.output() == 0);
    pwm.advancePerclk1(6); // Second sample, two words still queued.
    assert(read(pwm, 0) == 0x70 && pwm.output() == 1);
    pwm.advancePerclk1(4);
    assert(pwm.output() == 0);
    pwm.advancePerclk1(4); // Third sample is zero; one queued word posts IRQ.
    assert(pwm.interruptOutput() && pwm.output() == 0 && read(pwm, 0) == 0xf0);
    assert(!pwm.interruptOutput() && read(pwm, 0) == 0x70);
    pwm.advancePerclk1(8); // Above PERIOD+1 gives 100 percent duty.
    assert(pwm.output() == 1 && pwm.interruptOutput());
    pwm.advancePerclk1(UINT64_MAX);
    assert(pwm.output() == 1 && read(pwm, 12) == 3);
    write(pwm, 8, 0, Imx1PwmAccess::sideEffect); // Shortening past current count is transactional.
    assert(read(pwm, 8) == 2 && read(pwm, 12) == 3);
    write(pwm, 12, 0xffffffff);
    assert(read(pwm, 12) == 3);
    write(pwm, 0, 0xd0); // Debug IRQ set.
    assert(pwm.interruptOutput());
    assert(read(pwm, 0) == 0xf0 && !pwm.interruptOutput());
    for (auto control : {0x8050u, 0x150u, 0x51u, 0x54u})
        write(pwm, 0, control, Imx1PwmAccess::sideEffect);
    assert(read(pwm, 0) == 0x70 && read(pwm, 12) == 3);
    write(pwm, 0, 0);
    assert(pwm.output() == 0 && read(pwm, 12) == 0 && !pwm.interruptOutput());
    assert(read(pwm, 0) == 0x60 && read(pwm, 4) == 5);
    write(pwm, 8, 99); write(pwm, 4, 99);
    assert(read(pwm, 8) == 2 && read(pwm, 4) == 5);
    write(pwm, 0, 0x10010);
    assert(read(pwm, 8) == 0xfffe && pwm.output() == 0);
    assert(pwm.access(base + 4, 4, false, unknown) == Imx1PwmAccess::externalInput);
}
void repeatsSwapsAndBounds() {
    struct ClockCase { uint32_t bits, divisor; };
    for (const auto setting : {ClockCase{0, 2}, ClockCase{1, 4}, ClockCase{2, 8},
                               ClockCase{3, 16}, ClockCase{0x100, 4},
                               ClockCase{0x702, 64}, ClockCase{0x7f03, 2048}}) {
        for (auto source : {0u, 0x8000u}) {
            Imx1Pwm pwm;
            write(pwm, 0, 0x10 | setting.bits | source);
            if (source) pwm.advanceReference(setting.divisor - 1, 1);
            else pwm.advancePerclk1(setting.divisor - 1);
            assert(read(pwm, 12) == 0);
            if (source) pwm.advanceReference(1, 1);
            else pwm.advancePerclk1(1);
            assert(read(pwm, 12) == 1);
            if (source) pwm.advanceReference(setting.divisor * 9, 1);
            else pwm.advancePerclk1(setting.divisor * 9);
            assert(read(pwm, 12) == 10);
        }
    }
    for (unsigned repeat = 0; repeat < 4; ++repeat) {
        Imx1Pwm pwm;
        write(pwm, 0, 0x10 | repeat << 2);
        assert(read(pwm, 0) == 0xb0); // REPEAT is write-only.
        write(pwm, 8, 2);
        write(pwm, 4, 1); write(pwm, 4, 0);
        pwm.advancePerclk1(8);
        for (unsigned i = 0; i < (1u << repeat); ++i) {
            assert(pwm.output() == 1);
            pwm.advancePerclk1(8);
        }
        assert(pwm.output() == 0);
    }
    for (unsigned swap = 0; swap < 4; ++swap) {
        Imx1Pwm pwm;
        write(pwm, 0, 0x10 | swap << 17);
        write(pwm, 4, 0x12345678);
        assert(read(pwm, 4) == ((swap & 2) ? 0x1234u : 0x5678u));
        pwm.advancePerclk1(131072);
        const uint32_t expected[] = {0x5678, 0x7856, 0x1234, 0x3412};
        pwm.advancePerclk1(2 * (expected[swap] - 1));
        assert(pwm.output() == 1);
        pwm.advancePerclk1(2);
        assert(pwm.output() == 0);
    }
    for (auto period : {0xfffeu, 0xffffu}) {
        Imx1Pwm pwm;
        write(pwm, 0, 0x10); write(pwm, 8, period);
        pwm.advancePerclk1(131071);
        assert(read(pwm, 12) == 65535 && read(pwm, 8) == period);
        pwm.advancePerclk1(1);
        assert(read(pwm, 12) == 0);
    }
    Imx1Pwm whole;
    write(whole, 0, 0xff1f); // CLK32 / 16 / 128, eight plays.
    write(whole, 8, 3);
    for (unsigned i = 1; i <= 4; ++i) write(whole, 4, i);
    auto chunks = whole;
    whole.advanceReference(UINT64_MAX, 1);
    chunks.advanceReference(UINT64_MAX / 2, 1);
    chunks.advanceReference(UINT64_MAX - UINT64_MAX / 2, 1);
    assert(read(whole, 0) == read(chunks, 0) && read(whole, 12) == read(chunks, 12));
    assert(whole.output() == chunks.output());
    Imx1Pwm rational;
    write(rational, 0, 0x8010);
    rational.advancePerclk1(UINT64_MAX);
    assert(read(rational, 12) == 0);
    rational.advanceReference(5, 3); // 1 2/3 source edges.
    assert(read(rational, 12) == 0);
    rational.advanceReference(1, 6);
    assert(read(rational, 12) == 0);
    rational.advanceReference(1, 6);
    assert(read(rational, 12) == 1);
    rational.advanceReference(1, 3);
    assert(!rational.referenceRatioSupported(UINT64_MAX));
}
void bootstrapEnabledTimingAndIrq() {
    Imx1Bootstrap board;
    write(board, base, 0x10);
    write(board, pcdr, 0, Imx1BootstrapAccess::sideEffect);
    // A BCLK_DIV change is taken with the PWM on PERCLK1 (patch 0692: PERCLK1 is independent of BCLK).
    // Shown on a copy so this machine keeps its timing.
    { auto copy = board; write(copy, cscr, 0x0f00a003); }
    assert(read(board, pcdr) == 0xb00bb && read(board, cscr) == 0x0f00ac03);
    board.advanceFastBusCycles(2); // System /12 to BCLK, independent PERCLK1 /12, then PWM /2.
    assert(read(board, base + 12) == 1);
    write(board, base, 0);
    write(board, base, 0x8050);
    write(board, 0x00223008, 34, Imx1BootstrapAccess::sideEffect);
    assert(read(board, 0x00223014) == 0);
    assert(read(board, base) == 0x80f0); // Failed AITC write cannot consume PWM IRQ.
    write(board, 0x00223008, 34);
    write(board, base, 0x80d0, Imx1BootstrapAccess::sideEffect);
    assert(read(board, base) == 0x8070);
    board.connectCpuInterrupts();
    write(board, base, 0x80d0);
    assert(board.interruptLines().irq == Imx1AitcLine::asserted);
    assert(read(board, base) == 0x80f0 && board.interruptLines().irq == Imx1AitcLine::clear);
    write(board, pcdr, 0); // CLK32 does not use PERCLK1 or HCLK.
    write(board, cscr, 0x0f00a003);
    board.advanceFastBusCycles(666); // 5994 / 3000 CLK32 periods.
    assert(read(board, base + 12) == 0);
    board.advanceFastBusCycles(1);
    assert(read(board, base + 12) == 1);
    for (unsigned i = 0; i < 20; ++i) assert(read(board, base + 12) == 1);

    Imx1Bootstrap stalled;
    stalled.setSystemPllScenario(true);
    write(stalled, cscr, 0x57000003);
    write(stalled, cscr + 4, 0x03e717bf);
    write(stalled, cscr, 0x57200003);
    stalled.advanceFastBusCycles(6000);
    write(stalled, base, 0x8010);
    auto chunks = stalled;
    stalled.advanceCpuCycles(6103, 3, true);
    for (unsigned i = 0; i < 6103; ++i) chunks.advanceCpuCycles(1, 3, true);
    assert(read(stalled, base + 12) == 0 && read(chunks, base + 12) == 0);
    write(stalled, cscr, 0x57400003, Imx1BootstrapAccess::ok, 3);
    assert(stalled.finishSystemPllBusStall() > 0);
    assert(read(stalled, base + 12) == 1); // Two independent CLK32 periods pass during output hold.
}
}
int main() {
    resetAndRegisters(); bootstrapTimingAndIrq(); fifoCounterAndOutput();
    repeatsSwapsAndBounds(); bootstrapEnabledTimingAndIrq();
    std::puts("i.MX1 PWM reset, independent clocks, FIFO, repeats, output, counter and IRQ passed");
}
