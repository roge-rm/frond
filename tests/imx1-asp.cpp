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

Imx1AspPenInputs penStimulus(bool down = true) {
    Imx1AspPenInputs inputs;
    inputs.contactKnown = inputs.normalKnown = inputs.referenceKnown = true;
    inputs.contactDown = down;
    inputs.normal = {0x1100, 0x5500, 0xa900, 0x1234};
    inputs.reference = {0x0180, 0x3280, 0x7100};
    return inputs;
}
constexpr uint32_t penSlot = 1260 * 8 * 4, penIdle = 1260 * 4;
void startAuto(Imx1Asp& adc, bool acal = false) {
    write(adc, 0x2c, 0x63); write(adc, 0x14, 0x3814);
    const uint32_t config = acal ? 0x0600d201 : 0x0200e001;
    write(adc, 0x10, config); write(adc, 0x10, config | 2);
}
void penContact() {
    Imx1Asp adc;
    auto inputs = penStimulus(false);
    assert(adc.replacePenInputs(inputs) == Imx1AspAccess::ok);
    write(adc, 0x18, 0x10); assert(read(adc, 0x1c) == 0);
    inputs.contactDown = true;
    assert(adc.replacePenInputs(inputs) == Imx1AspAccess::ok);
    assert(read(adc, 0x1c) == 0x40 && adc.assertedInterrupts() == (uint64_t(1) << 46));
    write(adc, 0x1c, 0x40); assert(read(adc, 0x1c) == 0x40);
    write(adc, 0x18, 0); assert(adc.assertedInterrupts() == 0);
    write(adc, 0x1c, 0x40); assert(read(adc, 0x1c) == 0);
    write(adc, 0x18, 0x10); assert(read(adc, 0x1c) == 0x40);
    inputs.contactDown = false;
    assert(adc.replacePenInputs(inputs) == Imx1AspAccess::ok);
    assert(read(adc, 0x1c) == 0x40); write(adc, 0x1c, 0x40);
    assert(read(adc, 0x1c) == 0 && adc.assertedInterrupts() == 0);
    rejected(adc, 0x18, 0x30, Imx1AspAccess::externalInput);
    rejected(adc, 0x18, 0x50, Imx1AspAccess::externalInput);
    inputs.contactKnown = false;
    assert(adc.replacePenInputs(inputs) == Imx1AspAccess::externalInput);
    write(adc, 0x10, 1u << 23);
    write(adc, 0x18, 0x10); assert(read(adc, 0x1c) == 0);
}
void penSampling() {
    auto inputs = penStimulus();
    Imx1Asp adc(true, true, 0x9b00);
    assert(adc.replacePenInputs(inputs) == Imx1AspAccess::ok);
    startAuto(adc, true);
    adc.advance(penSlot - 1); assert(read(adc, 0x34) == 0 && read(adc, 0x1c) == 0);
    adc.advance(1); assert(read(adc, 0x34) == 1 && read(adc, 0) == inputs.reference[0]);
    adc.advance(2 * penSlot - 1); assert((read(adc, 0x34) & 15) == 2);
    adc.advance(1); assert((read(adc, 0x34) & 15) == 3);
    assert(read(adc, 0) == inputs.reference[1] && read(adc, 0) == inputs.reference[2]);
    adc.advance(penIdle + penSlot - 1); assert((read(adc, 0x34) & 15) == 3);
    adc.advance(1); assert(read(adc, 0) == inputs.reference[0]);
    // ACAL reset and normal restore preserve the external stimuli and manual U.
    write(adc, 0x10, 1u << 23);
    startAuto(adc);
    adc.advance(12 * penSlot + 2 * penIdle - 1);
    assert((read(adc, 0x34) & 15) == 11 && read(adc, 0x1c) == 1);
    adc.advance(1); assert(read(adc, 0x34) == 0 && read(adc, 0x1c) == 3);
    for (unsigned i = 0; i < 12; ++i) assert(read(adc, 0) == inputs.normal[i % 4]);
    assert(read(adc, 0x1c) == 0);
    uint32_t empty = 0x12345678;
    assert(adc.access(base, 4, false, empty) == Imx1AspAccess::sideEffect && empty == 0x12345678);
    write(adc, 0x10, 1u << 23);
    write(adc, 0x14, 0x1814); write(adc, 0x2c, 3);
    write(adc, 0x10, 0x02003001); write(adc, 0x10, 0x02003003);
    adc.advance(30240); assert(read(adc, 0) == 0x9b00);
    write(adc, 0x10, 1u << 23);
    write(adc, 0x14, 0x3814); write(adc, 0x2c, 3);
    write(adc, 0x10, 0x0200d201);
    rejected(adc, 0x10, 0x0200d203, Imx1AspAccess::externalInput); // Non-ACAL ZXY is unqualified.
}
void penCaptureAndClocks() {
    Imx1Asp adc;
    auto inputs = penStimulus();
    assert(adc.replacePenInputs(inputs) == Imx1AspAccess::ok); startAuto(adc);
    adc.advance(1);
    const auto original = inputs.normal;
    inputs.normal = {0x0200, 0x3300, 0x6400, 0xabcd};
    assert(adc.replacePenInputs(inputs) == Imx1AspAccess::ok);
    auto copy = adc;
    write(adc, 0x10, 0x0000e003); adc.advance(1000000);
    assert(read(adc, 0x1c) == 0 && adc.converting());
    write(adc, 0x10, 0x0200e003);
    rejected(adc, 0x2c, 4, Imx1AspAccess::sideEffect);
    rejected(adc, 0x14, 0x3815, Imx1AspAccess::sideEffect);
    auto unknown = inputs; unknown.normalKnown = false;
    assert(adc.replacePenInputs(unknown) == Imx1AspAccess::externalInput);
    adc.advance(penSlot - 2); assert(read(adc, 0x1c) == 0);
    adc.advance(1); copy.advance(penSlot - 1);
    assert(read(adc, 0) == original[0] && read(copy, 0) == original[0]);
    adc.advance(3 * penSlot); copy.advance(penSlot); copy.advance(2 * penSlot);
    for (unsigned i = 1; i < 4; ++i) assert(read(adc, 0) == inputs.normal[i] && read(copy, 0) == inputs.normal[i]);
    adc.advance(penIdle - 1); assert(read(adc, 0x1c) == 0);
    adc.advance(1); inputs.normal[0] = 0xffff;
    assert(adc.replacePenInputs(inputs) == Imx1AspAccess::ok);
    adc.advance(penSlot); assert(read(adc, 0) == 0x0200); // Slot captured at the end of idle.
    write(adc, 0x10, 0x0200e001); adc.advance(1000000);
    assert(!adc.converting() && read(adc, 0x34) == 0 && read(adc, 0x1c) == 0);
}
void penRingAndRelease() {
    Imx1Asp adc;
    auto inputs = penStimulus();
    assert(adc.replacePenInputs(inputs) == Imx1AspAccess::ok); startAuto(adc);
    adc.advance(12 * penSlot + 3 * penIdle); // At the next Z slot's start.
    for (unsigned round = 0; round < 4; ++round) {
        auto next = inputs;
        for (unsigned i = 1; i < 4; ++i) next.normal[i] += (round + 1) * 0x80;
        assert(adc.replacePenInputs(next) == Imx1AspAccess::ok);
        // First Z was already captured; further slots use the new transaction.
        adc.advance(4 * penSlot + penIdle);
        const auto ptr = read(adc, 0x34) & 15;
        assert(ptr == ((round + 1) * 4) % 12);
        assert(read(adc, 0x1c) == 0x83);
        uint16_t words[12];
        for (auto& word : words) word = read(adc, 0);
        const auto selected = ptr < 4 ? 8 : ptr < 8 ? 0 : 4;
        assert(uint16_t(words[selected + 1] - words[selected]) >> 7 ==
               uint16_t(next.normal[1] - next.normal[0]) >> 7);
        assert(uint16_t(words[selected + 2] - words[selected]) >> 7 ==
               uint16_t(next.normal[2] - next.normal[0]) >> 7);
        assert(read(adc, 0x1c) == 0x80); write(adc, 0x1c, 0x80);
        inputs = next;
        // Refill the physical ring before the next overwrite test.
        adc.advance(12 * penSlot + 3 * penIdle);
    }
    // A full ring's partial read clears PFF but preserves POV until W1C.
    adc.advance(penSlot); assert(read(adc, 0x1c) == 0x83);
    read(adc, 0); assert(read(adc, 0x1c) == 0x81);
    write(adc, 0x1c, 0x80); assert(read(adc, 0x1c) == 1);
    adc.advance(penSlot); assert((read(adc, 0x1c) & 0x83) == 3);
    // Release becomes visible at a subsequently captured U slot, without PUIE.
    inputs.contactDown = false; assert(adc.replacePenInputs(inputs) == Imx1AspAccess::ok);
    adc.advance(4 * penSlot + penIdle); assert(read(adc, 0x1c) & 0x400);
    assert(!(adc.assertedInterrupts() & (uint64_t(1) << 5)));
    write(adc, 0x18, 0x400); assert(adc.assertedInterrupts() == (uint64_t(1) << 5));
    write(adc, 0x1c, 0x400); assert(!(read(adc, 0x1c) & 0x400));
    adc.advance(4 * penSlot + penIdle); assert(read(adc, 0x1c) & 0x400);
    write(adc, 0x18, 0); assert(adc.assertedInterrupts() == 0);
    write(adc, 0x10, 1u << 23); assert(read(adc, 0x1c) == 0);
}
void penBoardTransaction() {
    Imx1Bootstrap board;
    auto inputs = penStimulus(false);
    assert(board.replaceAspPenInputs(inputs) == Imx1BootstrapAccess::ok);
    auto store = [&](uint32_t address, uint32_t value) { return board.access(address, 4, true, value); };
    auto status = [&]() { uint32_t value = 0; assert(board.access(base + 0x1c, 4, false, value) == Imx1BootstrapAccess::ok); return value; };
    assert(store(base + 0x18, 0x10) == Imx1BootstrapAccess::ok);
    assert(store(0x00223008, 46) == Imx1BootstrapAccess::ok);
    inputs.contactDown = true;
    assert(board.aspPenWakePossible(inputs));
    assert(board.replaceAspPenInputs(inputs) == Imx1BootstrapAccess::sideEffect);
    assert(status() == 0);
    board.connectCpuInterrupts();
    assert(board.replaceAspPenInputs(inputs, false) == Imx1BootstrapAccess::ok && status() == 0);
    assert(board.replaceAspPenInputs(inputs) == Imx1BootstrapAccess::ok && status() == 0x40);
    assert(board.interruptLines().irq == Imx1AitcLine::asserted);
    assert(store(base + 0x1c, 0x40) == Imx1BootstrapAccess::ok && status() == 0x40);
    inputs.contactDown = false; assert(board.replaceAspPenInputs(inputs) == Imx1BootstrapAccess::ok);
    assert(store(base + 0x1c, 0x40) == Imx1BootstrapAccess::ok && status() == 0);
    // An eligible unknown source prevents the complete input transaction.
    assert(store(0x00223008, 4) == Imx1BootstrapAccess::externalInput);
    inputs.contactDown = true;
    assert(board.replaceAspPenInputs(inputs) == Imx1BootstrapAccess::ok && status() == 0x40);
}


