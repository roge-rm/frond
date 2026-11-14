#include "s3c2442_blocks.h"

namespace {
    // TCON's start bit for each timer; manual update is the next bit up. Timer 4 has no inverter.
    constexpr unsigned startBit[5] = {0, 8, 12, 16, 20};
    unsigned reloadBit(unsigned timer) { return timer == 4 ? 22 : startBit[timer] + 3; }
}

void S3cPwm::reset() {
    tcfg0 = tcfg1 = tcon = 0;
    for (auto& t : timers) t = {};
}

uint64_t S3cPwm::divisor(unsigned timer) const {
    const uint32_t prescaler = timer < 2 ? tcfg0 & 0xff : (tcfg0 >> 8) & 0xff;
    const uint32_t mux = (tcfg1 >> (4 * timer)) & 0xf;
    // ASSUMPTION "no TCLK": the board has nothing on TCLK0 or TCLK1, so a timer on it never counts.
    if (mux >= 4) return 0;
    return uint64_t(prescaler + 1) << (mux + 1);
}

bool S3cPwm::autoReload(unsigned timer) const { return tcon & (1u << reloadBit(timer)); }

S3cAccess S3cPwm::read(uint32_t offset, uint32_t& value) {
    switch (offset) {
        case 0x00: value = tcfg0; return S3cAccess::ok;
        case 0x04: value = tcfg1; return S3cAccess::ok;
        case 0x08: value = tcon; return S3cAccess::ok;
        case 0x3c: value = timers[4].buffer; return S3cAccess::ok;
        case 0x40: value = timers[4].count; return S3cAccess::ok;
    }
    if (offset >= 0x0c && offset < 0x3c) {
        const unsigned timer = (offset - 0x0c) / 12, reg = (offset - 0x0c) % 12;
        value = reg == 0 ? timers[timer].buffer : reg == 4 ? timers[timer].compare : timers[timer].count;
        return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}

S3cAccess S3cPwm::write(uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0x00: tcfg0 = value & 0xffffff; return S3cAccess::ok;
        case 0x04: tcfg1 = value & 0xffffff; return S3cAccess::ok;
        case 0x08: writeTcon(value & 0x7fff1f); return S3cAccess::ok;
        case 0x3c: timers[4].buffer = value & 0xffff; return S3cAccess::ok;
        case 0x40: return S3cAccess::ok;  // read only
    }
    if (offset >= 0x0c && offset < 0x3c) {
        const unsigned timer = (offset - 0x0c) / 12, reg = (offset - 0x0c) % 12;
        if (reg == 0) timers[timer].buffer = value & 0xffff;
        else if (reg == 4) timers[timer].compare = value & 0xffff;
        return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}

// Manual update copies the buffers into the count while its bit is set. A timer counts while its start
// bit is set and manual update is clear.
void S3cPwm::writeTcon(uint32_t value) {
    tcon = value;
    for (unsigned i = 0; i < 5; ++i) {
        auto& t = timers[i];
        const bool start = value & (1u << startBit[i]), update = value & (2u << startBit[i]);
        if (update) {
            t.count = t.buffer;
            t.compareLatch = t.compare;
            t.phase = 0;
        }
        const bool run = start && !update;
        if (run && !t.running) t.phase = 0;
        t.running = run;
    }
}

// The count goes down one each timer tick. Reaching 0 raises the interrupt, and the next tick reloads
// TCNTB (or stops the timer without auto reload), so the period is TCNTB + 1 ticks.
void S3cPwm::advance(uint64_t pclkTicks, S3cIntc& intc) {
    for (unsigned i = 0; i < 5; ++i) {
        auto& t = timers[i];
        const uint64_t d = divisor(i);
        if (!t.running || !d) continue;
        const uint64_t total = t.phase + pclkTicks;
        uint64_t n = total / d;
        t.phase = total % d;
        bool fired = false;
        while (n) {
            if (!t.count) {
                if (!autoReload(i)) { t.running = false; break; }
                // Each whole period from here ends in one more interrupt, which SRCPND only takes once.
                const uint64_t period = uint64_t(t.buffer) + 1;
                if (n >= period) {
                    fired = true;
                    n %= period;
                    if (!n) break;
                }
                --n;
                t.count = t.buffer;
                t.compareLatch = t.compare;
                continue;
            }
            if (n < t.count) { t.count -= uint32_t(n); break; }
            n -= t.count;
            t.count = 0;
            fired = true;
        }
        if (fired) intc.pulse(s3cIrq::timer0 + i);
    }
}

uint64_t S3cPwm::ticksToEvent() const {
    uint64_t best = UINT64_MAX;
    for (unsigned i = 0; i < 5; ++i) {
        const auto& t = timers[i];
        const uint64_t d = divisor(i);
        if (!t.running || !d) continue;
        uint64_t steps;
        if (t.count) steps = t.count;
        else if (autoReload(i)) steps = uint64_t(t.buffer) + 1;
        else continue;
        const uint64_t ticks = steps * d - t.phase;
        if (ticks < best) best = ticks;
    }
    return best;
}
