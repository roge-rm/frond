#include "AirSync.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <set>
#include <tuple>
#include <stdexcept>

#include "Sha256.h"
#include "Wbxml.h"

namespace frondSync {
namespace {

using Clock = std::chrono::steady_clock;

// MS-ASWBXML code pages and the tokens used here.
constexpr uint8_t pageAirSync = 0, pageContacts = 1, pageCalendar = 4, pageEstimate = 6, pageFolders = 7,
                  pageTasks = 9, pageContacts2 = 12;
namespace as {
constexpr uint8_t Sync = 0x05, Responses = 0x06, Add = 0x07, Change = 0x08, Delete = 0x09, Fetch = 0x0a,
                  SyncKey = 0x0b, ClientId = 0x0c, ServerId = 0x0d, Status = 0x0e, Collection = 0x0f, Class = 0x10,
                  CollectionId = 0x12, GetChanges = 0x13, MoreAvailable = 0x14, WindowSize = 0x15, Commands = 0x16,
                  Collections = 0x1c, ApplicationData = 0x1d;
}
namespace fh {
constexpr uint8_t DisplayName = 0x07, ServerId = 0x08, ParentId = 0x09, Type = 0x0a, Status = 0x0c, Changes = 0x0e,
                  Add = 0x0f, SyncKey = 0x12, FolderSync = 0x16, Count = 0x17;
}
namespace ge {
constexpr uint8_t GetItemEstimate = 0x05, Collections = 0x07, Collection = 0x08, Class = 0x09, CollectionId = 0x0a,
                  Estimate = 0x0c, Response = 0x0d, Status = 0x0e;
}
namespace ct {
constexpr uint8_t Birthday = 0x08, Body = 0x09, BodyTruncated = 0x0b, Business2Phone = 0x0c, BusinessCity = 0x0d,
                  BusinessCountry = 0x0e, BusinessPostalCode = 0x0f, BusinessState = 0x10, BusinessStreet = 0x11,
                  BusinessFax = 0x12, BusinessPhone = 0x13, CarPhone = 0x14, Categories = 0x15, Category = 0x16,
                  CompanyName = 0x19, Email1 = 0x1b, Email2 = 0x1c, Email3 = 0x1d, FileAs = 0x1e, FirstName = 0x1f,
                  Home2Phone = 0x20, HomeCity = 0x21, HomeCountry = 0x22, HomePostalCode = 0x23, HomeState = 0x24,
                  HomeStreet = 0x25, HomeFax = 0x26, HomePhone = 0x27, JobTitle = 0x28, LastName = 0x29,
                  MobilePhone = 0x2b, OtherCity = 0x2d, OtherCountry = 0x2e, OtherPostalCode = 0x2f, OtherState = 0x30,
                  OtherStreet = 0x31, Pager = 0x32, RadioPhone = 0x33, WebPage = 0x37;
constexpr uint8_t Anniversary = 0x05, AssistantName = 0x06, AssistantPhone = 0x07, Children = 0x17, Child = 0x18,
                  Department = 0x1a, MiddleName = 0x2a, OfficeLocation = 0x2c, Spouse = 0x34, Suffix = 0x35,
                  Title = 0x36, YomiCompanyName = 0x38, YomiFirstName = 0x39, YomiLastName = 0x3a;
// Contacts2
constexpr uint8_t CustomerId = 0x05, GovernmentId = 0x06, IMAddress = 0x07, IMAddress2 = 0x08, IMAddress3 = 0x09,
                  ManagerName = 0x0a, CompanyMainPhone = 0x0b, AccountName = 0x0c, NickName = 0x0d, MMS = 0x0e;
}
namespace cal {
constexpr uint8_t TimeZone = 0x05, AllDayEvent = 0x06, Body = 0x0b, BodyTruncated = 0x0c, BusyStatus = 0x0d,
                  Categories = 0x0e, Category = 0x0f, DtStamp = 0x11, EndTime = 0x12, Exception = 0x13, Exceptions = 0x14,
                  Deleted = 0x15, ExceptionStartTime = 0x16, Location = 0x17, MeetingStatus = 0x18, Recurrence = 0x1b,
                  Type = 0x1c, Until = 0x1d, Occurrences = 0x1e, Interval = 0x1f, DayOfWeek = 0x20, DayOfMonth = 0x21,
                  WeekOfMonth = 0x22, MonthOfYear = 0x23, Reminder = 0x24, Sensitivity = 0x25, Subject = 0x26,
                  StartTime = 0x27, UID = 0x28;
}
namespace tk {
constexpr uint8_t Body = 0x05, BodyTruncated = 0x07, Categories = 0x08, Category = 0x09, Complete = 0x0a,
                  DateCompleted = 0x0b, DueDate = 0x0c, UtcDueDate = 0x0d, Importance = 0x0e, Sensitivity = 0x1d,
                  Subject = 0x20;
}

struct FolderInfo {
    PimKind kind;
    const char* id;
    const char* className;
    const char* name;
    const char* type;  // MS-ASCMD default folder types
};
constexpr std::array<FolderInfo, 3> folderInfo{{
    {PimKind::contacts, "1", "Contacts", "Contacts", "9"},
    {PimKind::calendar, "2", "Calendar", "Calendar", "8"},
    {PimKind::tasks, "3", "Tasks", "Tasks", "7"},
}};

const FolderInfo* FindFolder(const std::string& id, const std::string& className) {
    for (const auto& folder : folderInfo)
        if (id == folder.id || (id.empty() && className == folder.className)) return &folder;
    return nullptr;
}
// The folder if this server offers it.
const FolderInfo* OfferedFolder(const std::string& id, const std::string& className, const std::vector<PimKind>& kinds) {
    const auto* folder = FindFolder(id, className);
    return folder && std::find(kinds.begin(), kinds.end(), folder->kind) != kinds.end() ? folder : nullptr;
}

std::string Field(const PimRecord& r, const std::string& key) {
    auto it = r.fields.find(key);
    return it == r.fields.end() ? std::string() : it->second;
}

int Number(const std::string& text, int fallback = 0) {
    if (text.empty()) return fallback;
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (!end || *end || errno || value < INT32_MIN || value > INT32_MAX) return fallback;
    return int(value);
}

// A SyncKey or a Content-Length: digits only, short enough for int64.
bool Digits(const std::string& text) {
    return !text.empty() && text.size() <= 15 && text.find_first_not_of("0123456789") == std::string::npos;
}

// Record ids kept in a device value, comma separated.
std::set<int64_t> ParseIds(const std::string& text) {
    std::set<int64_t> ids;
    size_t from = 0;
    while (from < text.size()) {
        const auto comma = std::min(text.find(',', from), text.size());
        const auto item = text.substr(from, comma - from);
        if (Digits(item)) ids.insert(std::stoll(item));
        from = comma + 1;
    }
    return ids;
}
std::string FormatIds(const std::set<int64_t>& ids) {
    std::string text;
    for (const auto id : ids) text += (text.empty() ? "" : ",") + std::to_string(id);
    return text;
}

// Times. Profile dates and clock times are local to the host, as Palm Desktop keeps them;
// AirSync times are UTC.
struct Civil {
    int year{0}, month{0}, day{0}, hour{0}, minute{0}, second{0};
};

bool ParseDate(const std::string& text, Civil& c) {
    return text.size() == 10 && std::sscanf(text.c_str(), "%4d-%2d-%2d", &c.year, &c.month, &c.day) == 3;
}
bool ParseClock(const std::string& text, Civil& c) {
    return text.size() == 5 && std::sscanf(text.c_str(), "%2d:%2d", &c.hour, &c.minute) == 2;
}
std::string FormatDate(const Civil& c) {
    char text[16];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d", c.year, c.month, c.day);
    return text;
}
std::string FormatClock(const Civil& c) {
    char text[8];
    std::snprintf(text, sizeof(text), "%02d:%02d", c.hour, c.minute);
    return text;
}

int64_t DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * unsigned(m + (m > 2 ? -3 : 9)) + 2) / 5 + unsigned(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}
Civil CivilFromSeconds(int64_t seconds) {
    int64_t days = seconds / 86400, rest = seconds % 86400;
    if (rest < 0) rest += 86400, --days;
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = unsigned(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    Civil c;
    c.day = int(doy - (153 * mp + 2) / 5 + 1);
    c.month = int(mp < 10 ? mp + 3 : mp - 9);
    c.year = int(int64_t(yoe) + era * 400 + (c.month <= 2));
    c.hour = int(rest / 3600);
    c.minute = int(rest / 60 % 60);
    c.second = int(rest % 60);
    return c;
}
int64_t UtcSeconds(const Civil& c) {
    return DaysFromCivil(c.year, c.month, c.day) * 86400 + c.hour * 3600 + c.minute * 60 + c.second;
}

int64_t HostToUtc(const Civil& c) {
    std::tm tm{};
    tm.tm_year = c.year - 1900;
    tm.tm_mon = c.month - 1;
    tm.tm_mday = c.day;
    tm.tm_hour = c.hour;
    tm.tm_min = c.minute;
    tm.tm_sec = c.second;
    tm.tm_isdst = -1;
    return int64_t(std::mktime(&tm));
}
Civil HostFromUtc(int64_t seconds) {
    const std::time_t t = std::time_t(seconds);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec};
}

std::string Compact(int64_t utc) {
    const auto c = CivilFromSeconds(utc);
    char text[24];
    std::snprintf(text, sizeof(text), "%04d%02d%02dT%02d%02d%02dZ", c.year, c.month, c.day, c.hour, c.minute, c.second);
    return text;
}
std::string Extended(const Civil& c) {
    char text[32];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.000Z", c.year, c.month, c.day, c.hour, c.minute,
                  c.second);
    return text;
}
// Both the compact and the extended ISO 8601 forms, as UTC.
bool ParseUtc(const std::string& text, int64_t& utc) {
    Civil c;
    if (std::sscanf(text.c_str(), "%4d%2d%2dT%2d%2d%2d", &c.year, &c.month, &c.day, &c.hour, &c.minute, &c.second) != 6 &&
        std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &c.year, &c.month, &c.day, &c.hour, &c.minute, &c.second) != 6)
        return false;
    if (c.month < 1 || c.month > 12 || c.day < 1 || c.day > 31) return false;
    utc = UtcSeconds(c);
    return true;
}

