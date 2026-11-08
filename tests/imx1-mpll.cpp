#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "imx1_bootstrap.h"

static constexpr uint32_t cscr = 0x0021b000, mpctl0 = 0x0021b004;
static constexpr uint32_t restart = 0x00200000;

static void read(Imx1Cgm& clocks, uint32_t address, uint32_t expected) {
    uint32_t value = 0xdeadbeef;
    assert(clocks.access(address, 4, false, value) == Imx1CgmAccess::ok);
    assert(value == expected);
}

static void write(Imx1Cgm& clocks, uint32_t address, uint32_t value,
                  Imx1CgmAccess expected = Imx1CgmAccess::ok) {
    assert(clocks.access(address, 4, true, value) == expected);
}

int main() {
    Imx1Cgm clocks;
    assert(clocks.mcuClockReady(false) && clocks.mcuClockReady(true));
    const auto reset = clocks.mcuPllTargetRatio();
    assert(reset.numerator == 6000 && reset.denominator == 1);
    write(clocks, cscr, 0x57000003);
    // Reserved field values must not consume the first permitted programming operation.
    for (auto value : {0x83e717bfu, 0x03e757bfu, 0x000017bfu, 0x03e717ffu})
        write(clocks, mpctl0, value, Imx1CgmAccess::reservedBits);
    read(clocks, mpctl0, 0x003f1437);
    write(clocks, mpctl0, 0x03e717bf);
    read(clocks, mpctl0, 0x03e717bf);
    assert(!clocks.mcuClockReady(false) && !clocks.mcuClockReady(true));
    const auto target = clocks.mcuPllTargetRatio();
    // Table 12-9 gives unsigned MFN=959, MFD=999, MFI=5 and PD=0.
    assert(target.numerator == 762752 && target.denominator == 125);
    // FCLK becomes unqualified: even a long delay cannot prove the required 56 FCLK cycles.
    for (auto delay : {0u, 55u, 56u, 6000000u}) {
        clocks.advanceSystemPllCycles(delay);
        write(clocks, mpctl0, 0x003f1437, Imx1CgmAccess::externalInput);
        read(clocks, mpctl0, 0x03e717bf);
    }
    write(clocks, cscr, 0x57000003 | restart);
    for (unsigned i = 0; i < 100; ++i) read(clocks, cscr, 0x57000003 | restart);
    write(clocks, cscr, 0x57000003 | restart, Imx1CgmAccess::sideEffect);
    clocks.advanceSystemPllCycles(2999);
    read(clocks, cscr, 0x57000003 | restart);
    // Zero is no effect, and PRESC changes do not change FastBus elapsed time.
    write(clocks, cscr, 0x57008003);
    read(clocks, cscr, 0x57008003 | restart);
    clocks.advanceSystemPllCycles(3000);
    read(clocks, cscr, 0x57008003 | restart);
    write(clocks, cscr, 0x57408003, Imx1CgmAccess::sideEffect);
    write(clocks, cscr, 0x57018003, Imx1CgmAccess::externalInput);
    write(clocks, cscr, 0xd7008003, Imx1CgmAccess::reservedBits);
    assert(!clocks.mcuClockReady(true));
    clocks.advanceSystemPllCycles(1);
    assert(!clocks.mcuClockReady(false) && clocks.mcuClockReady(true));
    read(clocks, cscr, 0x57008003);
    write(clocks, mpctl0, 0x03e717bf, Imx1CgmAccess::externalInput);
    // Clearing the request establishes no lock or qualified FCLK timing.
    uint32_t unknown = 0xdeadbeef;
    assert(clocks.access(0x0021b010, 4, false, unknown) == Imx1CgmAccess::externalInput);
    assert(unknown == 0xdeadbeef);
    // Unchanged programming and a restart without programming are also conservative.
    Imx1Cgm unchanged, restarted;
    write(unchanged, mpctl0, 0x003f1437);
    unchanged.advanceSystemPllCycles(UINT64_MAX);
    write(unchanged, mpctl0, 0x003f1437, Imx1CgmAccess::externalInput);
    write(restarted, cscr, 0x0f00ac03 | restart);
    restarted.advanceSystemPllCycles(UINT64_MAX);
    read(restarted, cscr, 0x0f00ac03);
    write(restarted, mpctl0, 0x003f1437, Imx1CgmAccess::externalInput);

    // The MFI encoding clamps 0..5 to 5, with unsigned MFN across its range.
    for (uint32_t mfi = 0; mfi <= 5; ++mfi) {
        Imx1Cgm lowMfi;
        write(lowMfi, mpctl0, 0x003f0037 | (mfi << 10));
        const auto ratio = lowMfi.mcuPllTargetRatio();
        assert(ratio.numerator == 6000 && ratio.denominator == 1);
    }
    Imx1Cgm extremes;
    write(extremes, mpctl0, 0x3fff3ffe); // PD=15, MFD=1023, MFI=15, MFN=1022.
    const auto ratio = extremes.mcuPllTargetRatio();
    assert(ratio.numerator * 16 * 1024 == uint64_t(1024) * (15 * 1024 + 1022) * ratio.denominator);

    Imx1Bootstrap soc;
    auto busWrite = [&](uint32_t address, uint32_t value,
                        Imx1BootstrapAccess expected = Imx1BootstrapAccess::ok) {
        assert(soc.access(address, 4, true, value) == expected);
    };
    auto busRead = [&](uint32_t address, uint32_t expected) {
        uint32_t value = 0xdeadbeef;
        assert(soc.access(address, 4, false, value) == Imx1BootstrapAccess::ok);
        assert(value == expected);
    };
    busWrite(cscr, 0x57000003); busWrite(0x0021b020, 0x00100012);
    busWrite(0x00202004, 31); busWrite(0x00202000, 0x103);
    soc.advanceFastBusCycles(94);
    // Reject the whole transaction, including its otherwise permitted restart request.
    busWrite(cscr, 0x57200403, Imx1BootstrapAccess::sideEffect);
    busRead(cscr, 0x57000003);
    busWrite(mpctl0, 0x03e717bf);
    busWrite(cscr, 0x57208003);
    soc.advanceFastBusCycles(1); busRead(0x00202010, 0);
    soc.advanceFastBusCycles(1); busRead(0x00202010, 1);
    // Rejected writes preserve both Timer 1 phase and the restart deadline.
    busWrite(cscr, 0x57008403, Imx1BootstrapAccess::sideEffect);
    busWrite(mpctl0, 0x003f1437, Imx1BootstrapAccess::externalInput);
    busRead(cscr, 0x57208003); busRead(mpctl0, 0x03e717bf);
    soc.advanceFastBusCycles(5997);
    busRead(cscr, 0x57208003); busRead(0x00202010, 63);
    busWrite(cscr, 0x57000003);
    soc.advanceFastBusCycles(1);
    busRead(cscr, 0x57000003); busRead(0x00202010, 63);
    soc.advanceFastBusCycles(50); busRead(0x00202010, 64);
    // After disabling Timer 1, BCLK_DIV changes affect the elapsed reference relationship.
    busWrite(0x00202000, 0);
    busWrite(cscr, 0x57200c03); // Four System PLL cycles per charged FastBus cycle.
    soc.advanceFastBusCycles(1499); busRead(cscr, 0x57200c03);
    busWrite(cscr, 0x57008c03);
    soc.advanceFastBusCycles(1); busRead(cscr, 0x57008c03);
    // Nominal async clock, fractional System PLL phase and both divider phases survive handoffs.
    auto configure = [](Imx1Bootstrap& board) {
        uint32_t value = 0x57000003;
        assert(board.access(cscr, 4, true, value) == Imx1BootstrapAccess::ok);
        value = 0x03e717bf;
        assert(board.access(mpctl0, 4, true, value) == Imx1BootstrapAccess::ok);
        assert(!board.cpuClockModeSupported(3, false) && !board.cpuClockModeSupported(3, true));
        value = 0x57200003;
        assert(board.access(cscr, 4, true, value) == Imx1BootstrapAccess::ok);
        board.advanceFastBusCycles(5999);
        assert(!board.cpuClockModeSupported(3, true));
        board.advanceFastBusCycles(1);
        assert(!board.cpuClockModeSupported(3, false) && board.cpuClockModeSupported(3, true));
        assert(!board.cpuClockModeSupported(1, true) && !board.cpuClockModeSupported(2, true));
        value = 2;
        assert(board.access(0x0021b020, 4, true, value) == Imx1BootstrapAccess::ok);
        value = 31;
        assert(board.access(0x00202004, 4, true, value) == Imx1BootstrapAccess::ok);
        value = 0x103;
        assert(board.access(0x00202000, 4, true, value) == Imx1BootstrapAccess::ok);
    };
    auto counter = [](Imx1Bootstrap& board) {
        uint32_t value = 0;
        assert(board.access(0x00202010, 4, false, value) == Imx1BootstrapAccess::ok);
        return value;
    };
    // Async requires FCLK > BCLK, including PRESC, equality is unsupported.
    Imx1Bootstrap equal, slower;
    uint32_t equalBus = 0x57008003;
    assert(equal.access(cscr, 4, true, equalBus) == Imx1BootstrapAccess::ok);
    assert(!equal.cpuClockModeSupported(3, false) && !equal.cpuClockModeSupported(3, true));
    uint32_t slowPll = 0x043f1437; // MCU PLL / CLK32 = 3000, PRESC=2 gives FCLK=1500.
    assert(slower.access(cscr, 4, true, equalBus) == Imx1BootstrapAccess::ok);
    assert(slower.access(mpctl0, 4, true, slowPll) == Imx1BootstrapAccess::ok);
    uint32_t slowRestart = equalBus | restart;
    assert(slower.access(cscr, 4, true, slowRestart) == Imx1BootstrapAccess::ok);
    slower.advanceFastBusCycles(6000);
    assert(!slower.cpuClockModeSupported(3, true));
    Imx1Bootstrap divided;
    configure(divided);
    uint32_t presc = 0x57008003;
    assert(divided.access(cscr, 4, true, presc) == Imx1BootstrapAccess::ok);
    divided.advanceCpuCycles(762752, 3, true);
    assert(counter(divided) == 750000 / 96);
    // The maximum charge stays within the portable uint64_t clock conversion bounds.
    divided.advanceCpuCycles(UINT32_MAX, 3, true);
    assert(counter(divided) == (uint64_t(762752) + UINT32_MAX) * 750000 / 762752 / 96);
    Imx1Bootstrap chunks, whole;
    configure(chunks); configure(whole);
    // 3 async cycles leave 362248/762752 System PLL cycles plus one whole cycle.
    chunks.advanceCpuCycles(3, 3, true); whole.advanceCpuCycles(3, 3, true);
    chunks.advanceFastBusCycles(95); whole.advanceFastBusCycles(95);
    assert(counter(chunks) == 1 && counter(whole) == 1);
    for (unsigned i = 0; i < 10000; ++i) chunks.advanceCpuCycles(37, 3, true);
    whole.advanceCpuCycles(370000, 3, true);
    const uint64_t expected = (uint64_t(370003) * 375000 / 762752 + 95) / 96;
    assert(counter(chunks) == expected && counter(whole) == expected);
    // Complete a nominal period: 762752 core cycles = 375000 System PLL cycles.
    chunks.advanceCpuCycles(762752 - 370003, 3, true);
    assert(counter(chunks) == (375000 + 95) / 96);
    for (unsigned i = 0; i < 100; ++i) assert(counter(chunks) == (375000 + 95) / 96);
    // This edge loses a timer tick if the fractional phase is dropped on either handoff.
    chunks.advanceFastBusCycles(73);
    assert(counter(chunks) == 3908);
    uint32_t change = 3;
    assert(chunks.access(cscr + 32, 4, true, change, 3) == Imx1BootstrapAccess::sideEffect);
    change = 0x57200003;
    assert(chunks.access(cscr, 4, true, change, 3) == Imx1BootstrapAccess::sideEffect);
    change = 0x03e717bf;
    assert(chunks.access(mpctl0, 4, true, change, 3) == Imx1BootstrapAccess::sideEffect);
    change = 0x57010003;
    assert(chunks.access(cscr, 4, true, change, 3) == Imx1BootstrapAccess::externalInput);
    change = 0x57000002;
    assert(chunks.access(cscr, 4, true, change, 3) == Imx1BootstrapAccess::sideEffect);

    std::puts("PASS: MCU PLL fields, unsigned exact ratios, unknown FCLK gate, elapsed restart and FastBus Timer 1 phase");
}
