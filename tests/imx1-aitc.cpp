#include <cassert>
#include <cstdint>
#include <cstdio>

#include "imx1_aitc.h"
#include "imx1_bootstrap.h"

namespace {
constexpr uint32_t base = 0x00223000;
uint32_t read(Imx1Aitc& a, unsigned offset) {
    uint32_t value = 0xdeadbeef;
    assert(a.access(base + offset, 4, false, value) == Imx1AitcAccess::ok);
    return value;
}
void write(Imx1Aitc& a, unsigned offset, uint32_t value) {
    assert(a.access(base + offset, 4, true, value) == Imx1AitcAccess::ok);
}
void resetAndDecode() {
    Imx1Aitc a;
    for (unsigned offset = 0; offset <= 0x64; offset += 4) {
        assert(Imx1Aitc::registerName(base + offset));
        if (offset == 0x48 || offset == 0x4c) continue;
        const auto expected = offset == 4 ? 31u : offset == 0x40 || offset == 0x44 ? 0xffffffffu : 0u;
        assert(read(a, offset) == expected);
    }
    uint32_t value = 0xdeadbeef;
    assert(a.access(base + 0x48, 4, false, value) == Imx1AitcAccess::externalInput);
    assert(value == 0xdeadbeef);
    for (unsigned size : {1u, 2u, 8u})
        assert(a.access(base + 0x10, size, true, value) == Imx1AitcAccess::width);
    assert(a.access(base + 0x11, 4, true, value) == Imx1AitcAccess::width);
    assert(a.access(base + 0x68, 4, true, value) == Imx1AitcAccess::unmapped);
    for (unsigned offset : {0u, 4u, 8u, 12u}) {
        value = 0x80000000;
        assert(a.access(base + offset, 4, true, value) == Imx1AitcAccess::reservedBits);
    }
    write(a, 0, 0x00180000); assert(read(a, 0) == 0x00180000);
    assert(read(a, 0x10) == 0 && read(a, 0x14) == 0);
    for (unsigned source = 0; source < 64; ++source) {
        write(a, 8, source);
        assert(read(a, source < 32 ? 0x14 : 0x10) & (1u << (source % 32)));
        assert(read(a, 8) == 0);
    }
    assert(read(a, 0x10) == 0xffffffff && read(a, 0x14) == 0xffffffff);
    for (unsigned source = 0; source < 64; ++source) {
        write(a, 12, source);
        assert(!(read(a, source < 32 ? 0x14 : 0x10) & (1u << (source % 32))));
        assert(read(a, 12) == 0);
    }
    assert(read(a, 0x10) == 0 && read(a, 0x14) == 0);
}
void prioritiesAndForces() {
    Imx1Aitc a;
    a.setInputs(0, ~uint64_t(0));
    // Test every priority nibble, both halves and equal-level source ordering.
    write(a, 0x10, 0xffffffff); write(a, 0x14, 0xffffffff);
    write(a, 0x50, 0xffffffff); write(a, 0x54, 0xffffffff);
    assert(read(a, 0x40) == 0x003f0000);
    assert(read(a, 0x58) == 0xffffffff && read(a, 0x5c) == 0xffffffff);
    for (unsigned source = 0; source < 64; ++source) {
        const unsigned offset = 0x3c - (source / 8) * 4;
        write(a, offset, 15u << ((source % 8) * 4));
        assert(read(a, 0x40) == (source << 16 | 15));
        assert(a.irq() == Imx1AitcLine::asserted);
        write(a, 4, 15); assert(a.irq() == Imx1AitcLine::clear);
        assert(read(a, 0x40) == 0xffffffff);
        write(a, 4, 14); assert(read(a, 0x40) == (source << 16 | 15));
        write(a, 4, 0xffffffff); assert(read(a, 4) == 31);
        write(a, offset, 0);
    }
    write(a, 4, 0); assert(a.irq() == Imx1AitcLine::clear);
    for (unsigned mask : {16u, 30u}) {
        write(a, 4, mask); assert(a.irq() == Imx1AitcLine::clear);
    }
    write(a, 4, 31);
    write(a, 0x18, 0x80000001); write(a, 0x1c, 0x80000001);
    assert(a.fiq() == Imx1AitcLine::asserted && read(a, 0x44) == 63);
    assert(read(a, 0x60) == 0x80000001 && read(a, 0x64) == 0x80000001);
    assert(read(a, 0x58) == 0x7ffffffe && read(a, 0x5c) == 0x7ffffffe);
    assert(read(a, 0x40) == 0x003e0000);
    // Reading vectors and writing read-only registers never clears a level request.
    for (unsigned offset : {0x40u, 0x44u, 0x48u, 0x4cu, 0x58u, 0x5cu, 0x60u, 0x64u}) write(a, offset, 0);
    assert(read(a, 0x44) == 63 && read(a, 0x48) == 0xffffffff);
    write(a, 0x50, 0); assert(read(a, 0x44) == 31);
    write(a, 0x54, 0); assert(a.irq() == Imx1AitcLine::clear && a.fiq() == Imx1AitcLine::clear);
    // A peripheral must lower its own request; clearing FORCE cannot acknowledge it.
    a.setInputs(uint64_t(1) << 63, ~uint64_t(0));
    assert(read(a, 0x44) == 63);
    write(a, 0x50, 0); assert(read(a, 0x44) == 63);
    a.setInputs(0, ~uint64_t(0)); assert(read(a, 0x44) == 0xffffffff);
}
void unknownAndBootstrap() {
    Imx1Aitc a;
    assert(a.irq() == Imx1AitcLine::clear);
    write(a, 8, 3); assert(a.irq() == Imx1AitcLine::unknown);
    uint32_t value = 0xdeadbeef;
    assert(a.access(base + 0x40, 4, false, value) == Imx1AitcAccess::externalInput && value == 0xdeadbeef);
    write(a, 4, 0); assert(a.irq() == Imx1AitcLine::clear);
    write(a, 0x1c, 8); assert(a.fiq() == Imx1AitcLine::unknown);
    write(a, 0x54, 8); assert(a.fiq() == Imx1AitcLine::asserted);
    assert(read(a, 0x64) == 8 && read(a, 0x44) == 3);
    Imx1Aitc reserved;
    reserved.setInputs(~uint64_t(0), ~uint64_t(0));
    assert(read(reserved, 0x48) == 0xfffffffe && read(reserved, 0x4c) == 0x7fffffff);
    Imx1Bootstrap b;
    auto access = [&](unsigned offset, bool write, uint32_t value, Imx1BootstrapAccess status) {
        assert(b.access(base + offset, 4, write, value) == status);
        return value;
    };
    access(0x10, true, 0, Imx1BootstrapAccess::ok);
    access(0x14, true, 0, Imx1BootstrapAccess::ok);
    // Source 6 (CSI) has no model. UART3's sources 0 to 4 are known low in reset since patch 0748.
    access(8, true, 3, Imx1BootstrapAccess::ok);
    access(0xc, true, 3, Imx1BootstrapAccess::ok);
    access(8, true, 6, Imx1BootstrapAccess::externalInput);
    assert(access(0x14, false, 0, Imx1BootstrapAccess::ok) == 0);
    access(0x54, true, 0x80000000, Imx1BootstrapAccess::ok);
    access(8, true, 31, Imx1BootstrapAccess::sideEffect);
    assert(access(0x14, false, 0, Imx1BootstrapAccess::ok) == 0);
    access(0x54, true, 0, Imx1BootstrapAccess::ok);
    access(8, true, 31, Imx1BootstrapAccess::ok);
    access(0x54, true, 0x80000000, Imx1BootstrapAccess::sideEffect);
    assert(access(0x54, false, 0, Imx1BootstrapAccess::ok) == 0);
    access(4, true, 0, Imx1BootstrapAccess::ok);
    access(0x54, true, 0x80000000, Imx1BootstrapAccess::ok);
    access(4, true, 31, Imx1BootstrapAccess::sideEffect);
    assert(access(4, false, 0, Imx1BootstrapAccess::ok) == 0);
    value = 0;
    assert(!b.instructionWord(base + 0x40, value));
}
void maskedGpioRequests() {
    constexpr unsigned sources[] = {11, 12, 13, 62};
    constexpr uint64_t gpioMask = (uint64_t(7) << 11) | (uint64_t(1) << 62);
    Imx1Bootstrap b;
    auto access = [&](unsigned offset, bool write, uint32_t value, Imx1BootstrapAccess status) {
        assert(b.access(base + offset, 4, write, value) == status);
        return value;
    };
    // Sources with no known level in reset. The rest are known: the DMA's 60 and 61 (patch 0677), SSI1's 42 to 45
    // (0679), UART1 and UART2 (0722), the BTA's 55 to 57 (0746), and UART3, SSI2, SPI and USBD in reset (0748).
    // 27 is UART1's missing DTR and 35 the MMC/SD module before discovery.
    for (unsigned source : {6u, 7u, 8u, 10u, 14u, 15u, 16u, 27u, 35u}) {
        access(8, true, source, Imx1BootstrapAccess::externalInput);
        assert(access(0x10, false, 0, Imx1BootstrapAccess::ok) == 0);
        assert(access(0x14, false, 0, Imx1BootstrapAccess::ok) == 0);
    }
    for (unsigned port = 0; port < 4; ++port) {
        const uint32_t gpioBase = 0x0021c000 + port * 0x100;
        uint32_t value = 0xdeadbeef;
        assert(b.access(gpioBase + 0x24, 4, false, value) == Imx1BootstrapAccess::externalInput);
        assert(value == 0xdeadbeef);
        access(8, true, sources[port], Imx1BootstrapAccess::ok);
        const unsigned force = port == 3 ? 0x50 : 0x54;
        const uint32_t bit = 1u << (sources[port] % 32);
        access(force, true, bit, Imx1BootstrapAccess::sideEffect);
        assert(access(force, false, 0, Imx1BootstrapAccess::ok) == 0);
        for (auto mask : {1u, 0x80000000u, 0xffffffffu}) {
            value = mask;
            const auto expected = mask == 0x80000000 && (port == 1 || port == 3)
                ? Imx1BootstrapAccess::externalInput : Imx1BootstrapAccess::sideEffect;
            assert(b.access(gpioBase + 0x30, 4, true, value) == expected);
            assert(b.access(gpioBase + 0x30, 4, false, value) == Imx1BootstrapAccess::ok && value == 0);
        }
        value = 0xffffffff;
        assert(b.access(gpioBase + 0x34, 4, true, value) == Imx1BootstrapAccess::ok);
        value = 0;
        assert(b.access(gpioBase + 0x30, 4, true, value) == Imx1BootstrapAccess::ok);
    }
    assert(access(0x14, false, 0, Imx1BootstrapAccess::ok) == uint32_t(gpioMask));
    assert(access(0x10, false, 0, Imx1BootstrapAccess::ok) == uint32_t(gpioMask >> 32));
    for (auto type : {uint64_t(0), gpioMask}) {
        access(0x18, true, type >> 32, Imx1BootstrapAccess::ok);
        access(0x1c, true, type, Imx1BootstrapAccess::ok);
        for (unsigned offset : {0x58u, 0x5cu, 0x60u, 0x64u})
            assert(access(offset, false, 0, Imx1BootstrapAccess::ok) == 0);
        for (unsigned offset : {0x40u, 0x44u})
            assert(access(offset, false, 0, Imx1BootstrapAccess::ok) == 0xffffffff);
        access(0x50, true, uint32_t(1) << 30, Imx1BootstrapAccess::sideEffect);
        access(0x54, true, 1u << 11, Imx1BootstrapAccess::sideEffect);
        assert(access(0x50, false, 0, Imx1BootstrapAccess::ok) == 0);
        assert(access(0x54, false, 0, Imx1BootstrapAccess::ok) == 0);
    }
    // Whole source words still depend on the other peripheral inputs.
    for (unsigned offset : {0x48u, 0x4cu})
        assert(access(offset, false, 0xdeadbeef, Imx1BootstrapAccess::externalInput) == 0xdeadbeef);
    // Forcing only the other sources makes each whole source word observable.
    access(0x50, true, ~uint32_t(gpioMask >> 32), Imx1BootstrapAccess::ok);
    access(0x54, true, ~uint32_t(gpioMask), Imx1BootstrapAccess::ok);
    assert(access(0x48, false, 0, Imx1BootstrapAccess::ok) == ~uint32_t(gpioMask >> 32));
    assert(access(0x4c, false, 0, Imx1BootstrapAccess::ok) == ~uint32_t(gpioMask));
    uint32_t value = 0;
    assert(!b.instructionWord(base + 0x40, value));
    assert(access(0x44, false, 0, Imx1BootstrapAccess::ok) == 0xffffffff);
}
}
int main() {
    resetAndDecode(); prioritiesAndForces(); unknownAndBootstrap(); maskedGpioRequests();
    std::puts("PASS: AITC registers, priorities, level requests, explicit unknown inputs and transactional bootstrap guards");
}
