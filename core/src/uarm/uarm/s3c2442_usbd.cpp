#include <algorithm>
#include <cstring>

#include "s3c2442_blocks.h"

// Bit names follow the manual (and Linux's s3c2410_udc.h, which has the same layout).
namespace {
    constexpr uint32_t first = 0x140, last = 0x1fc, dmaFirst = 0x200, dmaLast = 0x26c;
    constexpr uint32_t funcAddr = 0x140, pwrReg = 0x144, epInt = 0x148, usbInt = 0x158, epIntEn = 0x15c,
                       usbIntEn = 0x16c, indexReg = 0x178, maxpReg = 0x180, csr0InCsr1 = 0x184, inCsr2Reg = 0x188,
                       outCsr1Reg = 0x190, outCsr2Reg = 0x194, outCount1 = 0x198, outCount2 = 0x19c;
    // EP0_CSR
    constexpr uint8_t e0OutReady = 0x01, e0InReady = 0x02, e0SentStall = 0x04, e0DataEnd = 0x08, e0SetupEnd = 0x10,
                      e0SendStall = 0x20, e0ServicedOut = 0x40, e0ServicedSetupEnd = 0x80;
    // IN_CSR1, IN_CSR2
    constexpr uint8_t inReady = 0x01, inFlush = 0x08, inSendStall = 0x10, inSentStall = 0x20, inClearToggle = 0x40;
    constexpr uint8_t in2AutoSet = 0x80, in2ModeIn = 0x20;
    // OUT_CSR1, OUT_CSR2
    constexpr uint8_t outPktReady = 0x01, outFlush = 0x10, outSendStall = 0x20, outSentStall = 0x40, outClearToggle = 0x80;
    constexpr uint8_t out2AutoClear = 0x80;
    constexpr uint32_t usbReset = 0x04;
    bool fifoReg(uint32_t offset) { return offset >= 0x1c0 && offset <= 0x1d0; }
}

void S3cUsbDevice::reset() {
    for (auto& r : regs) r = 0;
    reg(pwrReg) = 0x08;     // PWR_REG as after reset
    reg(epIntEn) = 0x1f;    // EP_INT_EN_REG
    reg(usbIntEn) = 0x04;   // USB_INT_EN_REG: reset interrupt on
    clearRun();
}

void S3cUsbDevice::clearRun() {
    std::fill(std::begin(maxp), std::end(maxp), 1);
    std::fill(std::begin(inCsr1), std::end(inCsr1), 0);
    std::fill(std::begin(inCsr2), std::end(inCsr2), 0);
    std::fill(std::begin(outCsr1), std::end(outCsr1), 0);
    std::fill(std::begin(outCsr2), std::end(outCsr2), 0);
    for (auto& f : fifo) f.clear();
    std::fill(std::begin(dmaRegs), std::end(dmaRegs), 0);
    ep0Csr = 0;
    activeAddress = uint8_t(reg(funcAddr) & 0x7f);
    controlOpen = addressPending = setupIn = false;
}

bool S3cUsbDevice::irq() const {
    return (reg(epInt) & reg(epIntEn) & 0x1f) || (reg(usbInt) & reg(usbIntEn) & 7);
}

// MAXP_REG counts 8-byte units (1, 2, 4 or 8).
unsigned S3cUsbDevice::packetSize(unsigned ep) const { return std::max(1u, unsigned(maxp[ep] & 0x0f)) * 8; }

