#ifndef FROND_MACHINE_STATE_ARCHIVE_H
#define FROND_MACHINE_STATE_ARCHIVE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "CPCrc.h"

// Field-by-field visitors for a private machine state. A model lists its fields once in
//     template <class A> void state(A& a);
// and the writer and the reader both run that list. Values are little endian, as the hosts are.
// Fixed fields come from the options at create and never change: the writer stores them and
// the reader compares them with the value already in the object, without replacing it.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "machine state archives are little endian"
#endif

class StateWriter {
   public:
    static constexpr bool loading = false;
    explicit StateWriter(std::vector<uint8_t>& out) : out(out) {}
    StateWriter& Do8(uint8_t& v) { return raw(&v, 1); }
    StateWriter& Do16(uint16_t& v) { return raw(&v, 2); }
    StateWriter& Do32(uint32_t& v) { return raw(&v, 4); }
    StateWriter& Do64(uint64_t& v) { return raw(&v, 8); }
    StateWriter& DoBool(bool& v) { out.push_back(v ? 1 : 0); return *this; }
    template <class E> StateWriter& DoEnum(E& v, uint32_t) { uint32_t n = uint32_t(v); return raw(&n, 4); }
    StateWriter& DoBuffer(void* data, size_t bytes) { return raw(data, bytes); }
    StateWriter& DoBuffer16(uint16_t* data, size_t count) { return raw(data, count * 2); }
    StateWriter& DoBuffer32(uint32_t* data, size_t count) { return raw(data, count * 4); }
    // Elements are 1, 2, 4 or 8 byte integers.
    template <class T> StateWriter& DoVector(std::vector<T>& v, uint64_t) {
        uint64_t count = v.size();
        raw(&count, 8);
        return raw(v.data(), v.size() * sizeof(T));
    }
    StateWriter& Fixed8(uint8_t& v) { return Do8(v); }
    StateWriter& Fixed16(uint16_t& v) { return Do16(v); }
    StateWriter& Fixed32(uint32_t& v) { return Do32(v); }
    StateWriter& FixedBool(bool& v) { return DoBool(v); }
    // A range the reader holds a decoded value to.
    StateWriter& Require(bool, const char*) { return *this; }
    bool ok() const { return true; }

   private:
    StateWriter& raw(const void* data, size_t bytes) {
        const auto* p = static_cast<const uint8_t*>(data);
        out.insert(out.end(), p, p + bytes);
        return *this;
    }
    std::vector<uint8_t>& out;
};

// The first failure sticks: later reads leave their targets alone and the detail stays.
class StateReader {
   public:
    static constexpr bool loading = true;
    StateReader(const uint8_t* data, size_t size) : data(data), size(size) {}
    StateReader& Do8(uint8_t& v) { return raw(&v, 1); }
    StateReader& Do16(uint16_t& v) { return raw(&v, 2); }
    StateReader& Do32(uint32_t& v) { return raw(&v, 4); }
    StateReader& Do64(uint64_t& v) { return raw(&v, 8); }
    StateReader& DoBool(bool& v) {
        uint8_t n = 0;
        raw(&n, 1);
        if (n > 1) fail("bool"); else if (!failure) v = n;
        return *this;
    }
    template <class E> StateReader& DoEnum(E& v, uint32_t count) {
        uint32_t n = 0;
        raw(&n, 4);
        if (n >= count) fail("enum"); else if (!failure) v = E(n);
        return *this;
    }
    StateReader& DoBuffer(void* target, size_t bytes) { return raw(target, bytes); }
    StateReader& DoBuffer16(uint16_t* target, size_t count) { return raw(target, count * 2); }
    StateReader& DoBuffer32(uint32_t* target, size_t count) { return raw(target, count * 4); }
    template <class T> StateReader& DoVector(std::vector<T>& v, uint64_t maxCount) {
        uint64_t count = 0;
        raw(&count, 8);
        if (failure) return *this;
        if (count > maxCount || count > (size - position) / sizeof(T)) return fail("vector length");
        v.resize(size_t(count));
        return raw(v.data(), size_t(count) * sizeof(T));
    }
    StateReader& Fixed8(uint8_t& v) { uint8_t n = 0; raw(&n, 1); return fixed(n == v); }
    StateReader& Fixed16(uint16_t& v) { uint16_t n = 0; raw(&n, 2); return fixed(n == v); }
    StateReader& Fixed32(uint32_t& v) { uint32_t n = 0; raw(&n, 4); return fixed(n == v); }
    StateReader& FixedBool(bool& v) { uint8_t n = 0; raw(&n, 1); return fixed(n == uint8_t(v)); }
    StateReader& Require(bool condition, const char* what) {
        if (!failure && !condition) fail(what);
        return *this;
    }
    StateReader& fail(const char* what) {
        if (!failure) failure = what;
        return *this;
    }
    bool ok() const { return !failure; }
    // Everything was read and nothing is left over.
    bool complete() const { return !failure && position == size; }
    const char* detail() const { return failure; }
    // The failure was a fixed field that differs from the machine's.
    bool fixedMismatch() const { return mismatch; }