std::string Base64(const Bytes& data) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = uint32_t(data[i]) << 16;
        if (i + 1 < data.size()) n |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < data.size()) n |= data[i + 2];
        out += digits[n >> 18 & 63];
        out += digits[n >> 12 & 63];
        out += i + 1 < data.size() ? digits[n >> 6 & 63] : '=';
        out += i + 2 < data.size() ? digits[n & 63] : '=';
    }
    return out;
}

bool Unbase64(const std::string& text, Bytes& out) {
    out.clear();
    uint32_t bits = 0;
    int count = 0;
    for (const char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=' || c == '\r' || c == '\n') continue;
        else return false;
        bits = bits << 6 | uint32_t(v);
        if ((count += 6) >= 8) {
            count -= 8;
            out.push_back(uint8_t(bits >> count));
        }
    }
    return true;
}

// The device's wall clock. Profile times are wall-clock times, as on a Palm, so they convert
// with the device's TIME_ZONE_INFORMATION (172 bytes, as AirSync's TimeZone carries it), or
// with the host's zone until the device has given one.
class Zone {
   public:
    Zone() = default;
    explicit Zone(const std::string& base64) {
        Bytes b;
        if (!Unbase64(base64, b) || b.size() != 172) return;
        const auto i32 = [&](size_t at) { return int32_t(b[at] | b[at + 1] << 8 | b[at + 2] << 16 | uint32_t(b[at + 3]) << 24); };
        const auto rule = [&](size_t at) {
            const auto u16 = [&](size_t o) { return int(b[at + o] | b[at + o + 1] << 8); };
            return Rule{u16(0), u16(2), u16(4), u16(6), u16(8), u16(10)};
        };
        bias = i32(0);
        standardDate = rule(68);
        standardBias = i32(84);
        daylightDate = rule(152);
        daylightBias = i32(168);
        if (std::abs(bias) > 24 * 60 || standardDate.month > 12 || daylightDate.month > 12) return;
        blob = base64;
    }
    bool Valid() const { return !blob.empty(); }
    bool SameRules(const Zone& other) const {
        const auto same = [](const Rule& a, const Rule& b) {
            return a.year == b.year && a.month == b.month && a.dayOfWeek == b.dayOfWeek && a.day == b.day &&
                   a.hour == b.hour && a.minute == b.minute;
        };
        return Valid() && other.Valid() && bias == other.bias && standardBias == other.standardBias &&
               daylightBias == other.daylightBias && same(standardDate, other.standardDate) &&
               same(daylightDate, other.daylightDate);
    }
    // The TimeZone to send: the device's, or UTC with host-converted instants.
    std::string Blob() const { return Valid() ? blob : Base64(Bytes(172, 0)); }

    int64_t ToUtc(const Civil& c) const {
        if (!Valid()) return HostToUtc(c);
        const int64_t wall = UtcSeconds(c);
        return wall + int64_t(bias + (Daylight(wall, c.year) ? daylightBias : standardBias)) * 60;
    }
    Civil ToLocal(int64_t utc) const {
        if (!Valid()) return HostFromUtc(utc);
        const int64_t daylight = utc - int64_t(bias + daylightBias) * 60;
        if (standardDate.month && Daylight(daylight, CivilFromSeconds(daylight).year)) return CivilFromSeconds(daylight);
        return CivilFromSeconds(utc - int64_t(bias + standardBias) * 60);
    }

   private:
    struct Rule {
        int year, month, dayOfWeek, day, hour, minute;
    };
    // The rule's wall-clock time in a year: day is the week (5 is the last) unless year is set.
    static int64_t Transition(const Rule& r, int year) {
        if (r.year) return UtcSeconds({r.year, r.month, r.day, r.hour, r.minute, 0});
        const int64_t first = DaysFromCivil(year, r.month, 1);
        const int firstDay = int(((first + 4) % 7 + 7) % 7);  // 1970-01-01 was a Thursday
        const int length = int(DaysFromCivil(r.month == 12 ? year + 1 : year, r.month == 12 ? 1 : r.month + 1, 1) - first);
        int day = 1 + (r.dayOfWeek - firstDay + 7) % 7 + (r.day - 1) * 7;
        while (day > length) day -= 7;
        return (first + day - 1) * 86400 + r.hour * 3600 + r.minute * 60;
    }
    bool Daylight(int64_t wall, int year) const {
        if (!standardDate.month || !daylightDate.month) return false;
        const int64_t start = Transition(daylightDate, year), end = Transition(standardDate, year);
        return start < end ? wall >= start && wall < end : wall >= start || wall < end;
    }

    std::string blob;
    int bias{0}, standardBias{0}, daylightBias{0};
    Rule standardDate{}, daylightDate{};
};

void AddText(WbxmlNode& node, uint8_t page, uint8_t tag, const std::string& value) {
    if (!value.empty()) node.Add(page, tag, value);
}

void AddCategory(WbxmlNode& node, uint8_t page, uint8_t list, uint8_t item, const std::string& category) {
    if (!category.empty()) node.Add(page, list).Add(page, item, category);
}
std::string ReadCategory(const WbxmlNode& data, uint8_t page, uint8_t list, uint8_t item) {
    const auto* categories = data.Find(page, list);
    return categories ? categories->Text(page, item) : std::string();
}

std::string Strip(std::string text) {
    // Pocket Outlook sends e-mail as "Name" <address>.
    const auto open = text.rfind('<');
    if (open != std::string::npos && !text.empty() && text.back() == '>') text = text.substr(open + 1, text.size() - open - 2);
    return text;
}

// Contacts.
struct TextMap {
    const char* field;
    uint8_t page, tag;
};
// The fields after birthday have no place in the Palm formats; the profile keeps them for
// Windows Mobile.
constexpr std::array<TextMap, 43> contactText{{
    {"firstName", pageContacts, ct::FirstName}, {"lastName", pageContacts, ct::LastName},
    {"company", pageContacts, ct::CompanyName}, {"title", pageContacts, ct::JobTitle},
    {"webpage", pageContacts, ct::WebPage}, {"note", pageContacts, ct::Body},
    {"chat1", pageContacts2, ct::IMAddress}, {"chat2", pageContacts2, ct::IMAddress2},
    {"street", pageContacts, ct::BusinessStreet}, {"city", pageContacts, ct::BusinessCity},
    {"state", pageContacts, ct::BusinessState}, {"postalCode", pageContacts, ct::BusinessPostalCode},
    {"country", pageContacts, ct::BusinessCountry}, {"street2", pageContacts, ct::HomeStreet},
    {"city2", pageContacts, ct::HomeCity}, {"state2", pageContacts, ct::HomeState},
    {"postalCode2", pageContacts, ct::HomePostalCode}, {"country2", pageContacts, ct::HomeCountry},
    {"street3", pageContacts, ct::OtherStreet}, {"city3", pageContacts, ct::OtherCity},
    {"state3", pageContacts, ct::OtherState}, {"postalCode3", pageContacts, ct::OtherPostalCode},
    {"country3", pageContacts, ct::OtherCountry}, {"birthday", pageContacts, ct::Birthday},
    {"anniversary", pageContacts, ct::Anniversary}, {"department", pageContacts, ct::Department},
    {"officeLocation", pageContacts, ct::OfficeLocation}, {"middleName", pageContacts, ct::MiddleName},
    {"suffix", pageContacts, ct::Suffix}, {"nameTitle", pageContacts, ct::Title},
    {"spouse", pageContacts, ct::Spouse}, {"assistantName", pageContacts, ct::AssistantName},
    {"assistantPhone", pageContacts, ct::AssistantPhone}, {"yomiFirstName", pageContacts, ct::YomiFirstName},
    {"yomiLastName", pageContacts, ct::YomiLastName}, {"yomiCompany", pageContacts, ct::YomiCompanyName},
    {"chat3", pageContacts2, ct::IMAddress3}, {"managerName", pageContacts2, ct::ManagerName},
    {"nickname", pageContacts2, ct::NickName}, {"customerId", pageContacts2, ct::CustomerId},
    {"governmentId", pageContacts2, ct::GovernmentId}, {"accountName", pageContacts2, ct::AccountName},
    {"mms", pageContacts2, ct::MMS},
}};
bool IsDate(const TextMap& map) { return map.page == pageContacts && (map.tag == ct::Birthday || map.tag == ct::Anniversary); }
// Children is a list of Child; the profile keeps them in one field, comma separated.
constexpr const char* childSeparator = ", ";

