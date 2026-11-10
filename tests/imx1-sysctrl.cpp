#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "imx1_bootstrap.h"

int main() {
    constexpr uint32_t gccr = 0x0021b810;
    Imx1Bootstrap soc;
    uint32_t setup = 0x57000003;
    assert(soc.access(0x0021b000, 4, true, setup) == Imx1BootstrapAccess::ok);
    setup = 2;
    assert(soc.access(0x0021b020, 4, true, setup) == Imx1BootstrapAccess::ok);
    auto read = [&](uint32_t address, uint32_t expected) {
        uint32_t value = 0xdeadbeef;
        assert(soc.access(address, 4, false, value, 3) == Imx1BootstrapAccess::ok);
        assert(value == expected);
    };
    auto write = [&](uint32_t address, uint32_t value,
                     Imx1BootstrapAccess expected = Imx1BootstrapAccess::ok) {
        assert(soc.access(address, 4, true, value, 3) == expected);
    };
    assert(!std::strcmp(Imx1Bootstrap::registerName(gccr), "SCM.GCCR"));
    read(gccr, 0x2f);
    // Each peripheral enable retains configuration, without supplying a device response.
    for (unsigned bit : {0u, 1u, 2u, 3u, 5u, 6u}) {
        write(gccr, 1u << bit);
        read(gccr, 1u << bit);
    }
    write(gccr, 0x6f);
    for (unsigned bit = 7; bit < 32; ++bit) {
        write(gccr, 0x6f | (1u << bit), Imx1BootstrapAccess::reservedBits);
        read(gccr, 0x6f);
    }
    // An unqualified BROM override rejects the whole combined change.
    write(gccr, 0x10, Imx1BootstrapAccess::externalInput);
    read(gccr, 0x6f);
    uint32_t unknown = 0xdeadbeef;
    for (uint32_t size : {0u, 1u, 2u, 8u}) {
        assert(soc.access(gccr, size, false, unknown, 3) == Imx1BootstrapAccess::width);
        assert(soc.access(gccr, size, true, unknown, 3) == Imx1BootstrapAccess::width);
    }
    for (unsigned offset = 1; offset < 4; ++offset) {
        assert(soc.access(gccr + offset, 4, false, unknown, 3) == Imx1BootstrapAccess::width);
        assert(soc.access(gccr + offset, 4, true, unknown, 3) == Imx1BootstrapAccess::width);
    }
    assert(unknown == 0xdeadbeef);
    read(gccr, 0x6f);
    // The reset module's RSR: 0 after power-on, EXR after RESET_IN, WDR after a watchdog time-out
    // (MC9328MX1RM table 6-3). Read-only, and only the 32-bit read is answered.
    constexpr uint32_t rsr = 0x0021b800;
    assert(!std::strcmp(Imx1Bootstrap::registerName(rsr), "RST.RSR"));
    read(rsr, 0);
    assert(soc.resetSource() == Imx1ResetSource::powerOn);
    soc.setResetSource(Imx1ResetSource::external); read(rsr, 1);
    soc.setResetSource(Imx1ResetSource::watchdog); read(rsr, 2);
    for (uint32_t value : {0u, 1u, 3u, 0xffffffffu}) write(rsr, value, Imx1BootstrapAccess::sideEffect);
    read(rsr, 2);
    for (uint32_t size : {0u, 1u, 2u, 8u}) {
        unknown = 0xdeadbeef;
        assert(soc.access(rsr, size, false, unknown, 3) == Imx1BootstrapAccess::width && unknown == 0xdeadbeef);
    }
    for (unsigned offset = 1; offset < 4; ++offset)
        assert(soc.access(rsr + offset, 4, false, unknown, 3) == Imx1BootstrapAccess::width);
    soc.setResetSource(Imx1ResetSource::powerOn); read(rsr, 0);
    for (auto address : {0x0021b804u, 0x0021b814u,
                         0x00208010u, 0x00212000u, 0x00222000u}) {
        unknown = 0xdeadbeef;
        assert(soc.access(address, 4, false, unknown, 3) == Imx1BootstrapAccess::unmapped);
        assert(unknown == 0xdeadbeef);
        assert(!soc.instructionWord(address, unknown));
    }
    assert(!soc.instructionWord(gccr, unknown));
    assert(!soc.instructionRead(gccr, 2, unknown));
    read(gccr, 0x6f);
    constexpr uint32_t gpcr = 0x0021b80c;
    assert(!std::strcmp(Imx1Bootstrap::registerName(gpcr), "SCM.GPCR"));
    read(gpcr, 0x3fb);
    // Each two-bit drive field retains all four settings, with no pad input or clock.
    for (unsigned shift : {4u, 6u, 8u, 10u}) {
        for (uint32_t field = 0; field < 4; ++field) {
            const uint32_t setting = (0x3fb & ~(3u << shift)) | (field << shift);
            write(gpcr, setting);
            read(gpcr, setting);
        }
    }
    write(gpcr, 0x3fb);
    // Native MMCSD startup clears DS_SLOW then selects 26 MHz/30 pF.
    write(gpcr, 0x3fb & ~0xc00u); read(gpcr, 0x3fb);
    write(gpcr, 0x7fb); read(gpcr, 0x7fb);
    for (unsigned bit = 0; bit < 32; ++bit) {
        if ((1u << bit) & 0x9ffb) continue;
        write(gpcr, 0x3fb | (1u << bit), Imx1BootstrapAccess::reservedBits);
        read(gpcr, 0x7fb);
    }
    write(gpcr, 0x3f3, Imx1BootstrapAccess::reservedBits);
    for (uint32_t effect : {1u, 2u, 0x8000u}) {
        write(gpcr, 0x3fb ^ effect, Imx1BootstrapAccess::sideEffect);
        read(gpcr, 0x7fb);
    }
    // BTAEN gates the BTA clock inputs (table 8-4). The ROM's Bluetooth power-up sets it at 200fc8c8.
    // The BTA answers only with BTAEN set, and then only the two registers the ROM's radio probe uses.
    constexpr uint32_t wakeControl = 0x00216110, clockControl = 0x00216118;
    unknown = 0;
    assert(soc.access(clockControl, 4, false, unknown) == Imx1BootstrapAccess::unmapped);
    write(wakeControl, 0, Imx1BootstrapAccess::unmapped);
    write(gpcr, 0x17fb); read(gpcr, 0x17fb);
    assert(soc.access(0x00216000, 4, false, unknown) == Imx1BootstrapAccess::unmapped);
    assert(soc.access(wakeControl, 4, false, unknown) == Imx1BootstrapAccess::unmapped);
    write(wakeControl, 0);
    write(wakeControl, 0x10, Imx1BootstrapAccess::sideEffect);
    write(wakeControl, 8, Imx1BootstrapAccess::sideEffect);
    write(wakeControl, 0x20, Imx1BootstrapAccess::reservedBits);
    read(clockControl, 0);
    write(clockControl, 0x6221); read(clockControl, 0x6221);
    // The ROM's radio probe polls INTSRCH for BTSYS (source 55, bit 23) at 200fc8dc. With the port D
    // pins and the card slot known every source in the upper word has a level: the modules still in reset are low (SSI2,
    // SPI1, SPI2, USBD, UART3 RX), and no radio answers. The lower word still has sources with none.
    assert(soc.setGpioInput(3, 0));
    unknown = 0xdeadbeef;
    assert(soc.access(0x00223048, 4, false, unknown) == Imx1BootstrapAccess::externalInput && unknown == 0xdeadbeef);
    soc.setMmcsdEmptySlot(true);
    read(0x00223048, 0);
    unknown = 0xdeadbeef;
    assert(soc.access(0x0022304c, 4, false, unknown) == Imx1BootstrapAccess::externalInput && unknown == 0xdeadbeef);
    write(clockControl, 0x1000, Imx1BootstrapAccess::reservedBits);
    write(clockControl, 0x10000, Imx1BootstrapAccess::reservedBits);
    read(clockControl, 0x6221);
    unknown = 0x6221;
    assert(soc.access(clockControl, 2, true, unknown) == Imx1BootstrapAccess::width);
    write(clockControl, 0); read(clockControl, 0);
    write(gpcr, 0x7fb); read(gpcr, 0x7fb);
    for (uint32_t size : {0u,1u,2u,8u}) {
        unknown = 0xdeadbeef;
        assert(soc.access(gpcr,size,false,unknown) == Imx1BootstrapAccess::width);
        assert(soc.access(gpcr,size,true,unknown) == Imx1BootstrapAccess::width);
        assert(unknown == 0xdeadbeef);
    }
    for (unsigned lane = 1; lane < 4; ++lane) {
        assert(soc.access(gpcr + lane,4,false,unknown) == Imx1BootstrapAccess::width);
        assert(soc.access(gpcr + lane,4,true,unknown) == Imx1BootstrapAccess::width);
    }
    assert(!soc.instructionWord(gpcr, unknown));
    assert(!soc.instructionRead(gpcr, 2, unknown));
    auto pinsCopy = soc;
    write(gpcr, 0x3fb);
    assert(pinsCopy.access(gpcr,4,false,unknown) == Imx1BootstrapAccess::ok && unknown == 0x7fb);
    Imx1Bootstrap pinsReset;
    assert(pinsReset.access(gpcr,4,false,unknown) == Imx1BootstrapAccess::ok && unknown == 0x3fb);
    // Table 8-3 input selectors retain configuration, without supplying pin levels.
    constexpr uint32_t fmcr = 0x0021b808;
    assert(!std::strcmp(Imx1Bootstrap::registerName(fmcr), "SCM.FMCR"));
    read(fmcr, 3);
    for (unsigned bit = 3; bit <= 8; ++bit) {
        write(fmcr, 3 | (1u << bit));
        read(fmcr, 3 | (1u << bit));
    }
    write(fmcr, 0x1fb);
    for (unsigned bit = 9; bit < 32; ++bit) {
        write(fmcr, 3 | (1u << bit), Imx1BootstrapAccess::reservedBits);
        read(fmcr, 0x1fb);
    }
    for (uint32_t changed : {0u, 1u, 2u}) {
        write(fmcr, changed, Imx1BootstrapAccess::sideEffect);
        read(fmcr, 0x1fb);
    }
    write(fmcr, 7, Imx1BootstrapAccess::externalInput);
    read(fmcr, 0x1fb);
    for (uint32_t size : {0u,1u,2u,8u}) {
        assert(soc.access(fmcr,size,false,unknown) == Imx1BootstrapAccess::width);
        assert(soc.access(fmcr,size,true,unknown) == Imx1BootstrapAccess::width);
    }
    for (unsigned lane = 1; lane < 4; ++lane)
        assert(soc.access(fmcr + lane,4,false,unknown) == Imx1BootstrapAccess::width);
    assert(!soc.instructionWord(fmcr, unknown));
    Imx1Bootstrap fresh;
    unknown = 0xdeadbeef;
    assert(fresh.access(fmcr,4,false,unknown) == Imx1BootstrapAccess::ok && unknown == 3);
    auto copied = soc;
    unknown = 0;
    assert(copied.access(fmcr,4,false,unknown) == Imx1BootstrapAccess::ok && unknown == 0x1fb);
    write(fmcr,3);
    // Table 8-5 does not gate Timer 1. Retained and rejected writes leave its phase alone.
    write(0x0021b000, 0x57000003);
    write(0x0021b020, 2);
    write(0x00202004, 31);
    write(0x00202000, 0x103);
    soc.advanceFastBusCycles(95);
    read(0x00202010, 0);
    write(gccr, 0);
    for (unsigned i = 0; i < 100; ++i) read(gccr, 0);
    write(gccr, 0x1f, Imx1BootstrapAccess::externalInput);
    write(gccr, 0x80, Imx1BootstrapAccess::reservedBits);
    write(gpcr, 0xffb);
    read(gpcr, 0xffb);
    write(gpcr, 0x93fb, Imx1BootstrapAccess::sideEffect);
    soc.advanceFastBusCycles(1);
    read(0x00202010, 1);
    write(gccr, 0x6f);
    soc.advanceFastBusCycles(96);
    read(0x00202010, 2);
    // SCM access must not weaken the CGM oscillator guards.
    write(0x00202000, 0);
    write(0x0021b000, 0x57000002, Imx1BootstrapAccess::sideEffect);
    read(0x0021b000, 0x57000003);
    std::puts("PASS: SCM GCCR/FMCR/GPCR reset/configuration, transactional guards, fetch isolation and Timer 1 phase");
}
