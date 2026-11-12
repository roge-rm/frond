#ifndef FROND_STORAGE_FAT_NAMES_H
#define FROND_STORAGE_FAT_NAMES_H

#include "storage/Errors.h"

#include <cstdint>
#include <ctime>
#include <set>
#include <string>
#include <vector>

// FAT names: UTF-8 and UTF-16, case folding for name matches, the 8.3 alias and long-name
// entries Frond writes, and DOS dates. Names are UTF-8 in the API and UTF-16 on the card.
namespace frondStorage {
namespace fat {

// UTF-8 to UTF-16. A byte that isn't valid UTF-8 becomes U+FFFD.
inline std::u16string utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const auto b = uint8_t(s[i]);
        uint32_t c = 0xfffd;
        size_t n = 1;
        if (b < 0x80) {
            c = b;
        } else if (b >= 0xc2 && b < 0xf5) {
            const size_t len = b < 0xe0 ? 2 : b < 0xf0 ? 3 : 4;
            uint32_t v = b & (0x7f >> len);
            bool ok = i + len <= s.size();
            for (size_t k = 1; ok && k < len; ++k) {
                const auto t = uint8_t(s[i + k]);
                if ((t & 0xc0) != 0x80) ok = false;
                v = (v << 6) | (t & 0x3f);
            }
            const uint32_t least = len == 2 ? 0x80 : len == 3 ? 0x800 : 0x10000;
            if (ok && v >= least && v <= 0x10ffff && (v < 0xd800 || v > 0xdfff)) {
                c = v;
                n = len;
            }
        }
        if (c >= 0x10000) {
            out += char16_t(0xd800 + ((c - 0x10000) >> 10));
            out += char16_t(0xdc00 + ((c - 0x10000) & 0x3ff));
        } else {
            out += char16_t(c);
        }
        i += n;
    }
    return out;
}

// UTF-16 to UTF-8. A lone surrogate becomes U+FFFD.
inline std::string utf8(const std::u16string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00);
            ++i;
        } else if (c >= 0xd800 && c < 0xe000) {
            c = 0xfffd;
        }
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xc0 | (c >> 6));
            out += char(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out += char(0xe0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3f));
            out += char(0x80 | (c & 0x3f));
        } else {
            out += char(0xf0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3f));
            out += char(0x80 | ((c >> 6) & 0x3f));
            out += char(0x80 | (c & 0x3f));
        }
    }
    return out;
}