using Tag = uint16_t;  // page << 8 | token
constexpr Tag T(uint8_t page, uint8_t tag) { return Tag(page << 8 | tag); }

// AirSync phone fields for each Palm phone label: work, home, fax, other, e-mail, main, pager, mobile.
const std::vector<Tag>& PhoneTags(int label) {
    static const std::array<std::vector<Tag>, 8> tags{{
        {T(pageContacts, ct::BusinessPhone), T(pageContacts, ct::Business2Phone)},
        {T(pageContacts, ct::HomePhone), T(pageContacts, ct::Home2Phone)},
        {T(pageContacts, ct::BusinessFax), T(pageContacts, ct::HomeFax)},
        {T(pageContacts, ct::RadioPhone), T(pageContacts, ct::CarPhone)},
        {T(pageContacts, ct::Email1), T(pageContacts, ct::Email2), T(pageContacts, ct::Email3)},
        {T(pageContacts2, ct::CompanyMainPhone)},
        {T(pageContacts, ct::Pager)},
        {T(pageContacts, ct::MobilePhone)},
    }};
    static const std::vector<Tag> none;
    return label >= 0 && label < 8 ? tags[size_t(label)] : none;
}
constexpr unsigned phoneSlots = 7;

// Slot (1-based) to AirSync field, in slot order, for the phones a record has.
std::map<unsigned, Tag> PhonePlaces(const PimRecord& r) {
    std::map<unsigned, Tag> places;
    std::set<Tag> used;
    for (unsigned slot = 1; slot <= phoneSlots; ++slot) {
        const auto n = std::to_string(slot);
        if (Field(r, "phone" + n).empty()) continue;
        const int fallback = slot <= 5 ? int(slot - 1) : 3;
        for (const auto tag : PhoneTags(Number(Field(r, "phoneLabel" + n), fallback))) {
            if (used.insert(tag).second) {
                places[slot] = tag;
                break;
            }
        }
    }
    return places;
}

// Birthdays and anniversaries go as the device's local midnight in UTC, as the device sends them.
WbxmlNode ContactData(const PimRecord& r, const Zone& zone) {
    WbxmlNode data(pageAirSync, as::ApplicationData);
    for (const auto& map : contactText) {
        auto value = Field(r, map.field);
        if (IsDate(map)) {
            Civil c;
            value = ParseDate(value, c) ? Extended(CivilFromSeconds(zone.ToUtc(c))) : std::string();
        }
        AddText(data, map.page, map.tag, value);
    }
    const auto last = Field(r, "lastName"), first = Field(r, "firstName");
    auto fileAs = !last.empty() && !first.empty() ? last + ", " + first : !last.empty() ? last : !first.empty() ? first
                                                                                                                : Field(r, "company");
    AddText(data, pageContacts, ct::FileAs, fileAs);
    for (const auto& [slot, tag] : PhonePlaces(r))
        data.Add(uint8_t(tag >> 8), uint8_t(tag), Field(r, "phone" + std::to_string(slot)));
    const auto children = Field(r, "children");
    if (!children.empty()) {
        auto& list = data.Add(pageContacts, ct::Children);
        size_t from = 0;
        for (;;) {
            const auto at = children.find(childSeparator, from);
            AddText(list, pageContacts, ct::Child, children.substr(from, at == std::string::npos ? at : at - from));
            if (at == std::string::npos) break;
            from = at + std::strlen(childSeparator);
        }
    }
    AddCategory(data, pageContacts, ct::Categories, ct::Category, r.category);
    return data;
}

void ApplyContact(const WbxmlNode& data, PimRecord& r, const Zone& zone) {
    const bool bodyTruncated = data.Text(pageContacts, ct::BodyTruncated) == "1";
    for (const auto& map : contactText) {
        if (map.tag == ct::Body && map.page == pageContacts && bodyTruncated) continue;
        auto value = data.Text(map.page, map.tag);
        if (IsDate(map) && !value.empty()) {
            int64_t utc;
            // Local midnight in UTC; the nearest local midnight gives the day.
            value = ParseUtc(value, utc) ? FormatDate(zone.ToLocal(utc + 12 * 3600)) : std::string();
        }
        if (value.empty()) r.fields.erase(map.field);
        else r.fields[map.field] = value;
    }
    // Phones the record had keep their slot and label; new ones take a free slot.
    std::map<Tag, std::string> phones;
    for (int label = 0; label < 8; ++label)
        for (const auto tag : PhoneTags(label)) {
            auto value = data.Text(uint8_t(tag >> 8), uint8_t(tag));
            if (label == 4) value = Strip(value);
            if (!value.empty()) phones[tag] = value;
        }
    for (const auto& [slot, tag] : PhonePlaces(r)) {
        const auto n = std::to_string(slot);
        auto it = phones.find(tag);
        if (it == phones.end()) {
            r.fields["phone" + n] = "";
        } else {
            r.fields["phone" + n] = it->second;
            phones.erase(it);
        }
    }
    for (int label = 0; label < 8 && !phones.empty(); ++label) {
        for (const auto tag : PhoneTags(label)) {
            auto it = phones.find(tag);
            if (it == phones.end()) continue;
            for (unsigned slot = 1; slot <= phoneSlots; ++slot) {
                const auto n = std::to_string(slot);
                if (!Field(r, "phone" + n).empty()) continue;
                r.fields["phone" + n] = it->second;
                r.fields["phoneLabel" + n] = std::to_string(label);
                break;
            }
            phones.erase(it);
        }
    }
    std::string children;
    if (const auto* list = data.Find(pageContacts, ct::Children))
        for (const auto& child : list->children)
            if (child.page == pageContacts && child.tag == ct::Child && !child.text.empty())
                children += (children.empty() ? "" : childSeparator) + child.text;
    if (children.empty()) r.fields.erase("children");
    else r.fields["children"] = children;
    r.category = ReadCategory(data, pageContacts, ct::Categories, ct::Category);
}

// Calendar.
// Palm repeats: 1 daily, 2 weekly (repeatOn a day mask, bit 0 Sunday), 3 monthly on the nth
// weekday (repeatOn week * 7 + day, week 4 the last), 4 monthly on a date, 5 yearly on a date.
// AirSync's yearly nth weekday (Type 6) is a Palm monthly nth weekday every 12 months.
int DayOfWeekOf(int64_t days) { return int(((days + 4) % 7 + 7) % 7); }  // 1970-01-01 was a Thursday
int MonthLength(int year, int month) {
    return int(DaysFromCivil(month == 12 ? year + 1 : year, month == 12 ? 1 : month + 1, 1) - DaysFromCivil(year, month, 1));
}

// Outlook's largest occurrence count.
constexpr int maxOccurrences = 999;

// The day of the nth occurrence (1 is the start) of a Palm repeat, or -1 if it can't be found.
int64_t NthOccurrence(int type, int interval, int on, const Civil& start, int n) {
    const int64_t first = DaysFromCivil(start.year, start.month, start.day);
    interval = std::min(std::max(1, interval), 1200);
    if (n < 1 || n > maxOccurrences) return -1;
    if (type == 1) return first + int64_t(n - 1) * interval;
    int count = 0;
    if (type == 2) {
        if (!(on & 0x7f)) return -1;
        // Every interval-th week from the start's week; at least one day a week matches.
        const int64_t weekStart = first - DayOfWeekOf(first);
        for (int64_t week = 0; week <= n; ++week)
            for (int dow = 0; dow < 7; ++dow) {
                const int64_t day = weekStart + week * interval * 7 + dow;
                if (day >= first && (on >> dow & 1) && ++count == n) return day;
            }
        return -1;
    }
    for (int64_t step = 0; step < 5000; ++step) {
        const int64_t months = type == 5 ? step * interval * 12 : step * interval;
        if (months > 12 * 10000) return -1;
        const int year = int(start.year + (start.month - 1 + months) / 12), month = int((start.month - 1 + months) % 12 + 1);
        int64_t day = -1;
        if (type == 3) {
            const int week = on / 7, dow = on % 7;
            const int64_t monthStart = DaysFromCivil(year, month, 1);
            int date = 1 + (dow - DayOfWeekOf(monthStart) + 7) % 7 + std::min(week, 4) * 7;
            while (date > MonthLength(year, month)) date -= 7;
            day = monthStart + date - 1;
        } else if (type == 4 || type == 5) {
            if (start.day <= MonthLength(year, month)) day = DaysFromCivil(year, month, start.day);
        } else {
            return -1;
        }
        if (day >= first && ++count == n) return day;
    }
    return -1;
}

