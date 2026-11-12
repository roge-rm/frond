#include "imx1_asp.h"

const char* Imx1Asp::registerName(uint32_t address) {
    switch (address & ~3u) {
        case 0x00215000: return "ASP.PADFIFO";
        case 0x00215010: return "ASP.ACNTLCR";
        case 0x00215014: return "ASP.PSMPLRG";
        case 0x00215018: return "ASP.ICNTLR";
        case 0x0021501c: return "ASP.ISTATR";
        case 0x0021502c: return "ASP.CLKDIV";
        case 0x00215030: return "ASP.CMPCNTL";
        case 0x00215034: return "ASP.FIFO_PTR";
        default: return nullptr;
    }
}

Imx1AspAccess Imx1Asp::access(uint32_t address, uint32_t size, bool write, uint32_t& value) {
    if (!registerName(address)) return Imx1AspAccess::unmapped;
    if (size != 4 || (address & 3)) return Imx1AspAccess::width;
    uint32_t* reg = nullptr;
    uint32_t mask = 0;
    switch (address) {
        case 0x00215000:
            // ASSUMPTION "ASP ring read": with nothing unread, auto sampling on and PADE set, a read gives the
            // ring word at the read pointer and moves on. The ROM reads all 12 slots every tick (20096cd0).
            if (write || (!count && !(automatic() && (control & 2)))) return Imx1AspAccess::sideEffect;
            value = fifo[readPointer];
            readPointer = (readPointer + 1) % fifo.size();
            if (count) --count;
            return Imx1AspAccess::ok;
        case 0x00215010: reg = &control; mask = 0x0eb0fff3; break;
        case 0x00215014: reg = &sampleRate; mask = 0x7fff; break;
        case 0x00215018: reg = &interrupts; mask = 0x473; break;
        case 0x0021501c:
            // Asserted level contact re-latches PEN in this functional policy.
            if (write) {
                if (value & 0x80) overflow = false;
                if (value & 0x40) pen = false;
                if (value & 0x400) penUp = false;
                refreshPen();
            }
            else value = (pen ? 0x40u : 0) | (penUp ? 0x400u : 0) | (overflow ? 0x80u : 0) | (count == 12 ? 2u : 0) | (count ? 1u : 0);
            return Imx1AspAccess::ok;
        case 0x0021502c: reg = &divider; mask = 0x1f; break;
        case 0x00215030: reg = &compare; mask = 0xfffff; break;
        case 0x00215034:
            if (!write) value = (readPointer << 4) | writePointer;
            return Imx1AspAccess::ok;
    }
    if (!write) { value = *reg; return Imx1AspAccess::ok; }
    // Reserved fields are read-only and read zero in tables 15-7 through 15-14.
    const uint32_t written = value & mask;
    if (address == 0x00215010) {
        if (written & (1u << 23)) {
            // Table 15-7 specifies complete reset and self-clear, without a cycle count.
            const auto inputs = penInputs;
            *this = Imx1Asp(px1HighFromReset, uSampleKnown, uSample);
            penInputs = inputs;
            return Imx1AspAccess::ok;
        }
        // PADE must be set after the other control fields (table 15-7).
        if (!(control & 2) && (written & 2) && ((control ^ written) & ~2u) && !oneWriteRestore(written))
            return Imx1AspAccess::sideEffect;
        if (samplingRequested(written, divider) && !qualifiedSampling(written, sampleRate))
            return Imx1AspAccess::externalInput;
        // Active conversions retain their configuration; only CLKEN can pause them.
        if ((control & 2) && (written & 2) && ((written ^ control) & ~(1u << 25)) &&
            (remaining || (written & 0x3000)))
            return Imx1AspAccess::sideEffect;
    }
    if (address == 0x00215014 && remaining && written != sampleRate) return Imx1AspAccess::sideEffect;
    if (address == 0x0021502c) {
        if (samplingRequested(control, written) && !qualifiedSampling(control, sampleRate))
            return Imx1AspAccess::externalInput;
        if (remaining && written != divider) return Imx1AspAccess::sideEffect;
    }
    // Edge and active-high detection remain unqualified.
    if (address == 0x00215018 && (written & 0x10) && !qualifiedPenDetection(written))
        return Imx1AspAccess::externalInput;
    if (address == 0x00215010) {
        if (!(written & 2)) {
            remaining = phase = count = readPointer = writePointer = 0;
            armed = idle = false;
            slot = 0;
        } else if (!(control & 2)) armed = true;
    }
    // INT is W1C; rewriting comparison settings does not clear a latched event.
    *reg = address == 0x00215030 ? (written & ~0x80000u) | ((compare & ~written) & 0x80000u) : written;
    if (armed && samplingRequested(control, divider)) {
        phase = slot = 0;
        idle = armed = false;
        startSlot();
    }
    refreshPen();
    return Imx1AspAccess::ok;
}