S3cAccess S3cUsbDevice::read(uint32_t offset, uint32_t& value) {
    if (offset & 3) return S3cAccess::unknown;
    if (offset >= dmaFirst && offset <= dmaLast) { value = dmaRegs[(offset - dmaFirst) / 4]; return S3cAccess::ok; }
    if (offset < first || offset > last) return S3cAccess::unknown;
    const unsigned ep = index();
    if (fifoReg(offset)) {
        auto& f = fifo[(offset - 0x1c0) / 4];
        value = 0;
        if (!f.empty()) { value = f.front(); f.erase(f.begin()); }
        // AUTO_CLR: the packet is done once the CPU has read all of it.
        const unsigned e = (offset - 0x1c0) / 4;
        if (e && f.empty() && (outCsr2[e] & out2AutoClear) && (outCsr1[e] & outPktReady)) outCsr1[e] &= ~outPktReady;
        return S3cAccess::ok;
    }
    switch (offset) {
        case funcAddr: value = reg(funcAddr) & (addressPending ? 0xff : 0x7f); return S3cAccess::ok;
        case maxpReg: value = ep < 5 ? maxp[ep] : 0; return S3cAccess::ok;
        case csr0InCsr1: value = ep == 0 ? ep0Csr & 0x3f : ep < 5 ? inCsr1[ep] : 0; return S3cAccess::ok;
        case inCsr2Reg: value = ep && ep < 5 ? inCsr2[ep] : 0; return S3cAccess::ok;
        case outCsr1Reg: value = ep && ep < 5 ? outCsr1[ep] : 0; return S3cAccess::ok;
        case outCsr2Reg: value = ep && ep < 5 ? outCsr2[ep] : 0; return S3cAccess::ok;
        case outCount1: value = ep < 5 ? fifo[ep].size() & 0xff : 0; return S3cAccess::ok;
        case outCount2: value = ep < 5 ? (fifo[ep].size() >> 8) & 0xff : 0; return S3cAccess::ok;
        default: value = reg(offset); return S3cAccess::ok;
    }
}

S3cAccess S3cUsbDevice::write(uint32_t offset, uint32_t value) {
    if (offset & 3) return S3cAccess::unknown;
    if (offset >= dmaFirst && offset <= dmaLast) {
        dmaRegs[(offset - dmaFirst) / 4] = value & 0xff;
        return S3cAccess::ok;
    }
    if (offset < first || offset > last) return S3cAccess::unknown;
    const uint8_t v = uint8_t(value);
    const unsigned ep = index();
    if (fifoReg(offset)) {
        const unsigned e = (offset - 0x1c0) / 4;
        auto& f = fifo[e];
        if (f.size() < 256) f.push_back(v);
        // AUTO_SET: a full packet goes out without the CPU setting IN_PKT_RDY.
        if (e && (inCsr2[e] & in2AutoSet) && f.size() >= packetSize(e)) inCsr1[e] |= inReady;
        return S3cAccess::ok;
    }
    switch (offset) {
        case funcAddr:
            reg(funcAddr) = v;
            // The new address counts after the status stage of the SET_ADDRESS that set it.
            if (controlOpen) addressPending = true;
            else activeAddress = v & 0x7f;
            break;
        case epInt: case usbInt: reg(offset) &= ~uint32_t(v); break;
        case pwrReg: reg(pwrReg) = (reg(pwrReg) & 0x0a) | (v & 0x85); break;
        case maxpReg: if (ep < 5) maxp[ep] = v; break;
        case csr0InCsr1:
            if (ep == 0) {
                // OUT_PKT_RDY and SETUP_END clear through their serviced bits; IN_PKT_RDY and DATA_END are set
                // only; SENT_STALL clears on a write of zero; SEND_STALL keeps what's written.
                if (v & e0ServicedOut) { ep0Csr &= ~e0OutReady; fifo[0].clear(); }
                if (v & e0ServicedSetupEnd) ep0Csr &= ~e0SetupEnd;
                if (!(v & e0SentStall)) ep0Csr &= ~e0SentStall;
                ep0Csr = uint8_t((ep0Csr & ~e0SendStall) | (v & (e0InReady | e0DataEnd | e0SendStall)));
            } else if (ep < 5) {
                auto& c = inCsr1[ep];
                if (v & inFlush) { fifo[ep].clear(); c &= ~inReady; }
                if (!(v & inSentStall)) c &= ~inSentStall;
                c = uint8_t((c & ~inSendStall) | (v & (inReady | inSendStall)));
                (void)inClearToggle;  // data toggles aren't modelled: the host takes every packet once
            }
            break;
        case inCsr2Reg: if (ep && ep < 5) inCsr2[ep] = v & 0xf0; break;
        case outCsr1Reg:
            // ASSUMPTION "USB EP0 serviced bits in OUT_CSR1": with INDEX 0, WM5's sc2440usbfn.dll sets
            // SERVICED_OUT_PKT_RDY (bit 6) and SERVICED_SETUP_END (bit 7) through OUT_CSR1, then rewrites EP0_CSR
            // with what it read; the driver works on a real unit, so EP0 takes them here too.
            if (ep == 0) {
                if (v & e0ServicedOut) { ep0Csr &= ~e0OutReady; fifo[0].clear(); }
                if (v & e0ServicedSetupEnd) ep0Csr &= ~e0SetupEnd;
            }
            if (ep && ep < 5) {
                auto& c = outCsr1[ep];
                if (v & outFlush) { fifo[ep].clear(); c &= ~outPktReady; }
                // OUT_PKT_RDY and SENT_STALL clear on a write of zero.
                if (!(v & outPktReady) && (c & outPktReady)) { c &= ~outPktReady; fifo[ep].clear(); }
                if (!(v & outSentStall)) c &= ~outSentStall;
                c = uint8_t((c & ~outSendStall) | (v & outSendStall));
                (void)outClearToggle;
            }
            break;
        case outCsr2Reg: if (ep && ep < 5) outCsr2[ep] = v & 0xe0; break;
        case outCount1: case outCount2: break;  // read only
        default: reg(offset) = v; break;
    }
    return S3cAccess::ok;
}

