#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "imx1_bootstrap.h"

namespace {
using W = Imx1WatchdogAccess;
using B = Imx1BootstrapAccess;
constexpr uint32_t base = 0x00201000, aitc = 0x00223000;
uint32_t read(Imx1Watchdog& watchdog, unsigned offset) {
    uint32_t value = 0xdeadbeef;
    assert(watchdog.access(base + offset, 4, false, value) == W::ok);
    return value;
}
void write(Imx1Watchdog& watchdog, unsigned offset, uint32_t value, W expected = W::ok) {
    const auto argument = value;
    assert(watchdog.access(base + offset, 4, true, value) == expected && value == argument);
}
uint32_t read(Imx1Bootstrap& soc, uint32_t address) {
    uint32_t value = 0xdeadbeef;
    assert(soc.access(address, 4, false, value) == B::ok);
    return value;
}
void write(Imx1Bootstrap& soc, uint32_t address, uint32_t value, B expected = B::ok) {
    const auto argument = value;
    assert(soc.access(address, 4, true, value) == expected && value == argument);
}
void registersAndGuards() {
    Imx1Watchdog watchdog;
    for (unsigned offset : {0u, 4u, 8u}) {
        assert(Imx1Watchdog::registerName(base + offset));
        assert(read(watchdog, offset) == 0);
        for (unsigned size : {0u, 1u, 2u, 8u}) for (bool write : {false, true}) {
            uint32_t value = 0xdeadbeef;
            assert(watchdog.access(base + offset, size, write, value) == W::width && value == 0xdeadbeef);
        }
        for (unsigned byte = 1; byte < 4; ++byte) for (bool write : {false, true}) {
            uint32_t value = 0xdeadbeef;
            assert(watchdog.access(base + offset + byte, 4, write, value) == W::width && value == 0xdeadbeef);
        }
    }
    uint32_t value = 0xdeadbeef;
    assert(!Imx1Watchdog::registerName(base + 12));
    assert(watchdog.access(base + 12, 4, false, value) == W::unmapped && value == 0xdeadbeef);
    assert(watchdog.access(base + 12, 4, true, value) == W::unmapped && value == 0xdeadbeef);
    for (uint32_t bad : {0x20u, 0x40u, 0x80u, 0x10000u, 0x80000000u}) {
        write(watchdog, 0, bad, W::reservedBits);
        assert(read(watchdog, 0) == 0);
    }
    write(watchdog, 4, 0x10000, W::reservedBits);
    write(watchdog, 8, 0, W::sideEffect);
    write(watchdog, 8, 0xffffffff, W::sideEffect);
    assert(read(watchdog, 4) == 0 && read(watchdog, 8) == 0);
    // Rejected enable/reset requests consume neither configuration nor the WDE write.
    write(watchdog, 0, 0x7f01, W::sideEffect);
    write(watchdog, 0, 0x7f07, W::resetRequest);
    write(watchdog, 0, 4, W::resetRequest);
    assert(read(watchdog, 0) == 0);
    write(watchdog, 0, 1, W::sideEffect);
    // The first disabled write leaves WDE at zero in write-once mode.
    write(watchdog, 0, 0x7f10);
    write(watchdog, 0, 0x8019);
    assert(read(watchdog, 0) == 0x8018);
    write(watchdog, 0, 0x1204, W::resetRequest);
    assert(read(watchdog, 0) == 0x8018);
    write(watchdog, 0, 0x7f13, W::sideEffect); // WDEC permits another WDE write.
    assert(read(watchdog, 0) == 0x8018);
    write(watchdog, 0, 0x7f1a); // Disabled WHALT, WT, WIE and test-clock configuration.
    assert(read(watchdog, 0) == 0x7f1a);
    write(watchdog, 0, 0x7f1b, W::sideEffect);
    assert(read(watchdog, 0) == 0x7f1a);
    // No disabled service sequence starts a counter or changes status/control.
    for (uint32_t service : {0xaaaau, 0x5555u, 0x1234u, 0xaaaau, 0x5555u, 0xaaaau}) {
        write(watchdog, 4, service);
        assert(read(watchdog, 4) == service);
        assert(read(watchdog, 8) == 0 && read(watchdog, 0) == 0x7f1a);
    }
}
void bootstrapInactiveRequest() {
    Imx1Bootstrap soc;
    assert(!std::strcmp(Imx1Bootstrap::registerName(base), "WDOG.WCR"));
    assert(!std::strcmp(imx1BootstrapAccessName(B::resetRequest), "unsupported-reset-request"));
    assert(Imx1Watchdog::interruptSource == uint64_t(1) << 63);
    write(soc, aitc + 8, 63); // WDT_INT, table 10-1, inactive while disabled.
    assert(read(soc, aitc + 0x10) == 0x80000000 && read(soc, aitc + 0x58) == 0);
    assert(soc.clockStatus() == B::ok);
    write(soc, base, 0x7f03, B::sideEffect);
    write(soc, base, 0x7f06, B::resetRequest);
    assert(read(soc, base) == 0 && read(soc, base + 8) == 0);
    write(soc, base, 0x7f12);
    write(soc, base + 4, 0x5555); write(soc, base + 4, 0xaaaa);
    for (unsigned i = 0; i < 100; ++i) {
        assert(read(soc, base) == 0x7f12 && read(soc, base + 8) == 0);
        uint32_t value = 0xdeadbeef;
        assert(!soc.instructionRead(base, 4, value));
    }
    soc.advanceFastBusCycles(UINT32_MAX);
    assert(read(soc, base) == 0x7f12 && read(soc, base + 4) == 0xaaaa && read(soc, base + 8) == 0);
    assert(soc.clockStatus() == B::ok && read(soc, aitc + 0x58) == 0);
    write(soc, aitc + 0x18, 0x80000000); // Fast routing keeps the same inactive input.
    assert(read(soc, aitc + 0x60) == 0);
    write(soc, aitc + 0x50, 0x80000000, B::sideEffect);
    assert(read(soc, aitc + 0x50) == 0);
    write(soc, aitc + 8, 60, B::externalInput); // Unmodelled DMA input remains unknown.
    write(soc, base, 0x7f16, B::resetRequest);
    write(soc, base, 0x7f13, B::sideEffect);
    assert(read(soc, base) == 0x7f12);
}
void activeBound() {
    Imx1Watchdog w;
    write(w, 0, 0x0202); write(w, 0, 0x0203);
    assert(read(w, 0) == 0x0203 && read(w, 8) == 0);
    w.advanceReference(32767, 2); // Unknown initial phase, still strictly below one half-second.
    assert(!w.possibleTimeout());
    auto split = w;
    w.advanceReference(1, 2);
    split.advanceReference(1, 6); split.advanceReference(1, 3);
    assert(w.possibleTimeout() && split.possibleTimeout());
    for (auto* x : {&w, &split}) {
        uint32_t value = 0xdeadbeef;
        assert(x->access(base + 8, 4, false, value) == W::possibleTimeout && value == 0xdeadbeef);
        write(*x, 4, 0x5555, W::possibleTimeout); write(*x, 4, 0xaaaa, W::possibleTimeout);
        write(*x, 0, 0x0202, W::possibleTimeout);
        x->advanceReference(UINT64_MAX, 1);
        assert(x->possibleTimeout());
    }
    Imx1Watchdog service;
    write(service, 4, 0x5555); // A disabled prefix cannot poison or complete an active pair.
    write(service, 0, 0x0202); write(service, 0, 0x0203);
    service.advanceReference(16000, 1);
    write(service, 4, 0x5555); write(service, 4, 0xaaaa);
    service.advanceReference(16000, 1);
    assert(!service.possibleTimeout() && read(service, 8) == 0);
    auto invalid = service, repeated = service;
    write(invalid, 4, 0x5555); write(invalid, 4, 0x1234); write(invalid, 4, 0xaaaa);
    write(repeated, 4, 0x5555); write(repeated, 4, 0x5555); write(repeated, 4, 0xaaaa);
    invalid.advanceReference(384, 1); repeated.advanceReference(384, 1);
    assert(invalid.possibleTimeout() && repeated.possibleTimeout());
    auto single = service;
    write(single, 4, 0xaaaa); single.advanceReference(384, 1);
    assert(single.possibleTimeout());
    auto unchanged = service;
    for (uint32_t v : {0x0303u, 0x8203u, 0x0213u, 0x020bu}) write(service, 0, v, W::sideEffect);
    write(service, 0, 0x0207, W::resetRequest);
    write(service, 4, 0x15555, W::reservedBits);
    assert(read(service, 0) == 0x0203 && read(service, 4) == 0xaaaa);
    service.advanceReference(384, 1); unchanged.advanceReference(384, 1);
    assert(service.possibleTimeout() && unchanged.possibleTimeout());
    Imx1Watchdog locked;
    write(locked, 0, 0x0202); write(locked, 0, 0x0203);
    write(locked, 0, 0x0200); assert(read(locked, 0) == 0x0201);
    locked.advanceReference(16000, 1);
    write(locked, 0, 0x0200); // Ignored WDE clear does not reload.
    locked.advanceReference(384, 1); assert(locked.possibleTimeout());
    Imx1Watchdog disabled;
    write(disabled, 0, 0x0202); write(disabled, 0, 0x0203);
    disabled.advanceReference(16000, 1); write(disabled, 0, 0x0202);
    disabled.advanceReference(UINT64_MAX, 1); assert(!disabled.possibleTimeout());
    write(disabled, 0, 0x0203); disabled.advanceReference(16383, 1);
    assert(!disabled.possibleTimeout()); disabled.advanceReference(1, 1); assert(disabled.possibleTimeout());
    for (uint32_t wt : {0u, 1u}) {
        Imx1Watchdog low;
        write(low, 0, wt << 8 | 2); write(low, 0, wt << 8 | 3, W::sideEffect);
        assert(read(low, 0) == (wt << 8 | 2));
    }
    Imx1Watchdog large;
    write(large, 0, 0x7f02); write(large, 0, 0x7f03);
    large.advanceReference(126 * 16384 - 1, 1); assert(!large.possibleTimeout());
    large.advanceReference(1, 1); assert(large.possibleTimeout());
    Imx1Watchdog ratio;
    write(ratio, 0, 0x7f02); write(ratio, 0, 0x7f03);
    ratio.advanceReference(1, 2147483647);
    assert(!ratio.referenceRatioSupported(2147483629ULL * 2147483587ULL));
    assert(!ratio.referenceRatioSupported(0));
    assert(ratio.referenceRatioSupported(2147483647));
    auto chunked = ratio;
    ratio.advanceReference(2147483647, 2147483647);
    chunked.advanceReference(1, 1);
    assert(!ratio.possibleTimeout() && !chunked.possibleTimeout());
}
void activeBootstrap() {
    Imx1Bootstrap soc;
    assert(soc.setRtcInitialTime({1, 12, 34, 56}));
    write(soc, base, 0x0202); write(soc, base, 0x0203);
    write(soc, aitc + 8, 63);
    assert(read(soc, base) == 0x0203 && read(soc, aitc + 0x58) == 0);
    // A live reference divisor change is rejected before the RTC has elapsed at all.
    write(soc, 0x00204010, 0xa0, B::sideEffect); assert(read(soc, 0x00204010) == 0x80);
    write(soc, 0x00204010, 0xe0, B::sideEffect); assert(read(soc, 0x00204010) == 0x80);
    write(soc, 0x00204018, 0, B::sideEffect);
    write(soc, 0x00204004, 1, B::sideEffect); assert(read(soc, 0x00204004) == 56);
    for (unsigned i = 0; i < 100; ++i) assert(read(soc, base + 8) == 0);
    soc.advanceFastBusCycles(16384 * 250 - 1);
    assert(soc.clockStatus() == B::ok && read(soc, base + 8) == 0);
    soc.advanceFastBusCycles(1);
    assert(soc.clockStatus() == B::possibleWatchdogTimeout);
    assert(!std::strcmp(imx1BootstrapAccessName(B::possibleWatchdogTimeout), "possible-watchdog-timeout"));
    write(soc, base, 0x0202, B::possibleWatchdogTimeout);
    write(soc, base + 4, 0x5555, B::possibleWatchdogTimeout);
    assert(soc.clockStatus() == B::possibleWatchdogTimeout);
    Imx1Bootstrap stall;
    stall.setSystemPllScenario(true);
    write(stall, 0x0021b000, 0x57000003); write(stall, 0x0021b004, 0x03e717bf);
    write(stall, 0x0021b000, 0x57200003); stall.advanceFastBusCycles(6000);
    write(stall, base, 0x0202); write(stall, base, 0x0203);
    stall.advanceCpuCycles(uint64_t(16383) * 762752 / 125, 3, true);
    uint32_t target = 0x123638ad, restart = 0x57400003;
    assert(stall.access(0x0021b00c, 4, true, target, 3) == B::ok);
    assert(stall.access(0x0021b000, 4, true, restart, 3) == B::ok);
    assert(stall.clockStatus() == B::ok);
    assert(stall.finishSystemPllBusStall() == 2 * 762752);
    assert(stall.clockStatus() == B::possibleWatchdogTimeout);
    // A 32000-divisor RTC cannot supply this interval proof, even before elapsed time.
    Imx1Bootstrap other;
    write(other, 0x00204010, 0xa0); write(other, base, 0x7f02);
    write(other, base, 0x7f03, B::sideEffect); assert(read(other, base) == 0x7f02);
    write(other, 0x00204010, 0x80); write(other, base, 0x7f03);
    assert(read(other, base) == 0x7f03);
}

}
int main() {
    registersAndGuards(); bootstrapInactiveRequest(); activeBound(); activeBootstrap();
    std::puts("i.MX1 watchdog cold reset, active safe interval, service sequences, WDE and sticky timeout/reset guards passed");
}
