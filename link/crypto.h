// Thin wrappers over Monocypher for the link: identity keys, BLAKE2b, HMAC and HKDF as Noise
// defines them, and ChaCha20-Poly1305 IETF with Noise's nonce layout.
#pragma once

#include "bytes.h"

namespace frond::link {

// From the kernel (getrandom, else /dev/urandom). Aborts if neither works.
void randomBytes(uint8_t* out, size_t n);

// An X25519 key pair. The secret is the install's long-term identity.
struct Identity {
    Key32 secret{};
    Key32 pub{};
};
Identity makeIdentity();
// False if the secret gives an unusable public key.
bool identityFromSecret(const Key32& secret, Identity& out);

// X25519; false when the result is all zero (a low-order peer key).
bool x25519(const Key32& secret, const Key32& peerPub, Key32& out);

constexpr size_t kHashLen = 64;  // BLAKE2b-512
using Hash = std::array<uint8_t, kHashLen>;

Hash blake2b(const uint8_t* a, size_t aLen, const uint8_t* b = nullptr, size_t bLen = 0);
// RFC 2104 HMAC over BLAKE2b with its 128-byte block, as Noise uses it.
Hash hmacBlake2b(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len);
// Noise HKDF with two outputs.
void noiseHkdf(const Hash& ck, const uint8_t* ikm, size_t ikmLen, Hash& out1, Hash& out2);
// BLAKE2b-256 keyed with up to 64 bytes, over a label then data.
Key32 keyedBlake2b256(const uint8_t* key, size_t keyLen, const std::string& label,
                      const uint8_t* data = nullptr, size_t len = 0);

constexpr size_t kTagLen = 16;
// ChaCha20-Poly1305 IETF, nonce = 4 zero bytes then the counter little-endian. Appends to out.
void aeadEncrypt(const Key32& key, uint64_t n, const uint8_t* ad, size_t adLen,
                 const uint8_t* pt, size_t ptLen, Bytes& out);
bool aeadDecrypt(const Key32& key, uint64_t n, const uint8_t* ad, size_t adLen,
                 const uint8_t* ct, size_t ctLen, Bytes& out);

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n);
void wipe(void* p, size_t n);

}  // namespace frond::link