// Simple case folding for the Basic Multilingual Plane (Unicode 15.1, one code unit to one):
// first, last, the amount added, and 1 for every code point in the run or 2 for every other one.
struct FoldRun {
    char16_t first, last;
    int32_t delta;
    uint8_t stride;
};
constexpr FoldRun foldRuns[] = {
    {0x0041, 0x005a, 32, 1}, {0x00b5, 0x00b5, 775, 1}, {0x00c0, 0x00d6, 32, 1}, {0x00d8, 0x00de, 32, 1},
    {0x0100, 0x012e, 1, 2}, {0x0132, 0x0136, 1, 2}, {0x0139, 0x0147, 1, 2}, {0x014a, 0x0176, 1, 2},
    {0x0178, 0x0178, -121, 1}, {0x0179, 0x017d, 1, 2}, {0x017f, 0x017f, -268, 1}, {0x0181, 0x0181, 210, 1},
    {0x0182, 0x0184, 1, 2}, {0x0186, 0x0186, 206, 1}, {0x0187, 0x0187, 1, 1}, {0x0189, 0x018a, 205, 1},
    {0x018b, 0x018b, 1, 1}, {0x018e, 0x018e, 79, 1}, {0x018f, 0x018f, 202, 1}, {0x0190, 0x0190, 203, 1},
    {0x0191, 0x0191, 1, 1}, {0x0193, 0x0193, 205, 1}, {0x0194, 0x0194, 207, 1}, {0x0196, 0x0196, 211, 1},
    {0x0197, 0x0197, 209, 1}, {0x0198, 0x0198, 1, 1}, {0x019c, 0x019c, 211, 1}, {0x019d, 0x019d, 213, 1},
    {0x019f, 0x019f, 214, 1}, {0x01a0, 0x01a4, 1, 2}, {0x01a6, 0x01a6, 218, 1}, {0x01a7, 0x01a7, 1, 1},
    {0x01a9, 0x01a9, 218, 1}, {0x01ac, 0x01ac, 1, 1}, {0x01ae, 0x01ae, 218, 1}, {0x01af, 0x01af, 1, 1},
    {0x01b1, 0x01b2, 217, 1}, {0x01b3, 0x01b5, 1, 2}, {0x01b7, 0x01b7, 219, 1}, {0x01b8, 0x01b8, 1, 1},
    {0x01bc, 0x01bc, 1, 1}, {0x01c4, 0x01c4, 2, 1}, {0x01c5, 0x01c5, 1, 1}, {0x01c7, 0x01c7, 2, 1},
    {0x01c8, 0x01c8, 1, 1}, {0x01ca, 0x01ca, 2, 1}, {0x01cb, 0x01db, 1, 2}, {0x01de, 0x01ee, 1, 2},
    {0x01f1, 0x01f1, 2, 1}, {0x01f2, 0x01f4, 1, 2}, {0x01f6, 0x01f6, -97, 1}, {0x01f7, 0x01f7, -56, 1},
    {0x01f8, 0x021e, 1, 2}, {0x0220, 0x0220, -130, 1}, {0x0222, 0x0232, 1, 2}, {0x023a, 0x023a, 10795, 1},
    {0x023b, 0x023b, 1, 1}, {0x023d, 0x023d, -163, 1}, {0x023e, 0x023e, 10792, 1}, {0x0241, 0x0241, 1, 1},
    {0x0243, 0x0243, -195, 1}, {0x0244, 0x0244, 69, 1}, {0x0245, 0x0245, 71, 1}, {0x0246, 0x024e, 1, 2},
    {0x0345, 0x0345, 116, 1}, {0x0370, 0x0372, 1, 2}, {0x0376, 0x0376, 1, 1}, {0x037f, 0x037f, 116, 1},
    {0x0386, 0x0386, 38, 1}, {0x0388, 0x038a, 37, 1}, {0x038c, 0x038c, 64, 1}, {0x038e, 0x038f, 63, 1},
    {0x0391, 0x03a1, 32, 1}, {0x03a3, 0x03ab, 32, 1}, {0x03c2, 0x03c2, 1, 1}, {0x03cf, 0x03cf, 8, 1},
    {0x03d0, 0x03d0, -30, 1}, {0x03d1, 0x03d1, -25, 1}, {0x03d5, 0x03d5, -15, 1}, {0x03d6, 0x03d6, -22, 1},
    {0x03d8, 0x03ee, 1, 2}, {0x03f0, 0x03f0, -54, 1}, {0x03f1, 0x03f1, -48, 1}, {0x03f4, 0x03f4, -60, 1},
    {0x03f5, 0x03f5, -64, 1}, {0x03f7, 0x03f7, 1, 1}, {0x03f9, 0x03f9, -7, 1}, {0x03fa, 0x03fa, 1, 1},
    {0x03fd, 0x03ff, -130, 1}, {0x0400, 0x040f, 80, 1}, {0x0410, 0x042f, 32, 1}, {0x0460, 0x0480, 1, 2},
    {0x048a, 0x04be, 1, 2}, {0x04c0, 0x04c0, 15, 1}, {0x04c1, 0x04cd, 1, 2}, {0x04d0, 0x052e, 1, 2},
    {0x0531, 0x0556, 48, 1}, {0x10a0, 0x10c5, 7264, 1}, {0x10c7, 0x10c7, 7264, 1}, {0x10cd, 0x10cd, 7264, 1},
    {0x13f8, 0x13fd, -8, 1}, {0x1c80, 0x1c80, -6222, 1}, {0x1c81, 0x1c81, -6221, 1}, {0x1c82, 0x1c82, -6212, 1},
    {0x1c83, 0x1c84, -6210, 1}, {0x1c85, 0x1c85, -6211, 1}, {0x1c86, 0x1c86, -6204, 1}, {0x1c87, 0x1c87, -6180, 1},
    {0x1c88, 0x1c88, 35267, 1}, {0x1c90, 0x1cba, -3008, 1}, {0x1cbd, 0x1cbf, -3008, 1}, {0x1e00, 0x1e94, 1, 2},
    {0x1e9b, 0x1e9b, -58, 1}, {0x1e9e, 0x1e9e, -7615, 1}, {0x1ea0, 0x1efe, 1, 2}, {0x1f08, 0x1f0f, -8, 1},
    {0x1f18, 0x1f1d, -8, 1}, {0x1f28, 0x1f2f, -8, 1}, {0x1f38, 0x1f3f, -8, 1}, {0x1f48, 0x1f4d, -8, 1},
    {0x1f59, 0x1f5f, -8, 2}, {0x1f68, 0x1f6f, -8, 1}, {0x1f88, 0x1f8f, -8, 1}, {0x1f98, 0x1f9f, -8, 1},
    {0x1fa8, 0x1faf, -8, 1}, {0x1fb8, 0x1fb9, -8, 1}, {0x1fba, 0x1fbb, -74, 1}, {0x1fbc, 0x1fbc, -9, 1},
    {0x1fbe, 0x1fbe, -7173, 1}, {0x1fc8, 0x1fcb, -86, 1}, {0x1fcc, 0x1fcc, -9, 1}, {0x1fd8, 0x1fd9, -8, 1},
    {0x1fda, 0x1fdb, -100, 1}, {0x1fe8, 0x1fe9, -8, 1}, {0x1fea, 0x1feb, -112, 1}, {0x1fec, 0x1fec, -7, 1},
    {0x1ff8, 0x1ff9, -128, 1}, {0x1ffa, 0x1ffb, -126, 1}, {0x1ffc, 0x1ffc, -9, 1}, {0x2126, 0x2126, -7517, 1},
    {0x212a, 0x212a, -8383, 1}, {0x212b, 0x212b, -8262, 1}, {0x2132, 0x2132, 28, 1}, {0x2160, 0x216f, 16, 1},
    {0x2183, 0x2183, 1, 1}, {0x24b6, 0x24cf, 26, 1}, {0x2c00, 0x2c2f, 48, 1}, {0x2c60, 0x2c60, 1, 1},
    {0x2c62, 0x2c62, -10743, 1}, {0x2c63, 0x2c63, -3814, 1}, {0x2c64, 0x2c64, -10727, 1}, {0x2c67, 0x2c6b, 1, 2},
    {0x2c6d, 0x2c6d, -10780, 1}, {0x2c6e, 0x2c6e, -10749, 1}, {0x2c6f, 0x2c6f, -10783, 1}, {0x2c70, 0x2c70, -10782, 1},
    {0x2c72, 0x2c72, 1, 1}, {0x2c75, 0x2c75, 1, 1}, {0x2c7e, 0x2c7f, -10815, 1}, {0x2c80, 0x2ce2, 1, 2},
    {0x2ceb, 0x2ced, 1, 2}, {0x2cf2, 0x2cf2, 1, 1}, {0xa640, 0xa66c, 1, 2}, {0xa680, 0xa69a, 1, 2},
    {0xa722, 0xa72e, 1, 2}, {0xa732, 0xa76e, 1, 2}, {0xa779, 0xa77b, 1, 2}, {0xa77d, 0xa77d, -35332, 1},
    {0xa77e, 0xa786, 1, 2}, {0xa78b, 0xa78b, 1, 1}, {0xa78d, 0xa78d, -42280, 1}, {0xa790, 0xa792, 1, 2},
    {0xa796, 0xa7a8, 1, 2}, {0xa7aa, 0xa7aa, -42308, 1}, {0xa7ab, 0xa7ab, -42319, 1}, {0xa7ac, 0xa7ac, -42315, 1},
    {0xa7ad, 0xa7ad, -42305, 1}, {0xa7ae, 0xa7ae, -42308, 1}, {0xa7b0, 0xa7b0, -42258, 1}, {0xa7b1, 0xa7b1, -42282, 1},
    {0xa7b2, 0xa7b2, -42261, 1}, {0xa7b3, 0xa7b3, 928, 1}, {0xa7b4, 0xa7c2, 1, 2}, {0xa7c4, 0xa7c4, -48, 1},
    {0xa7c5, 0xa7c5, -42307, 1}, {0xa7c6, 0xa7c6, -35384, 1}, {0xa7c7, 0xa7c9, 1, 2}, {0xa7d0, 0xa7d0, 1, 1},
    {0xa7d6, 0xa7d8, 1, 2}, {0xa7f5, 0xa7f5, 1, 1}, {0xab70, 0xabbf, -38864, 1}, {0xff21, 0xff3a, 32, 1},
};

