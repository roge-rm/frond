#include "ZodiacRom.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace frondZodiacRom {
namespace {
const char notTheUpdate[] = "This isn't the Zodiac 1.1 update.";

// SHA-256 (FIPS 180-4).
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block[64];
    size_t used = 0;
    uint64_t total = 0;
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void compress(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const uint8_t* p, size_t n) {
        total += n;
        while (n) {
            const size_t take = n < 64 - used ? n : 64 - used;
            std::memcpy(block + used, p, take);
            used += take; p += take; n -= take;
            if (used == 64) { compress(block); used = 0; }
        }
    }
    std::string finish() {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (used != 56) update(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; ++i) length[i] = uint8_t(bits >> (56 - 8 * i));
        update(length, 8);
        std::string out;
        for (uint32_t v : h) {
            char text[9];
            std::snprintf(text, sizeof text, "%08x", v);
            out += text;
        }
        return out;
    }
};

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

Result failure(const std::string& message) {
    Result r;
    r.status = Result::Status::error;
    r.error = message;
    return r;
}

// Inflates 'in' (windowBits as for inflateInit2) to exactly one complete stream of at most 'limit' bytes.
// The output grows as it inflates, so a claimed size costs nothing.
bool inflateAll(const uint8_t* in, size_t size, int windowBits, size_t limit, std::vector<uint8_t>& out) {
    z_stream z{};
    if (inflateInit2(&z, windowBits) != Z_OK) return false;
    out.clear();
    z.next_in = const_cast<Bytef*>(in);
    z.avail_in = uInt(size);
    int code = Z_OK;
    while (code == Z_OK) {
        const size_t used = out.size();
        if (used > limit) break;
        const size_t step = std::min<size_t>(1u << 20, limit + 1 - used);
        out.resize(used + step);
        z.next_out = out.data() + used;
        z.avail_out = uInt(step);
        code = inflate(&z, Z_NO_FLUSH);
        out.resize(used + (step - z.avail_out));
    }
    const bool whole = code == Z_STREAM_END && z.avail_in == 0;
    inflateEnd(&z);
    return whole && out.size() <= limit;
}

bool endsWithPrc(const uint8_t* name, size_t n) {
    return n >= 4 && name[n - 4] == '.' && (name[n - 3] | 0x20) == 'p' && (name[n - 2] | 0x20) == 'r' && (name[n - 1] | 0x20) == 'c';
}

// A resource database: the resource bit in the attributes and no next record list.
bool isPrc(const uint8_t* d, size_t size) {
    return size >= 0x4e && (be16(d + 0x20) & 1) && be32(d + 0x48) == 0;
}

const size_t zipLimit = 64u << 20;
// The real PRC is about 4.98 MB.
const size_t prcLimit = 16u << 20;

// The first .prc in the zip, stored or deflated. Empty and 'error' filled on a damaged or unusable zip.
bool zipEntry(const uint8_t* d, size_t size, std::vector<uint8_t>& prc, std::string& error) {
    error = notTheUpdate;
    size_t eocd = size;
    for (size_t at = size >= 22 ? size - 22 : size, stop = size > 22 + 65535 ? size - 22 - 65535 : 0; at != size; --at) {
        if (le32(d + at) == 0x06054b50) { eocd = at; break; }
        if (at == stop) break;
    }
    if (eocd == size) { error = "This zip is damaged or cut short."; return false; }
    const size_t count = le16(d + eocd + 10);
    size_t at = le32(d + eocd + 16);
    for (size_t i = 0; i < count; ++i) {
        if (at > size || size - at < 46 || le32(d + at) != 0x02014b50) { error = "This zip is damaged or cut short."; return false; }
        const uint16_t method = le16(d + at + 10);
        const uint32_t packed = le32(d + at + 20), unpacked = le32(d + at + 24);
        const size_t nameLength = le16(d + at + 28), extraLength = le16(d + at + 30), commentLength = le16(d + at + 32);
        const size_t local = le32(d + at + 42);
        if (size - at - 46 < nameLength + extraLength + commentLength) { error = "This zip is damaged or cut short."; return false; }
        const bool match = endsWithPrc(d + at + 46, nameLength);
        at += 46 + nameLength + extraLength + commentLength;
        if (!match) continue;
        if (local > size || size - local < 30 || le32(d + local) != 0x04034b50) { error = "This zip is damaged or cut short."; return false; }
        const size_t start = local + 30 + le16(d + local + 26) + le16(d + local + 28);
        if (start > size || size - start < packed || unpacked > prcLimit) { error = "This zip is damaged or cut short."; return false; }
        if (method == 0 && packed == unpacked) {
            prc.assign(d + start, d + start + packed);
            return true;
        }
        if (method == 8 && inflateAll(d + start, packed, -15, unpacked, prc) && prc.size() == unpacked) return true;
        error = "The update inside this zip is damaged.";
        return false;
    }
    return false;
}
}  // namespace

