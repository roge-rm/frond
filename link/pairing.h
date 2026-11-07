// Pairing by a 6-digit code with CPace (draft-irtf-cfrg-cpace-13, X25519 group with BLAKE2b in
// place of SHA-512), then a swap of static keys and names under the session key.
//
// A typed the code shown on B. Frames after the header:
//   A -> B  P1: 1, sid[16], idA[16], Ya[32]
//   B -> A  P2: 2, idB[16], Yb[32], tagB[32]
//   A -> B  P3: 3, tagA[32], AEAD(kAB, keyA[32] || name)
//   B -> A  P4: 4, AEAD(kBA, keyB[32] || name)
// Either side may send a Refuse frame instead and close.
#pragma once

#include "crypto.h"
#include "wire.h"

#include <string>

namespace frond::link {

// This install as the link sees it.
struct LocalInfo {
    Identity identity;
    InstallId id{};
    std::string name;
};

struct PeerInfo {
    InstallId id{};
    Key32 key{};
    std::string name;
};

// CPace pieces, exposed for the test vectors.
Bytes cpaceGeneratorString(const std::string& prs, const Bytes& ci, const uint8_t* sid, size_t sidLen);
// Elligator 2 map of the first 32 bytes of the hash. Monocypher ignores the top two bits where
// the draft clears only the top one.
Key32 cpaceGenerator(const std::string& prs, const Bytes& ci, const uint8_t* sid, size_t sidLen);
Key32 cpaceMapToCurve(const uint8_t hash32[32]);
// X25519 that refuses an all-zero result.
bool cpaceScalarMult(const Key32& scalar, const Key32& point, Key32& out);
Bytes cpaceIskInput(const uint8_t* sid, size_t sidLen, const Key32& k, const Key32& ya,
                    const Bytes& adA, const Key32& yb, const Bytes& adB);
Hash cpaceIsk(const uint8_t* sid, size_t sidLen, const Key32& k, const Key32& ya, const Bytes& adA,
              const Key32& yb, const Bytes& adB);

// The code B shows while its pair dialog is open: valid 2 minutes, 3 tries, then locked.
class PairCode {
public:
    static constexpr uint64_t kLifetimeMs = 120000;
    static constexpr int kTries = 3;

    explicit PairCode(uint64_t now);
    PairCode(const std::string& code, uint64_t now);
    const std::string& code() const { return code_; }

    enum class Check { Ok, Expired, LockedOut };
    // Each attempt takes a try when it starts, so an abandoned one counts too.
    Check begin(uint64_t now);
    void succeeded() { used_ = true; }
    int triesLeft() const { return kTries - tries_; }

private:
    std::string code_;
    uint64_t created_;
    int tries_ = 0;
    bool used_ = false;
};

enum class PairResult { Pending, Paired, WrongCode, Expired, LockedOut, Version, Protocol, Timeout };

class Pairing {
public:
    static constexpr uint64_t kTimeoutMs = 30000;

    // A: the code as typed, spaces allowed.
    Pairing(const LocalInfo& local, const std::string& code, uint64_t now);
    // B: one per incoming pairing connection, sharing the dialog's code.
    Pairing(const LocalInfo& local, PairCode& code, uint64_t now);
    ~Pairing();

    // Tests pin the CPace scalar and session id; call before start().
    void setTestScalar(const Key32& y) { y_ = y; }
    void setTestSid(const std::array<uint8_t, 16>& sid) { sid_ = sid; }

    // A queues P1. Nothing for B.
    void start();
    void receive(const uint8_t* p, size_t n, uint64_t now);
    void tick(uint64_t now);
    Bytes takeOutput();
    // The caller closes the socket once output is flushed and the result isn't Pending.
    PairResult result() const { return result_; }
    const PeerInfo& peer() const { return peer_; }

private:
    void fail(PairResult r, RefuseReason why, bool tellPeer);
    void handle(const Bytes& body, uint64_t now);
    void onP1(Reader& r, uint64_t now);
    void onP2(Reader& r);
    void onP3(Reader& r);
    void onP4(Reader& r);
    void deriveKeys();
    Bytes identityPlain() const;
    bool readIdentity(const Key32& key, Reader& r);
    void send(const Bytes& body);

    bool initiator_;
    LocalInfo local_;
    std::string code_;
    PairCode* pairCode_ = nullptr;
    uint64_t started_;
    int step_ = 0;
    PairResult result_ = PairResult::Pending;
    FrameReader reader_{1024};
    Bytes out_;
    std::array<uint8_t, 16> sid_{};
    Key32 y_{};
    Key32 g_{};
    Key32 ya_{}, yb_{};
    InstallId idA_{}, idB_{};
    Hash isk_{};
    Key32 kAB_{}, kBA_{};
    PeerInfo peer_;
};

}  // namespace frond::link