inline char16_t fold(char16_t c) {
    if (c < 0x41) return c;
    if (c <= 0x7f) return c <= 0x5a ? char16_t(c + 32) : c;
    size_t lo = 0, hi = sizeof(foldRuns) / sizeof(foldRuns[0]);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (foldRuns[mid].last < c) lo = mid + 1;
        else hi = mid;
    }
    if (lo == sizeof(foldRuns) / sizeof(foldRuns[0])) return c;
    const auto& run = foldRuns[lo];
    if (c < run.first || (c - run.first) % run.stride) return c;
    return char16_t(int32_t(c) + run.delta);
}

inline std::u16string folded(std::u16string s) {
    for (auto& c : s) c = fold(c);
    return s;
}

// Names match as FAT matches them, ignoring case.
inline bool sameName(const std::u16string& a, const std::u16string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (fold(a[i]) != fold(b[i])) return false;
    return true;
}
inline bool sameName(const std::string& a, const std::string& b) { return sameName(utf16(a), utf16(b)); }

inline std::string asciiUpper(std::string s) {
    for (auto& c : s)
        if (c >= 'a' && c <= 'z') c = char(c - 32);
    return s;
}

// Throws when FAT can't hold the name: empty, over 255 UTF-16 units, "." or "..", a leading or
// trailing space, a trailing dot, a control character or one of "*/:<>?\|.
inline void checkName(const std::string& utf8Name) {
    const auto name = utf16(utf8Name);
    bool bad = name.empty() || name.size() > 255 || name == u"." || name == u".." || name.back() == u'.' ||
               name.back() == u' ' || name.front() == u' ';
    for (const char16_t ch : name)
        if (ch < 0x20 || ch == 0x7f || std::u16string(u"\"*/:<>?\\|").find(ch) != std::u16string::npos) bad = true;
    if (bad) throw Error("FAT can't use the name \"" + utf8Name + "\".");
}

