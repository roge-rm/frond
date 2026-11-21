#include "PimRecord.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace frondSync {
namespace {

constexpr std::array<const char*, 4> kinds{"contacts", "calendar", "tasks", "memos"};
constexpr std::array<const char*, 4> actions{"sync", "desktop-overwrites", "handheld-overwrites", "skip"};
constexpr std::array<const char*, 19> classicContact{
    "lastName", "firstName", "company", "phone1", "phone2", "phone3", "phone4", "phone5", "street", "city",
    "state", "postalCode", "country", "title", "custom1", "custom2", "custom3", "custom4", "note"};
constexpr std::array<const char*, 39> garnetContact{
    "lastName", "firstName", "company", "title", "phone1", "phone2", "phone3", "phone4", "phone5", "phone6",
    "phone7", "chat1", "chat2", "webpage", "custom1", "custom2", "custom3", "custom4", "custom5", "custom6",
    "custom7", "custom8", "custom9", "street", "city", "state", "postalCode", "country", "street2", "city2",
    "state2", "postalCode2", "country2", "street3", "city3", "state3", "postalCode3", "country3", "note"};

// Undefined Windows-1252 entries keep their byte value so they can round trip.
constexpr std::array<uint16_t, 32> palmText{
    0x20ac, 0x81, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160, 0x2039, 0x152, 0x8d,
    0x17d, 0x8f, 0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a,
    0x153, 0x9d, 0x17e, 0x178};

void Put(Bytes& out, uint32_t value, unsigned width) {
    while (width) out.push_back(uint8_t(value >> (8 * --width)));
}

struct Reader {
    const Bytes& data;
    size_t pos{0};