   private:
    StateReader& raw(void* target, size_t bytes) {
        if (failure) return *this;
        if (bytes > size - position) return fail("section too short");
        if (bytes) std::memcpy(target, data + position, bytes);
        position += bytes;
        return *this;
    }
    StateReader& fixed(bool equal) {
        if (!failure && !equal) { mismatch = true; fail("fixed field"); }
        return *this;
    }
    const uint8_t* data;
    size_t size, position = 0;
    const char* failure = nullptr;
    bool mismatch = false;
};

// Container: header, sections, then the section table.
//   header  magic[8] formatVersion coreRevision layoutFingerprint(64) flags sectionCount
//           tableOffset(64) totalSize(64) tableCrc headerCrc
//   table   per section: fourcc version offset(64) size(64) crc
inline constexpr char stateContainerMagic[8] = {'F', 'R', 'Z', '2', 'S', 'A', 'V', 'E'};
inline constexpr uint32_t stateContainerFormatVersion = 1;
inline constexpr size_t stateContainerHeaderBytes = 56;
inline constexpr size_t stateContainerEntryBytes = 28;
inline constexpr uint32_t stateContainerMaxSections = 64;
constexpr uint32_t stateFourcc(const char (&name)[5]) {
    return uint32_t(uint8_t(name[0])) | uint32_t(uint8_t(name[1])) << 8 |
           uint32_t(uint8_t(name[2])) << 16 | uint32_t(uint8_t(name[3])) << 24;
}
struct StateSection {
    uint32_t fourcc = 0, version = 0;
    uint64_t offset = 0, size = 0;
    uint32_t crc = 0;
};

class StateContainerWriter {
   public:
    StateContainerWriter(std::vector<uint8_t>& out, uint32_t coreRevision, uint64_t layoutFingerprint, uint32_t flags)
        : out(out), coreRevision(coreRevision), layoutFingerprint(layoutFingerprint), flags(flags) {
        out.assign(stateContainerHeaderBytes, 0);
    }
    // Write the section's fields through writer(), then end it.
    StateWriter begin(uint32_t fourcc, uint32_t version) {
        sections.push_back({fourcc, version, out.size(), 0, 0});
        return StateWriter(out);
    }
    void end() {
        auto& s = sections.back();
        s.size = out.size() - s.offset;
        s.crc = crc::CRC32(out.data() + s.offset, size_t(s.size));
    }
    void finish() {
        const uint64_t tableOffset = out.size();
        StateWriter table(out);
        for (auto& s : sections) table.Do32(s.fourcc).Do32(s.version).Do64(s.offset).Do64(s.size).Do32(s.crc);
        uint32_t tableCrc = crc::CRC32(out.data() + tableOffset, out.size() - size_t(tableOffset));
        std::vector<uint8_t> header;
        StateWriter h(header);
        uint32_t format = stateContainerFormatVersion, count = uint32_t(sections.size());
        uint64_t table64 = tableOffset, total = out.size();
        h.DoBuffer(const_cast<char*>(stateContainerMagic), 8).Do32(format).Do32(coreRevision).Do64(layoutFingerprint)
            .Do32(flags).Do32(count).Do64(table64).Do64(total).Do32(tableCrc);
        uint32_t headerCrc = crc::CRC32(header.data(), header.size());
        h.Do32(headerCrc);
        std::memcpy(out.data(), header.data(), stateContainerHeaderBytes);
    }