void AddRecurrence(WbxmlNode& data, const PimRecord& r, const Civil& start, const Zone& zone) {
    const int type = Number(Field(r, "repeatType"));
    if (type < 1 || type > 5) return;
    auto& rec = data.Add(pageCalendar, cal::Recurrence);
    const int on = Number(Field(r, "repeatOn"));
    int interval = std::max(1, Number(Field(r, "repeatInterval"), 1));
    const bool yearlyNth = type == 3 && interval % 12 == 0;
    const char* asType = "0";
    if (type == 2) asType = "1";
    else if (yearlyNth) asType = "6", interval /= 12;
    else if (type == 3) asType = "3";
    else if (type == 4) asType = "2";
    else if (type == 5) asType = "5";
    // Schema order: Type, Occurrences, Interval, WeekOfMonth, DayOfWeek, MonthOfYear, Until, DayOfMonth.
    rec.Add(pageCalendar, cal::Type, asType);
    Civil until;
    const bool ends = ParseDate(Field(r, "repeatUntil"), until);
    // A count from the device goes back as a count while the end date still matches it.
    const int occurrences = Number(Field(r, "repeatOccurrences"));
    const bool counted = ends && occurrences > 0 &&
                         NthOccurrence(type, Number(Field(r, "repeatInterval"), 1), on, start, occurrences) ==
                             DaysFromCivil(until.year, until.month, until.day);
    if (counted) rec.Add(pageCalendar, cal::Occurrences, std::to_string(occurrences));
    rec.Add(pageCalendar, cal::Interval, std::to_string(interval));
    if (type == 3) rec.Add(pageCalendar, cal::WeekOfMonth, std::to_string(on / 7 + 1));
    if (type == 2) rec.Add(pageCalendar, cal::DayOfWeek, std::to_string(on & 0x7f));
    if (type == 3) rec.Add(pageCalendar, cal::DayOfWeek, std::to_string(1 << (on % 7)));
    if (type == 5 || yearlyNth) rec.Add(pageCalendar, cal::MonthOfYear, std::to_string(start.month));
    if (ends && !counted) {
        until.hour = start.hour;
        until.minute = start.minute;
        rec.Add(pageCalendar, cal::Until, Compact(zone.ToUtc(until)));
    }
    if (type == 4 || type == 5) rec.Add(pageCalendar, cal::DayOfMonth, std::to_string(start.day));
}