// The short name as Frond writes it (no case bits), "README.TXT".
inline std::string shortDisplay(const std::string& eleven) {
    auto trim = [](std::string s) {
        while (!s.empty() && s.back() == ' ') s.pop_back();
        size_t at = 0;
        while (at < s.size() && s[at] == ' ') ++at;
        return s.substr(at);
    };
    std::string base, ext;
    for (size_t i = 0; i < 8 && i < eleven.size(); ++i) base += eleven[i];
    for (size_t i = 8; i < 11 && i < eleven.size(); ++i) ext += eleven[i];
    // Latin-1 bytes, as a reader shows them.
    auto latin1 = [](const std::string& s) {
        std::u16string u;
        for (char c : s) u += char16_t(uint8_t(c));
        return utf8(u);
    };
    base = latin1(trim(base));
    ext = latin1(trim(ext));
    return ext.empty() ? base : base + "." + ext;
}

// The same 8.3 alias Android's FatImage writes: upper case, anything past ASCII and +,;=[] as _,
// spaces and inner dots dropped, a ~N tail when something was lost or the name is taken. `taken`
// holds upper-case names already in the folder ("NOTES.TXT"). Empty when every tail is taken.
// No case bits and no code page bytes, so every reader shows the long name as it was.
inline std::string shortName(const std::string& utf8Name, const std::set<std::string>& taken) {
    const auto name = utf16(utf8Name);
    size_t dot = name.rfind(u'.');
    if (dot == 0) dot = std::u16string::npos;
    bool lossy = false;
    auto clean = [&](const std::u16string& part) {
        std::string out;
        for (char16_t ch : part) {
            if (ch >= 'a' && ch <= 'z') ch = char16_t(ch - 32);
            if (ch == ' ' || ch == '.') {
                lossy = true;
            } else if (ch > 0x7e || std::string("+,;=[]").find(char(ch)) != std::string::npos) {
                out += '_';
                lossy = true;
            } else {
                out += char(ch);
            }
        }
        return out;
    };
    auto base = clean(dot != std::u16string::npos ? name.substr(0, dot) : name);
    auto ext = dot != std::u16string::npos ? clean(name.substr(dot + 1)) : std::string();
    if (base.size() > 8) lossy = true;
    if (ext.size() > 3) {
        ext.resize(3);
        lossy = true;
    }
    if (base.empty()) {
        base = "_";
        lossy = true;
    }
    auto key = [](const std::string& b, const std::string& e) { return e.empty() ? b : b + "." + e; };
    auto pack = [](const std::string& b, const std::string& e) {
        std::string out(11, ' ');
        for (size_t i = 0; i < 8 && i < b.size(); ++i) out[i] = b[i];
        for (size_t i = 0; i < 3 && i < e.size(); ++i) out[8 + i] = e[i];
        return out;
    };
    if (!lossy && base.size() <= 8 && !taken.count(key(base, ext))) return pack(base, ext);
    for (int n = 1; n <= 999999; ++n) {
        const auto tail = "~" + std::to_string(n);
        const auto b = base.substr(0, 8 - tail.size()) + tail;
        if (!taken.count(key(b, ext))) return pack(b, ext);
    }
    return {};
}

