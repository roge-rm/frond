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
    write(pwm, 0, 0x10, Imx1PwmAccess::sideEffect);
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
}
int main() {
    resetAndRegisters(); bootstrapTimingAndIrq();
    std::puts("i.MX1 PWM disabled/reset state, five-HCLK release, access guards and inactive IRQ passed");
}
