#include "messages.h"

namespace frond::link {

Bytes encodeMessage(const Message& m) {
    Bytes b;
    b.reserve(kMessageHeader + m.payload.size());
    putU8(b, uint8_t(m.type));
    putU16(b, m.channel);
    putBytes(b, m.payload.data(), m.payload.size());
    return b;
}

bool decodeMessage(const uint8_t* p, size_t n, Message& out) {
    if (n < kMessageHeader || n - kMessageHeader > kMaxPayload) return false;
    if (p[0] < uint8_t(MsgType::Hello) || p[0] > uint8_t(MsgType::Radio)) return false;
    out.type = MsgType(p[0]);
    out.channel = getU16(p + 1);
    out.payload.assign(p + kMessageHeader, p + n);
    return true;
}

Bytes encodeHello(const HelloBody& h) {
    Bytes b;
    putU16(b, h.minVersion);
    putU16(b, h.maxVersion);
    putStr8(b, h.name);
    return b;
}

bool decodeHello(const Bytes& p, HelloBody& out) {
    Reader r(p.data(), p.size());
    out.minVersion = r.u16();
    out.maxVersion = r.u16();
    out.name = r.str8();
    // Later versions may add fields after these.
    return r.ok() && out.minVersion <= out.maxVersion;
}

Bytes encodeOpen(const OpenBody& o) {
    Bytes b;
    putU8(b, uint8_t(o.kind));
    putStr8(b, o.machine);
    putStr8(b, o.device);
    return b;
}

bool decodeOpen(const Bytes& p, OpenBody& out) {
    Reader r(p.data(), p.size());
    uint8_t k = r.u8();
    out.machine = r.str8();
    out.device = r.str8();
    if (!r.ok() || k < 1 || k > 3) return false;
    out.kind = ChannelKind(k);
    return true;
}

Bytes encodeMachine(const MachineBody& m) {
    Bytes b;
    putStr8(b, m.machine);
    putStr8(b, m.device);
    return b;
}

bool decodeMachine(const Bytes& p, MachineBody& out) {
    Reader r(p.data(), p.size());
    out.machine = r.str8();
    out.device = r.str8();
    return r.ok();
}

Bytes encodeRadio(const RadioBody& r) {
    Bytes b;
    putU8(b, r.senderHolds ? 0 : 1);
    putU64(b, r.at);
    return b;
}

bool decodeRadio(const Bytes& p, RadioBody& out) {
    Reader r(p.data(), p.size());
    const uint8_t side = r.u8();
    out.at = r.u64();
    if (!r.ok() || side > 1) return false;
    out.senderHolds = side == 0;
    return true;
}

}  // namespace frond::link