Imx1AspAccess Imx1Asp::replacePenInputs(const Imx1AspPenInputs& inputs) {
    auto next = *this;
    next.penInputs = inputs;
    if ((next.interrupts & 0x10) && !next.qualifiedPenDetection(next.interrupts))
        return Imx1AspAccess::externalInput;
    if ((next.remaining || next.armed) && next.automatic() &&
        !next.qualifiedAuto(next.control, next.sampleRate)) return Imx1AspAccess::externalInput;
    next.refreshPen();
    *this = next;
    return Imx1AspAccess::ok;
}

uint64_t Imx1Asp::futureInterrupts() const {
    if (!remaining || !clockDivider()) return 0;
    uint64_t sources = 0;
    if (interrupts & 3) sources |= uint64_t(1) << 33;
    const auto selected = (compare >> 16) & 3;
    if (selected && (automatic() ? (selected < 3 || !calibration()) : selected == 3))
        sources |= uint64_t(1) << 9;
    if (automatic() && !calibration() && (interrupts & 0x400)) sources |= uint64_t(1) << 5;
    return sources;
}

void Imx1Asp::startSlot() {
    remaining = 1260u * ((sampleRate & 15) + ((sampleRate >> 12) & 7) + 1);
    captured = automatic() ? (calibration() ? penInputs.reference[slot] : penInputs.normal[slot]) : uSample;
    capturedUp = automatic() && !calibration() && slot == 3 && !contactDown();
}

void Imx1Asp::advance(uint64_t perclk2Cycles) {
    const auto divide = clockDivider();
    if (!remaining || !divide) return;
    uint64_t clocks = perclk2Cycles / divide + (perclk2Cycles % divide + phase) / divide;
    phase = (perclk2Cycles % divide + phase) % divide;
    while (remaining && clocks >= remaining) {
        clocks -= remaining;
        remaining = 0;
        if (idle) {
            idle = false;
            startSlot();
        } else {
            complete();
            if (automatic()) {
                if (++slot == (calibration() ? 3u : 4u)) {
                    slot = 0;
                    idle = true;
                    remaining = 1260u * ((sampleRate >> 4) & 63);
                    if (!remaining) { idle = false; startSlot(); }
                } else startSlot();
            }
        }
    }
    if (remaining) remaining -= clocks;
    else phase = 0;
}

void Imx1Asp::complete() {
    if (count == fifo.size()) {
        overflow = true;
        // Independent physical ring pointers are a virtual overrun policy.
    } else ++count;
    fifo[writePointer] = captured;
    if (capturedUp) penUp = true;
    writePointer = (writePointer + 1) % fifo.size();
    const auto threshold = compare & 0xffff;
    const auto channel = automatic() ? slot : 3u;
    if (channel && ((compare >> 16) & 3) == channel &&
        ((compare & 0x40000) ? captured > threshold : captured < threshold)) compare |= 0x80000;
}