void S3cUsbDevice::busReset() {
    reg(funcAddr) = 0;
    activeAddress = 0;
    reg(usbInt) |= usbReset;
    ep0Csr = 0;
    for (unsigned e = 0; e < 5; ++e) {
        inCsr1[e] = outCsr1[e] = 0;
        fifo[e].clear();
    }
    controlOpen = addressPending = setupIn = false;
}

void S3cUsbDevice::statusDone() {
    controlOpen = false;
    if (addressPending) {
        activeAddress = uint8_t(reg(funcAddr) & 0x7f);
        reg(funcAddr) &= 0x7f;
        addressPending = false;
    }
}

UsbPacketResult S3cUsbDevice::setup(const uint8_t* data) {
    using R = UsbPacketResult;
    // A device always takes a SETUP, even with the last one unread: the new one replaces it.
    // ASSUMPTION "USB SETUP_END": a SETUP inside an open control transfer ends it with SETUP_END.
    const bool ended = controlOpen;
    fifo[0].assign(data, data + 8);
    ep0Csr = uint8_t((ep0Csr & e0SetupEnd) | e0OutReady | (ended ? e0SetupEnd : 0));
    controlOpen = true;
    setupIn = data[0] & 0x80;
    ++setups;
    endpointInterrupt(0);
    return R::Ack;
}