    void Require(size_t size) const {
        if (size > data.size() - pos) throw std::runtime_error("Truncated Palm record");
    }
    Bytes Take(size_t size) {
        Require(size);
        const auto start = data.begin() + pos;
        pos += size;
        return {start, start + size};
    }
    uint32_t Read(unsigned size) {
        Require(size);
        uint32_t value = 0;
        while (size--) value = (value << 8) | data[pos++];
        return value;
    }
    std::string Text() {
        const auto end = std::find(data.begin() + pos, data.end(), 0);
        if (end == data.end()) throw std::runtime_error("Unterminated Palm text");
        auto text = PalmPimText(Take(size_t(end - (data.begin() + pos))));
        ++pos;
        return text;
    }
};

std::string Field(const PimRecord& r, const char* key) {
    auto it = r.fields.find(key);
    return it == r.fields.end() ? "" : it->second;
}

unsigned Number(std::string_view s, unsigned max) {
    unsigned n = 0;
    auto end = s.data() + s.size();
    const auto conversion = std::from_chars(s.data(), end, n);
    if (conversion.ec != std::errc() || conversion.ptr != end || n > max)
        throw std::runtime_error("Invalid Palm field value");
    return n;
}

unsigned Number(const PimRecord& r, const char* key, unsigned max, unsigned fallback = 0) {
    auto it = r.fields.find(key);
    return it == r.fields.end() ? fallback : Number(it->second, max);
}

void PutText(Bytes& bytes, const std::string& text) {
    Bytes encoded;
    if (!EncodePalmPimText(text, encoded)) throw std::runtime_error("Some text cannot be represented on this Palm");
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    bytes.push_back(0);
}

void NumberField(PimRecord& r, const char* key, uint32_t value) {
    r.fields[key] = std::to_string(value);
}

std::string Date(uint32_t d) {
    if (d == 0xffff) return {};
    char text[16];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u", 1904 + (d >> 9), (d >> 5) & 15, d & 31);
    return text;
}
unsigned Date(std::string_view s) {
    if (s.empty()) return 0xffff;
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') throw std::runtime_error("Invalid Palm date");
    const unsigned year = Number(s.substr(0, 4), 2031);
    const unsigned month = Number(s.substr(5, 2), 12);
    const unsigned day = Number(s.substr(8, 2), 31);
    if (year < 1904 || !month || !day) throw std::runtime_error("Date outside the Palm range");
    return ((year - 1904) << 9) | (month << 5) | day;
}
std::string Clock(uint32_t t) {
    if (t == 0xffff) return {};
    char text[16];
    std::snprintf(text, sizeof(text), "%02u:%02u", t >> 8, t & 255);
    return text;
}
unsigned Clock(std::string_view s) {
    if (s.empty()) return 0xffff;
    if (s.size() != 5 || s[2] != ':') throw std::runtime_error("Invalid Palm time");
    return (Number(s.substr(0, 2), 23) << 8) | Number(s.substr(3, 2), 59);
}

void Append(Bytes& out, const Bytes& data) {
    out.insert(out.end(), data.begin(), data.end());
}

void AppendExtra(Bytes& out, const PimRecord& r, const std::string& key) {
    auto it = r.extras.find(key);
    if (it != r.extras.end()) Append(out, it->second);
}

const std::string& Prefix(PalmPimFormat format) {
    static const std::string classic{"palm-classic."}, garnet{"palm-garnet."};
    return format == PalmPimFormat::classic ? classic : garnet;
}

void DecodeContact(Reader& in, bool garnet, PimRecord& r) {
    uint32_t options = in.Read(4);
    for (unsigned i = 0; i < (garnet ? 7u : 5u); ++i) {
        NumberField(r, ("phoneLabel" + std::to_string(i + 1)).c_str(), (options >> (4 * i)) & 15);
    }
    NumberField(r, "displayPhone", (options >> (garnet ? 28 : 20)) & 15);
    NumberField(r, "contactOptions", options >> 24);
    if (garnet) {
        NumberField(r, "addressOptions", in.Read(2));
        NumberField(r, "chatOptions", in.Read(2));
        r.fields.erase("contactOptions");
    }
    const uint32_t flags = in.Read(4);
    const uint32_t more = garnet ? in.Read(4) : 0;
    in.Read(1);  // Company offset, recomputed after edits.
    if (garnet) {
        // The second flag word starts at the second address (field 28).
        for (unsigned i = 0; i < garnetContact.size(); ++i) {
            if (i < 28 ? flags & (1u << i) : more & (1u << (i - 28)))
                r.fields[garnetContact[i]] = in.Text();
        }
        if (more & 0x0800) r.fields["birthday"] = Date(in.Read(2));
        if (more & 0x1000) NumberField(r, "birthdayOptions", in.Read(2));
        if (more & 0x2000) NumberField(r, "birthdayReminder", in.Read(1));
        if ((flags & 0xf0000000) || (more & ~0x3fffu))
            r.extras["palm-garnet.contactFlags"] = {uint8_t(flags >> 24 & 0xf0), uint8_t(more >> 24),
                                                   uint8_t(more >> 16), uint8_t(more >> 8 & 0xc0)};
    } else {
        for (unsigned i = 0; i < classicContact.size(); ++i) {
            if (flags & (1u << i)) r.fields[classicContact[i]] = in.Text();
        }
        if (flags & ~0x7ffffu) r.extras["palm-classic.contactFlags"] = {uint8_t(flags >> 24), uint8_t(flags >> 16 & 0xf8)};
    }
}

Bytes EncodeContact(const PimRecord& r, bool garnet) {
    Bytes out, strings;
    uint32_t options = Number(r, "displayPhone", 15) << (garnet ? 28 : 20);
    for (unsigned i = 0; i < (garnet ? 7u : 5u); ++i) {
        options |= Number(r, ("phoneLabel" + std::to_string(i + 1)).c_str(), 15) << (i * 4);
    }
    if (!garnet) options |= Number(r, "contactOptions", 255) << 24;
    Put(out, options, 4);
    if (garnet) {
        Put(out, Number(r, "addressOptions", 65535), 2);
        Put(out, Number(r, "chatOptions", 65535), 2);
    }
    uint64_t flags = 0;
    uint8_t company = 0;
    const auto pack = [&](const auto& keys) {
        for (unsigned i = 0; i < keys.size(); ++i) {
            auto it = r.fields.find(keys[i]);
            if (it == r.fields.end()) continue;
            if (i == 2) {
                if (strings.size() >= 255) throw std::runtime_error("Palm contact name is too long");
                company = uint8_t(strings.size() + 1);
            }
            flags |= uint64_t(1) << (garnet && i >= 28 ? i + 4 : i);
            PutText(strings, it->second);
        }
    };
    if (garnet) {
        pack(garnetContact);
        if (r.fields.count("birthday")) {
            flags |= uint64_t(1) << 43;
            Put(strings, Date(Field(r, "birthday")), 2);
        }
        if (r.fields.count("birthdayOptions")) {
            flags |= uint64_t(1) << 44;
            Put(strings, Number(r, "birthdayOptions", 65535), 2);
        }
        if (r.fields.count("birthdayReminder")) {
            flags |= uint64_t(1) << 45;
            Put(strings, Number(r, "birthdayReminder", 255), 1);
        }
        auto unknown = r.extras.find("palm-garnet.contactFlags");
        if (unknown != r.extras.end() && unknown->second.size() == 4) {
            flags |= uint64_t(unknown->second[0] & 0xf0) << 24 | uint64_t(unknown->second[1]) << 56 |
                     uint64_t(unknown->second[2]) << 48 | uint64_t(unknown->second[3] & 0xc0) << 40;
        }
    } else {
        pack(classicContact);
        auto unknown = r.extras.find("palm-classic.contactFlags");
        if (unknown != r.extras.end() && unknown->second.size() == 2) {
            flags |= uint32_t(unknown->second[0]) << 24 | uint32_t(unknown->second[1] & 0xf8) << 16;
        }
    }
    Put(out, uint32_t(flags), 4);
    if (garnet) Put(out, uint32_t(flags >> 32), 4);
    Put(out, company, 1);
    Append(out, strings);
    return out;
}

void DecodeRepeat(Reader& in, PimRecord& r, const std::string& prefix) {
    NumberField(r, "repeatType", in.Read(1));
    const uint8_t reserved1 = uint8_t(in.Read(1));
    r.fields["repeatUntil"] = Date(in.Read(2));
    NumberField(r, "repeatInterval", in.Read(1));
    NumberField(r, "repeatOn", in.Read(1));
    NumberField(r, "repeatWeekStart", in.Read(1));
    const uint8_t reserved2 = uint8_t(in.Read(1));
    if (reserved1 || reserved2) r.extras[prefix + "repeatReserved"] = {reserved1, reserved2};
}

void EncodeRepeat(Bytes& bytes, const PimRecord& r, const std::string& prefix) {
    std::array<uint8_t, 2> reserved{};
    auto it = r.extras.find(prefix + "repeatReserved");
    if (it != r.extras.end() && it->second.size() == 2) std::copy_n(it->second.begin(), 2, reserved.begin());
    Put(bytes, Number(r, "repeatType", 255), 1);
    Put(bytes, reserved[0], 1);
    Put(bytes, Date(Field(r, "repeatUntil")), 2);
    Put(bytes, Number(r, "repeatInterval", 255, 1), 1);
    Put(bytes, Number(r, "repeatOn", 255), 1);
    Put(bytes, Number(r, "repeatWeekStart", 255), 1);
    Put(bytes, reserved[1], 1);
}

void DecodeCalendar(Reader& in, bool garnet, const std::string& prefix, PimRecord& r) {
    r.fields["startTime"] = Clock(in.Read(2));
    r.fields["endTime"] = Clock(in.Read(2));
    r.fields["date"] = Date(in.Read(2));
    const uint8_t flags = uint8_t(in.Read(1));
    const uint8_t reserved = uint8_t(in.Read(1));
    const uint8_t unknown = flags & (garnet ? 0x81 : 0x83);
    if (unknown || reserved) r.extras[prefix + "calendarFlags"] = {unknown, reserved};
    if (flags & 0x40) {
        NumberField(r, "alarmAdvance", in.Read(1));
        NumberField(r, "alarmUnit", in.Read(1));
    }
    if (flags & 0x20) DecodeRepeat(in, r, prefix);
    if (flags & 0x08) {
        const size_t count = in.Read(2);
        in.Require(count * 2);
        std::string dates;
        for (size_t i = 0; i < count; ++i) {
            if (i) dates.push_back(',');
            dates += Date(in.Read(2));
        }
        r.fields["exceptions"] = dates;
    }
    if (flags & 0x04) r.fields["summary"] = in.Text();
    if (flags & 0x10) r.fields["note"] = in.Text();
    if (garnet && (flags & 0x02)) r.fields["location"] = in.Text();
}

Bytes EncodeCalendar(const PimRecord& r, bool garnet, const std::string& prefix) {
    Bytes out, optional;
    Put(out, Clock(Field(r, "startTime")), 2);
    Put(out, Clock(Field(r, "endTime")), 2);
    Put(out, Date(Field(r, "date")), 2);
    uint8_t flags = 0, reserved = 0;
    auto unknown = r.extras.find(prefix + "calendarFlags");
    if (unknown != r.extras.end() && unknown->second.size() == 2) {
        flags = unknown->second[0] & (garnet ? 0x81 : 0x83);
        reserved = unknown->second[1];
    }
    if (r.fields.count("alarmAdvance")) {
        flags |= 0x40;
        Put(optional, Number(r, "alarmAdvance", 255), 1);
        Put(optional, Number(r, "alarmUnit", 255), 1);
    }
    if (r.fields.count("repeatType")) {
        flags |= 0x20;
        EncodeRepeat(optional, r, prefix);
    }
    if (r.fields.count("exceptions")) {
        flags |= 0x08;
        std::string dates = Field(r, "exceptions");
        const size_t count = dates.empty() ? 0 : 1 + std::count(dates.begin(), dates.end(), ',');
        if (count > 65535) throw std::runtime_error("Too many Palm calendar exceptions");
        Put(optional, uint32_t(count), 2);
        std::string_view dateList = dates;
        for (size_t i = 0; i < count; ++i) {
            const size_t comma = dateList.find(',');
            Put(optional, Date(dateList.substr(0, comma)), 2);
            dateList.remove_prefix(comma == std::string_view::npos ? dateList.size() : comma + 1);
        }
    }
    const auto text = [&](const char* field, uint8_t bit) {
        auto it = r.fields.find(field);
        if (it == r.fields.end()) return;
        flags |= bit;
        PutText(optional, it->second);
    };
    text("summary", 0x04);
    text("note", 0x10);
    if (garnet) text("location", 0x02);
    Put(out, flags, 1);
    Put(out, reserved, 1);
    Append(out, optional);
    return out;
}

void DecodeTasks(Reader& in, bool garnet, PimRecord& r) {
    if (!garnet) {
        r.fields["dueDate"] = Date(in.Read(2));
        const unsigned priority = in.Read(1);
        NumberField(r, "priority", priority & 127);
        NumberField(r, "complete", priority >> 7);
        r.fields["summary"] = in.Text();
        r.fields["note"] = in.Text();
        return;
    }
    const unsigned flags = in.Read(2);
    const unsigned attributes = in.Read(2);
    NumberField(r, "complete", attributes & 1);
    NumberField(r, "taskOptions", attributes & 0xfffe);
    NumberField(r, "priority", in.Read(2));
    if (flags & 0x8000) r.fields["dueDate"] = Date(in.Read(2));
    if (flags & 0x4000) r.fields["completionDate"] = Date(in.Read(2));
    if (flags & 0x2000) {
        r.fields["alarmTime"] = Clock(in.Read(2));
        NumberField(r, "alarmDays", in.Read(2));
    }
    if (flags & 0x1000) {
        r.fields["repeatStart"] = Date(in.Read(2));
        DecodeRepeat(in, r, "palm-garnet.");
    }
    // Garnet keeps both terminating NULs even if the corresponding data flag is clear.
    for (const auto& field : {std::pair{"summary", 0x0800}, std::pair{"note", 0x0400}}) {
        if (in.pos == in.data.size() && !(flags & field.second)) continue;
        const auto value = in.Text();
        if ((flags & field.second) || !value.empty()) r.fields[field.first] = value;
    }
    if (flags & 0x03ff) r.extras["palm-garnet.taskFlags"] = {uint8_t(flags >> 8 & 3), uint8_t(flags)};
}

Bytes EncodeTasks(const PimRecord& r, bool garnet) {
    Bytes out, optional;
    const unsigned complete = Number(r, "complete", 1);
    if (!garnet) {
        Put(out, Date(Field(r, "dueDate")), 2);
        Put(out, complete << 7 | Number(r, "priority", 127, 1), 1);
    } else {
        unsigned flags = 0;
        auto unknown = r.extras.find("palm-garnet.taskFlags");
        if (unknown != r.extras.end() && unknown->second.size() == 2)
            flags = ((unknown->second[0] & 3) << 8) | unknown->second[1];
        if (!Field(r, "dueDate").empty()) {
            flags |= 0x8000;
            Put(optional, Date(Field(r, "dueDate")), 2);
        }
        if (!Field(r, "completionDate").empty()) {
            flags |= 0x4000;
            Put(optional, Date(Field(r, "completionDate")), 2);
        }
        if (r.fields.count("alarmTime")) {
            flags |= 0x2000;
            Put(optional, Clock(Field(r, "alarmTime")), 2);
            Put(optional, Number(r, "alarmDays", 65535), 2);
        }
        if (r.fields.count("repeatType")) {
            flags |= 0x1000;
            Put(optional, Date(Field(r, "repeatStart")), 2);
            EncodeRepeat(optional, r, "palm-garnet.");
        }
        if (r.fields.count("summary")) flags |= 0x0800;
        if (r.fields.count("note")) flags |= 0x0400;
        Put(out, flags, 2);
        Put(out, (Number(r, "taskOptions", 65535) & 0xfffe) | complete, 2);
        Put(out, Number(r, "priority", 65535, 1), 2);
        Append(out, optional);
    }
    PutText(out, Field(r, "summary"));
    PutText(out, Field(r, "note"));
    return out;
}

}  // namespace

