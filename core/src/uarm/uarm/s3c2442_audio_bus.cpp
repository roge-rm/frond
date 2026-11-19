#include "s3c2442_blocks.h"

// DMA, IIS and IIC: the blocks the rx1950's sound driver (wavedev.dll) runs on.

void S3cDma::reset() {
    for (auto& c : channel) c = {};
    for (auto& c : current) c = {};
}

uint32_t S3cDma::unitBytes(unsigned ch) const {
    const uint32_t dsz = (channel[ch].dcon >> 20) & 3;
    return dsz == 3 ? 0 : 1u << dsz;
}

void S3cDma::load(unsigned ch) {
    auto& c = channel[ch];
    current[ch] = {c.disrc, c.didst, c.dcon & 0xfffff};
}

S3cAccess S3cDma::read(uint32_t offset, uint32_t& value) {
    if (offset >= 0x100 || offset & 3 || (offset & 0x3f) > 0x20) return S3cAccess::unknown;
    const unsigned ch = offset / 0x40;
    const auto& c = channel[ch];
    switch (offset & 0x3f) {
        case 0x00: value = c.disrc; break;
        case 0x04: value = c.disrcc; break;
        case 0x08: value = c.didst; break;
        case 0x0c: value = c.didstc; break;
        case 0x10: value = c.dcon; break;
        // DSTAT: STAT ready (a unit moves within one request), the current count. An off channel keeps the
        // count it stopped at.
        case 0x14: value = current[ch].count & 0xfffff; break;
        case 0x18: value = current[ch].src; break;  // DCSRC
        case 0x1c: value = current[ch].dst; break;  // DCDST
        case 0x20: value = c.dmasktrig; break;
    }
    return S3cAccess::ok;
}

S3cAccess S3cDma::write(uint32_t offset, uint32_t value) {
    if (offset >= 0x100 || offset & 3 || (offset & 0x3f) > 0x20) return S3cAccess::unknown;
    const unsigned ch = offset / 0x40;
    auto& c = channel[ch];
    switch (offset & 0x3f) {
        case 0x00: c.disrc = value & 0x7fffffff; break;
        case 0x04: c.disrcc = value & 3; break;
        case 0x08: c.didst = value & 0x7fffffff; break;
        case 0x0c: c.didstc = value & 7; break;
        case 0x10: c.dcon = value; break;
        case 0x14: case 0x18: case 0x1c: break;  // read only
        case 0x20: {
            // SW_TRIG on a channel set to software requests would move memory now.
            if ((value & 1) && !(c.dcon & (1u << 23))) return S3cAccess::unsupported;
            const bool was = on(ch);
            c.dmasktrig = value & 6;  // STOP and ON; a stop turns the channel off at once
            if (value & 4) c.dmasktrig = 0;
            if (!was && on(ch)) load(ch);
            break;
        }
    }
    return S3cAccess::ok;
}

int S3cDma::channelFor(S3cDmaRequest request) const {
    for (unsigned ch = 0; ch < 4; ++ch) {
        const uint32_t dcon = channel[ch].dcon;
        if (!on(ch) || !(dcon & (1u << 23))) continue;
        const uint32_t source = (dcon >> 24) & 7;
        if (request == S3cDmaRequest::iisOut && ch == 2 && source == 0) return int(ch);
        if (request == S3cDmaRequest::iisIn && ((ch == 1 && source == 2) || (ch == 2 && source == 1))) return int(ch);
    }
    return -1;
}

void S3cDma::service(unsigned ch, S3cIntc& intc) {
    if (!on(ch)) return;
    auto& c = channel[ch];
    auto& cur = current[ch];
    const uint32_t size = unitBytes(ch);
    const unsigned n = (c.dcon & (1u << 28)) ? 4 : 1;  // TSZ: a burst of four
    for (unsigned i = 0; i < n && cur.count; ++i) {
        uint32_t value = 0;
        bool ok = size && bus && bus(cur.src, size, false, value);
        if (ok) ok = bus(cur.dst, size, true, value);
        if (!ok) {
            ++busErrors;
            lastBusError = cur.src;
        }
        ++units;
        // INC: 0 steps the address by the unit, 1 keeps it fixed (a FIFO).
        if (!(c.disrcc & 1)) cur.src += size;
        if (!(c.didstc & 1)) cur.dst += size;
        --cur.count;
    }
    if (cur.count) return;
    if (c.dcon & (1u << 29)) intc.pulse(s3cIrq::dma0 + ch);
    // RELOAD set turns the channel off at the end; clear starts the next run from the initial registers.
    if (c.dcon & (1u << 22)) c.dmasktrig &= ~2u;
    else load(ch);
}

