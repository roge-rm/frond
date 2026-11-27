#include "storage/TarStream.h"

#include "storage/Errors.h"
#include "zip/miniz.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

using frondStorage::Error;

namespace frondTar {

size_t FdReader::read(uint8_t* data, size_t n) {
    size_t done = 0;
    while (done < n) {
        const auto r = ::read(fd_, data + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) throw Error("Could not read the archive.");
        if (r == 0) break;
        done += size_t(r);
        if (onRead_) onRead_(size_t(r));
    }
    return done;
}

size_t MemoryReader::read(uint8_t* data, size_t n) {
    const size_t count = std::min(n, size_ - at_);
    if (count) memcpy(data, data_ + at_, count);
    at_ += count;
    return count;
}

bool PeekReader::gzipped() {
    if (!peeked_) {
        have_ = source_.read(head_, 2);
        peeked_ = true;
    }
    return have_ == 2 && head_[0] == 0x1f && head_[1] == 0x8b;
}

size_t PeekReader::read(uint8_t* data, size_t n) {
    if (!peeked_) gzipped();
    size_t done = 0;
    while (done < n && at_ < have_) data[done++] = head_[at_++];
    if (done < n) done += source_.read(data + done, n - done);
    return done;
}

namespace {
constexpr size_t gzipChunk = 64 * 1024;
constexpr const char* gzipCut = "The archive's gzip data is cut off.";
}  // namespace

struct GzipReader::State {
    Reader& source;
    mz_stream z{};
    bool open{false}, member{false}, done{false};
    uint32_t crc{0};
    uint32_t length{0};
    uint8_t in[gzipChunk + 16];

    explicit State(Reader& s) : source(s) {}
    ~State() {
        if (open) mz_inflateEnd(&z);
    }

    bool fill() {
        if (z.avail_in) return true;
        z.next_in = in;
        z.avail_in = unsigned(source.read(in, sizeof in));
        return z.avail_in != 0;
    }
    bool byte(uint8_t& b) {
        if (!fill()) return false;
        b = *z.next_in++;
        --z.avail_in;
        return true;
    }
    // Puts back bytes inflate read past the end of a member.
    void unread(const uint8_t* bytes, size_t n) {
        const size_t consumed = size_t(z.next_in - in);
        if (consumed >= n) {
            z.next_in -= n;
        } else {
            memmove(in + n, z.next_in, z.avail_in);
            z.next_in = in;
        }
        memcpy(const_cast<uint8_t*>(z.next_in), bytes, n);
        z.avail_in += unsigned(n);
    }
    uint8_t need() {
        uint8_t b;
        if (!byte(b)) throw Error(gzipCut);
        return b;
    }
    uint32_t le32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(need()) << (8 * i);
        return v;
    }

