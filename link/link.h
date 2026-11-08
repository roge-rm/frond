// One authenticated link between two paired installs: the KK handshake, encrypted records,
// hello and version, ping and pong, and the channel mux. No sockets: the owner feeds received
// bytes and the clock, and writes takeOutput() to the socket.
//
// First frame from the initiator: header 'K', idI[16], idR[16], then Noise message 1.
// The header and both ids are the Noise prologue. The responder answers with header 'K' and
// Noise message 2, or a Refuse frame. After that each frame is one record:
//   u16 length || ChaCha20-Poly1305(type u8, channel u16, payload up to 16 KiB)
// with the length as associated data and Noise's per-direction counter as the nonce, so a
// dropped, repeated or reordered record fails to open and the link drops.
#pragma once

#include "mux.h"
#include "noise.h"
#include "pairing.h"
#include "wire.h"

#include <functional>
#include <memory>

namespace frond::link {

constexpr uint16_t kLinkVersion = 1;
constexpr size_t kMaxRecordBody = kMessageHeader + kMaxPayload + kTagLen;

enum class LinkState { Handshake, Hello, Up, Down };

enum class LinkError {
    None,
    Closed,       // the peer closed it
    Protocol,
    Decrypt,      // a record failed to open: tampered, replayed or reordered
    Version,      // no version in common
    UnknownPeer,  // the responder has no key for us
    KeyChanged,   // the handshake failed against the pinned key: pair again
    Timeout,      // handshake too slow or 3 pongs missed
};

struct LinkConfig {
    LocalInfo local;
    uint16_t minVersion = kLinkVersion;
    uint16_t maxVersion = kLinkVersion;
};

class Link {
public:
    static constexpr uint64_t kPingMs = 2000;
    static constexpr int kMissedPongs = 3;
    static constexpr uint64_t kHandshakeMs = 10000;

    // Returns the pinned key for a paired install id, or false if it isn't paired.
    using KeyLookup = std::function<bool(const InstallId&, Key32&)>;

    // We dial a paired peer.
    Link(const LinkConfig& cfg, const InstallId& peerId, const Key32& peerKey, uint64_t now);
    // A peer dialled us.
    Link(const LinkConfig& cfg, KeyLookup lookup, uint64_t now);
    ~Link();

    void setTestEphemeral(const Key32& secret) { testEphemeral_ = secret; haveTestEphemeral_ = true; }

    // The initiator queues its first frame.
    void start();
    void receive(const uint8_t* p, size_t n, uint64_t now);
    void tick(uint64_t now);
    // Wire bytes to write, up to about max: whole frames only, at least one if any is ready.
    Bytes takeOutput(size_t max = SIZE_MAX);
    bool hasOutput() const;
    // Sends a link close; the owner closes the socket after flushing.
    void close(CloseReason reason = CloseReason::Normal);

    LinkState state() const { return state_; }
    LinkError error() const { return error_; }
    const InstallId& peerId() const { return peerId_; }
    const std::string& peerName() const { return peerName_; }
    uint16_t version() const { return version_; }
    // Last ping round trip, 0 before the first pong.
    uint64_t rttMs() const { return rtt_; }
    const Hash& handshakeHash() const;

    // Channels: usable once state() is Up.
    Mux& mux() { return mux_; }
    // The Bluetooth radio choice, once Up. What the peer sends lands in radio, oldest first.
    void sendRadio(const RadioBody& r);
    std::deque<RadioBody> radio;

private:
    void fail(LinkError e, bool refuse = false, RefuseReason why = RefuseReason::Protocol);
    void handleHandshake(const Bytes& body, uint64_t now);
    void handleRecord(const Bytes& body, uint64_t now);
    void handleLinkMessage(const Message& m, uint64_t now);
    void becameSecure(uint64_t now);
    void writeRecord(const Message& m, Bytes& out);
    Bytes prologue() const;

    bool initiator_;
    LinkConfig cfg_;
    KeyLookup lookup_;
    InstallId peerId_{};
    Key32 peerKey_{};
    std::unique_ptr<KKHandshake> hs_;
    CipherState send_, recv_;
    Key32 testEphemeral_{};
    bool haveTestEphemeral_ = false;
    FrameReader reader_{65535};
    Bytes out_;
    Mux mux_;
    LinkState state_ = LinkState::Handshake;
    LinkError error_ = LinkError::None;
    std::string peerName_;
    uint16_t version_ = 0;
    uint64_t started_;
    uint64_t lastPing_ = 0;
    uint64_t lastPong_ = 0;
    int unanswered_ = 0;
    uint64_t rtt_ = 0;
    bool sentHello_ = false;
    bool knownPeer_ = false;
};

}  // namespace frond::link
