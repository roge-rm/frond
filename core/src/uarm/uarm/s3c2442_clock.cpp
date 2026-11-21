#include "s3c2442_blocks.h"

// Clock and power. Reset values from the manual's register table.
void S3cClock::reset() {
    locktime = 0xffffffff;
    mpllcon = 0x00096030;
    upllcon = 0x0004d030;
    clkcon = 0x00fffff0;
    clkslow = 0x4;
    clkdivn = 0x4;
    camdivn = 0;
    refreshRates();
}

S3cAccess S3cClock::read(uint32_t offset, uint32_t& value) {
    switch (offset) {
        case 0x00: value = locktime; break;
        case 0x04: value = mpllcon; break;
        case 0x08: value = upllcon; break;
        case 0x0c: value = clkcon; break;
        case 0x10: value = clkslow; break;
        case 0x14: value = clkdivn; break;
        case 0x18: value = camdivn; break;
        default: return S3cAccess::unknown;
    }
    return S3cAccess::ok;
}

S3cAccess S3cClock::write(uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0x00: locktime = value; return S3cAccess::ok;
        case 0x04: mpllcon = value & 0xff3f3; break;
        case 0x08: upllcon = value & 0xff3f3; break;
        case 0x0c: clkcon = value & 0x00fffffc; return S3cAccess::ok;
        case 0x10: clkslow = value & 0xb7; break;
        case 0x14: clkdivn = value & 0xf; break;
        case 0x18: camdivn = value & 0x1f1f; break;
        default: return S3cAccess::unknown;
    }
    refreshRates();
    if (changed) changed();
    return S3cAccess::ok;
}

// ASSUMPTION "S3C2442 MPLL formula": the 2440's, MPLL = 2 * m * Fin / (p * 2^s) with m = MDIV + 8 and
// p = PDIV + 2. The UPLL has no doubling.
uint64_t S3cClock::mpll() const {
    const uint64_t m = ((mpllcon >> 12) & 0xff) + 8, p = ((mpllcon >> 4) & 0x3f) + 2, s = mpllcon & 3;
    return 2 * m * crystal / (p << s);
}

uint64_t S3cClock::upll() const {
    const uint64_t m = ((upllcon >> 12) & 0xff) + 8, p = ((upllcon >> 4) & 0x3f) + 2, s = upllcon & 3;
    return m * crystal / (p << s);
}

void S3cClock::refreshRates() {
    // Slow mode uses the crystal, with SLOW_VAL 0 leaving it undivided.
    const uint32_t slowVal = clkslow & 7;
    fastHz = clkslow & 0x10 ? crystal / (slowVal ? 2 * slowVal : 1) : mpll();
    static constexpr unsigned dividers[] = {1, 2, 4, 3};
    const unsigned select = (clkdivn >> 1) & 3;
    unsigned divisor = dividers[select];
    if ((select == 2 && (camdivn & 0x200)) || (select == 3 && (camdivn & 0x100))) divisor *= 2;
    busHz = fastHz / divisor;
    peripheralHz = clkdivn & 1 ? busHz / 2 : busHz;
}

void S3cClock::restoreRegisters(const S3cClock& saved) {
    locktime = saved.locktime;
    mpllcon = saved.mpllcon;
    upllcon = saved.upllcon;
    clkcon = saved.clkcon;
    clkslow = saved.clkslow;
    clkdivn = saved.clkdivn;
    camdivn = saved.camdivn;
    refreshRates();
}

// The memory controller's registers, BWSCON to MRSRB7, with their reset values.
void S3cMemoryController::reset() {
    const uint32_t values[13] = {0, 0x700, 0x700, 0x700, 0x700, 0x700, 0x700, 0x18008, 0x18008, 0xac0000, 0, 0, 0};
    for (unsigned i = 0; i < 13; ++i) regs[i] = values[i];
}

S3cAccess S3cMemoryController::read(uint32_t offset, uint32_t& value) {
    if (offset >= 13 * 4) return S3cAccess::unknown;
    value = regs[offset / 4];
    return S3cAccess::ok;
}

S3cAccess S3cMemoryController::write(uint32_t offset, uint32_t value) {
    if (offset >= 13 * 4) return S3cAccess::unknown;
    regs[offset / 4] = value;
    return S3cAccess::ok;
}

// Watchdog: WTCON's prescaler in bits 15-8, enable bit 5, division factor 16 << bits 4-3, interrupt
// bit 2 and reset output bit 0.
void S3cWatchdog::reset() {
    wtcon = 0x8021;
    wtdat = 0x8000;
    wtcnt = 0x8000;
    phase = 0;
    resetRequested = false;
}

S3cAccess S3cWatchdog::read(uint32_t offset, uint32_t& value) {
    switch (offset) {
        case 0x0: value = wtcon; break;
        case 0x4: value = wtdat; break;
        case 0x8: value = wtcnt; break;
        default: return S3cAccess::unknown;
    }
    return S3cAccess::ok;
}

S3cAccess S3cWatchdog::write(uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0x0: wtcon = value & 0xff3d; break;
        case 0x4: wtdat = value & 0xffff; break;
        case 0x8: wtcnt = value & 0xffff; break;
        default: return S3cAccess::unknown;
    }
    return S3cAccess::ok;
}

uint64_t S3cWatchdog::divisor() const { return uint64_t((wtcon >> 8) + 1) << (4 + ((wtcon >> 3) & 3)); }

void S3cWatchdog::advance(uint64_t pclkTicks, S3cIntc& intc) {
    if (!(wtcon & 0x20)) return;
    const uint64_t d = divisor(), total = phase + pclkTicks;
    uint64_t n = total / d;
    phase = total % d;
    while (n) {
        if (!wtcnt) {
            // The count starts again from WTDAT on the tick after it ran out.
            --n;
            wtcnt = wtdat;
            if (!wtcnt) n = 0;
            continue;
        }
        if (n < wtcnt) { wtcnt -= uint32_t(n); return; }
        n -= wtcnt;
        wtcnt = 0;
        if (wtcon & 4) intc.subPulse(s3cSubIrq::wdt);
        if (wtcon & 1) resetRequested = true;
    }
}

bool S3cWatchdog::countAfter(uint64_t pclkTicks, uint32_t& count) const {
    if (!(wtcon & 0x20)) return false;
    const uint64_t n = (phase + pclkTicks) / divisor();
    if (n >= wtcnt) return false;
    count = wtcnt - uint32_t(n);
    return true;
}

uint64_t S3cWatchdog::ticksToEvent(bool interrupts) const {
    if (!(wtcon & 0x20) || !(wtcon & (interrupts ? 5 : 1))) return UINT64_MAX;
    // Reloading 0 from WTDAT leaves it stopped at 0 (see advance).
    if (!wtcnt && !wtdat) return UINT64_MAX;
    const uint64_t d = divisor();
    const uint64_t steps = wtcnt ? wtcnt : uint64_t(wtdat) + 1;
    return steps * d - phase;
}