    // Starts the next member; false at the end of the stream.
    bool header(bool first) {
        uint8_t b;
        if (!byte(b)) return false;
        if (!first) {
            // Zero padding after a member, as gzip.py skips it.
            while (b == 0)
                if (!byte(b)) return false;
        }
        if (b != 0x1f || need() != 0x8b) throw Error("The archive is not gzip data.");
        if (need() != 8) throw Error("The archive uses an unknown gzip compression method.");
        const uint8_t flags = need();
        for (int i = 0; i < 6; ++i) need();
        if (flags & 4) {
            size_t extra = need();
            extra |= size_t(need()) << 8;
            while (extra--) need();
        }
        if (flags & 8)
            while (need()) {
            }
        if (flags & 16)
            while (need()) {
            }
        if (flags & 2) {
            need();
            need();
        }
        if (open) mz_inflateEnd(&z);
        const auto* next = z.next_in;
        const auto avail = z.avail_in;
        z = mz_stream{};
        z.next_in = next;
        z.avail_in = avail;
        if (mz_inflateInit2(&z, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) throw Error("Could not start gzip decoding.");
        open = true;
        crc = uint32_t(mz_crc32(0, nullptr, 0));
        length = 0;
        member = true;
        return true;
    }
};

GzipReader::GzipReader(Reader& source) : state_(std::make_unique<State>(source)) {}
GzipReader::~GzipReader() = default;

size_t GzipReader::read(uint8_t* data, size_t n) {
    auto& s = *state_;
    size_t done = 0;
    bool first = !s.open;
    while (done < n && !s.done) {
        if (!s.member) {
            if (!s.header(first)) {
                s.done = true;
                break;
            }
            first = false;
        }
        if (!s.z.avail_in) s.fill();
        s.z.next_out = data + done;
        s.z.avail_out = unsigned(std::min(n - done, size_t(1) << 30));
        const auto before = s.z.avail_out;
        const int rc = mz_inflate(&s.z, MZ_NO_FLUSH);
        const size_t produced = before - s.z.avail_out;
        s.crc = uint32_t(mz_crc32(s.crc, data + done, produced));
        s.length += uint32_t(produced);
        done += produced;
        if (rc == MZ_STREAM_END) {
            // This miniz keeps up to 8 bytes past the stream in its bit buffer; they're the trailer.
            const auto* d = reinterpret_cast<const tinfl_decompressor*>(s.z.state);
            uint32_t bits = d->m_num_bits;
            uint64_t buf = uint64_t(d->m_bit_buf) >> (bits & 7);
            bits -= bits & 7;
            uint8_t back[8];
            const size_t count = std::min<size_t>(bits / 8, sizeof back);
            for (size_t i = 0; i < count; ++i) back[i] = uint8_t(buf >> (8 * i));
            if (count) s.unread(back, count);
            if (s.le32() != s.crc) throw Error("The archive's gzip CRC check failed.");
            if (s.le32() != s.length) throw Error("The archive's gzip length check failed.");
            s.member = false;
        } else if (rc == MZ_BUF_ERROR || (rc == MZ_OK && !produced && !s.z.avail_in)) {
            if (!s.fill()) throw Error(gzipCut);
        } else if (rc != MZ_OK) {
            throw Error("The archive's gzip data is damaged.");
        }
    }
    return done;
}

namespace {

constexpr size_t block = 512;
constexpr uint64_t maxHeaderRead = 256 * 1024;

uint64_t padded(uint64_t size) { return (size + block - 1) / block * block; }

std::string nts(const uint8_t* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len]) ++len;
    return std::string(reinterpret_cast<const char*>(p), len);
}

// A number field as tarfile's nti reads it: base-256 after 0x80 or 0xff, else octal text.
int64_t nti(const uint8_t* p, size_t n) {
    if (p[0] == 0x80 || p[0] == 0xff) {
        uint64_t v = 0;
        for (size_t i = 1; i < n; ++i) {
            if (v >> 55) throw Error("The archive has a number too large to use.");
            v = (v << 8) | p[i];
        }
        if (p[0] == 0xff) {
            // -(256^(n-1) - v); only small negatives fit.
            if (n - 1 > 7) {
                const uint64_t magnitude = ~v + 1;
                if (magnitude >> 62) throw Error("The archive has a number too large to use.");
                return -int64_t(magnitude);
            }
            return int64_t(v) - (int64_t(1) << (8 * (n - 1)));
        }
        if (v >> 62) throw Error("The archive has a number too large to use.");
        return int64_t(v);
    }
    std::string s = nts(p, n);
    auto space = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
    while (!s.empty() && space(s.back())) s.pop_back();
    size_t at = 0;
    while (at < s.size() && space(s[at])) ++at;
    s = s.substr(at);
    if (s.empty()) return 0;
    bool negative = false;
    at = 0;
    if (s[0] == '+' || s[0] == '-') {
        negative = s[0] == '-';
        at = 1;
    }
    if (at == s.size()) throw Error("The archive has a damaged tar header.");
    uint64_t v = 0;
    for (; at < s.size(); ++at) {
        if (s[at] < '0' || s[at] > '7') throw Error("The archive has a damaged tar header.");
        if (v >> 60) throw Error("The archive has a number too large to use.");
        v = v * 8 + uint64_t(s[at] - '0');
    }
    return negative ? -int64_t(v) : int64_t(v);
}