UsbPacketResult S3cUsbDevice::out(unsigned ep, const uint8_t* data, unsigned size) {
    using R = UsbPacketResult;
    if (ep > 4) return R::Unsupported;
    if (ep == 0) {
        // ASSUMPTION "USB EP0 SEND_STALL clears": the stall handshake ends the transfer and clears SEND_STALL.
        if (ep0Csr & e0SendStall) {
            ep0Csr = uint8_t((ep0Csr & ~e0SendStall) | e0SentStall);
            controlOpen = false;
            endpointInterrupt(0);
            return R::Stall;
        }
        if (!size) {
            // Status stage of an IN transfer: done once the CPU set DATA_END and the last packet went.
            if ((ep0Csr & e0DataEnd) && !(ep0Csr & e0InReady)) {
                ep0Csr &= ~e0DataEnd;
                statusDone();
                endpointInterrupt(0);
                ++outs;
                return R::Ack;
            }
            // ASSUMPTION "USB early status ends with SETUP_END": the host's status stage while the CPU still has
            // data loaded (or hasn't set DATA_END) ends the transfer: SETUP_END, the FIFO flushed, ACK.
            if (controlOpen && setupIn && !(ep0Csr & e0OutReady)) {
                ep0Csr = uint8_t((ep0Csr & ~(e0InReady | e0DataEnd)) | e0SetupEnd);
                fifo[0].clear();
                statusDone();
                endpointInterrupt(0);
                ++outs;
                return R::Ack;
            }
            return R::Nak;
        }
        if (ep0Csr & e0OutReady) return R::Nak;
        fifo[0].assign(data, data + size);
        ep0Csr |= e0OutReady;
        ++outs;
        endpointInterrupt(0);
        return R::Ack;
    }
    if (inCsr2[ep] & in2ModeIn) return R::Unsupported;
    auto& c = outCsr1[ep];
    if (c & outSendStall) {
        c |= outSentStall;
        endpointInterrupt(ep);
        return R::Stall;
    }
    if (c & outPktReady) return R::Nak;
    fifo[ep].assign(data, data + size);
    c |= outPktReady;
    ++outs;
    endpointInterrupt(ep);
    return R::Ack;
}

UsbPacketResult S3cUsbDevice::in(unsigned ep, uint8_t* data, unsigned* size, bool take) {
    using R = UsbPacketResult;
    if (ep > 4) return R::Unsupported;
    // One packet of at most MAXP bytes; the rest stays in the FIFO for the next IN_PKT_RDY.
    auto give = [&](std::vector<uint8_t>& f) {
        const unsigned n = unsigned(std::min<size_t>(f.size(), packetSize(ep)));
        if (size) *size = n;
        if (take && data && n) std::memcpy(data, f.data(), n);
        if (take) f.erase(f.begin(), f.begin() + n);
    };
    if (ep == 0) {
        if (ep0Csr & e0SendStall) {
            if (!take) return R::Stall;
            ep0Csr = uint8_t((ep0Csr & ~e0SendStall) | e0SentStall);
            controlOpen = false;
            endpointInterrupt(0);
            return R::Stall;
        }
        if (ep0Csr & e0OutReady) return R::Nak;
        if (ep0Csr & e0InReady) {
            give(fifo[0]);
            if (!take) return R::Ack;
            ep0Csr &= ~e0InReady;
            ++ins;
            endpointInterrupt(0);
            return R::Ack;
        }
        // Status stage of a transfer with no data or OUT data: a zero-length packet once DATA_END is set.
        if ((ep0Csr & e0DataEnd) && !setupIn) {
            if (size) *size = 0;
            if (!take) return R::Ack;
            ep0Csr &= ~e0DataEnd;
            statusDone();
            ++ins;
            endpointInterrupt(0);
            return R::Ack;
        }
        return R::Nak;
    }
    if (!(inCsr2[ep] & in2ModeIn)) return R::Unsupported;
    auto& c = inCsr1[ep];
    if (c & inSendStall) {
        if (!take) return R::Stall;
        c |= inSentStall;
        endpointInterrupt(ep);
        return R::Stall;
    }
    if (!(c & inReady)) return R::Nak;
    give(fifo[ep]);
    if (!take) return R::Ack;
    c &= ~inReady;
    // AUTO_SET: another full packet left in the FIFO goes out next.
    if ((inCsr2[ep] & in2AutoSet) && fifo[ep].size() >= packetSize(ep)) c |= inReady;
    ++ins;
    endpointInterrupt(ep);
    return R::Ack;
}

bool S3cUsbDevice::outReady(unsigned ep) const {
    if (ep > 4) return false;
    if (ep == 0) return !(ep0Csr & e0OutReady);
    return !(inCsr2[ep] & in2ModeIn) && !(outCsr1[ep] & outPktReady);
}
