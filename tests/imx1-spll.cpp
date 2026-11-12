#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include "imx1_bootstrap.h"

constexpr uint32_t cscr = 0x0021b000, spctl0 = cscr + 12, spctl1 = cscr + 16;
constexpr uint32_t restart = 0x00400000, target = 0x123638ad;
static void write(Imx1Bootstrap& soc, uint32_t address, uint32_t value,
                  Imx1BootstrapAccess status = Imx1BootstrapAccess::ok, uint32_t mode = 3) {
    const auto actual = soc.access(address, 4, true, value, mode);
    if (actual != status) std::fprintf(stderr, "address=%08x value=%08x mode=%u actual=%s expected=%s\n", address, value, mode, imx1BootstrapAccessName(actual), imx1BootstrapAccessName(status));
    assert(actual == status);
}
static uint32_t read(Imx1Bootstrap& soc, uint32_t address) {
    uint32_t value = 0xdeadbeef;
    assert(soc.access(address, 4, false, value) == Imx1BootstrapAccess::ok);
    return value;
}
static void configure(Imx1Bootstrap& soc) {
    soc.setSystemPllScenario(true);
    write(soc, cscr, 0x57000003, Imx1BootstrapAccess::ok, 0);
    write(soc, cscr + 4, 0x03e717bf, Imx1BootstrapAccess::ok, 0);
    write(soc, cscr, 0x57200003, Imx1BootstrapAccess::ok, 0);
    soc.advanceFastBusCycles(6000);
    write(soc, cscr + 32, 2, Imx1BootstrapAccess::ok, 0); // PERCLK1 / 3, Timer 1 / 32.
    write(soc, 0x00202004, 31);
    write(soc, 0x00202000, 0x103);
}
int main() {
    Imx1Bootstrap strict;
    write(strict, spctl0, target, Imx1BootstrapAccess::sideEffect, 0);
    uint32_t unknown = 0xdeadbeef;
    assert(strict.access(spctl1, 4, false, unknown) == Imx1BootstrapAccess::externalInput);
    assert(unknown == 0xdeadbeef);
    Imx1Cgm fields;
    fields.setSystemPllScenario(true);
    auto ratio = fields.systemPllActiveRatio();
    assert(ratio.numerator == 3000 && ratio.denominator == 1);
    uint32_t value = target;
    assert(fields.access(spctl0, 4, true, value) == Imx1CgmAccess::ok);
    ratio = fields.systemPllTargetRatio();
    assert(ratio.numerator == 8305664 && ratio.denominator == 2835);
    assert(fields.systemPllActiveRatio().numerator == 3000);
    Imx1Bootstrap soc;
    configure(soc);
    assert(read(soc, spctl1) == 0x8000);
    for (auto bad : {0x923638adu, 0x123678adu, 0x100038adu, 0x12363bffu})
        write(soc, spctl0, bad, Imx1BootstrapAccess::reservedBits);
    write(soc, spctl0, target);
    assert(read(soc, spctl0) == target && read(soc, spctl1) == 0x8000);
    for (unsigned i = 0; i < 100; ++i) assert(read(soc, spctl1) == 0x8000);
    write(soc, spctl0, target, Imx1BootstrapAccess::sideEffect);
    soc.advanceCpuCycles(61, 3, true); // floor(61 * 375000/762752) = 29 output edges.
    write(soc, spctl0, target, Imx1BootstrapAccess::sideEffect);
    soc.advanceCpuCycles(1, 3, true);
    write(soc, spctl0, target); // Exactly 30 elapsed old output edges.
    write(soc, cscr, 0x57400003, Imx1BootstrapAccess::sideEffect, 0);
    assert(read(soc, cscr) == 0x57000003);
    // A target equal to FCLK, or above it, must fail without consuming write spacing.
    soc.advanceCpuCycles(62, 3, true);
    write(soc, spctl0, 0x03e717bf, Imx1BootstrapAccess::sideEffect);
    write(soc, spctl0, 0x03ff3ffe, Imx1BootstrapAccess::sideEffect);
    assert(read(soc, spctl0) == target);
    write(soc, spctl0, target);
    const auto count = read(soc, 0x00202010);
    write(soc, cscr, 0x57400003);
    assert(read(soc, spctl1) == 0 && !soc.cpuClockModeSupported(0, true));
    for (unsigned i = 0; i < 100; ++i) assert(read(soc, cscr) == 0x57400003);
    write(soc, cscr, 0x57400003, Imx1BootstrapAccess::sideEffect);
    write(soc, spctl0, target, Imx1BootstrapAccess::sideEffect);
    write(soc, spctl1, 0x8000, Imx1BootstrapAccess::sideEffect);
    write(soc, spctl1, 0x40, Imx1BootstrapAccess::sideEffect);
    write(soc, spctl1, 0); // Unchanged status has no effect.
    write(soc, cscr, 0x57000003); // Zero does not cancel a pending request.
    soc.advanceCpuCycles(12204, 3, true);
    assert(read(soc, spctl1) == 0 && read(soc, 0x00202010) == count);
    soc.advanceCpuCycles(1, 3, true); // 2 * 762752 / 125 = 12204.032 core cycles.
    assert(read(soc, spctl1) == 0x8000 && read(soc, cscr) == 0x57000003);
    assert(read(soc, 0x00202010) == count);
    write(soc, spctl1, 0, Imx1BootstrapAccess::sideEffect);
    write(soc, spctl1, 0x8000);
    soc.advanceCpuCycles(762752, 3, true);
    assert(read(soc, 0x00202010) > count);
    // Thirty output edges can include a fraction from before the write: require full periods.
    Imx1Bootstrap spacing;
    configure(spacing);
    spacing.advanceCpuCycles(37, 3, true);
    write(spacing, spctl0, target);
    spacing.advanceCpuCycles(61, 3, true);
    write(spacing, spctl0, target, Imx1BootstrapAccess::sideEffect);
    spacing.advanceCpuCycles(1, 3, true);
    write(spacing, spctl0, target);
    // Crossing the activation event in one large charge matches one-cycle charges.
    Imx1Bootstrap chunks, whole;
    configure(chunks); configure(whole);
    for (auto* board : {&chunks, &whole}) {
        board->advanceCpuCycles(37, 3, true);
        write(*board, spctl0, target);
        write(*board, cscr, 0x57400003);
    }
    for (unsigned i = 0; i < 37000; ++i) chunks.advanceCpuCycles(1, 3, true);
    whole.advanceCpuCycles(37000, 3, true);
    const uint64_t afterRestart = uint64_t(37000) * 125 - 2 * 762752;
    assert(read(chunks, 0x00202010) == (18 + afterRestart * 8305664 / (uint64_t(762752) * 2835)) / 96);
    assert(read(chunks, 0x00202010) == read(whole, 0x00202010));
    assert(read(chunks, cscr) == read(whole, cscr));
    chunks.advanceCpuCycles(UINT32_MAX, 3, true);
    whole.advanceCpuCycles(UINT32_MAX - 17, 3, true);
    whole.advanceCpuCycles(17, 3, true);
    assert(read(chunks, 0x00202010) == read(whole, 0x00202010));
    // One-cycle follow-up exposes lost fractional output or PERCLK/prescaler phase.
    for (unsigned i = 0; i < 500; ++i) {
        chunks.advanceCpuCycles(1, 3, true); whole.advanceCpuCycles(1, 3, true);
        assert(read(chunks, 0x00202010) == read(whole, 0x00202010));
    }
    // Returning to FastBus after activation keeps the fractional System PLL progress.
    chunks.advanceFastBusCycles(73); whole.advanceFastBusCycles(73);
    for (unsigned i = 0; i < 500; ++i) {
        chunks.advanceCpuCycles(1, 3, true); whole.advanceCpuCycles(1, 3, true);
        const uint64_t units = (uint64_t(37000) + UINT32_MAX + 501 + i) * 125 - 2 * 762752;
        const uint64_t edges = units * 8305664 / (uint64_t(762752) * 2835);
        assert(read(chunks, 0x00202010) == (18 + edges + 73) / 96);
        assert(read(whole, 0x00202010) == read(chunks, 0x00202010));
    }
    // The private probe jumps to the bus-ready event without CPU instructions or timer edges.
    Imx1Bootstrap stalled;
    configure(stalled);
    stalled.advanceCpuCycles(37, 3, true);
    write(stalled, spctl0, target); write(stalled, cscr, 0x57400003);
    stalled.advanceCpuCycles(17, 3, true);
    const auto frozen = read(stalled, 0x00202010);
    uint32_t instruction = 0xdeadbeef;
    for (unsigned i = 0; i < 100; ++i) {
        assert(stalled.instructionRead(0x00300000, 4, instruction) && instruction == 0);
        assert(!stalled.instructionRead(spctl1, 4, instruction));
    }
    assert(read(stalled, spctl1) == 0);
    assert(stalled.finishSystemPllBusStall() == 2 * 762752 - 17 * 125);
    assert(stalled.finishSystemPllBusStall() == 0);
    assert(read(stalled, 0x00202010) == frozen && read(stalled, spctl1) == 0x8000);
    stalled.advanceCpuCycles(762752, 3, true);
    assert(read(stalled, 0x00202010) == (18 + uint64_t(125) * 8305664 / 2835) / 96);
    // Extreme denominators and maximum charges exercise bounded portable multiplication.
    Imx1Bootstrap extreme;
    extreme.setSystemPllScenario(true);
    write(extreme, cscr, 0x57003c03, Imx1BootstrapAccess::ok, 0);
    write(extreme, cscr + 4, 0x3bfd3ffd, Imx1BootstrapAccess::ok, 0);
    write(extreme, cscr, 0x57203c03, Imx1BootstrapAccess::ok, 0);
    extreme.advanceFastBusCycles(375);
    write(extreme, spctl0, 0x3bfd3ffd);
    write(extreme, cscr, 0x57403c03);
    write(extreme, 0x00202000, 0x103);
    auto extremeChunks = extreme;
    extreme.advanceCpuCycles(UINT32_MAX, 3, true);
    extremeChunks.advanceCpuCycles(100000, 3, true);
    extremeChunks.advanceCpuCycles(UINT32_MAX - 100000, 3, true);
    assert(read(extreme, 0x00202010) == read(extremeChunks, 0x00202010));
    Imx1Cgm extremeFields;
    extremeFields.setSystemPllScenario(true);
    value = 0x3bfd3ffd;
    assert(extremeFields.access(spctl0, 4, true, value) == Imx1CgmAccess::ok);
    const auto extremeRatio = extremeFields.systemPllTargetRatio();
    assert(uint64_t(UINT32_MAX) * extremeRatio.denominator > UINT64_MAX / extremeRatio.numerator);
    const uint64_t edges = (uint64_t(UINT32_MAX) * extremeRatio.denominator - 2 * extremeRatio.numerator) / extremeRatio.denominator;
    assert(read(extreme, 0x00202010) == edges / 12);
    assert(read(extreme, spctl1) == 0x8000);
    extreme.advanceCpuCycles(UINT32_MAX, 3, true);
    std::puts("PASS: nominal System PLL staging, LF/output hold, elapsed restart, spacing, guard and rational phase");
}
