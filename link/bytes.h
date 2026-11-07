// Byte buffers and big-endian helpers shared by the link library.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace frond::link {

using Bytes = std::vector<uint8_t>;
using Key32 = std::array<uint8_t, 32>;
using InstallId = std::array<uint8_t, 16>;

inline void putU8(Bytes& b, uint8_t v) { b.push_back(v); }
inline void putU16(Bytes& b, uint16_t v) {
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}
inline void putU32(Bytes& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}
inline void putU64(Bytes& b, uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}
inline void putBytes(Bytes& b, const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
template <size_t N>
inline void putBytes(Bytes& b, const std::array<uint8_t, N>& a) { b.insert(b.end(), a.begin(), a.end()); }

inline uint16_t getU16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t getU32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint64_t getU64(const uint8_t* p) { return (uint64_t(getU32(p)) << 32) | getU32(p + 4); }

// Reads fields from a received buffer; any short read sets ok to false and returns zeros.
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ok() const { return ok_; }
    size_t left() const { return n_ - pos_; }
    bool atEnd() const { return pos_ == n_; }
    const uint8_t* take(size_t n) {
        if (!ok_ || n > left()) {
            ok_ = false;
            return nullptr;
        }
        const uint8_t* r = p_ + pos_;
        pos_ += n;
        return r;
    }
    uint8_t u8() { auto* q = take(1); return q ? q[0] : 0; }
    uint16_t u16() { auto* q = take(2); return q ? getU16(q) : 0; }
    uint32_t u32() { auto* q = take(4); return q ? getU32(q) : 0; }
    uint64_t u64() { auto* q = take(8); return q ? getU64(q) : 0; }
    template <size_t N>
    std::array<uint8_t, N> array() {
        std::array<uint8_t, N> a{};
        if (auto* q = take(N)) std::copy(q, q + N, a.begin());
        return a;
    }
    // A u8 length then that many bytes of UTF-8.
    std::string str8() {
        uint8_t n = u8();
        auto* q = take(n);
        return q ? std::string(reinterpret_cast<const char*>(q), n) : std::string();
    }
    Bytes rest() {
        Bytes r(p_ + pos_, p_ + n_);
        pos_ = n_;
        return r;
    }

private:
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// Writes a u8 length then the string, cut to 255 bytes.
inline void putStr8(Bytes& b, const std::string& s) {
    size_t n = s.size() > 255 ? 255 : s.size();
    b.push_back(uint8_t(n));
    b.insert(b.end(), s.begin(), s.begin() + n);
}

}  // namespace frond::link
