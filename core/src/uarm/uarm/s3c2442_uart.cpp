#include "s3c2442_blocks.h"

namespace {
    constexpr size_t fifoSize = 64;
}

void S3cUart::reset() {
    ulcon = ucon = ufcon = umcon = ubrdiv = 0;
    rx.clear();
}

// UCON: receive mode bits 1-0 and transmit mode bits 3-2 (01 interrupt or polling), receive interrupt
// type bit 8 and transmit interrupt type bit 9 (0 pulse, 1 level).
void S3cUart::interrupts(S3cIntc& intc, bool sent) {
    const unsigned rxSub = channel * 3, txSub = rxSub + 1;
    const bool txOn = ((ucon >> 2) & 3) == 1, rxOn = (ucon & 3) == 1;
    intc.subLevel(txSub, txOn && (ucon & 0x200));
    // ASSUMPTION "pulse TX interrupt per byte": each byte leaves at once, so each write ends in an empty
    // transmitter, which is the pulse.
    if (sent && txOn && !(ucon & 0x200)) intc.subPulse(txSub);
    intc.subLevel(rxSub, rxOn && (ucon & 0x100) && !rx.empty());
}

S3cAccess S3cUart::read(uint32_t offset, uint32_t& value, S3cIntc& intc) {
    switch (offset) {
        case 0x00: value = ulcon; break;
        case 0x04: value = ucon; break;
        case 0x08: value = ufcon; break;
        case 0x0c: value = umcon; break;
        case 0x10: value = 6u | (rx.empty() ? 0u : 1u); break;
        case 0x14: value = 0; break;
        case 0x18:
            value = ufcon & 1 ? uint32_t(rx.size() >= fifoSize ? 0x40 : rx.size()) : 0;
            break;
        case 0x1c: value = cts ? 1 : 0; break;  // UMSTAT: CTS
        case 0x20: value = 0; break;
        case 0x24:
            value = 0;
            if (!rx.empty()) { value = rx.front(); rx.pop_front(); }
            interrupts(intc, false);
            break;
        case 0x28: value = ubrdiv; break;
        default: return S3cAccess::unknown;
    }
    return S3cAccess::ok;
}

S3cAccess S3cUart::write(uint32_t offset, uint32_t value, S3cIntc& intc) {
    switch (offset) {
        case 0x00: ulcon = value & 0x7f; break;
        case 0x04: ucon = value & 0xffff; interrupts(intc, false); break;
        case 0x08:
            // The FIFO reset bits clear themselves.
            if (value & 2) rx.clear();
            ufcon = value & 0xf9;
            interrupts(intc, false);
            break;
        case 0x0c: umcon = value & 0x11; break;
        case 0x10: case 0x14: case 0x18: case 0x1c: case 0x24: break;  // read only
        case 0x20:
            if (sink) sink(channel, uint8_t(value));
            interrupts(intc, true);
            break;
        case 0x28: ubrdiv = value & 0xffff; break;
        default: return S3cAccess::unknown;
    }
    return S3cAccess::ok;
}

void S3cUart::receive(uint8_t byte, S3cIntc& intc) {
    if (rx.size() < fifoSize) rx.push_back(byte);
    const bool rxOn = (ucon & 3) == 1;
    if (rxOn && !(ucon & 0x100)) intc.subPulse(channel * 3);
    interrupts(intc, false);
}
