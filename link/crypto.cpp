#include "crypto.h"

extern "C" {
#include "monocypher/monocypher.h"
}

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace frond::link {

void randomBytes(uint8_t* out, size_t n) {
    size_t got = 0;
#if defined(SYS_getrandom)
    // The syscall directly: Android's libc wrapper only exists from API 28.
    while (got < n) {
        long r = syscall(SYS_getrandom, out + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        got += size_t(r);
    }
#endif
    if (got < n) {
        FILE* f = std::fopen("/dev/urandom", "rb");
        if (f) {
            got += std::fread(out + got, 1, n - got, f);
            std::fclose(f);
        }
    }
    if (got < n) std::abort();
}

Identity makeIdentity() {
    Identity id;
    randomBytes(id.secret.data(), id.secret.size());
    identityFromSecret(id.secret, id);
    return id;
}

bool identityFromSecret(const Key32& secret, Identity& out) {
    out.secret = secret;
    crypto_x25519_public_key(out.pub.data(), secret.data());
    return true;
}

static bool allZero(const uint8_t* p, size_t n) {
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) acc |= p[i];
    return acc == 0;
}

bool x25519(const Key32& secret, const Key32& peerPub, Key32& out) {
    crypto_x25519(out.data(), secret.data(), peerPub.data());
    return !allZero(out.data(), out.size());
}

Hash blake2b(const uint8_t* a, size_t aLen, const uint8_t* b, size_t bLen) {
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, kHashLen);
    crypto_blake2b_update(&ctx, a, aLen);
    if (bLen) crypto_blake2b_update(&ctx, b, bLen);
    Hash h;
    crypto_blake2b_final(&ctx, h.data());
    return h;
}

Hash hmacBlake2b(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len) {
    constexpr size_t kBlock = 128;
    uint8_t k[kBlock] = {};
    if (keyLen > kBlock) {
        Hash kh = blake2b(key, keyLen);
        std::memcpy(k, kh.data(), kh.size());
    } else {
        std::memcpy(k, key, keyLen);
    }
    uint8_t pad[kBlock];
    for (size_t i = 0; i < kBlock; i++) pad[i] = k[i] ^ 0x36;
    Hash inner = blake2b(pad, kBlock, data, len);
    for (size_t i = 0; i < kBlock; i++) pad[i] = k[i] ^ 0x5c;
    Hash outer = blake2b(pad, kBlock, inner.data(), inner.size());
    wipe(k, sizeof k);
    wipe(pad, sizeof pad);
    wipe(inner.data(), inner.size());
    return outer;
}

void noiseHkdf(const Hash& ck, const uint8_t* ikm, size_t ikmLen, Hash& out1, Hash& out2) {
    Hash temp = hmacBlake2b(ck.data(), ck.size(), ikm, ikmLen);
    uint8_t one = 1;
    out1 = hmacBlake2b(temp.data(), temp.size(), &one, 1);
    uint8_t buf[kHashLen + 1];
    std::memcpy(buf, out1.data(), kHashLen);
    buf[kHashLen] = 2;
    out2 = hmacBlake2b(temp.data(), temp.size(), buf, sizeof buf);
    wipe(temp.data(), temp.size());
    wipe(buf, sizeof buf);
}

Key32 keyedBlake2b256(const uint8_t* key, size_t keyLen, const std::string& label,
                      const uint8_t* data, size_t len) {
    crypto_blake2b_ctx ctx;
    crypto_blake2b_keyed_init(&ctx, 32, key, keyLen);
    crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(label.data()), label.size());
    if (len) crypto_blake2b_update(&ctx, data, len);
    Key32 out;
    crypto_blake2b_final(&ctx, out.data());
    return out;
}

static void noiseNonce(uint64_t n, uint8_t nonce[12]) {
    std::memset(nonce, 0, 4);
    for (int i = 0; i < 8; i++) nonce[4 + i] = uint8_t(n >> (8 * i));
}

void aeadEncrypt(const Key32& key, uint64_t n, const uint8_t* ad, size_t adLen,
                 const uint8_t* pt, size_t ptLen, Bytes& out) {
    uint8_t nonce[12];
    noiseNonce(n, nonce);
    size_t at = out.size();
    out.resize(at + ptLen + kTagLen);
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key.data(), nonce);
    crypto_aead_write(&ctx, out.data() + at, out.data() + at + ptLen, ad, adLen, pt, ptLen);
    crypto_wipe(&ctx, sizeof ctx);
}

bool aeadDecrypt(const Key32& key, uint64_t n, const uint8_t* ad, size_t adLen,
                 const uint8_t* ct, size_t ctLen, Bytes& out) {
    if (ctLen < kTagLen) return false;
    uint8_t nonce[12];
    noiseNonce(n, nonce);
    size_t textLen = ctLen - kTagLen;
    size_t at = out.size();
    out.resize(at + textLen);
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key.data(), nonce);
    int r = crypto_aead_read(&ctx, out.data() + at, ct + textLen, ad, adLen, ct, textLen);
    crypto_wipe(&ctx, sizeof ctx);
    if (r != 0) {
        out.resize(at);
        return false;
    }
    return true;
}

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) acc |= uint8_t(a[i] ^ b[i]);
    return acc == 0;
}

void wipe(void* p, size_t n) { crypto_wipe(p, n); }

}  // namespace frond::link
