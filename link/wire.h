// Framing on the TCP stream: every frame is a big-endian u16 length then the body.
// Before the session key, bodies start with "FRL", the wire version and a mode byte.
// After it, each body is one AEAD record.
#pragma once

#include "bytes.h"

namespace frond::link {

constexpr uint8_t kWireVersion = 1;
constexpr size_t kHeaderLen = 5;  // "FRL", version, mode

enum class FrameMode : uint8_t { Noise = 'K', Pair = 'P', Refuse = 'R' };

// Sent in a Refuse frame, cleartext, just before closing.
enum class RefuseReason : uint8_t {
    Version = 1,
    UnknownPeer = 2,
    KeyChanged = 3,
    WrongCode = 4,
    Expired = 5,
    LockedOut = 6,
    NotPairing = 7,
    Protocol = 8,
};

void putHeader(Bytes& b, FrameMode mode);
// Checks magic and mode; version is returned so the caller can refuse it.
bool parseHeader(const Bytes& body, uint8_t& version, FrameMode& mode);
void writeFrame(Bytes& out, const uint8_t* body, size_t n);
inline void writeFrame(Bytes& out, const Bytes& body) { writeFrame(out, body.data(), body.size()); }
Bytes refuseFrame(RefuseReason reason);

// Splits the stream into frame bodies. A frame longer than maxBody is a protocol error.
class FrameReader {
public:
    explicit FrameReader(size_t maxBody = 65535) : max_(maxBody) {}
    void setMaxBody(size_t n) { max_ = n; }
    void feed(const uint8_t* p, size_t n);
    bool next(Bytes& body);
    bool error() const { return error_; }

private:
    Bytes buf_;
    size_t pos_ = 0;
    size_t max_;
    bool error_ = false;
};

// What a listener sees in the first frame of a new connection, so it can hand it to a Link
// (Noise) or a Pairing (Pair).
enum class FirstFrame { NeedMore, Noise, Pair, BadVersion, Garbage };
FirstFrame peekFirstFrame(const uint8_t* p, size_t n);

}  // namespace frond::link