const char* PimKindName(PimKind kind) { return kinds.at(size_t(kind)); }
const char* PimActionName(PimAction action) { return actions.at(size_t(action)); }

bool ParsePimKind(const std::string& name, PimKind& kind) {
    auto it = std::find(kinds.begin(), kinds.end(), name);
    if (it == kinds.end()) return false;
    kind = PimKind(it - kinds.begin());
    return true;
}

bool ParsePimAction(const std::string& name, PimAction& action) {
    auto it = std::find(actions.begin(), actions.end(), name);
    if (it == actions.end()) return false;
    action = PimAction(it - actions.begin());
    return true;
}

std::string PalmPimText(const Bytes& raw) {
    std::string text;
    for (uint16_t c : raw) {
        if (c >= 0x80 && c <= 0x9f) c = palmText[c - 0x80];
        if (c >= 0x800) text.push_back(char(0xe0 | (c >> 12)));
        if (c >= 0x80) {
            text.push_back(char((c >= 0x800 ? 0x80 : 0xc0) | ((c >> 6) & 0x3f)));
            text.push_back(char(0x80 | (c & 0x3f)));
        } else text.push_back(char(c));
    }
    return text;
}

bool EncodePalmPimText(const std::string& utf8, Bytes& out) {
    Bytes encoded;
    for (size_t i = 0; i < utf8.size();) {
        const uint8_t first = uint8_t(utf8[i++]);
        unsigned c = first, count = 0;
        if (!first || first >= 0xf0 || (first >= 0x80 && first < 0xc2)) return false;
        if (first >= 0xc2) {
            count = first >= 0xe0 ? 2 : 1;
            c &= count == 2 ? 0x0f : 0x1f;
            if (count > utf8.size() - i) return false;
            for (unsigned n = 0; n < count; ++n) {
                const uint8_t next = uint8_t(utf8[i++]);
                if ((next & 0xc0) != 0x80) return false;
                c = (c << 6) | (next & 0x3f);
            }
            if (count == 2 && c < 0x800) return false;
        }
        if (c < 0x80 || (c >= 0xa0 && c < 0x100)) encoded.push_back(uint8_t(c));
        else {
            const auto it = std::find(palmText.begin(), palmText.end(), c);
            if (it == palmText.end()) return false;
            encoded.push_back(uint8_t(0x80 + (it - palmText.begin())));
        }
    }
    out = std::move(encoded);
    return true;
}