// Elements in the order of the MS-ASCMD calendar schema. The stamp is left out of item hashes.
WbxmlNode CalendarData(const PimRecord& r, const Zone& zone, bool stamp) {
    WbxmlNode data(pageAirSync, as::ApplicationData);
    data.Add(pageCalendar, cal::TimeZone, zone.Blob());
    Civil start, end;
    if (!ParseDate(Field(r, "date"), start)) throw std::runtime_error("Calendar event without a date");
    end = start;
    const bool timed = ParseClock(Field(r, "startTime"), start);
    if (timed) {
        if (!ParseClock(Field(r, "endTime"), end)) end.hour = start.hour, end.minute = start.minute;
    } else {
        start.hour = start.minute = 0;
        end = CivilFromSeconds(UtcSeconds(start) + 86400);
    }
    data.Add(pageCalendar, cal::AllDayEvent, timed ? "0" : "1");
    AddText(data, pageCalendar, cal::Body, Field(r, "note"));
    data.Add(pageCalendar, cal::BusyStatus, "2");
    if (stamp) data.Add(pageCalendar, cal::DtStamp, Compact(int64_t(std::time(nullptr))));
    data.Add(pageCalendar, cal::EndTime, Compact(zone.ToUtc(end)));
    AddText(data, pageCalendar, cal::Location, Field(r, "location"));
    if (r.fields.count("alarmAdvance")) {
        const int unit = Number(Field(r, "alarmUnit"));
        const int minutes = Number(Field(r, "alarmAdvance")) * (unit == 2 ? 1440 : unit == 1 ? 60 : 1);
        data.Add(pageCalendar, cal::Reminder, std::to_string(minutes));
    }
    data.Add(pageCalendar, cal::Sensitivity, r.secret ? "2" : "0");
    AddText(data, pageCalendar, cal::Subject, Field(r, "summary"));
    data.Add(pageCalendar, cal::StartTime, Compact(zone.ToUtc(start)));
    const auto name = "Frond calendar " + std::to_string(r.id);
    auto uid = Sha256Hex(reinterpret_cast<const uint8_t*>(name.data()), name.size()).substr(0, 32);
    std::transform(uid.begin(), uid.end(), uid.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    data.Add(pageCalendar, cal::UID, uid);
    data.Add(pageCalendar, cal::MeetingStatus, "0");
    AddCategory(data, pageCalendar, cal::Categories, cal::Category, r.category);
    AddRecurrence(data, r, start, zone);
    const auto exceptions = Field(r, "exceptions");
    if (!exceptions.empty()) {
        auto& list = data.Add(pageCalendar, cal::Exceptions);
        size_t from = 0;
        while (from <= exceptions.size()) {
            const auto comma = std::min(exceptions.find(',', from), exceptions.size());
            Civil day;
            if (ParseDate(exceptions.substr(from, comma - from), day)) {
                day.hour = start.hour;
                day.minute = start.minute;
                auto& item = list.Add(pageCalendar, cal::Exception);
                item.Add(pageCalendar, cal::Deleted, "1");
                item.Add(pageCalendar, cal::ExceptionStartTime, Compact(zone.ToUtc(day)));
            }
            from = comma + 1;
        }
    }
    return data;
}

void ApplyCalendar(const WbxmlNode& data, PimRecord& r, const Zone& zone) {
    int64_t startUtc = 0, endUtc = 0;
    if (!ParseUtc(data.Text(pageCalendar, cal::StartTime), startUtc))
        throw std::runtime_error("Calendar item without a start time");
    if (!ParseUtc(data.Text(pageCalendar, cal::EndTime), endUtc)) endUtc = startUtc;
    const bool allDay = data.Text(pageCalendar, cal::AllDayEvent) == "1";
    // An all-day event's midnight is the device's; the nearest local midnight gives the day.
    const Civil start = zone.ToLocal(allDay ? startUtc + 12 * 3600 : startUtc);
    Civil end = zone.ToLocal(endUtc);
    r.fields["date"] = FormatDate(start);
    if (allDay) {
        r.fields["startTime"] = "";
        r.fields["endTime"] = "";
    } else {
        if (FormatDate(end) != FormatDate(start) || endUtc < startUtc) end.hour = 23, end.minute = 59;
        r.fields["startTime"] = FormatClock(start);
        r.fields["endTime"] = FormatClock(end);
    }
    const auto set = [&](const char* field, const std::string& value) {
        if (value.empty()) r.fields.erase(field);
        else r.fields[field] = value;
    };
    set("summary", data.Text(pageCalendar, cal::Subject));
    set("location", data.Text(pageCalendar, cal::Location));
    if (data.Text(pageCalendar, cal::BodyTruncated) != "1") set("note", data.Text(pageCalendar, cal::Body));
    const auto reminder = data.Text(pageCalendar, cal::Reminder);
    if (reminder.empty()) {
        r.fields.erase("alarmAdvance");
        r.fields.erase("alarmUnit");
    } else {
        const int minutes = std::max(0, Number(reminder));
        const int unit = minutes && minutes % 1440 == 0 && minutes / 1440 <= 99 ? 2 : minutes % 60 == 0 && minutes / 60 <= 99 && minutes ? 1 : 0;
        const int advance = unit == 2 ? minutes / 1440 : unit == 1 ? minutes / 60 : std::min(minutes, 99);
        r.fields["alarmAdvance"] = std::to_string(advance);
        r.fields["alarmUnit"] = std::to_string(unit);
    }
    for (const char* key : {"repeatType", "repeatUntil", "repeatInterval", "repeatOn", "repeatWeekStart", "repeatOccurrences"})
        r.fields.erase(key);
    if (const auto* rec = data.Find(pageCalendar, cal::Recurrence)) {
        const int type = Number(rec->Text(pageCalendar, cal::Type), -1);
        const int dayOfWeek = Number(rec->Text(pageCalendar, cal::DayOfWeek));
        int interval = std::min(std::max(Number(rec->Text(pageCalendar, cal::Interval), 1), 1), 99);
        int palm = 0, on = 0;
        if (type == 0) palm = 1;
        else if (type == 1) palm = 2, on = dayOfWeek & 0x7f;
        else if (type == 2) palm = 4;
        else if (type == 3 || type == 6) {
            int dow = 0;
            while (dow < 6 && !(dayOfWeek >> dow & 1)) ++dow;
            palm = 3;
            on = (std::min(std::max(Number(rec->Text(pageCalendar, cal::WeekOfMonth), 1), 1), 5) - 1) * 7 + dow;
            if (type == 6) interval = std::min(interval, 8) * 12;
        } else if (type == 5) palm = 5;
        if (palm) {
            r.fields["repeatType"] = std::to_string(palm);
            r.fields["repeatInterval"] = std::to_string(interval);
            r.fields["repeatOn"] = std::to_string(on);
            r.fields["repeatWeekStart"] = "0";
            int64_t until;
            const int occurrences = Number(rec->Text(pageCalendar, cal::Occurrences));
            const int64_t last = occurrences > 0 ? NthOccurrence(palm, interval, on, start, occurrences) : -1;
            if (ParseUtc(rec->Text(pageCalendar, cal::Until), until)) {
                r.fields["repeatUntil"] = FormatDate(zone.ToLocal(until));
            } else if (last >= 0) {
                // Palm repeats end on a date: the last occurrence's, with the count kept for AirSync.
                r.fields["repeatUntil"] = FormatDate(CivilFromSeconds(last * 86400));
                r.fields["repeatOccurrences"] = std::to_string(occurrences);
            } else {
                r.fields["repeatUntil"] = "";
            }
        }
    }
    std::string exceptions;
    if (const auto* list = data.Find(pageCalendar, cal::Exceptions)) {
        for (const auto& item : list->children) {
            int64_t when;
            if (item.Text(pageCalendar, cal::Deleted) != "1" || !ParseUtc(item.Text(pageCalendar, cal::ExceptionStartTime), when))
                continue;
            if (!exceptions.empty()) exceptions += ',';
            exceptions += FormatDate(zone.ToLocal(when));
        }
    }
    set("exceptions", exceptions);
    const auto sensitivity = data.Text(pageCalendar, cal::Sensitivity);
    r.secret = sensitivity == "2" || sensitivity == "3";
    r.category = ReadCategory(data, pageCalendar, cal::Categories, cal::Category);
}

// Tasks. Palm priority 1 is the highest; AirSync importance 2 is high.
int Importance(int priority) { return priority <= 1 ? 2 : priority <= 3 ? 1 : 0; }

WbxmlNode TaskData(const PimRecord& r, const Zone& zone) {
    WbxmlNode data(pageAirSync, as::ApplicationData);
    AddText(data, pageTasks, tk::Body, Field(r, "note"));
    AddCategory(data, pageTasks, tk::Categories, tk::Category, r.category);
    const bool complete = Field(r, "complete") == "1";
    data.Add(pageTasks, tk::Complete, complete ? "1" : "0");
    Civil done;
    if (complete && ParseDate(Field(r, "completionDate"), done))
        data.Add(pageTasks, tk::DateCompleted, Extended(CivilFromSeconds(zone.ToUtc(done))));
    Civil due;
    if (ParseDate(Field(r, "dueDate"), due)) {
        data.Add(pageTasks, tk::DueDate, Extended(due));
        data.Add(pageTasks, tk::UtcDueDate, Extended(CivilFromSeconds(zone.ToUtc(due))));
    }
    data.Add(pageTasks, tk::Importance, std::to_string(Importance(Number(Field(r, "priority"), 1))));
    data.Add(pageTasks, tk::Sensitivity, r.secret ? "2" : "0");
    AddText(data, pageTasks, tk::Subject, Field(r, "summary"));
    return data;
}

void ApplyTask(const WbxmlNode& data, PimRecord& r, const Zone& zone) {
    r.fields["summary"] = data.Text(pageTasks, tk::Subject);
    if (data.Text(pageTasks, tk::BodyTruncated) != "1") r.fields["note"] = data.Text(pageTasks, tk::Body);
    const bool complete = data.Text(pageTasks, tk::Complete) == "1";
    r.fields["complete"] = complete ? "1" : "0";
    int64_t when;
    if (complete && ParseUtc(data.Text(pageTasks, tk::DateCompleted), when)) r.fields["completionDate"] = FormatDate(zone.ToLocal(when));
    else if (!complete) r.fields.erase("completionDate");
    // DueDate is the device's local date written as UTC.
    const auto due = data.Text(pageTasks, tk::DueDate);
    r.fields["dueDate"] = ParseUtc(due, when) ? FormatDate(CivilFromSeconds(when)) : "";
    const int importance = Number(data.Text(pageTasks, tk::Importance), 1);
    if (Importance(Number(Field(r, "priority"), 1)) != importance)
        r.fields["priority"] = importance >= 2 ? "1" : importance == 1 ? "3" : "5";
    const auto sensitivity = data.Text(pageTasks, tk::Sensitivity);
    r.secret = sensitivity == "2" || sensitivity == "3";
    r.category = ReadCategory(data, pageTasks, tk::Categories, tk::Category);
}

WbxmlNode ItemData(const PimRecord& r, const Zone& zone, bool stamp = true) {
    switch (r.kind) {
        case PimKind::contacts: return ContactData(r, zone);
        case PimKind::calendar: return CalendarData(r, zone, stamp);
        case PimKind::tasks: return TaskData(r, zone);
        default: throw std::runtime_error("Unsupported AirSync item");
    }
}

void ApplyItem(const WbxmlNode& data, PimRecord& r, const Zone& zone) {
    switch (r.kind) {
        case PimKind::contacts: ApplyContact(data, r, zone); break;
        case PimKind::calendar: ApplyCalendar(data, r, zone); break;
        case PimKind::tasks: ApplyTask(data, r, zone); break;
        default: throw std::runtime_error("Unsupported AirSync item");
    }
}

// The device's view of a record, or nothing for a record the device can't show, like an event
// with no date.
std::optional<Bytes> ItemView(const PimRecord& r, const Zone& zone) {
    try {
        return EncodeWbxml(ItemData(r, zone, false));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// The view's hash, to tell whether a client change differs from the profile's.
std::optional<std::string> ItemHash(const PimRecord& r, const Zone& zone) {
    const auto view = ItemView(r, zone);
    if (!view) return std::nullopt;
    return Sha256Hex(view->data(), view->size());
}

std::string ItemId(const FolderInfo& folder, int64_t recordId) { return std::string(folder.id) + ":" + std::to_string(recordId); }
int64_t RecordIdOf(const FolderInfo& folder, const std::string& serverId) {
    const std::string prefix = std::string(folder.id) + ":";
    if (serverId.compare(0, prefix.size(), prefix) != 0) return 0;
    const auto rest = serverId.substr(prefix.size());
    return Digits(rest) ? std::stoll(rest) : 0;
}

// One transaction; rolled back unless committed. Not nested: AirSyncServer opens one for each
// request, so a request is applied whole or not at all.
class Change {
   public:
    explicit Change(SyncProfile& profile) : profile(profile) {
        std::string error;
        if (!profile.BeginPimChange(error)) throw std::runtime_error(error);
    }
    ~Change() {
        if (active) profile.RollBackPimChange();
    }
    void Commit() {
        std::string error;
        if (!profile.CommitPimChange(error)) throw std::runtime_error(error);
        active = false;
    }

   private:
    SyncProfile& profile;
    bool active{true};
};

void Must(bool ok, const std::string& error) {
    if (!ok) throw std::runtime_error(error.empty() ? "Could not update sync profile" : error);
}

// One collection of one Sync request, inside the request's transaction. With an overwrite
// action the side that wins replaces the other, as the Palm PIM sync does: desktop overwrites
// sends every profile record again and removes the device's own adds, device overwrites takes
// every device change and puts the device's last view back over profile edits.
//
// Kept in the profile under the folder's scope, beside the links:
// - airsync-resend: linked records to send again whatever their revision (a time zone change,
//   desktop overwrites). The link revision stays the base the device last saw.
// - airsync-unconfirmed: "<key>:<ids>", records sent as Adds the device hasn't named since, and
//   the SyncKey of the answer that last sent one. After a new partnership the device uploads
//   its own copies of them; a copy matching one is dropped again, as both sides have it.
class CollectionSync {
   public:
    CollectionSync(SyncProfile& profile, const std::string& deviceKey, const FolderInfo& folder, AirSyncCounts& counts,
                   std::string& timeZone, PimAction action, bool& overwriteQueued)
        : profile(profile), folder(folder), scope("airsync:" + deviceKey + "/" + folder.className),
          zoneKey("airsync:" + deviceKey), counts(counts), timeZone(timeZone), zone(timeZone), action(action),
          overwriteQueued(overwriteQueued) {}

    // Fills the response collection. Returns false when there is more to send.
    bool Run(const WbxmlNode& request, WbxmlNode& response) {
        const auto key = request.Text(pageAirSync, as::SyncKey, "0");
        const auto stored = profile.DeviceValue(scope, "airsync-synckey");
        response.Add(pageAirSync, as::Class, folder.className);
        std::string error;
        if (key == "0") {
            // A new start: the device keeps no items from earlier syncs in this folder.
            for (const auto& link : profile.PimLinks(scope, error)) Must(profile.RemovePimLink(scope, link.guestId, error), error);
            Must(error.empty(), error);
            Must(profile.SetDeviceValue(scope, "airsync-synckey", "1", &error), error);
            SaveState();
            response.Add(pageAirSync, as::SyncKey, "1");
            response.Add(pageAirSync, as::CollectionId, folder.id);
            response.Add(pageAirSync, as::Status, "1");
            return false;
        }
        if (!Digits(key) || key != stored) {
            // An empty, garbled or stale key, or none stored: the device starts again from 0.
            response.Add(pageAirSync, as::SyncKey, "0");
            response.Add(pageAirSync, as::CollectionId, folder.id);
            response.Add(pageAirSync, as::Status, "3");
            return false;
        }
        const int64_t current = std::stoll(stored);
        next = current + 1;
        response.Add(pageAirSync, as::SyncKey, std::to_string(next));
        response.Add(pageAirSync, as::CollectionId, folder.id);
        response.Add(pageAirSync, as::Status, "1");

        LoadState();
        QueueOverwrite();

        const auto* commands = request.Find(pageAirSync, as::Commands);
        bool adds = false;
        if (commands)
            for (const auto& command : commands->children) adds |= command.tag == as::Add;
        // A request with no adds after the one that followed the last server Adds: the device's
        // first upload is over.
        if (!adds && current > unconfirmedKey) unconfirmed.clear();

        WbxmlNode replies(pageAirSync, as::Responses);
        if (commands)
            for (const auto& command : commands->children) Client(command, replies);

        bool more = false;
        WbxmlNode out(pageAirSync, as::Commands);
        for (const auto& serverId : dropped) {
            out.Add(pageAirSync, as::Delete).Add(pageAirSync, as::ServerId, serverId);
            ++counts.deleted;
        }
        const bool getChanges = request.Find(pageAirSync, as::GetChanges) != nullptr;
        if (getChanges) {
            const int window = std::max(1, Number(request.Text(pageAirSync, as::WindowSize), 100));
            more = Server(out, window);
        }
        Must(profile.SetDeviceValue(scope, "airsync-synckey", std::to_string(next), &error), error);
        SaveState();
        if (!replies.children.empty()) response.children.push_back(std::move(replies));
        if (!out.children.empty()) response.children.push_back(std::move(out));
        if (more) response.Add(pageAirSync, as::MoreAvailable);
        return !more && getChanges;
    }

    // Server changes waiting for the device.
    int Pending() {
        LoadState();
        QueueOverwrite();
        SaveState();
        if (action == PimAction::handheldOverwrites) return 0;
        int count = 0;
        for (const auto& record : profile.PimRecords(folder.kind, true)) {
            auto link = links.find(record.id);
            const bool gone = record.deleted || record.archived;
            if (link == links.end()) count += !gone && ItemView(record, zone).has_value();
            else count += record.revision != link->second.revision || resend.count(record.id);
        }
        return count;
    }

   private:
    void LoadState() {
        std::string error;
        for (auto& link : profile.PimLinks(scope, error)) links.emplace(link.recordId, link);
        Must(error.empty(), error);
        resend = ParseIds(profile.DeviceValue(scope, "airsync-resend"));
        const auto text = profile.DeviceValue(scope, "airsync-unconfirmed");
        const auto colon = text.find(':');
        if (colon != std::string::npos && Digits(text.substr(0, colon))) {
            unconfirmedKey = std::stoll(text.substr(0, colon));
            unconfirmed = ParseIds(text.substr(colon + 1));
        }
    }

    void SaveState() {
        std::string error;
        Must(profile.SetDeviceValue(scope, "airsync-resend", FormatIds(resend), &error), error);
        const auto text = unconfirmed.empty() ? std::string() : std::to_string(unconfirmedKey) + ":" + FormatIds(unconfirmed);
        Must(profile.SetDeviceValue(scope, "airsync-unconfirmed", text, &error), error);
    }

    // Desktop overwrites: every linked record goes to the device again, once per connection.
    void QueueOverwrite() {
        if (action != PimAction::desktopOverwrites || overwriteQueued) return;
        for (const auto& [id, link] : links) resend.insert(id);
        overwriteQueued = true;
    }

    // A calendar item carries the device's time zone; keep the latest for later conversions.
    const Zone& Learn(const WbxmlNode& data) {
        const auto text = data.Text(pageCalendar, cal::TimeZone);
        if (text.empty() || text == timeZone || !Zone(text).Valid() || Zone(text).SameRules(zone)) return zone;
        timeZone = text;
        zone = Zone(text);
        std::string error;
        Must(profile.SetDeviceValue(zoneKey, "airsync-timezone", text, &error), error);
        // Items sent with the earlier zone go again with the right times. This is only needed
        // when the zone couldn't be read from the device's registry at connect.
        for (const auto& [id, link] : links) resend.insert(id);
        return zone;
    }

    void Reply(WbxmlNode& replies, uint8_t command, const std::string& clientId, const std::string& serverId,
               const char* status) {
        auto& reply = replies.Add(pageAirSync, command);
        if (!clientId.empty()) reply.Add(pageAirSync, as::ClientId, clientId);
        if (!serverId.empty()) reply.Add(pageAirSync, as::ServerId, serverId);
        reply.Add(pageAirSync, as::Status, status);
    }

    // The link keeps the device's view of the record (its ApplicationData as WBXML in base64),
    // so a device overwrite can put it back after profile edits.
    void Remember(const PimRecord& record, std::string& error) {
        const auto view = ItemView(record, zone);
        PimLink link{uint32_t(record.id), record.id, record.revision, view ? Base64(*view) : std::string()};
        Must(profile.SavePimLink(scope, link, error), error);
        links[record.id] = link;
        resend.erase(record.id);
    }

    void Forget(int64_t recordId, std::string& error) {
        Must(profile.RemovePimLink(scope, uint32_t(recordId), error), error);
        links.erase(recordId);
        resend.erase(recordId);
        unconfirmed.erase(recordId);
    }

    // Puts the device's last view back over a profile record. False when the link has none.
    bool Restore(const PimRecord& record, const PimLink& link, std::string& error) {
        Bytes bytes;
        WbxmlNode view;
        if (link.guestHash.empty() || !Unbase64(link.guestHash, bytes) || !DecodeWbxml(bytes.data(), bytes.size(), view, error)) {
            error.clear();
            return false;
        }
        PimRecord restored = record;
        try {
            ApplyItem(view, restored, zone);
        } catch (const std::exception&) {
            return false;
        }
        restored.deleted = false;
        restored.archived = false;
        Must(profile.SavePimRecord(restored, error), error);
        Remember(restored, error);
        ++counts.fromDevice;
        return true;
    }

    // A live profile record with this view that the device may already have: unlinked, as after
    // a new partnership, or just sent as an Add the device hasn't confirmed.
    std::optional<PimRecord> Same(const std::string& hash, bool sent) {
        if (sent) {
            for (const auto id : unconfirmed) {
                const auto local = profile.FindPimRecord(id);
                if (local && !local->deleted && !local->archived && links.count(id) && ItemHash(*local, zone) == hash)
                    return local;
            }
            return std::nullopt;
        }
        for (const auto& local : profile.PimRecords(folder.kind))
            if (!local.archived && !links.count(local.id) && ItemHash(local, zone) == hash) return local;
        return std::nullopt;
    }

    void Client(const WbxmlNode& command, WbxmlNode& replies) {
        std::string error;
        const auto* data = command.Find(pageAirSync, as::ApplicationData);
        const auto serverId = command.Text(pageAirSync, as::ServerId);
        if (command.tag == as::Add) {
            const auto clientId = command.Text(pageAirSync, as::ClientId);
            if (!data) return Reply(replies, as::Add, clientId, {}, "6");
            const auto droppedId = std::string(folder.id) + ":new" + clientId;
            if (action == PimAction::desktopOverwrites) {
                // Not in the profile, so it goes: take it, then delete it in the same answer.
                Learn(*data);
                dropped.push_back(droppedId);
                return Reply(replies, as::Add, clientId, droppedId, "1");
            }
            PimRecord record;
            record.kind = folder.kind;
            try {
                ApplyItem(*data, record, Learn(*data));
            } catch (const std::exception&) {
                return Reply(replies, as::Add, clientId, {}, "6");
            }
            const auto hash = ItemHash(record, zone);
            if (!hash) return Reply(replies, as::Add, clientId, {}, "6");
            // An item both sides already have is linked, not copied.
            if (const auto same = Same(*hash, false)) {
                Remember(*same, error);
                return Reply(replies, as::Add, clientId, ItemId(folder, same->id), "1");
            }
            // The device's own copy of an item it was just sent: the sent one stays.
            if (const auto same = Same(*hash, true)) {
                unconfirmed.erase(same->id);
                dropped.push_back(droppedId);
                return Reply(replies, as::Add, clientId, droppedId, "1");
            }
            Must(profile.SavePimRecord(record, error), error);
            Remember(record, error);
            ++counts.fromDevice;
            return Reply(replies, as::Add, clientId, ItemId(folder, record.id), "1");
        }
        const auto id = RecordIdOf(folder, serverId);
        unconfirmed.erase(id);
        auto link = links.find(id);
        auto local = link == links.end() ? std::nullopt : profile.FindPimRecord(id);
        if (command.tag == as::Fetch) {
            const auto item = local && !local->deleted ? ItemView(*local, zone) : std::nullopt;
            if (!item) return Reply(replies, as::Fetch, {}, serverId, "8");
            auto& reply = replies.Add(pageAirSync, as::Fetch);
            reply.Add(pageAirSync, as::ServerId, serverId);
            reply.Add(pageAirSync, as::Status, "1");
            reply.children.push_back(ItemData(*local, zone));
            return;
        }
        if (command.tag == as::Delete) {
            // With DeletesAsMoves the device moves the item to its Deleted Items; here it's a delete.
            if (!local) return;
            const bool live = !local->deleted && !local->archived;
            if (live && (action == PimAction::handheldOverwrites ||
                         (action == PimAction::synchronize && local->revision == link->second.revision))) {
                Must(profile.DeletePimRecord(local->id, local->revision, error), error);
                ++counts.deleted;
            }
            // Otherwise the profile's record wins and goes back as an add.
            Forget(id, error);
            return;
        }
        if (command.tag != as::Change) return;
        if (!local || !data) return Reply(replies, as::Change, {}, serverId, "8");
        if (action == PimAction::desktopOverwrites) {
            // The profile's version goes back over the device's change.
            Learn(*data);
            resend.insert(id);
            return;
        }
        PimRecord applied = *local;
        try {
            ApplyItem(*data, applied, Learn(*data));
        } catch (const std::exception&) {
            return Reply(replies, as::Change, {}, serverId, "6");
        }
        const bool localChanged = local->revision != link->second.revision;
        if (action == PimAction::synchronize && localChanged && !local->deleted && !local->archived) {
            const auto mine = ItemHash(*local, zone);
            if (!mine || ItemHash(applied, zone) != mine) {
                // Both changed: keep the device's version as a new record and send the
                // profile's version back to the device, so each side ends with both.
                PimRecord copy = applied;
                copy.id = 0;
                copy.revision = 0;
                Must(profile.SavePimRecord(copy, error), error);
                ++counts.conflicts;
            } else {
                Remember(*local, error);
            }
        } else {
            applied.deleted = false;
            applied.archived = false;
            Must(profile.SavePimRecord(applied, error), error);
            Remember(applied, error);
            ++counts.fromDevice;
        }
    }

    // Returns true when the window is full and more is waiting.
    bool Server(WbxmlNode& out, int window) {
        std::string error;
        int sent = 0;
        const auto records = profile.PimRecords(folder.kind, true);
        for (const auto& record : records) {
            auto link = links.find(record.id);
            const bool gone = record.deleted || record.archived;
            if (link == links.end() ? gone : record.revision == link->second.revision && !resend.count(record.id))
                continue;
            if (action == PimAction::handheldOverwrites) {
                if (link == links.end()) {
                    // Only in the profile, so it goes.
                    Must(profile.DeletePimRecord(record.id, record.revision, error), error);
                    ++counts.deleted;
                    continue;
                }
                if (Restore(record, link->second, error)) continue;
                // A link from before views were kept: send the profile's version as a sync would.
            }
            if (sent == window) return true;
            WbxmlNode data;
            if (!gone) {
                try {
                    data = ItemData(record, zone);
                } catch (const std::exception&) {
                    continue;  // A record the device can't show, like an event with no date.
                }
            }
            ++sent;
            if (link == links.end()) {
                auto& add = out.Add(pageAirSync, as::Add);
                add.Add(pageAirSync, as::ServerId, ItemId(folder, record.id));
                add.children.push_back(std::move(data));
                Remember(record, error);
                unconfirmed.insert(record.id);
                unconfirmedKey = next;
                ++counts.toDevice;
            } else if (gone) {
                out.Add(pageAirSync, as::Delete).Add(pageAirSync, as::ServerId, ItemId(folder, record.id));
                Forget(record.id, error);
                ++counts.deleted;
            } else {
                auto& item = out.Add(pageAirSync, as::Change);
                item.Add(pageAirSync, as::ServerId, ItemId(folder, record.id));
                item.children.push_back(std::move(data));
                Remember(record, error);
                ++counts.toDevice;
            }
        }
        return false;
    }

    SyncProfile& profile;
    const FolderInfo& folder;
    const std::string scope, zoneKey;
    AirSyncCounts& counts;
    std::string& timeZone;
    Zone zone;
    const PimAction action;
    bool& overwriteQueued;  // per folder for the connection
    std::map<int64_t, PimLink> links;
    std::set<int64_t> resend, unconfirmed;
    int64_t unconfirmedKey{0}, next{0};  // next: the SyncKey of this answer
    std::vector<std::string> dropped;  // device adds deleted again in the same answer
};

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::string QueryValue(const std::string& target, const std::string& name) {
    const auto query = target.find('?');
    if (query == std::string::npos) return {};
    size_t at = query + 1;
    while (at < target.size()) {
        const auto amp = std::min(target.find('&', at), target.size());
        const auto pair = target.substr(at, amp - at);
        const auto equals = pair.find('=');
        if (equals != std::string::npos && Lower(pair.substr(0, equals)) == Lower(name)) return pair.substr(equals + 1);
        at = amp + 1;
    }
    return {};
}

Bytes Text(const std::string& text) { return Bytes(text.begin(), text.end()); }

std::string Hex(const Bytes& data) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto b : data) out += digits[b >> 4], out += digits[b & 15];
    return out;
}

}  // namespace