inline uint8_t shortChecksum(const std::string& eleven) {
    uint8_t sum = 0;
    for (size_t i = 0; i < 11; ++i) sum = uint8_t(((sum & 1) << 7) + (sum >> 1) + uint8_t(eleven[i]));
    return sum;
}

// A DOS date and time, local time, two-second steps. Date 0 means none.
struct Stamp {
    uint16_t date{}, time{};
};
inline Stamp stampOf(const std::tm& t) {
    int year = t.tm_year + 1900 - 1980;
    year = year < 0 ? 0 : year > 127 ? 127 : year;
    return {uint16_t((year << 9) | ((t.tm_mon + 1) << 5) | t.tm_mday),
            uint16_t((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec / 2))};
}
inline Stamp stampNow() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return stampOf(local);
}

inline void put16(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
inline void put32(uint8_t* p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}
inline uint32_t get16(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }
inline uint32_t get32(const uint8_t* p) { return get16(p) | (get16(p + 2) << 16); }

constexpr int longOffsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

// The dates of a short entry: created, accessed, written.
inline void putStamp(uint8_t* entry, Stamp s) {
    put16(entry + 14, s.time);
    put16(entry + 16, s.date);
    put16(entry + 18, s.date);
    put16(entry + 22, s.time);
    put16(entry + 24, s.date);
}

// One directory entry with the long-name entries before it, which it has whenever the short name
// isn't the name exactly.
inline std::vector<uint8_t> dirEntry(const std::string& utf8Name, const std::string& eleven, uint8_t attr,
                                     uint32_t cluster, uint32_t size, Stamp when) {
    const auto name = utf16(utf8Name);
    const bool needsLong = shortDisplay(eleven) != utf8Name;
    const size_t longCount = needsLong ? (name.size() + 12) / 13 : 0;
    std::vector<uint8_t> out((longCount + 1) * 32, 0);
    const auto sum = shortChecksum(eleven);
    for (size_t k = 0; k < longCount; ++k) {
        const size_t order = longCount - k;
        uint8_t* at = out.data() + k * 32;
        at[0] = uint8_t(order | (k == 0 ? 0x40 : 0));
        at[11] = 0x0f;
        at[13] = sum;
        for (size_t j = 0; j < 13; ++j) {
            const size_t index = (order - 1) * 13 + j;
            const uint32_t c = index < name.size() ? name[index] : index == name.size() ? 0 : 0xffff;
            put16(at + longOffsets[j], c);
        }
    }
    uint8_t* at = out.data() + longCount * 32;
    for (size_t i = 0; i < 11; ++i) at[i] = uint8_t(eleven[i]);
    at[11] = attr;
    putStamp(at, when);
    put16(at + 26, cluster);
    put32(at + 28, size);
    return out;
}

}  // namespace fat
}  // namespace frondStorage

#endif
