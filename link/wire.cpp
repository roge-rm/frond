#include "wire.h"

namespace frond::link {

void putHeader(Bytes& b, FrameMode mode) {
    b.push_back('F');
    b.push_back('R');
    b.push_back('L');
    b.push_back(kWireVersion);
    b.push_back(uint8_t(mode));
}

bool parseHeader(const Bytes& body, uint8_t& version, FrameMode& mode) {
    if (body.size() < kHeaderLen || body[0] != 'F' || body[1] != 'R' || body[2] != 'L') return false;
    version = body[3];
    uint8_t m = body[4];
    if (m != 'K' && m != 'P' && m != 'R') return false;
    mode = FrameMode(m);
    return true;
}

void writeFrame(Bytes& out, const uint8_t* body, size_t n) {
    putU16(out, uint16_t(n));
    putBytes(out, body, n);
}

Bytes refuseFrame(RefuseReason reason) {
    Bytes body;
    putHeader(body, FrameMode::Refuse);
    body.push_back(uint8_t(reason));
    Bytes out;
    writeFrame(out, body);
    return out;
}

void FrameReader::feed(const uint8_t* p, size_t n) {
    if (error_) return;
    if (pos_ > 0 && pos_ == buf_.size()) {
        buf_.clear();
        pos_ = 0;
    } else if (pos_ > 65536) {
        buf_.erase(buf_.begin(), buf_.begin() + long(pos_));
        pos_ = 0;
    }
    buf_.insert(buf_.end(), p, p + n);
}

bool FrameReader::next(Bytes& body) {
    if (error_ || buf_.size() - pos_ < 2) return false;
    size_t len = getU16(buf_.data() + pos_);
    if (len > max_) {
        error_ = true;
        return false;
    }
    if (buf_.size() - pos_ < 2 + len) return false;
    body.assign(buf_.begin() + long(pos_ + 2), buf_.begin() + long(pos_ + 2 + len));
    pos_ += 2 + len;
    return true;
}

FirstFrame peekFirstFrame(const uint8_t* p, size_t n) {
    if (n < 2 + kHeaderLen) {
        // Reject early garbage without waiting for a whole header.
        const char magic[3] = {'F', 'R', 'L'};
        for (size_t i = 2; i < n && i < 5; i++)
            if (p[i] != uint8_t(magic[i - 2])) return FirstFrame::Garbage;
        return FirstFrame::NeedMore;
    }
    Bytes head(p + 2, p + 2 + kHeaderLen);
    uint8_t version;
    FrameMode mode;
    if (!parseHeader(head, version, mode)) return FirstFrame::Garbage;
    if (version != kWireVersion) return FirstFrame::BadVersion;
    if (mode == FrameMode::Noise) return FirstFrame::Noise;
    if (mode == FrameMode::Pair) return FirstFrame::Pair;
    return FirstFrame::Garbage;
}

}  // namespace frond::link
