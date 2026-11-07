#include "link.h"

#include <algorithm>

namespace frond::link {

Link::Link(const LinkConfig& cfg, const InstallId& peerId, const Key32& peerKey, uint64_t now)
    : initiator_(true), cfg_(cfg), peerId_(peerId), peerKey_(peerKey), mux_(true), started_(now) {}

Link::Link(const LinkConfig& cfg, KeyLookup lookup, uint64_t now)
    : initiator_(false), cfg_(cfg), lookup_(std::move(lookup)), mux_(false), started_(now) {}

Link::~Link() { wipe(&cfg_.local.identity, sizeof cfg_.local.identity); }

const Hash& Link::handshakeHash() const {
    static const Hash none{};
    return hs_ ? hs_->handshakeHash() : none;
}

Bytes Link::prologue() const {
    Bytes p;
    putHeader(p, FrameMode::Noise);
    if (initiator_) {
        putBytes(p, cfg_.local.id);
        putBytes(p, peerId_);
    } else {
        putBytes(p, peerId_);
        putBytes(p, cfg_.local.id);
    }
    return p;
}

void Link::fail(LinkError e, bool refuse, RefuseReason why) {
    if (state_ == LinkState::Down) return;
    state_ = LinkState::Down;
    error_ = e;
    if (refuse) {
        Bytes f = refuseFrame(why);
        putBytes(out_, f.data(), f.size());
    }
    mux_.reset(e == LinkError::Timeout ? CloseReason::Timeout : CloseReason::Gone);
    send_.clear();
    recv_.clear();
}

void Link::start() {
    if (!initiator_ || hs_) return;
    Bytes pro = prologue();
    hs_ = std::make_unique<KKHandshake>(true, cfg_.local.identity, peerKey_, pro.data(), pro.size());
    if (haveTestEphemeral_) hs_->setEphemeral(testEphemeral_);
    Bytes body = pro;
    if (!hs_->writeMessage(nullptr, 0, body)) return fail(LinkError::Protocol);
    writeFrame(out_, body);
}

void Link::receive(const uint8_t* p, size_t n, uint64_t now) {
    if (state_ == LinkState::Down) return;
    reader_.feed(p, n);
    Bytes body;
    while (state_ != LinkState::Down && reader_.next(body)) {
        if (state_ == LinkState::Handshake)
            handleHandshake(body, now);
        else
            handleRecord(body, now);
    }
    if (reader_.error()) fail(LinkError::Protocol);
}

void Link::handleHandshake(const Bytes& body, uint64_t now) {
    uint8_t version;
    FrameMode mode;
    if (!parseHeader(body, version, mode)) return fail(LinkError::Protocol);
    if (mode == FrameMode::Refuse) {
        if (body.size() < kHeaderLen + 1) return fail(LinkError::Protocol);
        switch (RefuseReason(body[kHeaderLen])) {
            case RefuseReason::Version: return fail(LinkError::Version);
            case RefuseReason::UnknownPeer: return fail(LinkError::UnknownPeer);
            case RefuseReason::KeyChanged: return fail(LinkError::KeyChanged);
            default: return fail(LinkError::Protocol);
        }
    }
    if (version != kWireVersion) return fail(LinkError::Version, !initiator_, RefuseReason::Version);
    if (mode != FrameMode::Noise) return fail(LinkError::Protocol, !initiator_, RefuseReason::NotPairing);
    Bytes payload;
    if (!initiator_) {
        if (hs_) return fail(LinkError::Protocol);
        Reader r(body.data() + kHeaderLen, body.size() - kHeaderLen);
        peerId_ = r.array<16>();
        InstallId target = r.array<16>();
        if (!r.ok()) return fail(LinkError::Protocol);
        if (target != cfg_.local.id || !lookup_ || !lookup_(peerId_, peerKey_))
            return fail(LinkError::UnknownPeer, true, RefuseReason::UnknownPeer);
        knownPeer_ = true;
        Bytes pro = prologue();
        hs_ = std::make_unique<KKHandshake>(false, cfg_.local.identity, peerKey_, pro.data(), pro.size());
        if (haveTestEphemeral_) hs_->setEphemeral(testEphemeral_);
        size_t at = kHeaderLen + 32;
        // A paired id whose handshake fails means one side's key isn't the pinned one.
        if (!hs_->readMessage(body.data() + at, body.size() - at, payload))
            return fail(LinkError::KeyChanged, true, RefuseReason::KeyChanged);
        Bytes reply;
        putHeader(reply, FrameMode::Noise);
        if (!hs_->writeMessage(nullptr, 0, reply)) return fail(LinkError::Protocol);
        writeFrame(out_, reply);
    } else {
        if (!hs_ || !hs_->readMessage(body.data() + kHeaderLen, body.size() - kHeaderLen, payload))
            return fail(LinkError::KeyChanged);
    }
    becameSecure(now);
}

void Link::becameSecure(uint64_t now) {
    hs_->split(send_, recv_);
    state_ = LinkState::Hello;
    reader_.setMaxBody(kMaxRecordBody);
    lastPong_ = now;
    lastPing_ = now;
    Message hello{MsgType::Hello, 0, encodeHello({cfg_.minVersion, cfg_.maxVersion, cfg_.local.name})};
    writeRecord(hello, out_);
    sentHello_ = true;
}

void Link::writeRecord(const Message& m, Bytes& out) {
    if (!send_.hasKey()) return;  // down: never write a record in the clear
    Bytes plain = encodeMessage(m);
    uint16_t len = uint16_t(plain.size() + kTagLen);
    uint8_t ad[2] = {uint8_t(len >> 8), uint8_t(len)};
    putU16(out, len);
    if (!send_.encrypt(ad, 2, plain.data(), plain.size(), out)) fail(LinkError::Protocol);
    wipe(plain.data(), plain.size());
}

void Link::handleRecord(const Bytes& body, uint64_t now) {
    uint8_t ad[2] = {uint8_t(body.size() >> 8), uint8_t(body.size())};
    Bytes plain;
    if (!recv_.decrypt(ad, 2, body.data(), body.size(), plain)) return fail(LinkError::Decrypt);
    Message m;
    if (!decodeMessage(plain.data(), plain.size(), m)) return fail(LinkError::Protocol);
    wipe(plain.data(), plain.size());
    if (m.channel == 0 && m.type != MsgType::State) return handleLinkMessage(m, now);
    if (state_ != LinkState::Up) return fail(LinkError::Protocol);  // channels only after hello
    if (!mux_.receive(m)) {
        Message c{MsgType::Close, 0, Bytes{uint8_t(CloseReason::Protocol)}};
        writeRecord(c, out_);
        fail(LinkError::Protocol);
    }
}

void Link::handleLinkMessage(const Message& m, uint64_t now) {
    switch (m.type) {
        case MsgType::Hello: {
            HelloBody h;
            if (state_ != LinkState::Hello || !decodeHello(m.payload, h)) return fail(LinkError::Protocol);
            uint16_t lo = std::max(h.minVersion, cfg_.minVersion);
            uint16_t hi = std::min(h.maxVersion, cfg_.maxVersion);
            if (lo > hi) {
                Message c{MsgType::Close, 0, Bytes{uint8_t(CloseReason::Version)}};
                writeRecord(c, out_);
                return fail(LinkError::Version);
            }
            version_ = hi;
            peerName_ = h.name;
            state_ = LinkState::Up;
            lastPong_ = now;
            lastPing_ = now;
            return;
        }
        case MsgType::Ping:
            if (m.payload.size() != 8) return fail(LinkError::Protocol);
            mux_.queueControl({MsgType::Pong, 0, m.payload});
            return;
        case MsgType::Pong: {
            if (m.payload.size() != 8) return fail(LinkError::Protocol);
            uint64_t sent = getU64(m.payload.data());
            if (sent <= now) rtt_ = now - sent;
            lastPong_ = now;
            unanswered_ = 0;
            return;
        }
        case MsgType::Close: {
            uint8_t why = m.payload.empty() ? 0 : m.payload[0];
            return fail(why == uint8_t(CloseReason::Version) ? LinkError::Version : LinkError::Closed);
        }
        default:
            return fail(LinkError::Protocol);
    }
}

void Link::tick(uint64_t now) {
    if (state_ == LinkState::Down) return;
    if (state_ != LinkState::Up) {
        if (now - started_ >= kHandshakeMs) fail(LinkError::Timeout);
        return;
    }
    if (now - lastPing_ >= kPingMs) {
        // Down when the 4th ping is due with 3 still unanswered.
        if (unanswered_ >= kMissedPongs) return fail(LinkError::Timeout);
        Bytes b;
        putU64(b, now);
        mux_.queueControl({MsgType::Ping, 0, std::move(b)});
        unanswered_++;
        lastPing_ = now;
    }
}

bool Link::hasOutput() const {
    return !out_.empty() || (state_ == LinkState::Up && mux_.hasOutput());
}

Bytes Link::takeOutput(size_t max) {
    Bytes o;
    o.swap(out_);
    if (state_ != LinkState::Up) return o;
    Message m;
    while (o.size() < max && mux_.next(m)) writeRecord(m, o);
    return o;
}

void Link::close(CloseReason reason) {
    if (state_ == LinkState::Down) return;
    if (state_ != LinkState::Handshake) {
        Message c{MsgType::Close, 0, Bytes{uint8_t(reason)}};
        writeRecord(c, out_);
    }
    fail(LinkError::Closed);
}

}  // namespace frond::link