bool checksumOk(const uint8_t* b) {
    int64_t stored;
    try {
        stored = nti(b + 148, 8);
    } catch (const Error&) {
        return false;
    }
    int64_t unsignedSum = 256, signedSum = 256;
    for (size_t i = 0; i < block; ++i) {
        if (i >= 148 && i < 156) continue;
        unsignedSum += b[i];
        signedSum += int8_t(b[i]);
    }
    return stored == unsignedSum || stored == signedSum;
}

void rstripSlash(std::string& s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
}

// int() and float() as tarfile uses them on pax numbers; anything else is 0.
int64_t paxInt(const std::string& v) {
    std::string s;
    for (char c : v)
        if (c != '_') s += c;
    char* end = nullptr;
    errno = 0;
    const long long n = strtoll(s.c_str(), &end, 10);
    while (end && *end && (*end == ' ' || (*end >= '\t' && *end <= '\r'))) ++end;
    if (s.empty() || errno || !end || *end) return 0;
    return n;
}

int64_t paxTime(const std::string& v) {
    char* end = nullptr;
    const double d = strtod(v.c_str(), &end);
    while (end && *end && (*end == ' ' || (*end >= '\t' && *end <= '\r'))) ++end;
    if (v.empty() || !end || *end || !std::isfinite(d) || std::fabs(d) > 9e15) return 0;
    return int64_t(std::floor(d));
}

void applyPax(Member& m, const std::map<std::string, std::string>& pax) {
    for (const auto& [key, value] : pax) {
        if (key == "path") {
            m.name = value;
            rstripSlash(m.name);
        } else if (key == "linkpath") {
            m.linkname = value;
        } else if (key == "size") {
            m.size = paxInt(value);
        } else if (key == "mtime") {
            m.mtime = paxTime(value);
        } else if (key == "uid") {
            m.uid = paxInt(value);
        } else if (key == "gid") {
            m.gid = paxInt(value);
        }
    }
}

}  // namespace

void TarReader::readExact(uint8_t* data, size_t n) {
    if (offset_ + n > limit_) throw Error("The archive is larger than its partitions can hold.");
    if (source_.read(data, n) != n) throw Error("The archive is cut off.");
    offset_ += n;
}

void TarReader::skip(uint64_t n) {
    uint8_t scratch[8192];
    while (n) {
        const size_t count = size_t(std::min<uint64_t>(n, sizeof scratch));
        readExact(scratch, count);
        n -= count;
    }
}

void TarReader::readBlock(uint8_t* b, bool& empty) {
    if (offset_ + block > limit_) throw Error("The archive is larger than its partitions can hold.");
    const size_t got = source_.read(b, block);
    offset_ += got;
    empty = got == 0;
    if (got && got != block) throw Error("The archive is cut off.");
}

std::string TarReader::payload(uint64_t size) {
    if (padded(size) > maxHeaderRead) throw Error("The archive has a tar header that is too large.");
    std::string out(size_t(padded(size)), '\0');
    readExact(reinterpret_cast<uint8_t*>(&out[0]), out.size());
    out.resize(size_t(size));
    return out;
}

