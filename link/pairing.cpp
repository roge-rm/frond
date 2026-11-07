#include "pairing.h"

extern "C" {
#include "monocypher/monocypher.h"
}

#include <cstdio>
#include <cstring>

namespace frond::link {

namespace {

const char kDsi[] = "CPace255";
const char kDsiIsk[] = "CPace255_ISK";
const char kChannelId[] = "FrondLinkPair1";
constexpr size_t kHashInBlock = 128;  // BLAKE2b block, the same as SHA-512's

void putLeb128(Bytes& b, size_t n) {
    do {
        uint8_t byte = n & 0x7f;
        n >>= 7;
        if (n) byte |= 0x80;
        b.push_back(byte);
    } while (n);
}

size_t leb128Len(size_t n) {
    size_t len = 1;
    while (n >>= 7) len++;
    return len;
}

void putLv(Bytes& b, const uint8_t* p, size_t n) {
    putLeb128(b, n);
    putBytes(b, p, n);
}

void putLv(Bytes& b, const std::string& s) {
    putLv(b, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

Bytes stringBytes(const char* s) { return Bytes(s, s + std::strlen(s)); }

std::string normaliseCode(const std::string& typed) {
    std::string digits;
    for (char c : typed) {
        if (c == ' ') continue;
        if (c < '0' || c > '9') return std::string();
        digits.push_back(c);
    }
    return digits.size() == 6 ? digits : std::string();
}

}  // namespace

Bytes cpaceGeneratorString(const std::string& prs, const Bytes& ci, const uint8_t* sid, size_t sidLen) {
    size_t dsiLen = std::strlen(kDsi);
    size_t used = 1 + leb128Len(prs.size()) + prs.size() + leb128Len(dsiLen) + dsiLen;
    size_t zpad = used < kHashInBlock ? kHashInBlock - used : 0;
    Bytes s;
    putLv(s, std::string(kDsi));
    putLv(s, prs);
    Bytes zeros(zpad, 0);
    putLv(s, zeros.data(), zeros.size());
    putLv(s, ci.data(), ci.size());
    putLv(s, sid, sidLen);
    return s;
}

Key32 cpaceMapToCurve(const uint8_t hash32[32]) {
    Key32 g;
    crypto_elligator_map(g.data(), hash32);
    return g;
}

Key32 cpaceGenerator(const std::string& prs, const Bytes& ci, const uint8_t* sid, size_t sidLen) {
    Bytes s = cpaceGeneratorString(prs, ci, sid, sidLen);
    Hash h = blake2b(s.data(), s.size());
    Key32 g = cpaceMapToCurve(h.data());
    wipe(s.data(), s.size());
    wipe(h.data(), h.size());
    return g;
}

bool cpaceScalarMult(const Key32& scalar, const Key32& point, Key32& out) {
    return x25519(scalar, point, out);
}

Bytes cpaceIskInput(const uint8_t* sid, size_t sidLen, const Key32& k, const Key32& ya,
                    const Bytes& adA, const Key32& yb, const Bytes& adB) {
    Bytes in;
    putLv(in, std::string(kDsiIsk));
    putLv(in, sid, sidLen);
    putLv(in, k.data(), k.size());
    // transcript_ir: initiator then responder
    putLv(in, ya.data(), ya.size());
    putLv(in, adA.data(), adA.size());
    putLv(in, yb.data(), yb.size());
    putLv(in, adB.data(), adB.size());
    return in;
}

Hash cpaceIsk(const uint8_t* sid, size_t sidLen, const Key32& k, const Key32& ya, const Bytes& adA,
              const Key32& yb, const Bytes& adB) {
    Bytes in = cpaceIskInput(sid, sidLen, k, ya, adA, yb, adB);
    Hash isk = blake2b(in.data(), in.size());
    wipe(in.data(), in.size());
    return isk;
}

PairCode::PairCode(uint64_t now) : created_(now) {
    // Uniform over 000000-999999: drop the top of the u32 range.
    uint32_t v;
    do {
        uint8_t b[4];
        randomBytes(b, 4);
        v = getU32(b);
    } while (v >= 4294000000u);
    v %= 1000000;
    char buf[8];
    std::snprintf(buf, sizeof buf, "%06u", unsigned(v));
    code_ = buf;
}

PairCode::PairCode(const std::string& code, uint64_t now) : code_(code), created_(now) {}

PairCode::Check PairCode::begin(uint64_t now) {
    if (used_ || now - created_ >= kLifetimeMs) return Check::Expired;
    if (tries_ >= kTries) return Check::LockedOut;
    tries_++;
    return Check::Ok;
}

Pairing::Pairing(const LocalInfo& local, const std::string& code, uint64_t now)
    : initiator_(true), local_(local), code_(normaliseCode(code)), started_(now) {
    randomBytes(sid_.data(), sid_.size());
    randomBytes(y_.data(), y_.size());
}

Pairing::Pairing(const LocalInfo& local, PairCode& code, uint64_t now)
    : initiator_(false), local_(local), code_(code.code()), pairCode_(&code), started_(now) {
    randomBytes(y_.data(), y_.size());
}

Pairing::~Pairing() {
    wipe(&local_.identity, sizeof local_.identity);
    wipe(&code_[0], code_.size());
    wipe(y_.data(), y_.size());
    wipe(isk_.data(), isk_.size());
    wipe(kAB_.data(), kAB_.size());
    wipe(kBA_.data(), kBA_.size());
}

void Pairing::send(const Bytes& body) { writeFrame(out_, body); }

void Pairing::fail(PairResult r, RefuseReason why, bool tellPeer) {
    if (result_ != PairResult::Pending) return;
    result_ = r;
    if (tellPeer) {
        Bytes f = refuseFrame(why);
        putBytes(out_, f.data(), f.size());
    }
}

void Pairing::start() {
    if (!initiator_ || step_ != 0 || result_ != PairResult::Pending) return;
    if (code_.empty()) {
        result_ = PairResult::WrongCode;  // not 6 digits, nothing sent
        return;
    }
    idA_ = local_.id;
    g_ = cpaceGenerator(code_, stringBytes(kChannelId), sid_.data(), sid_.size());
    if (!cpaceScalarMult(y_, g_, ya_)) {
        result_ = PairResult::Protocol;
        return;
    }
    Bytes b;
    putHeader(b, FrameMode::Pair);
    b.push_back(1);
    putBytes(b, sid_);
    putBytes(b, idA_);
    putBytes(b, ya_);
    send(b);
    step_ = 1;
}

void Pairing::receive(const uint8_t* p, size_t n, uint64_t now) {
    if (result_ != PairResult::Pending) return;
    reader_.feed(p, n);
    Bytes body;
    while (result_ == PairResult::Pending && reader_.next(body)) handle(body, now);
    if (reader_.error()) fail(PairResult::Protocol, RefuseReason::Protocol, true);
}

void Pairing::tick(uint64_t now) {
    if (result_ == PairResult::Pending && now - started_ >= kTimeoutMs)
        fail(PairResult::Timeout, RefuseReason::Protocol, false);
}

Bytes Pairing::takeOutput() {
    Bytes o;
    o.swap(out_);
    return o;
}

void Pairing::handle(const Bytes& body, uint64_t now) {
    uint8_t version;
    FrameMode mode;
    if (!parseHeader(body, version, mode)) return fail(PairResult::Protocol, RefuseReason::Protocol, true);
    if (version != kWireVersion) return fail(PairResult::Version, RefuseReason::Version, true);
    Reader r(body.data() + kHeaderLen, body.size() - kHeaderLen);
    if (mode == FrameMode::Refuse) {
        switch (RefuseReason(r.u8())) {
            case RefuseReason::WrongCode: return fail(PairResult::WrongCode, {}, false);
            case RefuseReason::Expired: return fail(PairResult::Expired, {}, false);
            case RefuseReason::LockedOut: return fail(PairResult::LockedOut, {}, false);
            case RefuseReason::Version: return fail(PairResult::Version, {}, false);
            default: return fail(PairResult::Protocol, {}, false);
        }
    }
    if (mode != FrameMode::Pair) return fail(PairResult::Protocol, RefuseReason::NotPairing, true);
    uint8_t step = r.u8();
    if (!initiator_ && step == 1 && step_ == 0) return onP1(r, now);
    if (initiator_ && step == 2 && step_ == 1) return onP2(r);
    if (!initiator_ && step == 3 && step_ == 2) return onP3(r);
    if (initiator_ && step == 4 && step_ == 3) return onP4(r);
    fail(PairResult::Protocol, RefuseReason::Protocol, true);
}

void Pairing::deriveKeys() {
    Bytes adA(idA_.begin(), idA_.end()), adB(idB_.begin(), idB_.end());
    Key32 k;
    // Either side's view of K: ya*Yb on A, yb*Ya on B.
    bool ok = cpaceScalarMult(y_, initiator_ ? yb_ : ya_, k);
    if (!ok) {
        k.fill(0);
        isk_.fill(0);
        return;
    }
    isk_ = cpaceIsk(sid_.data(), sid_.size(), k, ya_, adA, yb_, adB);
    kAB_ = keyedBlake2b256(isk_.data(), isk_.size(), "FrondPair key A to B");
    kBA_ = keyedBlake2b256(isk_.data(), isk_.size(), "FrondPair key B to A");
    wipe(k.data(), k.size());
}

Bytes Pairing::identityPlain() const {
    Bytes p;
    putBytes(p, local_.identity.pub);
    putStr8(p, local_.name);
    return p;
}

bool Pairing::readIdentity(const Key32& key, Reader& r) {
    Bytes ct = r.rest();
    Bytes pt;
    if (!aeadDecrypt(key, 0, nullptr, 0, ct.data(), ct.size(), pt)) return false;
    Reader ir(pt.data(), pt.size());
    peer_.key = ir.array<32>();
    peer_.name = ir.str8();
    peer_.id = initiator_ ? idB_ : idA_;
    wipe(pt.data(), pt.size());
    return ir.ok() && ir.atEnd();
}

void Pairing::onP1(Reader& r, uint64_t now) {
    sid_ = r.array<16>();
    idA_ = r.array<16>();
    ya_ = r.array<32>();
    if (!r.ok() || !r.atEnd()) return fail(PairResult::Protocol, RefuseReason::Protocol, true);
    switch (pairCode_->begin(now)) {
        case PairCode::Check::Expired: return fail(PairResult::Expired, RefuseReason::Expired, true);
        case PairCode::Check::LockedOut: return fail(PairResult::LockedOut, RefuseReason::LockedOut, true);
        case PairCode::Check::Ok: break;
    }
    idB_ = local_.id;
    g_ = cpaceGenerator(code_, stringBytes(kChannelId), sid_.data(), sid_.size());
    Key32 k;
    if (!cpaceScalarMult(y_, g_, yb_) || !cpaceScalarMult(y_, ya_, k))
        return fail(PairResult::Protocol, RefuseReason::Protocol, true);
    wipe(k.data(), k.size());
    deriveKeys();
    Key32 tagB = keyedBlake2b256(isk_.data(), isk_.size(), "FrondPair confirm B");
    Bytes b;
    putHeader(b, FrameMode::Pair);
    b.push_back(2);
    putBytes(b, idB_);
    putBytes(b, yb_);
    putBytes(b, tagB);
    send(b);
    step_ = 2;
}

void Pairing::onP2(Reader& r) {
    idB_ = r.array<16>();
    yb_ = r.array<32>();
    Key32 tagB = r.array<32>();
    if (!r.ok() || !r.atEnd()) return fail(PairResult::Protocol, RefuseReason::Protocol, true);
    Key32 k;
    if (!cpaceScalarMult(y_, yb_, k)) return fail(PairResult::Protocol, RefuseReason::Protocol, true);
    wipe(k.data(), k.size());
    deriveKeys();
    Key32 want = keyedBlake2b256(isk_.data(), isk_.size(), "FrondPair confirm B");
    if (!constantTimeEqual(want.data(), tagB.data(), 32))
        return fail(PairResult::WrongCode, RefuseReason::WrongCode, true);
    Key32 tagA = keyedBlake2b256(isk_.data(), isk_.size(), "FrondPair confirm A");
    Bytes b;
    putHeader(b, FrameMode::Pair);
    b.push_back(3);
    putBytes(b, tagA);
    Bytes plain = identityPlain();
    aeadEncrypt(kAB_, 0, nullptr, 0, plain.data(), plain.size(), b);
    send(b);
    step_ = 3;
}

void Pairing::onP3(Reader& r) {
    Key32 tagA = r.array<32>();
    if (!r.ok()) return fail(PairResult::Protocol, RefuseReason::Protocol, true);
    Key32 want = keyedBlake2b256(isk_.data(), isk_.size(), "FrondPair confirm A");
    if (!constantTimeEqual(want.data(), tagA.data(), 32))
        return fail(PairResult::WrongCode, RefuseReason::WrongCode, true);
    if (!readIdentity(kAB_, r)) return fail(PairResult::Protocol, RefuseReason::Protocol, true);
    pairCode_->succeeded();
    Bytes b;
    putHeader(b, FrameMode::Pair);
    b.push_back(4);
    Bytes plain = identityPlain();
    aeadEncrypt(kBA_, 0, nullptr, 0, plain.data(), plain.size(), b);
    send(b);
    step_ = 4;
    result_ = PairResult::Paired;
}

void Pairing::onP4(Reader& r) {
    if (!readIdentity(kBA_, r)) return fail(PairResult::Protocol, RefuseReason::Protocol, true);
    step_ = 4;
    result_ = PairResult::Paired;
}

}  // namespace frond::link