Expected zodiac() {
    return {8192000, "2207a735e812299b7d1ede9da01c5e867bd932a1f15ce46b81c8737e42e9111d"};
}

std::string sha256Hex(const uint8_t* data, size_t size) {
    Sha256 s;
    s.update(data, size);
    return s.finish();
}

static Result fromPrc(const uint8_t* d, size_t size, const Expected& expected) {
    // Palm database header: 0x4c has the record count, then 10 bytes a resource: type, id, offset.
    const size_t count = be16(d + 0x4c);
    if (size < 0x4e + count * 10) return failure("This update is damaged or cut short.");
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* entry = d + 0x4e + i * 10;
        if (std::memcmp(entry, "ROMZ", 4) != 0 || be16(entry + 4) != 2) continue;
        const size_t start = be32(entry + 6);
        // A resource runs to the next one in the list by offset, or to the end of the file.
        size_t end = size;
        for (size_t j = 0; j < count; ++j) {
            const size_t other = be32(d + 0x4e + j * 10 + 6);
            if (other > start && other < end) end = other;
        }
        if (start >= size || end <= start) return failure("This update is damaged or cut short.");
        Result r;
        if (!inflateAll(d + start, end - start, 31, expected.length, r.rom))
            return failure("This update is damaged or cut short.");
        if (r.rom.size() != expected.length || sha256Hex(r.rom.data(), r.rom.size()) != expected.sha256)
            return failure(notTheUpdate);
        r.status = Result::Status::ok;
        return r;
    }
    return failure(notTheUpdate);
}

Result extract(const uint8_t* data, size_t size, const Expected& expected) {
    Result r;
    if (size == expected.length && sha256Hex(data, size) == expected.sha256) {
        r.status = Result::Status::ok;
        r.rom.assign(data, data + size);
        return r;
    }
    if (size >= 4 && le32(data) == 0x04034b50) {
        std::vector<uint8_t> prc;
        std::string error;
        if (!zipEntry(data, size, prc, error)) return failure(error);
        if (!isPrc(prc.data(), prc.size())) return failure(notTheUpdate);
        return fromPrc(prc.data(), prc.size(), expected);
    }
    if (isPrc(data, size)) return fromPrc(data, size, expected);
    return r;
}

bool looksLikeUpdateFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t head[0x4e];
    const size_t n = std::fread(head, 1, sizeof head, f);
    std::fclose(f);
    return (n >= 4 && le32(head) == 0x04034b50) || isPrc(head, n);
}

Result extractFile(const std::string& path, const Expected& expected) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return failure("Couldn't read the file.");
    std::vector<uint8_t> bytes;
    uint8_t buffer[1 << 16];
    for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;) {
        bytes.insert(bytes.end(), buffer, buffer + n);
        if (bytes.size() > zipLimit) { std::fclose(f); return Result{}; }
    }
    const bool bad = std::ferror(f);
    std::fclose(f);
    if (bad) return failure("Couldn't read the file.");
    return extract(bytes.data(), bytes.size(), expected);
}
}  // namespace frondZodiacRom
