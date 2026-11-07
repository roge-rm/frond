// Noise_KK_25519_ChaChaPoly_BLAKE2b (Noise spec revision 34). Both sides know each other's
// static key from pairing:
//   -> s
//   <- s
//   -> e, es, ss
//   <- e, ee, se
#pragma once

#include "crypto.h"

namespace frond::link {

class CipherState {
public:
    void init(const uint8_t key[32]);
    bool hasKey() const { return hasKey_; }
    uint64_t nonce() const { return n_; }
    // Appends to out. Without a key the text passes through as Noise says.
    bool encrypt(const uint8_t* ad, size_t adLen, const uint8_t* pt, size_t len, Bytes& out);
    bool decrypt(const uint8_t* ad, size_t adLen, const uint8_t* ct, size_t len, Bytes& out);
    void clear();

private:
    Key32 k_{};
    uint64_t n_ = 0;
    bool hasKey_ = false;
};

class KKHandshake {
public:
    static constexpr const char* kProtocolName = "Noise_KK_25519_ChaChaPoly_BLAKE2b";
    static constexpr size_t kMaxMessage = 65535;

    KKHandshake(bool initiator, const Identity& local, const Key32& remotePub, const uint8_t* prologue,
                size_t prologueLen);
    ~KKHandshake();
    // Tests pin the ephemeral key; otherwise one is made on the first write.
    void setEphemeral(const Key32& secret);

    // Message 1 is written by the initiator, message 2 by the responder.
    bool writeMessage(const uint8_t* payload, size_t len, Bytes& out);
    bool readMessage(const uint8_t* msg, size_t len, Bytes& payload);
    bool done() const { return step_ == 2; }
    // After done(): the cipher for each direction from this side's view.
    void split(CipherState& send, CipherState& recv);
    const Hash& handshakeHash() const { return h_; }

private:
    void mixHash(const uint8_t* d, size_t n);
    bool mixKeyDh(const Key32& secret, const Key32& pub);

    bool initiator_;
    Identity s_;
    Identity e_;
    bool haveE_ = false;
    Key32 rs_{};
    Key32 re_{};
    Hash ck_{};
    Hash h_{};
    CipherState cs_;
    int step_ = 0;
    bool failed_ = false;
};

}  // namespace frond::link
