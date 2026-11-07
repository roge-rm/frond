#include "noise.h"

#include <cstring>
#include <limits>

namespace frond::link {

void CipherState::init(const uint8_t key[32]) {
    std::memcpy(k_.data(), key, 32);
    n_ = 0;
    hasKey_ = true;
}

void CipherState::clear() {
    wipe(k_.data(), k_.size());
    n_ = 0;
    hasKey_ = false;
}

bool CipherState::encrypt(const uint8_t* ad, size_t adLen, const uint8_t* pt, size_t len, Bytes& out) {
    if (!hasKey_) {
        putBytes(out, pt, len);
        return true;
    }
    // 2^64-1 is reserved; a link never gets near it.
    if (n_ == std::numeric_limits<uint64_t>::max()) return false;
    aeadEncrypt(k_, n_++, ad, adLen, pt, len, out);
    return true;
}

bool CipherState::decrypt(const uint8_t* ad, size_t adLen, const uint8_t* ct, size_t len, Bytes& out) {
    if (!hasKey_) {
        putBytes(out, ct, len);
        return true;
    }
    if (n_ == std::numeric_limits<uint64_t>::max()) return false;
    if (!aeadDecrypt(k_, n_, ad, adLen, ct, len, out)) return false;
    n_++;
    return true;
}

KKHandshake::KKHandshake(bool initiator, const Identity& local, const Key32& remotePub,
                         const uint8_t* prologue, size_t prologueLen)
    : initiator_(initiator), s_(local), rs_(remotePub) {
    size_t nameLen = std::strlen(kProtocolName);
    h_.fill(0);
    std::memcpy(h_.data(), kProtocolName, nameLen);  // 33 bytes, under HASHLEN so padded
    ck_ = h_;
    mixHash(prologue, prologueLen);
    // Pre-messages: initiator's static, then responder's.
    if (initiator_) {
        mixHash(s_.pub.data(), 32);
        mixHash(rs_.data(), 32);
    } else {
        mixHash(rs_.data(), 32);
        mixHash(s_.pub.data(), 32);
    }
}

KKHandshake::~KKHandshake() {
    wipe(&s_, sizeof s_);
    wipe(&e_, sizeof e_);
    wipe(ck_.data(), ck_.size());
    cs_.clear();
}

void KKHandshake::setEphemeral(const Key32& secret) {
    identityFromSecret(secret, e_);
    haveE_ = true;
}

void KKHandshake::mixHash(const uint8_t* d, size_t n) { h_ = blake2b(h_.data(), h_.size(), d, n); }

bool KKHandshake::mixKeyDh(const Key32& secret, const Key32& pub) {
    Key32 dh;
    if (!x25519(secret, pub, dh)) return false;
    Hash ck, k;
    noiseHkdf(ck_, dh.data(), dh.size(), ck, k);
    ck_ = ck;
    cs_.init(k.data());
    wipe(dh.data(), dh.size());
    wipe(k.data(), k.size());
    return true;
}

bool KKHandshake::writeMessage(const uint8_t* payload, size_t len, Bytes& out) {
    bool mine = (step_ == 0 && initiator_) || (step_ == 1 && !initiator_);
    if (failed_ || done() || !mine) return false;
    if (!haveE_) {
        e_ = makeIdentity();
        haveE_ = true;
    }
    size_t start = out.size();
    putBytes(out, e_.pub);
    mixHash(e_.pub.data(), 32);
    bool ok;
    if (step_ == 0) {
        ok = mixKeyDh(e_.secret, rs_) && mixKeyDh(s_.secret, rs_);  // es, ss
    } else {
        ok = mixKeyDh(e_.secret, re_) && mixKeyDh(e_.secret, rs_);  // ee, se
    }
    size_t ctAt = out.size();
    ok = ok && cs_.encrypt(h_.data(), h_.size(), payload, len, out);
    if (ok) mixHash(out.data() + ctAt, out.size() - ctAt);
    if (!ok || out.size() - start > kMaxMessage) {
        out.resize(start);
        failed_ = true;
        return false;
    }
    step_++;
    return true;
}

bool KKHandshake::readMessage(const uint8_t* msg, size_t len, Bytes& payload) {
    bool theirs = (step_ == 0 && !initiator_) || (step_ == 1 && initiator_);
    if (failed_ || done() || !theirs || len < 32 + kTagLen || len > kMaxMessage) {
        failed_ = true;
        return false;
    }
    std::memcpy(re_.data(), msg, 32);
    mixHash(re_.data(), 32);
    bool ok;
    if (step_ == 0) {
        ok = mixKeyDh(s_.secret, re_) && mixKeyDh(s_.secret, rs_);  // es, ss
    } else {
        ok = mixKeyDh(e_.secret, re_) && mixKeyDh(s_.secret, re_);  // ee, se
    }
    Bytes pt;
    ok = ok && cs_.decrypt(h_.data(), h_.size(), msg + 32, len - 32, pt);
    if (!ok) {
        failed_ = true;
        return false;
    }
    mixHash(msg + 32, len - 32);
    payload = std::move(pt);
    step_++;
    return true;
}

void KKHandshake::split(CipherState& send, CipherState& recv) {
    Hash k1, k2;
    noiseHkdf(ck_, nullptr, 0, k1, k2);
    if (initiator_) {
        send.init(k1.data());
        recv.init(k2.data());
    } else {
        send.init(k2.data());
        recv.init(k1.data());
    }
    wipe(k1.data(), k1.size());
    wipe(k2.data(), k2.size());
}

}  // namespace frond::link