void penReleaseCaptureAndWake() {
    Imx1Asp adc;
    auto inputs = penStimulus();
    assert(adc.replacePenInputs(inputs) == Imx1AspAccess::ok); startAuto(adc);
    adc.advance(3 * penSlot); // U has captured contact down.
    inputs.contactDown = false; assert(adc.replacePenInputs(inputs) == Imx1AspAccess::ok);
    adc.advance(penSlot); assert(!(read(adc, 0x1c) & 0x400));
    adc.advance(penIdle + 4 * penSlot - 1); assert(!(read(adc, 0x1c) & 0x400));
    adc.advance(1); assert(read(adc, 0x1c) & 0x400);
    Imx1Bootstrap board;
    assert(board.replaceAspPenInputs(inputs) == Imx1BootstrapAccess::ok);
    auto store = [&](uint32_t address, uint32_t value) { return board.access(address, 4, true, value); };
    assert(store(0x00223008, 5) == Imx1BootstrapAccess::ok);
    assert(store(base + 0x2c, 3) == Imx1BootstrapAccess::ok);
    assert(store(base + 0x14, 0x3814) == Imx1BootstrapAccess::ok);
    assert(store(base + 0x10, 0x0200e001) == Imx1BootstrapAccess::ok);
    assert(store(base + 0x10, 0x0200e003) == Imx1BootstrapAccess::ok);
    assert(!board.idleWakePossible());
    board.advanceHclkCycles(4 * penSlot);
    assert(board.clockStatus() == Imx1BootstrapAccess::ok && board.interruptLines().irq == Imx1AitcLine::clear);
    assert(store(base + 0x18, 0x400) == Imx1BootstrapAccess::sideEffect);
    board.connectCpuInterrupts();
    assert(store(base + 0x18, 0x400) == Imx1BootstrapAccess::ok);
    assert(board.interruptLines().irq == Imx1AitcLine::asserted);
    assert(store(base + 0x1c, 0x400) == Imx1BootstrapAccess::ok);
    assert(board.interruptLines().irq == Imx1AitcLine::clear && board.idleWakePossible());
    assert(store(base + 0x10, 0x0000e003) == Imx1BootstrapAccess::ok);
    assert(!board.idleWakePossible());
    assert(store(base + 0x10, 0x0200e003) == Imx1BootstrapAccess::ok && board.idleWakePossible());
    board.advanceHclkCycles(penIdle + 4 * penSlot);
    assert(board.clockStatus() == Imx1BootstrapAccess::ok && board.interruptLines().irq == Imx1AitcLine::asserted);
    Imx1Asp heldHigh(true);
    write(heldHigh, 0x18, 0x10);
    inputs.contactKnown = false;
    assert(heldHigh.replacePenInputs(inputs) == Imx1AspAccess::externalInput);
}


