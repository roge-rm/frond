#include "PalmDb.h"

#include <cstring>

namespace frondSync {

namespace {
uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Put16(Bytes& out, uint16_t v) { out.push_back(uint8_t(v >> 8)), out.push_back(uint8_t(v)); }
void Put32(Bytes& out, uint32_t v) { Put16(out, uint16_t(v >> 16)), Put16(out, uint16_t(v)); }
void Set32(Bytes& out, size_t at, uint32_t v) {
    out[at] = uint8_t(v >> 24), out[at + 1] = uint8_t(v >> 16), out[at + 2] = uint8_t(v >> 8), out[at + 3] = uint8_t(v);
}

constexpr size_t headerBytes = 78;

// Days from 1904-01-01 to the given date (proleptic Gregorian).
long DaysFrom1904(int year, int month, int day) {
    // Civil-from-days algorithm, counted from 1970 and shifted.
    year -= month <= 2;
    const long era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = unsigned(year - era * 400);
    const unsigned doy = unsigned((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + long(doe) - 719468 + 24107;  // 24107 days from 1904 to 1970
}
}  // namespace

std::string FourCc(uint32_t code) {
    std::string text(4, ' ');
    for (int i = 0; i < 4; ++i) {
        const char c = char(code >> (24 - 8 * i));
        text[size_t(i)] = c >= 32 && c < 127 ? c : '?';
    }
    return text;
}

uint32_t FourCc(const char* text) {
    uint32_t code = 0;
    for (int i = 0; i < 4; ++i) code = code << 8 | uint8_t(text[i] ? text[i] : ' ');
    return code;
}

uint32_t PalmTime(int year, int month, int day, int hour, int minute, int second) {
    if (year < 1904) return 0;
    return uint32_t(DaysFrom1904(year, month, day) * 86400L + hour * 3600L + minute * 60L + second);
}

void PalmTimeParts(uint32_t seconds, int& year, int& month, int& day, int& hour, int& minute, int& second) {
    long days = long(seconds / 86400) - 24107 + 719468;
    const unsigned rest = seconds % 86400;
    hour = int(rest / 3600), minute = int(rest / 60 % 60), second = int(rest % 60);
    const long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = unsigned(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = int(doy - (153 * mp + 2) / 5 + 1);
    month = int(mp < 10 ? mp + 3 : mp - 9);
    year = int(long(yoe) + era * 400 + (month <= 2));
}

bool PalmDb::Write(Bytes& out, std::string* error) const {
    if (name.size() > 31 || records.size() > 0xffff) {
        if (error) *error = name.size() > 31 ? "Database name is over 31 bytes." : "Too many records.";
        return false;
    }
    const bool resource = Resource();
    out.clear();
    out.resize(32, 0);
    std::memcpy(out.data(), name.data(), name.size());
    Put16(out, attributes & ~dbOpen);
    Put16(out, version);
    Put32(out, created), Put32(out, modified), Put32(out, backedUp), Put32(out, modNumber);
    const size_t offsets = out.size();
    Put32(out, 0), Put32(out, 0);  // app info and sort info offsets, set below
    Put32(out, type), Put32(out, creator), Put32(out, uniqueIdSeed);
    Put32(out, 0);  // next record list
    Put16(out, uint16_t(records.size()));
    const size_t entryBytes = resource ? 10 : 8;
    size_t at = headerBytes + entryBytes * records.size() + 2;
    const size_t list = out.size();
    out.resize(out.size() + entryBytes * records.size() + 2, 0);  // two bytes of filler, as Palm writes them
    if (!appInfo.empty()) Set32(out, offsets, uint32_t(at)), at += appInfo.size();
    if (!sortInfo.empty()) Set32(out, offsets + 4, uint32_t(at)), at += sortInfo.size();
    out.insert(out.end(), appInfo.begin(), appInfo.end());
    out.insert(out.end(), sortInfo.begin(), sortInfo.end());
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        uint8_t* e = &out[list + entryBytes * i];
        const uint32_t offset = uint32_t(at);
        if (resource) {
            for (int b = 0; b < 4; ++b) e[b] = uint8_t(r.type >> (24 - 8 * b));
            e[4] = uint8_t(r.id >> 8), e[5] = uint8_t(r.id);
            for (int b = 0; b < 4; ++b) e[6 + b] = uint8_t(offset >> (24 - 8 * b));
        } else {
            for (int b = 0; b < 4; ++b) e[b] = uint8_t(offset >> (24 - 8 * b));
            e[4] = r.attributes;
            e[5] = uint8_t(r.uid >> 16), e[6] = uint8_t(r.uid >> 8), e[7] = uint8_t(r.uid);
        }
        at += r.data.size();
    }
    for (const auto& r : records) out.insert(out.end(), r.data.begin(), r.data.end());
    return true;
}

bool PalmDb::Read(const uint8_t* data, size_t size, PalmDb& db, std::string& error) {
    db = {};
    if (size < headerBytes) {
        error = "Not a PDB or PRC file: too short.";
        return false;
    }
    db.name.assign(reinterpret_cast<const char*>(data), strnlen(reinterpret_cast<const char*>(data), 32));
    if (db.name.empty() || db.name.size() > 31) {
        error = "Not a PDB or PRC file: no database name.";
        return false;
    }
    db.attributes = Get16(data + 32);
    db.version = Get16(data + 34);
    db.created = Get32(data + 36), db.modified = Get32(data + 40), db.backedUp = Get32(data + 44);
    db.modNumber = Get32(data + 48);
    const uint32_t appInfo = Get32(data + 52), sortInfo = Get32(data + 56);
    db.type = Get32(data + 60), db.creator = Get32(data + 64), db.uniqueIdSeed = Get32(data + 68);
    const size_t count = Get16(data + 76);
    const bool resource = db.Resource();
    const size_t entryBytes = resource ? 10 : 8;
    if (headerBytes + count * entryBytes > size) {
        error = "Not a PDB or PRC file: the record list runs past the end.";
        return false;
    }
    // Times before 1970 in the 1904 epoch look tiny; a file with a Unix-epoch time has it
    // under 2^31 with a creation date after 1970, so move those up.
    auto fix = [](uint32_t& t) {
        if (t && t < 0x80000000u) t += palmEpochOffset;
    };
    if (db.created && db.created < 0x80000000u) fix(db.created), fix(db.modified), fix(db.backedUp);
    std::vector<uint32_t> offsets(count);
    db.records.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* e = data + headerBytes + entryBytes * i;
        auto& r = db.records[i];
        if (resource) {
            r.type = Get32(e), r.id = Get16(e + 4), offsets[i] = Get32(e + 6);
        } else {
            offsets[i] = Get32(e), r.attributes = e[4], r.uid = uint32_t(e[5]) << 16 | uint32_t(e[6]) << 8 | e[7];
        }
    }
    // Each block runs to the next offset in file order; the first data offset ends the
    // info blocks.
    auto next = [&](uint32_t from) {
        uint32_t end = uint32_t(size);
        if (appInfo > from && appInfo < end) end = appInfo;
        if (sortInfo > from && sortInfo < end) end = sortInfo;
        for (auto o : offsets)
            if (o > from && o < end) end = o;
        return end;
    };
    auto block = [&](uint32_t from, Bytes& out) {
        if (!from) return true;
        if (from > size) return false;
        const uint32_t end = next(from);
        out.assign(data + from, data + end);
        return true;
    };
    if (!block(appInfo, db.appInfo) || !block(sortInfo, db.sortInfo)) {
        error = "Not a PDB or PRC file: an info block is past the end.";
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (offsets[i] > size) {
            error = "Not a PDB or PRC file: a record is past the end.";
            return false;
        }
        // Equal offsets are empty records; the next larger offset ends this one.
        const uint32_t end = next(offsets[i]);
        db.records[i].data.assign(data + offsets[i], data + end);
    }
    return true;
}

}  // namespace frondSync