std::string AirSyncFolderId(PimKind kind) {
    for (const auto& folder : folderInfo)
        if (folder.kind == kind) return folder.id;
    return {};
}

std::string AirSyncBase64(const Bytes& data) { return Base64(data); }

AirSyncServer::AirSyncServer(SyncProfile& profile, std::string deviceKey, std::vector<PimKind> kinds)
    : profile(profile), deviceKey(std::move(deviceKey)), kinds(std::move(kinds)), last(Clock::now()) {
    timeZone = profile.DeviceValue("airsync:" + this->deviceKey, "airsync-timezone");
}

void AirSyncServer::SetDeviceTimeZone(const std::string& base64) {
    std::lock_guard lock(mutex);
    if (base64.empty() || base64 == timeZone || !Zone(base64).Valid()) return;
    timeZone = base64;
    profile.SetDeviceValue("airsync:" + deviceKey, "airsync-timezone", base64);
}

PimAction AirSyncServer::Action(PimKind kind) {
    auto it = actions.find(kind);
    if (it == actions.end()) it = actions.emplace(kind, profile.GetPimAction(deviceKey, kind)).first;
    return it->second;
}

bool AirSyncServer::FinishActions(std::string& error) {
    std::lock_guard lock(mutex);
    for (const auto& [kind, action] : actions) {
        auto done = settled.find(kind);
        if (done != settled.end() && done->second && !profile.FinishPimAction(deviceKey, kind, action, error)) return false;
    }
    return true;
}