void penComparisons() {
    for (bool acal : {false, true}) {
        for (unsigned selected = 0; selected < 4; ++selected) {
            Imx1Asp adc;
            assert(adc.replacePenInputs(penStimulus()) == Imx1AspAccess::ok);
            write(adc, 0x30, 0x40001 | selected << 16);
            startAuto(adc, acal);
            for (unsigned slot = 0; slot < (acal ? 3u : 4u); ++slot) {
                adc.advance(penSlot - 1);
                const bool before = selected && selected < (acal ? 3u : 4u) && selected < slot;
                assert(bool(read(adc, 0x30) & 0x80000) == before);
                adc.advance(1);
                const bool after = selected && selected < (acal ? 3u : 4u) && selected <= slot;
                assert(bool(read(adc, 0x30) & 0x80000) == after);
                assert(adc.assertedInterrupts() == (after ? uint64_t(1) << 9 : 0));
            }
        }
    }
}

}
// The battery routine's restore with the pen down: one write sets the mode and PADE together.
void oneWriteRestore() {
    Imx1AspPenInputs inputs;
    inputs.contactKnown = inputs.contactDown = inputs.normalKnown = inputs.referenceKnown = true;
    inputs.normal = {0x1100, 0x7580, 0x7880, 0x1234};
    const auto prepared = [&](uint32_t rate = 0x3814, uint32_t interrupts = 0x10, uint32_t divider = 99) {
        Imx1Asp adc(true);
        assert(adc.replacePenInputs(inputs) == Imx1AspAccess::ok);
        // Sampling as the pen driver leaves it, then the routine's reset and rewrites.
        write(adc, 0x2c, 3); write(adc, 0x14, 0x3814); write(adc, 0x18, 0x10);
        write(adc, 0x10, 0x0200e001); write(adc, 0x10, 0x0200e003); adc.advance(40000000);
        assert(read(adc, 0x1c) & 1);
        write(adc, 0x10, 0x0200e001); write(adc, 0x10, 0x0280e001);
        assert(read(adc, 0x10) == 0x02000000 && !(read(adc, 0x1c) & 3) && read(adc, 0x34) == 0);
        write(adc, 0x2c, divider); write(adc, 0x14, rate); write(adc, 0x18, interrupts);
        return adc;
    };
    {
        auto adc = prepared();
        write(adc, 0x10, 0x0200e003);
        assert(read(adc, 0x10) == 0x0200e003 && adc.converting() && !(read(adc, 0x1c) & 3) && read(adc, 0x34) == 0);
        // The same samples, in the same time, as the mode write followed by the PADE write.
        auto two = prepared();
        write(two, 0x10, 0x0200e001); write(two, 0x10, 0x0200e003);
        for (unsigned step = 0; step < 400; ++step) {
            adc.advance(100000); two.advance(100000);
            assert(read(adc, 0x1c) == read(two, 0x1c) && read(adc, 0x34) == read(two, 0x34));
        }
        assert(read(adc, 0x1c) & 2);
        for (const uint16_t expected : inputs.normal) assert(read(adc, 0) == expected && read(two, 0) == expected);
    }
    // Any other word in one write, or the word on a module the routine didn't leave, still refuses.
    for (const uint32_t value : {0x0200e002u, 0x0200d003u, 0x0200f003u, 0x0000e003u, 0x0600d203u, 0x02003003u, 0x0200e203u}) {
        auto adc = prepared();
        rejected(adc, 0x10, value, Imx1AspAccess::sideEffect);
    }
    { auto adc = prepared(0x1814); rejected(adc, 0x10, 0x0200e003, Imx1AspAccess::sideEffect); }
    { auto adc = prepared(0x3814, 0); rejected(adc, 0x10, 0x0200e003, Imx1AspAccess::sideEffect); }
    { auto adc = prepared(0x3814, 0x10, 4); rejected(adc, 0x10, 0x0200e003, Imx1AspAccess::sideEffect); }
    {
        auto adc = prepared();
        write(adc, 0x30, 0x10100);
        rejected(adc, 0x10, 0x0200e003, Imx1AspAccess::sideEffect);
    }
    {
        // Unknown pen samples refuse it the way they refuse the two writes.
        auto adc = prepared();
        auto unknown = inputs; unknown.normalKnown = false;
        assert(adc.replacePenInputs(unknown) == Imx1AspAccess::ok);
        rejected(adc, 0x10, 0x0200e003, Imx1AspAccess::externalInput);
    }
}
int main() {
    penComparisons(); penReleaseCaptureAndWake(); penContact(); penSampling(); penCaptureAndClocks(); penRingAndRelease(); penBoardTransaction();
    registersAndReset(); clocksAndInputGuards(); heldHighDetector(); bootstrapInputsAndTime(); manualUConversion(); manualUComparisons(); manualUInterruptGate();
    std::puts("i.MX1 ASP configuration, reset, clock/input guards, held-high PX1 and inactive IRQs passed");
    oneWriteRestore();
}
