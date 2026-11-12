#include "Sir.h"

namespace frondIr {

namespace {

constexpr uint8_t xbof = 0xff, bof = 0xc0, eof = 0xc1, escape = 0x7d;

void Put(uint8_t byte, std::vector<uint8_t>& out) {
    if (byte == bof || byte == eof || byte == escape) {
        out.push_back(escape);
        out.push_back(byte ^ 0x20);
    } else {
        out.push_back(byte);
    }
}

}  // namespace

uint16_t Fcs16(const uint8_t* data, size_t size, uint16_t fcs) {
    for (size_t i = 0; i < size; ++i) {
        fcs ^= data[i];
        for (int bit = 0; bit < 8; ++bit) fcs = (fcs & 1) ? (fcs >> 1) ^ 0x8408 : fcs >> 1;
    }
    return fcs;
}

void SirWrap(const uint8_t* frame, size_t size, unsigned extraBofs, std::vector<uint8_t>& out) {
    out.insert(out.end(), extraBofs, xbof);
    out.push_back(bof);
    for (size_t i = 0; i < size; ++i) Put(frame[i], out);
    const uint16_t fcs = ~Fcs16(frame, size);
    Put(fcs & 0xff, out);
    Put(fcs >> 8, out);
    out.push_back(eof);
}

bool SirUnwrapper::Feed(uint8_t byte) {
    if (byte == bof) {
        // A BOF inside a frame starts it over; several BOFs in a row are allowed.
        inFrame = true;
        escaped = false;
        frame.clear();
        return false;
    }
    if (!inFrame) return false;
    if (byte == eof) {
        inFrame = false;
        if (escaped || frame.size() < 4 || Fcs16(frame.data(), frame.size()) != fcsGood) {
            ++bad;
            return false;
        }
        done.assign(frame.begin(), frame.end() - 2);
        return true;
    }
    if (byte == escape) {
        escaped = true;
        return false;
    }
    if (escaped) byte ^= 0x20;
    escaped = false;
    if (frame.size() >= maxFrame) {
        ++bad;
        inFrame = false;
        return false;
    }
    frame.push_back(byte);
    return false;
}

void SirUnwrapper::Reset() {
    inFrame = escaped = false;
    frame.clear();
}

}  // namespace frondIr