void AirSyncServer::Trace(const std::string& text) {
    if (!trace) return;
    std::fprintf(trace, "%s\n", text.c_str());
    std::fflush(trace);
}

AirSyncResponse AirSyncServer::Handle(const AirSyncRequest& request) {
    std::lock_guard lock(mutex);
    last = Clock::now();
    ++requests;
    Trace("> " + request.method + " " + request.target + " " + Hex(request.body));
    AirSyncResponse response;
    try {
        response = HandleLocked(request);
    } catch (const std::exception& error) {
        lastError = error.what();
        response = {};
        response.status = 500;
    }
    Trace("< " + std::to_string(response.status) + " " + Hex(response.body));
    return response;
}

AirSyncResponse AirSyncServer::HandleLocked(const AirSyncRequest& request) {
    AirSyncResponse response;
    response.headers.emplace_back("MS-Server-ActiveSync", "6.5.7638.1");
    response.headers.emplace_back("Cache-Control", "private");
    const auto path = request.target.substr(0, request.target.find('?'));
    if (Lower(path) == "/microsoft-server-activesync/syncstat.dll") {
        // Progress from the guest's sync engine, which needs only a success answer.
        const std::string text(request.body.begin(), request.body.end());
        if (text.find("<SyncEnd") != std::string::npos) syncEnd = true;
        return response;
    }
    if (Lower(path) != "/microsoft-server-activesync") {
        response.status = 404;
        return response;
    }
    if (request.method == "OPTIONS") {
        response.headers.emplace_back("MS-ASProtocolVersions", "1.0,2.0,2.1,2.5");
        response.headers.emplace_back("MS-ASProtocolCommands",
                                      "Sync,SendMail,SmartForward,SmartReply,GetAttachment,GetHierarchy,CreateCollection,"
                                      "DeleteCollection,MoveCollection,FolderSync,FolderCreate,FolderDelete,FolderUpdate,"
                                      "MoveItems,GetItemEstimate,MeetingResponse,ResolveRecipients,ValidateCert,Provision,"
                                      "Search,Ping");
        response.headers.emplace_back("Public", "OPTIONS,POST");
        response.headers.emplace_back("Allow", "OPTIONS,POST");
        return response;
    }
    if (request.method != "POST") {
        response.status = 405;
        return response;
    }
    const auto command = QueryValue(request.target, "Cmd");
    int status = 200;
    if (command == "FolderSync") response.body = FolderSync(request.body, status);
    else if (command == "Sync") response.body = Sync(request.body, status);
    else if (command == "GetItemEstimate") response.body = Estimate(request.body, status);
    else status = 501;
    response.status = status;
    if (!response.body.empty()) response.headers.emplace_back("Content-Type", "application/vnd.ms-sync.wbxml");
    return response;
}

