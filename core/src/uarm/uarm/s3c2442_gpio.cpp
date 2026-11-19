#include "s3c2442_blocks.h"

namespace {
    // Each port's CON register; DAT and UP follow it. Port J sits past the misc registers.
    constexpr uint32_t portBase[S3cGpio::ports] = {0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0xd0};
}

uint32_t S3cGpio::pinMask(Port port) {
    static const uint32_t masks[ports] = {0x1ffffff, 0x7ff, 0xffff, 0xffff, 0xffff, 0xff, 0xffff, 0x7ff, 0x1fff};
    return masks[port];
}

void S3cGpio::reset(uint32_t chipId, uint32_t resetCause) {
    for (unsigned p = 0; p < ports; ++p) {
        con[p] = 0;
        dat[p] = 0;
        up[p] = 0;
        input[p] = pinMask(Port(p));
    }
    con[a] = 0x1ffffff;
    up[d] = 0xf000;
    up[g] = 0xf800;
    misccr = 0x10330;
    dclkcon = 0;
    extint[0] = extint[1] = extint[2] = 0;
    for (auto& f : eintflt) f = 0;
    eintmask = 0xfffff0;
    eintpend = 0;
    // ASSUMPTION "GSTATUS0 pins": nWAIT high, NCON low (small page NAND), RnB ready, nBATT_FLT high (no fault).
    gstatus[0] = 0xb;
    gstatus[1] = chipId;
    gstatus[2] = resetCause;
    gstatus[3] = gstatus[4] = 0;
    dsc[0] = dsc[1] = 0;
    mslcon = 0;
    lastEintLevels = 0;
    for (unsigned n = 0; n < 24; ++n) {
        bool level;
        if (eintPin(n, level) && level) lastEintLevels |= 1u << n;
    }
}

bool S3cGpio::pin(Port port, unsigned bit) const {
    bool output;
    if (port == a) output = !(con[a] & (1u << bit));
    else output = ((con[port] >> (2 * bit)) & 3) == 1;
    return (output ? dat[port] : input[port]) & (1u << bit);
}

// The level of the pin behind an EINT, and whether the EINT sees it (GPF0-7 for EINT0-7, GPG0-15 for
// EINT8-23). ASSUMPTION "S3C2442 EINT4-7 and EINT16 on inputs": EINT4-7 and EINT16 also see a pin set as an
// input. The rx1950 OAL enables EINT5 (SD card detect), EINT6 (cable sense) and EINT16 (the Wi-Fi chip) with
// GPF5, GPF6 and GPG8 left as inputs, and the SD and Wi-Fi drivers wait on those interrupts alone. The other
// EINT8-23 need their pin set to EINT: GPGCON 0x02a8aaaa leaves GPG12-15 (EINT20-23) as inputs, and with
// EXTINT low level they would latch EINTPEND. EINT0-3 need their pin set to EINT too: they go straight to
// SRCPND and wake the chip whatever INTMSK says, and the OAL leaves GPF3 (charge done, low while charging)
// an input with EINT3 on low level.
bool S3cGpio::eintPin(unsigned eint, bool& level) const {
    const Port port = eint < 8 ? f : g;
    const unsigned bit = eint < 8 ? eint : eint - 8;
    level = input[port] & (1u << bit);
    const uint32_t mode = (con[port] >> (2 * bit)) & 3;
    return mode == 2 || (mode == 0 && ((eint >= 4 && eint < 8) || eint == 16));
}

