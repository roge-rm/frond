#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "imx1_bootstrap.h"

namespace {
constexpr uint32_t base = 0x00215000;
uint32_t read(Imx1Asp& c, uint32_t offset) {
    uint32_t value = 0xdeadbeef;
    assert(c.access(base + offset, 4, false, value) == Imx1AspAccess::ok);
    return value;
}
void write(Imx1Asp& c, uint32_t offset, uint32_t value) {
    const auto status = c.access(base + offset, 4, true, value);
    if (status != Imx1AspAccess::ok)
        std::fprintf(stderr, "ASP rejected write offset=%x value=%x status=%u\n", offset, value, unsigned(status));
    assert(status == Imx1AspAccess::ok);
}
void rejected(Imx1Asp& c, uint32_t offset, uint32_t value, Imx1AspAccess expected) {
    const auto before = read(c, offset);
    const auto argument = value;
    assert(c.access(base + offset, 4, true, value) == expected && value == argument);
    assert(read(c, offset) == before);
}
void registersAndReset() {
    Imx1Asp c;
    for (auto offset : {0u, 0x10u, 0x14u, 0x18u, 0x1cu, 0x2cu, 0x30u, 0x34u}) {
        assert(Imx1Asp::registerName(base + offset));
        for (auto size : {0u, 1u, 2u, 8u}) {
            uint32_t value = 0xdeadbeef;
            assert(c.access(base + offset, size, false, value) == Imx1AspAccess::width && value == 0xdeadbeef);
            assert(c.access(base + offset, size, true, value) == Imx1AspAccess::width && value == 0xdeadbeef);
        }
        for (unsigned byte = 1; byte < 4; ++byte) {
            uint32_t value = 0;
            assert(c.access(base + offset + byte, 4, true, value) == Imx1AspAccess::width);
        }
        if (offset) assert(read(c, offset) == (offset == 0x10 ? 0x02000000u : 0));
    }
    uint32_t value = 0xdeadbeef;
    assert(c.access(base + 4, 4, false, value) == Imx1AspAccess::unmapped && value == 0xdeadbeef);
    assert(!Imx1Asp::registerName(base + 0x38));
    assert(c.access(base, 4, false, value) == Imx1AspAccess::sideEffect && value == 0xdeadbeef);
    assert(c.access(base, 4, true, value) == Imx1AspAccess::sideEffect);
    write(c, 0x34, 0xffffffff); assert(read(c, 0x34) == 0);
    write(c, 0x14, 0x7fff); write(c, 0x30, 0xfffff); write(c, 0x18, 0x463); write(c, 0x2c, 31);
    write(c, 0x10, 0x0e30fff1);
    assert(read(c, 0x10) == 0x0e30fff1 && read(c, 0x14) == 0x7fff);
    assert(read(c, 0x30) == 0x7ffff && read(c, 0x18) == 0x463 && read(c, 0x2c) == 31);
    write(c, 0x1c, 0x4c3); assert(read(c, 0x1c) == 0 && read(c, 0x34) == 0);
    for (auto offset : {0x10u, 0x14u, 0x18u, 0x1cu, 0x2cu, 0x30u}) {
        write(c, offset, 0x80000000);
        assert(read(c, offset) == 0);
    }
    write(c, 0x10, 0x02040200); assert(read(c, 0x10) == 0x02000200);
    write(c, 0x10, 1u << 23);
    for (auto offset : {0x10u, 0x14u, 0x18u, 0x1cu, 0x2cu, 0x30u, 0x34u})
        assert(read(c, offset) == (offset == 0x10 ? 0x02000000u : 0));
}
void clocksAndInputGuards() {
    Imx1Asp c;
    assert(c.clockDivider() == 0);
    for (unsigned n = 1; n <= 31; ++n) {
        write(c, 0x2c, n); assert(c.clockDivider() == n + 1);
        write(c, 0x10, 0); assert(c.clockDivider() == 0);
        write(c, 0x10, 0x02000000); assert(c.clockDivider() == n + 1);
    }
    write(c, 0x2c, 0x63); assert(read(c, 0x2c) == 3 && c.clockDivider() == 4);
    write(c, 0x2c, 0); assert(c.clockDivider() == 0);
    rejected(c, 0x18, 0x10, Imx1AspAccess::externalInput);
    // PADE is set in a separate write, after configuration.
    rejected(c, 0x10, 0x02001003, Imx1AspAccess::sideEffect);
    write(c, 0x10, 0x02001001); write(c, 0x10, 0x02001003);
    assert(read(c, 0x1c) == 0); // Divider zero supplies no ADC clock.
    rejected(c, 0x2c, 1, Imx1AspAccess::externalInput);
    write(c, 0x10, 0x00001003); write(c, 0x2c, 1);
    rejected(c, 0x10, 0x02001003, Imx1AspAccess::externalInput);
    write(c, 0x10, 0x02000003); // MOD=00 remains idle with PADE set.
    rejected(c, 0x10, 0x02003003, Imx1AspAccess::externalInput);
    write(c, 0x10, 0x02000001); write(c, 0x10, 0x02003001);
    rejected(c, 0x10, 0x02003003, Imx1AspAccess::externalInput);
    assert(c.inactiveInterrupts() == ((uint64_t(1) << 5) | (uint64_t(1) << 9) |
                                    (uint64_t(1) << 33) | (uint64_t(1) << 46)));
}
void heldHighDetector() {
    Imx1Asp c(true);
    assert(c.inactiveInterrupts() == Imx1Asp::interruptSources);
    write(c, 0x18, 0x10);
    assert(read(c, 0x18) == 0x10 && read(c, 0x1c) == 0);
    assert(c.inactiveInterrupts() == Imx1Asp::interruptSources);
    for (auto setting : {0x30u, 0x50u, 0x70u}) {
        rejected(c, 0x18, setting, Imx1AspAccess::externalInput);
        assert(c.inactiveInterrupts() == Imx1Asp::interruptSources && read(c, 0x1c) == 0);
    }
    // Inactive W1C has no event to retrigger, and enable/disable cannot change PX1.
    write(c, 0x1c, 0x4c3); write(c, 0x18, 0); write(c, 0x18, 0x10);
    write(c, 0x10, 1u << 23);
    assert(read(c, 0x18) == 0 && read(c, 0x1c) == 0);
    write(c, 0x18, 0x413);
    assert(c.inactiveInterrupts() == Imx1Asp::interruptSources);
    write(c, 0x2c, 3); write(c, 0x10, 0x0200d201);
    rejected(c, 0x10, 0x0200d203, Imx1AspAccess::externalInput);
    uint32_t value = 0xdeadbeef;
    assert(c.access(base, 4, false, value) == Imx1AspAccess::sideEffect && value == 0xdeadbeef);
    assert(c.access(base, 4, true, value) == Imx1AspAccess::sideEffect && value == 0xdeadbeef);

    Imx1Bootstrap b(true);
    value = 46;
    assert(b.access(0x00223008, 4, true, value) == Imx1BootstrapAccess::ok);
    value = 0;
    assert(b.access(0x0022305c, 4, false, value) == Imx1BootstrapAccess::ok && value == 0);
    value = 0x10;
    assert(b.access(base + 0x18, 4, true, value) == Imx1BootstrapAccess::ok);
    for (unsigned i = 0; i < 100; ++i) {
        b.advanceFastBusCycles(1000000);
        assert(b.clockStatus() == Imx1BootstrapAccess::ok);
        for (auto address : {base + 0x1c, 0x0022305cu}) {
            value = 0xdeadbeef;
            assert(b.access(address, 4, false, value) == Imx1BootstrapAccess::ok && value == 0);
        }
    }
    value = 1u << 14; // Source 46 in the high force register, with CPU delivery guarded.
    assert(b.access(0x00223050, 4, true, value) == Imx1BootstrapAccess::sideEffect);
    value = 0;
    assert(b.access(0x00223050, 4, false, value) == Imx1BootstrapAccess::ok && value == 0);
}
void bootstrapInputsAndTime() {
    Imx1Bootstrap b;
    auto access = [&](uint32_t address, bool write, uint32_t& value) {
        return b.access(address, 4, write, value);
    };
    for (auto source : {5u, 9u, 33u, 46u}) {
        uint32_t value = source;
        assert(access(0x00223008, true, value) == Imx1BootstrapAccess::ok);
    }
    uint32_t value = 0x463;
    assert(access(base + 0x18, true, value) == Imx1BootstrapAccess::ok);
    value = 0x7ffff;
    assert(access(base + 0x30, true, value) == Imx1BootstrapAccess::ok);
    for (unsigned i = 0; i < 100; ++i) {
        value = 0xdeadbeef;
        assert(access(base + 0x1c, false, value) == Imx1BootstrapAccess::ok && value == 0);
        b.advanceFastBusCycles(1000000);
        assert(b.clockStatus() == Imx1BootstrapAccess::ok);
    }
    value = 0;
    assert(access(0x00223058, false, value) == Imx1BootstrapAccess::ok && value == 0);
    assert(access(0x0022305c, false, value) == Imx1BootstrapAccess::ok && value == 0);
    value = 0x473;
    assert(access(base + 0x18, true, value) == Imx1BootstrapAccess::externalInput);
    assert(access(base + 0x18, false, value) == Imx1BootstrapAccess::ok && value == 0x463);
    // CPU interrupt delivery stays guarded even when the ASP source is software forced.
    value = 1u << 1;
    assert(access(0x00223050, true, value) == Imx1BootstrapAccess::sideEffect);
}
void manualUConversion() {
    Imx1Asp c(true, true, 0x8000);
    write(c, 0x2c, 3); write(c, 0x14, 0x1814); write(c, 0x10, 0x02003001);
    write(c, 0x18, 0x11); write(c, 0x30, 0x3ffff);
    write(c, 0x10, 0x02003003);
    for (unsigned i = 0; i < 100; ++i) assert(read(c, 0x1c) == 0);
    c.advance(30239);
    assert(read(c, 0x1c) == 0 && read(c, 0x34) == 0);
    rejected(c, 0x2c, 4, Imx1AspAccess::sideEffect);
    rejected(c, 0x14, 0x1815, Imx1AspAccess::sideEffect);
    rejected(c, 0x10, 0x02003803, Imx1AspAccess::sideEffect);
    write(c, 0x10, 0x00003003); // Clock gating preserves the fractional conversion phase.
    c.advance(UINT64_MAX); assert(read(c, 0x1c) == 0);
    write(c, 0x10, 0x02003003); c.advance(1);
    assert(read(c, 0x1c) == 1 && read(c, 0x34) == 1);
    assert(c.assertedInterrupts() == ((uint64_t(1) << 9) | (uint64_t(1) << 33)));
    assert(read(c, 0x30) == 0xbffff);
    assert(read(c, 0) == 0x8000 && read(c, 0x1c) == 0 && read(c, 0x34) == 0x11);
    assert(c.assertedInterrupts() == (uint64_t(1) << 9));
    write(c, 0x30, 0x3ffff); assert(read(c, 0x30) == 0xbffff);
    write(c, 0x30, 0xbffff); assert(read(c, 0x30) == 0x3ffff);
    c.advance(UINT64_MAX); assert(read(c, 0x1c) == 0); // Manual PADE stays set without retriggering.
    write(c, 0x10, 0x02003003); c.advance(UINT64_MAX); assert(read(c, 0x1c) == 0);
    write(c, 0x10, 0x02003001); assert(read(c, 0x34) == 0);
    write(c, 0x10, 0x02003003); c.advance(30240); assert(read(c, 0x1c) == 1);
    write(c, 0x10, 0x02003001); assert(read(c, 0x1c) == 0); // PADE clear flushes unread data.
    write(c, 0x10, 0x02003003); c.advance(13); write(c, 0x10, 1u << 23);
    c.advance(UINT64_MAX); assert(read(c, 0x1c) == 0 && read(c, 0x34) == 0);
    write(c, 0x2c, 3); write(c, 0x14, 0x1814); write(c, 0x10, 0x02003001);
    write(c, 0x10, 0x02003003); c.advance(30240); assert(read(c, 0) == 0x8000);

    for (auto code : {0u, 0xffffu}) {
        for (unsigned divide = 1; divide <= 31; ++divide) {
            Imx1Asp adc(false, true, code), chunks(false, true, code);
            for (auto* a : {&adc, &chunks}) {
                write(*a, 0x2c, divide); write(*a, 0x14, 0x7bff);
                write(*a, 0x10, 0x02003001); write(*a, 0x10, 0x02003003);
            }
            const unsigned clocks = (divide + 1) * 1260 * 23;
            adc.advance(clocks - 1);
            for (unsigned n = 0; n < clocks - 1; ++n) chunks.advance(1);
            assert(read(adc, 0x1c) == 0 && read(chunks, 0x1c) == 0);
            adc.advance(1); chunks.advance(1);
            assert(read(adc, 0x1c) == 1 && read(chunks, 0x1c) == 1);
            assert(read(adc, 0) == code && read(chunks, 0) == code);
        }
    }
    Imx1Asp guarded(false, true, 0x8000);
    write(guarded, 0x2c, 3); write(guarded, 0x14, 0x1814);
    for (auto control : {0x02001001u, 0x02002001u, 0x02007001u, 0x02203001u, 0x02003000u}) {
        write(guarded, 0x10, control);
        rejected(guarded, 0x10, control | 2, Imx1AspAccess::externalInput);
    }
    write(guarded, 0x10, 0x02003001); write(guarded, 0x14, 0x1414);
    rejected(guarded, 0x10, 0x02003003, Imx1AspAccess::externalInput);
}
void manualUComparisons() {
    for (unsigned select = 0; select < 4; ++select) {
        for (unsigned condition = 0; condition < 2; ++condition) {
            for (unsigned threshold : {0x7fffu, 0x8000u, 0x8001u}) {
                Imx1Asp adc(false, true, 0x8000);
                write(adc, 0x2c, 3); write(adc, 0x14, 0x1814);
                write(adc, 0x10, 0x02003001);
                const auto compare = (select << 16) | (condition << 18) | threshold;
                write(adc, 0x30, compare); write(adc, 0x18, 2);
                write(adc, 0x10, 0x02003003); adc.advance(30240);
                const bool event = select == 3 && (condition ? 0x8000 > threshold : 0x8000 < threshold);
                assert(read(adc, 0x30) == (compare | (event ? 0x80000u : 0)));
                assert(adc.assertedInterrupts() == (event ? uint64_t(1) << 9 : 0));
                assert(read(adc, 0x1c) == 1); // One manual word cannot cause FIFO full.
                if (event) {
                    write(adc, 0x30, compare & ~0x30000u);
                    assert(adc.assertedInterrupts() == 0 && (read(adc, 0x30) & 0x80000));
                    write(adc, 0x30, compare);
                    assert(adc.assertedInterrupts() == (uint64_t(1) << 9));
                    write(adc, 0x30, compare | 0x80000u);
                    assert(adc.assertedInterrupts() == 0 && !(read(adc, 0x30) & 0x80000));
                }
            }
        }
    }
}

void manualUInterruptGate() {
    Imx1Bootstrap b(true, true, 0x8000);
    auto access = [&](uint32_t offset, uint32_t value) { return b.access(base + offset, 4, true, value); };
    assert(access(0x2c, 3) == Imx1BootstrapAccess::ok);
    assert(access(0x14, 0x1814) == Imx1BootstrapAccess::ok);
    assert(access(0x10, 0x02003001) == Imx1BootstrapAccess::ok);
    uint32_t source = 33;
    assert(b.access(0x00223008, 4, true, source) == Imx1BootstrapAccess::ok);
    assert(access(0x10, 0x02003003) == Imx1BootstrapAccess::ok);
    b.advanceFastBusCycles(30239); // Each FastBus cycle is one HCLK edge, independent of BCLKDIV.
    uint32_t value = 0;
    assert(b.access(base + 0x1c, 4, false, value) == Imx1BootstrapAccess::ok && value == 0);
    b.advanceFastBusCycles(1);
    assert(b.access(base + 0x1c, 4, false, value) == Imx1BootstrapAccess::ok && value == 1);
    assert(access(0x18, 1) == Imx1BootstrapAccess::sideEffect);
    value = 123;
    assert(b.access(base + 0x18, 4, false, value) == Imx1BootstrapAccess::ok && value == 0);
    b.connectCpuInterrupts();
    assert(access(0x18, 1) == Imx1BootstrapAccess::ok);
    assert(b.interruptLines().irq == Imx1AitcLine::asserted);
    value = 0;
    assert(b.access(base, 4, false, value) == Imx1BootstrapAccess::ok && value == 0x8000);
    assert(b.interruptLines().irq == Imx1AitcLine::clear);
}

}
int main() {
    registersAndReset(); clocksAndInputGuards(); heldHighDetector(); bootstrapInputsAndTime(); manualUConversion(); manualUComparisons(); manualUInterruptGate();
    std::puts("i.MX1 ASP configuration, reset, clock/input guards, held-high PX1 and inactive IRQs passed");
}