Bytes AirSyncServer::FolderSync(const Bytes& body, int& status) {
    WbxmlNode request;
    std::string error;
    if (!DecodeWbxml(body.data(), body.size(), request, error) || request.page != pageFolders || request.tag != fh::FolderSync) {
        status = 400;
        return {};
    }
    const auto key = request.Text(pageFolders, fh::SyncKey, "0");
    WbxmlNode out(pageFolders, fh::FolderSync);
    out.Add(pageFolders, fh::Status, "1");
    // The folder list never changes, so key 1 always stays current.
    out.Add(pageFolders, fh::SyncKey, "1");
    if (key == "0") {
        auto& changes = out.Add(pageFolders, fh::Changes);
        std::vector<const FolderInfo*> offered;
        for (const auto& folder : folderInfo)
            if (std::find(kinds.begin(), kinds.end(), folder.kind) != kinds.end()) offered.push_back(&folder);
        changes.Add(pageFolders, fh::Count, std::to_string(offered.size()));
        for (const auto* folder : offered) {
            auto& add = changes.Add(pageFolders, fh::Add);
            add.Add(pageFolders, fh::ServerId, folder->id);
            add.Add(pageFolders, fh::ParentId, "0");
            add.Add(pageFolders, fh::DisplayName, folder->name);
            add.Add(pageFolders, fh::Type, folder->type);
        }
    } else if (key != "1") {
        out.children.clear();
        out.Add(pageFolders, fh::Status, "9");  // invalid sync key: start again from 0
    }
    return EncodeWbxml(out);
}

Bytes AirSyncServer::Sync(const Bytes& body, int& status) {
    if (!lastSyncRequest.empty() && body == lastSyncRequest) return lastSyncResponse;
    WbxmlNode request;
    std::string error;
    if (!DecodeWbxml(body.data(), body.size(), request, error) || request.page != pageAirSync || request.tag != as::Sync) {
        status = 400;
        return {};
    }
    WbxmlNode out(pageAirSync, as::Sync);
    auto& collections = out.Add(pageAirSync, as::Collections);
    // All or nothing: a request that fails leaves the profile and the keys as they were, so
    // the device's retry is applied once.
    const auto saved = std::make_tuple(timeZone, settled, overwriteQueued, counts);
    try {
        Change change(profile);
        if (const auto* list = request.Find(pageAirSync, as::Collections)) {
            for (const auto& collection : list->children) {
                if (collection.tag != as::Collection) continue;
                const auto id = collection.Text(pageAirSync, as::CollectionId);
                const auto* folder = OfferedFolder(id, collection.Text(pageAirSync, as::Class), kinds);
                auto& result = collections.Add(pageAirSync, as::Collection);
                if (!folder) {
                    result.Add(pageAirSync, as::Class, collection.Text(pageAirSync, as::Class));
                    result.Add(pageAirSync, as::SyncKey, collection.Text(pageAirSync, as::SyncKey, "0"));
                    result.Add(pageAirSync, as::CollectionId, id);
                    result.Add(pageAirSync, as::Status, "8");
                    continue;
                }
                CollectionSync sync(profile, deviceKey, *folder, counts, timeZone, Action(folder->kind),
                                    overwriteQueued[folder->kind]);
                settled[folder->kind] = sync.Run(collection, result);
            }
        }
        change.Commit();
    } catch (...) {
        std::tie(timeZone, settled, overwriteQueued, counts) = saved;
        throw;
    }
    lastSyncRequest = body;
    lastSyncResponse = EncodeWbxml(out);
    return lastSyncResponse;
}

Bytes AirSyncServer::Estimate(const Bytes& body, int& status) {
    WbxmlNode request;
    std::string error;
    if (!DecodeWbxml(body.data(), body.size(), request, error) || request.page != pageEstimate ||
        request.tag != ge::GetItemEstimate) {
        status = 400;
        return {};
    }
    WbxmlNode out(pageEstimate, ge::GetItemEstimate);
    const auto saved = overwriteQueued;
    try {
        Change change(profile);
        if (const auto* list = request.Find(pageEstimate, ge::Collections)) {
            for (const auto& collection : list->children) {
                const auto id = collection.Text(pageEstimate, ge::CollectionId);
                // A folder that isn't offered is an invalid collection, as Sync refuses it.
                const auto* folder = OfferedFolder(id, collection.Text(pageEstimate, ge::Class), kinds);
                auto& response = out.Add(pageEstimate, ge::Response);
                response.Add(pageEstimate, ge::Status, folder ? "1" : "2");
                auto& item = response.Add(pageEstimate, ge::Collection);
                if (!folder) {
                    item.Add(pageEstimate, ge::CollectionId, id);
                    continue;
                }
                item.Add(pageEstimate, ge::Class, folder->className);
                item.Add(pageEstimate, ge::CollectionId, folder->id);
                CollectionSync sync(profile, deviceKey, *folder, counts, timeZone, Action(folder->kind),
                                    overwriteQueued[folder->kind]);
                item.Add(pageEstimate, ge::Estimate, std::to_string(sync.Pending()));
            }
        }
        change.Commit();
    } catch (...) {
        overwriteQueued = saved;
        throw;
    }
    return EncodeWbxml(out);
}

void AirSyncServer::Serve(SyncStream& stream) {
    Bytes buffer;
    uint8_t chunk[4096];
    for (;;) {
        {
            std::lock_guard lock(mutex);
            if (stopping) return;
        }
        static const char separator[] = "\r\n\r\n";
        auto headerEnd = std::search(buffer.begin(), buffer.end(), separator, separator + 4);
        if (headerEnd == buffer.end()) {
            if (buffer.size() > 65536) return;
            const long count = stream.Read(chunk, sizeof(chunk), 500);
            if (count < 0) return;
            buffer.insert(buffer.end(), chunk, chunk + count);
            continue;
        }
        const std::string head(buffer.begin(), headerEnd);
        AirSyncRequest request;
        size_t lineEnd = head.find("\r\n");
        const auto line = head.substr(0, lineEnd);
        const auto space1 = line.find(' '), space2 = line.find(' ', space1 + 1);
        if (space1 == std::string::npos || space2 == std::string::npos) return;
        request.method = line.substr(0, space1);
        request.target = line.substr(space1 + 1, space2 - space1 - 1);
        while (lineEnd != std::string::npos && lineEnd < head.size()) {
            const auto start = lineEnd + 2;
            lineEnd = head.find("\r\n", start);
            const auto header = head.substr(start, lineEnd == std::string::npos ? std::string::npos : lineEnd - start);
            const auto colon = header.find(':');
            if (colon == std::string::npos) continue;
            auto value = header.substr(colon + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            request.headers[Lower(header.substr(0, colon))] = value;
        }
        // A Content-Length that isn't a plain number up to 8 MB closes the connection.
        const auto lengthText = request.headers["content-length"];
        if (!lengthText.empty() && !Digits(lengthText)) return;
        const size_t length = lengthText.empty() ? 0 : size_t(std::stoll(lengthText));
        if (length > (8u << 20)) return;
        const size_t bodyStart = size_t(headerEnd - buffer.begin()) + 4;
        while (buffer.size() < bodyStart + length) {
            const long count = stream.Read(chunk, sizeof(chunk), 500);
            if (count < 0) return;
            buffer.insert(buffer.end(), chunk, chunk + count);
            std::lock_guard lock(mutex);
            if (stopping) return;
        }
        request.body.assign(buffer.begin() + long(bodyStart), buffer.begin() + long(bodyStart + length));
        buffer.erase(buffer.begin(), buffer.begin() + long(bodyStart + length));

        const auto response = Handle(request);
        const char* reason = response.status == 200 ? "OK" : response.status == 404 ? "Not Found"
                             : response.status == 400 ? "Bad Request" : response.status == 501 ? "Not Implemented"
                                                                                                : "Error";
        std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " + reason + "\r\n";
        for (const auto& [name, value] : response.headers) out += name + ": " + value + "\r\n";
        out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n\r\n";
        Bytes bytes = Text(out);
        bytes.insert(bytes.end(), response.body.begin(), response.body.end());
        if (!stream.Write(bytes.data(), bytes.size())) return;
    }
}

void AirSyncServer::Stop() {
    std::lock_guard lock(mutex);
    stopping = true;
}

bool AirSyncServer::Finished() const {
    std::lock_guard lock(mutex);
    if (syncEnd) return true;
    for (const auto kind : kinds) {
        auto it = settled.find(kind);
        if (it == settled.end() || !it->second) return false;
    }
    return !kinds.empty();
}

std::chrono::steady_clock::duration AirSyncServer::Idle() const {
    std::lock_guard lock(mutex);
    return Clock::now() - last;
}

unsigned AirSyncServer::Requests() const {
    std::lock_guard lock(mutex);
    return requests;
}

AirSyncCounts AirSyncServer::Counts() const {
    std::lock_guard lock(mutex);
    return counts;
}

std::string AirSyncServer::LastError() const {
    std::lock_guard lock(mutex);
    return lastError;
}

}  // namespace frondSync