bool DecodePalmPim(PimKind kind, PalmPimFormat format, const Bytes& data, PimRecord& out, std::string& error) {
    try {
        const bool garnet = format == PalmPimFormat::garnet;
        const auto& prefix = Prefix(format);
        PimRecord r;
        r.kind = kind;
        Reader in{data};
        switch (kind) {
            case PimKind::contacts: DecodeContact(in, garnet, r); break;
            case PimKind::calendar: DecodeCalendar(in, garnet, prefix, r); break;
            case PimKind::tasks: DecodeTasks(in, garnet, r); break;
            case PimKind::memos: r.fields["text"] = in.Text(); break;
        }
        if (in.pos < data.size()) r.extras[prefix + "tail"] = in.Take(data.size() - in.pos);
        out = std::move(r);
        return true;
    } catch (const std::runtime_error& e) {
        error = e.what();
        return false;
    }
}

bool EncodePalmPim(const PimRecord& r, PalmPimFormat format, Bytes& out, std::string& error) {
    try {
        const bool garnet = format == PalmPimFormat::garnet;
        const auto& prefix = Prefix(format);
        Bytes bytes;
        switch (r.kind) {
            case PimKind::contacts: bytes = EncodeContact(r, garnet); break;
            case PimKind::calendar: bytes = EncodeCalendar(r, garnet, prefix); break;
            case PimKind::tasks: bytes = EncodeTasks(r, garnet); break;
            case PimKind::memos: PutText(bytes, Field(r, "text")); break;
        }
        AppendExtra(bytes, r, prefix + "tail");
        if (bytes.size() > 0xf000) throw std::runtime_error("Palm record is too large for HotSync");
        out = std::move(bytes);
        return true;
    } catch (const std::runtime_error& e) {
        error = e.what();
        return false;
    }
}