bool TarReader::header(Member& m, bool dircheck, bool first) {
    uint8_t b[block];
    bool empty;
    readBlock(b, empty);
    if (empty) {
        if (first) throw Error("The archive is empty.");
        return false;
    }
    if (std::all_of(b, b + block, [](uint8_t c) { return c == 0; })) {
        if (first) throw Error("The archive is not a tar archive.");
        return false;
    }
    if (!checksumOk(b)) throw Error(first ? "The archive is not a tar archive." : "The archive has a damaged tar header.");

    m = Member{};
    m.name = nts(b, 100);
    m.mode = uint32_t(nti(b + 100, 8));
    m.uid = nti(b + 108, 8);
    m.gid = nti(b + 116, 8);
    m.size = nti(b + 124, 12);
    m.mtime = nti(b + 136, 12);
    m.type = char(b[156]);
    m.linkname = nts(b + 157, 100);
    nti(b + 329, 8);
    nti(b + 337, 8);
    const std::string prefix = nts(b + 345, 155);
    if (dircheck && m.type == '\0' && !m.name.empty() && m.name.back() == '/') m.type = '5';
    if (m.type == 'S') throw Error("The archive has sparse GNU tar entries, which aren't supported.");
    if (m.type == '5') rstripSlash(m.name);
    if (!prefix.empty() && m.type != 'L' && m.type != 'K') m.name = prefix + "/" + m.name;

    if (m.type == 'L' || m.type == 'K') {
        const auto text = payload(uint64_t(std::max<int64_t>(m.size, 0)));
        const auto name = nts(reinterpret_cast<const uint8_t*>(text.data()), text.size());
        const char type = m.type;
        if (!header(m, false, false)) throw Error("The archive has a damaged tar header.");
        if (type == 'L')
            m.name = name;
        else
            m.linkname = name;
        if (m.type == '5' && !m.name.empty() && m.name.back() == '/') m.name.pop_back();
        return true;
    }

    if (m.type == 'x' || m.type == 'g' || m.type == 'X') {
        const char type = m.type;
        const auto buf = payload(uint64_t(std::max<int64_t>(m.size, 0)));
        std::map<std::string, std::string> local = global_;
        auto& pax = type == 'g' ? global_ : local;
        size_t pos = 0;
        while (pos < buf.size() && buf[pos] != 0) {
            size_t digits = pos;
            while (digits < buf.size() && buf[digits] >= '0' && buf[digits] <= '9') ++digits;
            if (digits == pos || digits >= buf.size() || buf[digits] != ' ' || digits - pos > 9)
                throw Error("The archive has a damaged pax header.");
            const size_t length = size_t(std::stoul(buf.substr(pos, digits - pos)));
            if (length < 5 || pos + length > buf.size()) throw Error("The archive has a damaged pax header.");
            const size_t end = pos + length - 1;
            if (end <= digits + 1 || buf[end] != '\n') throw Error("The archive has a damaged pax header.");
            const auto record = buf.substr(digits + 1, end - digits - 1);
            const auto equals = record.find('=');
            if (equals == 0 || equals == std::string::npos) throw Error("The archive has a damaged pax header.");
            const auto key = record.substr(0, equals);
            if (key.compare(0, 11, "GNU.sparse.") == 0)
                throw Error("The archive has sparse pax tar entries, which aren't supported.");
            pax[key] = record.substr(equals + 1);
            pos += length;
        }
        if (!header(m, false, false)) throw Error("The archive has a damaged tar header.");
        if (type != 'g') applyPax(m, local);
        return true;
    }

    if (m.type == '\0' || m.type == '7') m.type = '0';
    applyPax(m, global_);
    if (m.type == '5') rstripSlash(m.name);
    return true;
}

bool TarReader::next(Member& m) {
    skip(left_ + padding_);
    left_ = padding_ = 0;
    if (!header(m, true, offset_ == 0)) return false;
    // Data follows regular files and types tarfile doesn't know.
    static const std::string known = "0125346";
    const bool data = m.type == '0' || known.find(m.type) == std::string::npos;
    if (data && m.size > 0) {
        left_ = uint64_t(m.size);
        padding_ = padded(left_) - left_;
    }
    return true;
}

size_t TarReader::readData(uint8_t* data, size_t n) {
    const size_t count = size_t(std::min<uint64_t>(n, left_));
    if (!count) return 0;
    readExact(data, count);
    left_ -= count;
    return count;
}

}  // namespace frondTar