S3cAccess S3cGpio::read(uint32_t offset, uint32_t& value) {
    for (unsigned p = 0; p < ports; ++p) {
        if (offset < portBase[p] || offset >= portBase[p] + 0x0c) continue;
        const uint32_t reg = offset - portBase[p];
        if (reg == 0) value = con[p];
        else if (reg == 8) {
            if (p == a) return S3cAccess::unknown;
            value = up[p];
        } else {
            value = 0;
            for (unsigned bit = 0; bit < 32; ++bit)
                if (pinMask(Port(p)) & (1u << bit) && pin(Port(p), bit)) value |= 1u << bit;
        }
        return S3cAccess::ok;
    }
    switch (offset) {
        case 0x80: value = misccr; break;
        case 0x84: value = dclkcon; break;
        case 0x88: case 0x8c: case 0x90: value = extint[(offset - 0x88) / 4]; break;
        case 0x94: case 0x98: case 0x9c: case 0xa0: value = eintflt[(offset - 0x94) / 4]; break;
        case 0xa4: value = eintmask; break;
        case 0xa8: value = eintpend; break;
        case 0xac: case 0xb0: case 0xb4: case 0xb8: case 0xbc: value = gstatus[(offset - 0xac) / 4]; break;
        case 0xc4: case 0xc8: value = dsc[(offset - 0xc4) / 4]; break;
        case 0xcc: value = mslcon; break;
        default: return S3cAccess::unknown;
    }
    return S3cAccess::ok;
}

S3cAccess S3cGpio::write(uint32_t offset, uint32_t value, S3cIntc& intc) {
    for (unsigned p = 0; p < ports; ++p) {
        if (offset < portBase[p] || offset >= portBase[p] + 0x0c) continue;
        const uint32_t reg = offset - portBase[p];
        const uint32_t mask = pinMask(Port(p));
        if (reg == 0) {
            con[p] = p == a ? value & mask : value & (mask == 0xffff ? 0xffffffff : (1u << (2 * __builtin_popcount(mask))) - 1);
            scanEints(intc);
        } else if (reg == 4) dat[p] = value & mask;
        else {
            if (p == a) return S3cAccess::unknown;
            up[p] = value & mask;
        }
        return S3cAccess::ok;
    }
    switch (offset) {
        case 0x80: misccr = value & 0x3f7f7b; break;
        case 0x84: dclkcon = value & 0x0ff30ff3; break;
        case 0x88: case 0x8c: case 0x90: extint[(offset - 0x88) / 4] = value; scanEints(intc); break;
        case 0x94: case 0x98: case 0x9c: case 0xa0: eintflt[(offset - 0x94) / 4] = value; break;
        case 0xa4: eintmask = value & 0xfffff0; scanEints(intc); break;
        case 0xa8: eintpend &= ~(value & 0xfffff0); scanEints(intc); break;
        case 0xac: case 0xb0: break;  // read only
        case 0xb4: gstatus[2] &= ~value; break;
        case 0xb8: case 0xbc: gstatus[(offset - 0xac) / 4] = value; break;
        case 0xc4: case 0xc8: dsc[(offset - 0xc4) / 4] = value; break;
        case 0xcc: mslcon = value; break;
        default: return S3cAccess::unknown;
    }
    return S3cAccess::ok;
}

void S3cGpio::setInputs(Port port, uint32_t mask, uint32_t levels, S3cIntc& intc) {
    mask &= pinMask(port);
    input[port] = (input[port] & ~mask) | (levels & mask);
    scanEints(intc);
}

// EXTINTn mode per EINT: 000 low level, 001 high level, 01x falling edge, 10x rising edge, 11x both
// edges. EINT0-3 go straight to SRCPND; EINT4-23 latch in EINTPEND and reach SRCPND through EINTMASK.
void S3cGpio::scanEints(S3cIntc& intc) {
    uint32_t levels = 0;
    for (unsigned n = 0; n < 24; ++n) {
        bool level;
        const bool selected = eintPin(n, level);
        if (level) levels |= 1u << n;
        if (!selected) {
            if (n < 4) intc.level(n, false);
            continue;
        }
        const bool was = lastEintLevels & (1u << n);
        const unsigned mode = (extint[n / 8] >> (4 * (n % 8))) & 7;
        bool held = false, edge = false;
        if (mode == 0) held = !level;
        else if (mode == 1) held = level;
        else if (mode < 4) edge = was && !level;
        else if (mode < 6) edge = !was && level;
        else edge = was != level;
        if (n < 4) {
            intc.level(n, held);
            if (edge) intc.pulse(n);
        } else if (held || edge) {
            eintpend |= 1u << n;
        }
    }
    lastEintLevels = levels;
    const uint32_t live = eintpend & ~eintmask;
    intc.level(s3cIrq::eint4to7, live & 0xf0);
    intc.level(s3cIrq::eint8to23, live & 0xffff00);
}