   private:
    std::vector<uint8_t>& out;
    uint32_t coreRevision;
    uint64_t layoutFingerprint;
    uint32_t flags;
    std::vector<StateSection> sections;
};

enum class StateContainerError { ok, truncated, badMagic, badFormatVersion, corruptHeader, corruptSection };

// Checks the header, the table and every section's bounds and CRC. Borrows the bytes.
class StateContainerReader {
   public:
    StateContainerError open(const uint8_t* bytes, size_t length) {
        data = bytes;
        sections.clear();
        failed = nullptr;
        if (length < 8) return StateContainerError::truncated;
        if (std::memcmp(bytes, stateContainerMagic, 8)) return StateContainerError::badMagic;
        if (length < stateContainerHeaderBytes) return StateContainerError::truncated;
        StateReader h(bytes + 8, stateContainerHeaderBytes - 8);
        uint32_t format = 0, count = 0, tableCrc = 0, headerCrc = 0;
        uint64_t tableOffset = 0, total = 0;
        h.Do32(format).Do32(coreRevision).Do64(layoutFingerprint).Do32(flags).Do32(count).Do64(tableOffset)
            .Do64(total).Do32(tableCrc).Do32(headerCrc);
        if (crc::CRC32(bytes, stateContainerHeaderBytes - 4) != headerCrc) return StateContainerError::corruptHeader;
        formatVersion = format;
        if (format != stateContainerFormatVersion) return StateContainerError::badFormatVersion;
        if (total > length) return StateContainerError::truncated;
        if (total != length || count > stateContainerMaxSections || tableOffset < stateContainerHeaderBytes ||
            tableOffset > total || total - tableOffset != uint64_t(count) * stateContainerEntryBytes)
            return StateContainerError::corruptHeader;
        if (crc::CRC32(bytes + tableOffset, size_t(total - tableOffset)) != tableCrc)
            return StateContainerError::corruptHeader;
        StateReader table(bytes + tableOffset, size_t(total - tableOffset));
        for (uint32_t i = 0; i < count; ++i) {
            StateSection s;
            table.Do32(s.fourcc).Do32(s.version).Do64(s.offset).Do64(s.size).Do32(s.crc);
            if (s.offset < stateContainerHeaderBytes || s.offset > tableOffset || s.size > tableOffset - s.offset)
                return StateContainerError::corruptHeader;
            for (const auto& other : sections)
                if (other.fourcc == s.fourcc) return StateContainerError::corruptHeader;
            sections.push_back(s);
        }
        for (const auto& s : sections)
            if (crc::CRC32(bytes + s.offset, size_t(s.size)) != s.crc) {
                failed = &s;
                return StateContainerError::corruptSection;
            }
        return StateContainerError::ok;
    }
    const StateSection* find(uint32_t fourcc) const {
        for (const auto& s : sections) if (s.fourcc == fourcc) return &s;
        return nullptr;
    }
    StateReader reader(const StateSection& s) const { return StateReader(data + s.offset, size_t(s.size)); }
    // The section whose CRC failed.
    const StateSection* corrupt() const { return failed; }
    uint32_t formatVersion = 0, coreRevision = 0, flags = 0;
    uint64_t layoutFingerprint = 0;

   private:
    const uint8_t* data = nullptr;
    std::vector<StateSection> sections;
    const StateSection* failed = nullptr;
};

// FNV-1a over the words that pin a layout: section, state version and object size per model.
constexpr uint64_t stateLayoutMix(uint64_t hash, uint64_t word) {
    for (unsigned i = 0; i < 8; ++i) hash = (hash ^ ((word >> (i * 8)) & 0xff)) * 1099511628211ull;
    return hash;
}

#endif