void S3cIis::reset() {
    iiscon = 0x100;
    iismod = iispsr = iisfcon = 0;
    txHead = txCount = rxHead = rxCount = 0;
    for (auto& v : tx) v = 0;
    for (auto& v : rx) v = 0;
}

S3cAccess S3cIis::read(uint32_t offset, uint32_t& value) {
    switch (offset) {
        // IISCON: left/right index, transmit FIFO ready (room in it), receive FIFO ready (data in it).
        case 0x00: value = iiscon | (txCount < depth ? 0x80 : 0) | (rxCount ? 0x40 : 0); return S3cAccess::ok;
        case 0x04: value = iismod; return S3cAccess::ok;
        case 0x08: value = iispsr; return S3cAccess::ok;
        case 0x0c: value = (iisfcon & 0xf000) | (txCount << 6) | rxCount; return S3cAccess::ok;
        case 0x10: value = pop(); return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}

S3cAccess S3cIis::write(uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0x00: iiscon = (iiscon & 0x100) | (value & 0x3f); return S3cAccess::ok;
        case 0x04: iismod = value & 0x3ff; return S3cAccess::ok;
        case 0x08: iispsr = value & 0x3ff; return S3cAccess::ok;
        case 0x0c:
            iisfcon = value & 0xf000;
            // A FIFO turned off empties.
            if (!(iisfcon & 0x2000)) txHead = txCount = 0;
            if (!(iisfcon & 0x1000)) rxHead = rxCount = 0;
            return S3cAccess::ok;
        case 0x10: push(uint16_t(value)); return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}

void S3cIis::push(uint16_t value) {
    if (!(iisfcon & 0x2000) || txCount == depth) return;
    tx[(txHead + txCount++) % depth] = value;
}

uint16_t S3cIis::pop() {
    if (!rxCount) return 0;
    const uint16_t v = rx[rxHead];
    rxHead = (rxHead + 1) % depth;
    --rxCount;
    return v;
}

bool S3cIis::frame(int16_t out[2], const int16_t in[2]) {
    bool sent = false;
    if (transmitting()) {
        // ASSUMPTION "IIS left first": the first halfword of each pair is the left channel.
        for (unsigned i = 0; i < 2; ++i) {
            if (txCount) {
                out[i] = int16_t(tx[txHead]);
                txHead = (txHead + 1) % depth;
                --txCount;
            } else {
                out[i] = 0;
                if (i == 0) ++underruns;
            }
        }
        ++framesSent;
        sent = true;
    }
    if (receiving() && (iisfcon & 0x1000)) {
        for (unsigned i = 0; i < 2; ++i) {
            if (rxCount < depth) rx[(rxHead + rxCount++) % depth] = uint16_t(in[i]);
            else if (i == 0) ++overruns;
        }
        ++framesReceived;
    }
    return sent;
}

namespace {
    constexpr uint32_t pending = 1u << 4, busy = 1u << 5, txrx = 1u << 4, nack = 1u << 0;
}

void S3cIic::reset() { iiccon = 0; iicstat = 0; iicadd = 0; iicds = 0; iiclc = 0; }

S3cAccess S3cIic::read(uint32_t offset, uint32_t& value) {
    switch (offset) {
        case 0x00: value = iiccon; return S3cAccess::ok;
        case 0x04: value = iicstat & ~nack; return S3cAccess::ok;
        case 0x08: value = iicadd; return S3cAccess::ok;
        case 0x0c: value = iicds; return S3cAccess::ok;
        case 0x10: value = iiclc; return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}

S3cAccess S3cIic::write(uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0x00: {
            // Clearing the pending bit lets the next byte go, which finishes at once.
            const bool resume = (iiccon & pending) && !(value & pending);
            iiccon = value & 0xff;
            if (resume && (iicstat & busy) && (iicstat & txrx)) {
                ++bytes;
                // Mode bits 7:6: 11 master transmit, 10 master receive.
                if (iicstat & (1u << 6)) {
                    if (device) device->write(uint8_t(iicds));
                } else {
                    iicds = device ? device->read() : 0;
                }
                iiccon |= pending;
            }
            return S3cAccess::ok;
        }
        case 0x04: {
            const bool start = (value & busy) && (value & txrx);
            const bool stop = (iicstat & busy) && !(value & busy);
            iicstat = value & 0xf0;
            if (stop && device) device->stop();
            if (start) {
                ++bytes;  // the address byte
                if (device) device->start(uint8_t(iicds));
                iiccon |= pending;
            }
            return S3cAccess::ok;
        }
        case 0x08: iicadd = value & 0xfe; return S3cAccess::ok;
        case 0x0c: iicds = value & 0xff; return S3cAccess::ok;
        case 0x10: iiclc = value & 7; return S3cAccess::ok;
    }
    return S3cAccess::unknown;
}
