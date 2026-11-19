#include "s3c2442_blocks.h"

namespace {
    constexpr uint32_t ecflg = 1u << 15, prscen = 1u << 14, standby = 1u << 2, readStart = 1u << 1,
                       enableStart = 1u << 0;
    // ADCTSC
    constexpr uint32_t udSen = 1u << 8, autoPst = 1u << 2;
    constexpr uint32_t stylusUp = 1u << 15;
    // What a touch channel converts to with no stylus on the panel. ASSUMPTION "touch reads full scale up".
    constexpr uint16_t openPanel = 0x3ff;
}

void S3cAdc::reset() {
    adccon = 0x3fc4;  // prescaler 0xff, standby
    adctsc = 0x58;
    adcdly = 0xff;
    adcdat0 = adcdat1 = adcupdn = 0;
    for (auto& c : channel) c = 0x200;
    remaining = 0;
    touchCondition = false;
}

S3cAccess S3cAdc::read(uint32_t offset, uint32_t& value) {
    switch (offset) {
        case 0x00: value = adccon; return S3cAccess::ok;
        case 0x04: value = adctsc; return S3cAccess::ok;
        case 0x08: value = adcdly; return S3cAccess::ok;
        case 0x0c:
        case 0x10:
            // UPDOWN is the stylus as it is now; AUTO_PST and XY_PST echo ADCTSC.
            value = (offset == 0x0c ? adcdat0 : adcdat1) | (penDown ? 0 : stylusUp) | (adctsc & 7) << 12;
            // READ_START: reading the data starts the next conversion.
            if (adccon & readStart) start();
            return S3cAccess::ok;
        case 0x14: value = adcupdn; return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}

S3cAccess S3cAdc::write(uint32_t offset, uint32_t value, S3cIntc& intc) {
    switch (offset) {
        case 0x00:
            adccon = (adccon & ecflg) | (value & 0x7fff);
            if ((value & enableStart) && !(value & standby)) {
                adccon &= ~enableStart;  // clears as the conversion starts
                start();
            }
            return S3cAccess::ok;
        case 0x04:
            adctsc = value & 0x1ff;
            checkTouch(intc);
            return S3cAccess::ok;
        case 0x08: adcdly = value & 0xffff; return S3cAccess::ok;
        // ASSUMPTION "ADCUPDN write": the history bits take what's written, so 0 clears them.
        case 0x14: adcupdn = value & 3; return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}

// The conversion's length in PCLK ticks: ADCDLY first in the touch modes, then 5 ADC clocks, the ADC clock
// being PCLK / (PRSCVL + 1) with the prescaler on.
void S3cAdc::start() {
    const uint64_t divider = (adccon & prscen) ? ((adccon >> 6) & 0xff) + 1 : 1;
    const bool touchMode = (adctsc & autoPst) || (adctsc & 3) == 1 || (adctsc & 3) == 2;
    remaining = 5 * divider + (touchMode ? adcdly : 0);
    adccon &= ~ecflg;
}

void S3cAdc::finish(S3cIntc& intc) {
    remaining = 0;
    ++conversions;
    const unsigned xy = adctsc & 3;
    const uint16_t x = penDown ? penX : openPanel, y = penDown ? penY : openPanel;
    if (adctsc & autoPst) {
        adcdat0 = x;
        adcdat1 = y;
    } else if (xy == 1) {
        adcdat0 = x;
    } else if (xy == 2) {
        adcdat1 = y;
    } else {
        const unsigned mux = (adccon >> 3) & 7;
        if (firstChannel < 0) firstChannel = int(mux);
        channelsRead |= 1u << mux;
        adcdat0 = channel[mux] & 0x3ff;
    }
    adccon |= ecflg;
    intc.subPulse(s3cSubIrq::adc);
}

void S3cAdc::advance(uint64_t pclkTicks, S3cIntc& intc) {
    if (!remaining) return;
    if (pclkTicks < remaining) { remaining -= pclkTicks; return; }
    finish(intc);
}

uint64_t S3cAdc::ticksToEvent() const { return remaining ? remaining : UINT64_MAX; }

// In wait-for-interrupt mode the pen detect compares the stylus with UD_SEN: 0 waits for down, 1 for up.
// The interrupt comes once each time that becomes true.
void S3cAdc::checkTouch(S3cIntc& intc) {
    const bool waiting = (adctsc & 3) == 3;
    const bool wantUp = adctsc & udSen;
    const bool now = waiting && (wantUp ? !penDown : penDown);
    if (now && !touchCondition) {
        ++touchInterrupts;
        adcupdn |= wantUp ? 2 : 1;
        intc.subPulse(s3cSubIrq::tc);
    }
    touchCondition = now;
}

void S3cAdc::setStylus(bool down, uint16_t x, uint16_t y, S3cIntc& intc) {
    penDown = down;
    penX = x & 0x3ff;
    penY = y & 0x3ff;
    checkTouch(intc);
}
