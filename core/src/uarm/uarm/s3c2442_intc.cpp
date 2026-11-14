#include "s3c2442_blocks.h"

namespace {
    // The six first level arbiters' requests 0 to 5 as SRCPND bits, -1 where nothing is wired
    // (manual figure 14-2).
    constexpr int arbiters[6][6] = {
        {-1, 0, 1, 2, 3, -1},
        {4, 5, 6, 7, 8, 9},
        {10, 11, 12, 13, 14, 15},
        {16, 17, 18, 19, 20, 21},
        {22, 23, 24, 25, 26, 27},
        {-1, 28, 29, 30, 31, -1},
    };
    // Request order for each ARB_SEL value. REQ0 is always first and REQ5 always last.
    constexpr unsigned orders[4][6] = {
        {0, 1, 2, 3, 4, 5},
        {0, 2, 3, 4, 1, 5},
        {0, 3, 4, 1, 2, 5},
        {0, 4, 1, 2, 3, 5},
    };
    // Sub-sources by the SRCPND bit they raise.
    struct Group { unsigned source; uint32_t subs; };
    constexpr Group groups[] = {
        {s3cIrq::uart0, 0x7u}, {s3cIrq::uart1, 0x38u}, {s3cIrq::uart2, 0x1c0u},
        {s3cIrq::adc, 0x600u}, {s3cIrq::cam, 0x1800u}, {s3cIrq::wdtAc97, 0x6000u},
    };
}

void S3cIntc::reset() {
    srcpnd = 0;
    intmod = 0;
    intmsk = 0xffffffff;
    priority = 0x7f;
    intpnd = 0;
    intoffset = 0;
    subsrcpnd = 0;
    intsubmsk = 0x7fff;
    levels = subLevels = 0;
    fiqLine = false;
}

S3cAccess S3cIntc::read(uint32_t offset, uint32_t& value) {
    switch (offset) {
        case 0x00: value = srcpnd; break;
        case 0x04: value = intmod; break;
        case 0x08: value = intmsk; break;
        case 0x0c: value = priority; break;
        case 0x10: value = intpnd; break;
        case 0x14: value = intoffset; break;
        case 0x18: value = subsrcpnd; break;
        case 0x1c: value = intsubmsk; break;
        default: return S3cAccess::unknown;
    }
    return S3cAccess::ok;
}

S3cAccess S3cIntc::write(uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0x00: srcpnd &= ~value; break;
        case 0x04: intmod = value; break;
        case 0x08: intmsk = value; break;
        case 0x0c: priority = value & 0x1fffff; break;
        case 0x10:
            // Clearing the bit that is pending lets the next request through.
            if (value & intpnd) { intpnd = 0; intoffset = 0; }
            break;
        case 0x14: break;  // read only
        case 0x18: subsrcpnd &= ~value; break;
        case 0x1c: intsubmsk = value & 0x7fff; break;
        default: return S3cAccess::unknown;
    }
    update();
    return S3cAccess::ok;
}

void S3cIntc::pulse(unsigned source) { srcpnd |= 1u << source; update(); }

void S3cIntc::level(unsigned source, bool on) {
    if (on) levels |= 1u << source; else levels &= ~(1u << source);
    update();
}

void S3cIntc::subPulse(unsigned sub) { subsrcpnd |= 1u << sub; update(); }

void S3cIntc::subLevel(unsigned sub, bool on) {
    if (on) subLevels |= 1u << sub; else subLevels &= ~(1u << sub);
    update();
}

// The winning SRCPND bit, or -1. ASSUMPTION "arbiter rotation at selection": with ARB_MODE on, ARB_SEL
// moves on as a request wins, so the winner goes last (the manual says "after it's serviced").
int S3cIntc::arbitrate(uint32_t requests) {
    int firstLevel[6];
    for (unsigned a = 0; a < 6; ++a) {
        firstLevel[a] = -1;
        const unsigned sel = (priority >> (7 + 2 * a)) & 3;
        for (unsigned i = 0; i < 6; ++i) {
            const unsigned req = orders[sel][i];
            const int source = arbiters[a][req];
            if (source < 0 || !(requests & (1u << source))) continue;
            firstLevel[a] = int(req);
            break;
        }
    }
    const unsigned sel6 = (priority >> 19) & 3;
    for (unsigned i = 0; i < 6; ++i) {
        const unsigned a = orders[sel6][i];
        if (firstLevel[a] < 0) continue;
        const unsigned req = unsigned(firstLevel[a]);
        auto rotate = [&](unsigned arbiter, unsigned served) {
            if (!(priority & (1u << arbiter)) || served == 0 || served == 5) return;
            const unsigned next = served == 4 ? 0 : served;
            const unsigned shift = 7 + 2 * arbiter;
            priority = (priority & ~(3u << shift)) | (next << shift);
        };
        rotate(a, req);
        rotate(6, a);
        return arbiters[a][req];
    }
    return -1;
}

void S3cIntc::update() {
    subsrcpnd |= subLevels;
    for (const auto& g : groups)
        if (subsrcpnd & ~intsubmsk & g.subs) srcpnd |= 1u << g.source;
    srcpnd |= levels;
    const uint32_t live = srcpnd & ~intmsk;
    fiqLine = live & intmod;
    if (!intpnd) {
        const int winner = arbitrate(live & ~intmod);
        if (winner >= 0) {
            intpnd = 1u << winner;
            intoffset = uint32_t(winner);
        }
    }
}