void ApplyPalmPim(const PimRecord& decoded, PalmPimFormat format, PimRecord& record) {
    const bool garnet = format == PalmPimFormat::garnet;
    const auto remove = [&](const auto& keys) {
        for (const auto& key : keys) record.fields.erase(key);
    };
    switch (decoded.kind) {
        case PimKind::contacts:
            if (garnet) {
                remove(garnetContact);
                remove(std::array{"phoneLabel6", "phoneLabel7", "addressOptions", "chatOptions", "birthday",
                                  "birthdayOptions", "birthdayReminder"});
            } else {
                remove(classicContact);
                record.fields.erase("contactOptions");
            }
            remove(std::array{"phoneLabel1", "phoneLabel2", "phoneLabel3", "phoneLabel4", "phoneLabel5", "displayPhone"});
            break;
        case PimKind::calendar:
            if (garnet) record.fields.erase("location");
            remove(std::array{"startTime", "endTime", "date", "alarmAdvance", "alarmUnit", "repeatType", "repeatUntil",
                              "repeatInterval", "repeatOn", "repeatWeekStart", "exceptions", "summary", "note"});
            break;
        case PimKind::tasks:
            if (garnet)
                remove(std::array{"taskOptions", "completionDate", "alarmTime", "alarmDays", "repeatStart", "repeatType",
                                  "repeatUntil", "repeatInterval", "repeatOn", "repeatWeekStart"});
            remove(std::array{"dueDate", "priority", "complete", "summary", "note"});
            break;
        case PimKind::memos:
            record.fields.erase("text");
            break;
    }
    const auto& prefix = Prefix(format);
    for (auto it = record.extras.begin(); it != record.extras.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) it = record.extras.erase(it);
        else ++it;
    }
    for (const auto& field : decoded.fields) record.fields.insert_or_assign(field.first, field.second);
    record.extras.insert(decoded.extras.begin(), decoded.extras.end());
    record.category = decoded.category;
    record.secret = decoded.secret;
}

}  // namespace frondSync
