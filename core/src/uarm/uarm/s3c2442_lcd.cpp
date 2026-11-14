#include "s3c2442_blocks.h"

void S3cLcd::reset() {
    for (auto& r : lcdcon) r = 0;
    for (auto& r : saddr) r = 0;
    redlut = greenlut = bluelut = 0;
    dithmode = tpal = 0;
    lcdintpnd = lcdsrcpnd = 0;
    lcdintmsk = 3;
    tconsel = 0xf84;
    for (auto& p : palette) p = 0;
    phase = 0;
}

// TFT timing: VCLK is HCLK / ((CLKVAL + 1) * 2), a line is HSPW + HBPD + HOZVAL + HFPD VCLKs (each field
// plus one), a frame VSPW + VBPD + LINEVAL + VFPD lines.
uint64_t S3cLcd::lineTicks() const {
    const uint64_t clkval = (lcdcon[0] >> 8) & 0x3ff;
    const uint64_t vclks = ((lcdcon[3] & 0xff) + 1) + (((lcdcon[2] >> 19) & 0x7f) + 1) +
                           (((lcdcon[2] >> 8) & 0x7ff) + 1) + ((lcdcon[2] & 0xff) + 1);
    return vclks * (clkval + 1) * 2;
}

uint32_t S3cLcd::frameLines() const {
    return ((lcdcon[1] & 0x3f) + 1) + (((lcdcon[1] >> 24) & 0xff) + 1) + (((lcdcon[1] >> 14) & 0x3ff) + 1) +
           (((lcdcon[1] >> 6) & 0xff) + 1);
}

S3cAccess S3cLcd::read(uint32_t offset, uint32_t& value) {
    if (offset >= 0x400 && offset < 0x800) { value = palette[(offset - 0x400) / 4]; return S3cAccess::ok; }
    switch (offset) {
        case 0x00: {
            // LINECNT counts down from LINEVAL through the active lines.
            value = lcdcon[0] & 0x3ffff;
            if (enabled()) {
                const uint64_t line = phase / lineTicks();
                const uint32_t lineval = (lcdcon[1] >> 14) & 0x3ff;
                const uint64_t first = ((lcdcon[1] & 0x3f) + 1) + (((lcdcon[1] >> 24) & 0xff) + 1);
                uint32_t count = lineval;
                if (line >= first) count = line - first > lineval ? 0 : uint32_t(lineval - (line - first));
                value |= count << 18;
            }
            break;
        }
        case 0x04: value = lcdcon[1]; break;
        case 0x08: value = lcdcon[2]; break;
        case 0x0c: value = lcdcon[3]; break;
        case 0x10: {
            // VSTATUS and HSTATUS: 0 sync, 1 back porch, 2 active, 3 front porch.
            value = lcdcon[4] & 0x1fff;
            if (enabled()) {
                const uint64_t lt = lineTicks(), line = phase / lt, inLine = phase % lt;
                const uint64_t vspw = (lcdcon[1] & 0x3f) + 1, vbpd = ((lcdcon[1] >> 24) & 0xff) + 1,
                               lines = ((lcdcon[1] >> 14) & 0x3ff) + 1;
                const unsigned v = line < vspw ? 0 : line < vspw + vbpd ? 1 : line < vspw + vbpd + lines ? 2 : 3;
                const uint64_t vclk = (((lcdcon[0] >> 8) & 0x3ff) + 1) * 2, at = inLine / vclk;
                const uint64_t hspw = (lcdcon[3] & 0xff) + 1, hbpd = ((lcdcon[2] >> 19) & 0x7f) + 1,
                               pixels = ((lcdcon[2] >> 8) & 0x7ff) + 1;
                const unsigned hs = at < hspw ? 0 : at < hspw + hbpd ? 1 : at < hspw + hbpd + pixels ? 2 : 3;
                value |= (v << 15) | (hs << 13);
            }
            break;
        }
        case 0x14: case 0x18: case 0x1c: value = saddr[(offset - 0x14) / 4]; break;
        case 0x20: value = redlut; break;
        case 0x24: value = greenlut; break;
        case 0x28: value = bluelut; break;
        case 0x4c: value = dithmode; break;
        case 0x50: value = tpal; break;
        case 0x54: value = lcdintpnd; break;
        case 0x58: value = lcdsrcpnd; break;
        case 0x5c: value = lcdintmsk; break;
        case 0x60: value = tconsel; break;
        default: return S3cAccess::unknown;
    }
    return S3cAccess::ok;
}

S3cAccess S3cLcd::write(uint32_t offset, uint32_t value, S3cIntc& intc) {
    if (offset >= 0x400 && offset < 0x800) { palette[(offset - 0x400) / 4] = value & 0xffff; return S3cAccess::ok; }
    switch (offset) {
        case 0x00:
            if ((value ^ lcdcon[0]) & 1) phase = 0;
            lcdcon[0] = value & 0x3ffff;
            break;
        case 0x04: lcdcon[1] = value; break;
        case 0x08: lcdcon[2] = value & 0x3ffffff; break;
        case 0x0c: lcdcon[3] = value & 0xffff; break;
        case 0x10: lcdcon[4] = value & 0x1fff; break;
        case 0x14: case 0x18: case 0x1c: saddr[(offset - 0x14) / 4] = value; break;
        case 0x20: redlut = value; break;
        case 0x24: greenlut = value; break;
        case 0x28: bluelut = value; break;
        case 0x4c: dithmode = value; break;
        case 0x50: tpal = value & 0x1ffffff; break;
        case 0x54: lcdintpnd &= ~value; lcdsrcpnd &= ~value; break;
        case 0x58: lcdsrcpnd &= ~value; break;
        case 0x5c:
            // The FIFO interrupt (bit 0) comes as the FIFO drains, which isn't modelled.
            if (!(value & 1)) return S3cAccess::unsupported;
            lcdintmsk = value & 7;
            break;
        case 0x60: tconsel = value; break;
        default: return S3cAccess::unknown;
    }
    interrupts(intc);
    return S3cAccess::ok;
}

void S3cLcd::interrupts(S3cIntc& intc) {
    lcdintpnd |= lcdsrcpnd & ~lcdintmsk & 3;
    intc.level(s3cIrq::lcd, lcdintpnd);
}

// The frame sync interrupt comes as each frame starts, with VSYNC.
void S3cLcd::advance(uint64_t hclkTicks, S3cIntc& intc) {
    if (!enabled()) return;
    const uint64_t frame = lineTicks() * frameLines();
    const uint64_t total = phase + hclkTicks;
    if (total >= frame) {
        lcdsrcpnd |= 2;
        interrupts(intc);
    }
    phase = total % frame;
}

uint64_t S3cLcd::ticksToEvent() const {
    if (!enabled()) return UINT64_MAX;
    return lineTicks() * frameLines() - phase;
}
