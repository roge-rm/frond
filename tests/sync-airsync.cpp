// Desktop AirSync server: requests captured from a WM5 rx1950 (ActiveSync 2.5) go through
// AirSyncServer against a scratch profile. Also the device time zone read over RAPI.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>

#include "ActiveSyncPartnership.h"
#include "AirSync.h"
#include "Wbxml.h"

using namespace frondSync;

namespace {

void Check(bool ok, const char* expression, int line) {
    if (ok) return;
    std::fprintf(stderr, "sync-airsync.cpp:%d: %s\n", line, expression);
    std::exit(1);
}

#define CHECK(...) Check(bool((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// Captured request bodies.
const char folderSync[] = "03016a00000756520330000101";
const char contactsStart[] = "03016a00455c4f5003436f6e746163747300014b033000015203310001010101";
const char calendarStart[] = "03016a00455c4f500343616c656e64617200014b033000015203320001010101";
const char tasksStart[] = "03016a00455c4f50035461736b7300014b033000015203330001010101";
const char estimate[] = "03016a0000064547484903436f6e746163747300014a03310001000058033000014b0331000101000648490343616c656e64617200014a03320001000058033400014b033100010100064849035461736b7300014a03330001000058033000014b03310001010101";
const char contactsChanges[] = "03016a00455c4f5003436f6e746163747300014b0331000152033100011e13550331303000015759033400015a033400015b0331000101010101";
const char calendarChanges[] = "03016a00455c4f500343616c656e64617200014b0331000152033200011e135503313030000157580334000159033400015a033400015b0331000101010101";
const char tasksChanges[] = "03016a00455c4f50035461736b7300014b0331000152033300011e13550331303000015759033400015a033400015b0331000101010101";
// Grace Hopper, typed on the device.
const char contactsAdd[] = "03016a00455c4f5003436f6e746163747300014b0333000152033100011e13550331303000015759033400015a033400015b033100010156474c033231343734383336353000015d000159034e61767900015e03486f707065722c20477261636500015f03477261636500016903486f7070657200013b3c010101010101";
// Tea with Grace, 12:00 to 13:00 on 1 Jan 2006 in the device's Pacific zone.
const char calendarAdd[] = "03016a00455c4f500343616c656e64617200014b0333000152033200011e135503313030000157580334000159033400015a033400015b033100010156474c033130373337343138323600015d00044503344145414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141416f41414141464141494141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414151414141414241414941414141414141414178502f2f2f773d3d000146033000014d033200011051033230303630313031543230303333315a000152033230303630313031543231303030305a000157034c69627261727900015803300001640331350001650330000166035465612077697468204772616365000167033230303630313031543230303030305a0001680330343030303030303832303045303030373443354237313031413832453030383030303030303030423530323744413041394532363144423838424635334134364542453539463844374233454132344337444645453842353737454539454344463046383245310001010101010101";
// "Oil the engine" ticked as complete.
const char taskChange[] = "03016a00455c4f50035461736b7300014b0332000152033300011e13550331303000015759033400015a033400015b033100010156484d03333a3300015d00094a033100014b03323030362d30312d30315430383a30303a30302e3030305a00014c03323032362d31312d32365430303a30303a30302e3030305a00014d03323032362d31312d32365430383a30303a30302e3030305a00014e033100015d0330000160034f696c2074686520656e67696e650001010101010101";
// "Write compiler".
const char taskAdd[] = "03016a00455c4f50035461736b7300014b0333000152033300011e13550331303000015759033400015a033400015b033100010156474c033332323132323534373400015d00094a033000014e033100015d033000016003577269746520636f6d70696c6572000121010101010101";

Bytes Hex(const char* text) {
    Bytes out;
    for (const char* p = text; p[0] && p[1]; p += 2) out.push_back(uint8_t(std::stoi(std::string(p, 2), nullptr, 16)));
    return out;
}

WbxmlNode Decode(const Bytes& body) {
    WbxmlNode node;
    std::string error;
    CHECK(DecodeWbxml(body.data(), body.size(), node, error));
    return node;
}

// Changes the first node with this tag, at any depth.
bool Set(WbxmlNode& node, uint8_t page, uint8_t tag, const std::string& value) {
    if (node.page == page && node.tag == tag) {
        node.text = value;
        return true;
    }
    for (auto& child : node.children)
        if (Set(child, page, tag, value)) return true;
    return false;
}

const WbxmlNode* Find(const WbxmlNode& node, uint8_t page, uint8_t tag, int index = 0) {
    std::vector<const WbxmlNode*> stack{&node};
    int count = 0;
    while (!stack.empty()) {
        const auto* at = stack.back();
        stack.pop_back();
        if (at->page == page && at->tag == tag && count++ == index) return at;
        for (auto it = at->children.rbegin(); it != at->children.rend(); ++it) stack.push_back(&*it);
    }
    return nullptr;
}

int CountTag(const WbxmlNode& node, uint8_t page, uint8_t tag) {
    int count = node.page == page && node.tag == tag;
    for (const auto& child : node.children) count += CountTag(child, page, tag);
    return count;
}

std::string TextOf(const WbxmlNode& node, uint8_t page, uint8_t tag, int index = 0) {
    const auto* found = Find(node, page, tag, index);
    return found ? found->text : "<none>";
}

constexpr uint8_t SyncKey = 0x0b, ClientId = 0x0c, ServerId = 0x0d, Status = 0x0e, Add = 0x07, Change = 0x08,
                  Delete = 0x09, Commands = 0x16, MoreAvailable = 0x14;

struct Client {
    AirSyncServer& server;
    AirSyncResponse last;

    WbxmlNode Post(const std::string& command, const Bytes& body) {
        AirSyncRequest request;
        request.method = "POST";
        request.target = "/Microsoft-Server-ActiveSync?User=DEFAULT&DeviceId=B97255B2BF6FC351D0C41D5FD635657D"
                         "&DeviceType=PocketPC&Cmd=" + command;
        request.body = body;
        last = server.Handle(request);
        CHECK(last.status == 200);
        return Decode(last.body);
    }
    // A captured Sync request with this client's current key and, if given, a ServerId.
    WbxmlNode Sync(const char* hex, const std::string& key, const std::string& serverId = {}) {
        auto request = Decode(Hex(hex));
        CHECK(Set(request, 0, SyncKey, key));
        if (!serverId.empty()) CHECK(Set(request, 0, ServerId, serverId));
        return Post("Sync", EncodeWbxml(request));
    }
};

std::string NextKey(const WbxmlNode& response) {
    CHECK(TextOf(response, 0, Status) == "1");
    return TextOf(response, 0, SyncKey);
}

// A scripted device for the RAPI registry calls: each read serves the next queued answer.
struct FakeRapi {
    Bytes replies;
    size_t at{0};
    void Reply(std::initializer_list<uint32_t> words, const Bytes& data = {}) {
        Bytes body;
        for (const auto word : words)
            for (int i = 0; i < 4; ++i) body.push_back(uint8_t(word >> (8 * i)));
        body.insert(body.end(), data.begin(), data.end());
        for (int i = 0; i < 4; ++i) replies.push_back(uint8_t(body.size() >> (8 * i)));
        replies.insert(replies.end(), body.begin(), body.end());
    }
    static bool Read(void* self, void* data, size_t size, int) {
        auto* fake = static_cast<FakeRapi*>(self);
        if (fake->at + size > fake->replies.size()) return false;
        std::memcpy(data, fake->replies.data() + fake->at, size);
        fake->at += size;
        return true;
    }
    static bool Write(void*, const void*, size_t) { return true; }
    static void Close(void*) {}
    std::string TimeZone(std::string& source) {
        FrondRapi* client = FrondRapiOpen({this, Read, Write, Close});
        const auto zone = ReadActiveSyncTimeZone(client, &source);
        FrondRapiClose(client);
        return zone;
    }
};

Bytes Utf16(const std::string& text) {
    Bytes out;
    for (const char c : text) out.push_back(uint8_t(c)), out.push_back(0);
    out.push_back(0), out.push_back(0);
    return out;
}

// The TimeZone a captured calendar add carries: Pacific, US rules before 2007.
std::string CapturedZone() {
    const auto add = Decode(Hex(calendarAdd));
    return TextOf(add, 4, 0x05);
}

void TestTimeZoneRead() {
    std::string source;
    {
        // Neither HKLM\Time nor HKLM\Time Zones: the ROM's built-in Pacific zone.
        FakeRapi fake;
        fake.Reply({2, 2});
        fake.Reply({2, 2});
        CHECK(fake.TimeZone(source) == CapturedZone());
        CHECK(source.find("built-in Pacific") != std::string::npos);
    }
    {
        // HKLM\Time\TimeZoneInformation, as after the zone is set on the device.
        Bytes info(172, 0);
        info[0] = 0x3c;  // bias 60: UTC-1
        FakeRapi fake;
        fake.Reply({0, 0, 7});
        fake.Reply({0, 0, 3, 172}, info);
        fake.Reply({0, 0});
        CHECK(fake.TimeZone(source) == AirSyncBase64(info) && source == "HKLM\\Time");
    }
    {
        // HKLM\Time Zones\Default names a zone whose TZI is a REG_TZI_FORMAT.
        Bytes tzi(44, 0);
        tzi[0] = 0xe0, tzi[1] = 0x01;  // bias 480
        tzi[8] = tzi[9] = tzi[10] = tzi[11] = 0xff, tzi[8] = 0xc4;  // daylight bias -60
        tzi[14] = 10, tzi[18] = 5, tzi[20] = 2;  // standard: last Sunday in October, 2:00
        tzi[30] = 4, tzi[34] = 1, tzi[36] = 2;   // daylight: first Sunday in April, 2:00
        FakeRapi fake;
        fake.Reply({2, 2});
        fake.Reply({0, 0, 8});
        fake.Reply({0, 0, 1, 44}, Utf16("Pacific Standard Time"));
        fake.Reply({0, 0});
        fake.Reply({0, 0, 9});
        fake.Reply({0, 0, 3, 44}, tzi);
        fake.Reply({0, 0});
        CHECK(fake.TimeZone(source) == CapturedZone() && source == "HKLM\\Time Zones\\Pacific Standard Time");
    }
}

// A captured Sync request for a folder with this key and these client commands.
Bytes SyncWith(const char* hex, const std::string& key, std::vector<WbxmlNode> commands) {
    auto request = Decode(Hex(hex));
    CHECK(Set(request, 0, SyncKey, key));
    if (!commands.empty()) {
        auto* collection = const_cast<WbxmlNode*>(Find(request, 0, 0x0f));
        collection->Add(0, Commands).children = std::move(commands);
    }
    return EncodeWbxml(request);
}

WbxmlNode Command(uint8_t tag, const std::string& id, WbxmlNode data) {
    WbxmlNode command(0, tag);
    command.Add(0, tag == Add ? ClientId : ServerId, id);
    data.page = 0, data.tag = 0x1d;
    command.children.push_back(std::move(data));
    return command;
}

WbxmlNode Node(uint8_t page, std::vector<std::pair<uint8_t, std::string>> fields) {
    WbxmlNode node;
    for (auto& [tag, value] : fields) node.Add(page, tag, value);
    return node;
}

// The device's zone read at connect: calendar items go with the right time from the first add,
// so a device add in the same zone sends nothing again. Recurrence, contact fields and the
// overwrite actions use the same profile.
void TestKnownZone(SyncLibrary& library) {
    std::string error;
    ProfileInfo info;
    CHECK(library.CreateProfile("Zone", info, error));
    SyncProfile profile;
    CHECK(profile.Open(library.ProfilePath(info), error));

    PimRecord meeting;
    meeting.kind = PimKind::calendar;
    meeting.fields = {{"date", "2026-11-24"}, {"startTime", "10:00"}, {"endTime", "11:30"}, {"summary", "Frond desk meeting"}};
    CHECK(profile.SavePimRecord(meeting, error));
    // Thanksgiving: the fourth Thursday of November, a Palm monthly repeat every 12 months.
    PimRecord dinner;
    dinner.kind = PimKind::calendar;
    dinner.fields = {{"date", "2026-11-26"}, {"startTime", "18:00"}, {"endTime", "20:00"}, {"summary", "Dinner"},
                     {"repeatType", "3"}, {"repeatInterval", "12"}, {"repeatOn", "25"}, {"repeatUntil", ""},
                     {"repeatWeekStart", "0"}};
    CHECK(profile.SavePimRecord(dinner, error));
    PimRecord ada;
    ada.kind = PimKind::contacts;
    ada.fields = {{"firstName", "Ada"}, {"lastName", "Lovelace"}, {"department", "Engines"},
                  {"officeLocation", "Room 12"}, {"children", "Byron, Anne"}, {"nickname", "Countess"}};
    CHECK(profile.SavePimRecord(ada, error));
    PimRecord engine;
    engine.kind = PimKind::tasks;
    engine.fields = {{"summary", "Oil the engine"}, {"complete", "0"}};
    CHECK(profile.SavePimRecord(engine, error));

    AirSyncServer server(profile, "machine:zone", {PimKind::contacts, PimKind::calendar, PimKind::tasks});
    server.SetDeviceTimeZone(CapturedZone());
    Client client{server};
    std::string contactsKey = NextKey(client.Post("Sync", Hex(contactsStart)));
    std::string calendarKey = NextKey(client.Post("Sync", Hex(calendarStart)));
    std::string tasksKey = NextKey(client.Post("Sync", Hex(tasksStart)));
    const auto meetingId = "2:" + std::to_string(meeting.id), dinnerId = "2:" + std::to_string(dinner.id),
               adaId = "1:" + std::to_string(ada.id), engineId = "3:" + std::to_string(engine.id);
    {
        const auto response = client.Sync(calendarChanges, calendarKey);
        calendarKey = NextKey(response);
        CHECK(CountTag(response, 0, Add) == 2 && TextOf(response, 0, ServerId) == meetingId);
        CHECK(TextOf(response, 4, 0x05) == CapturedZone());
        CHECK(TextOf(response, 4, 0x27) == "20261124T180000Z");  // 10:00 Pacific standard time
        // Yearly on the fourth Thursday of November.
        CHECK(TextOf(response, 0, ServerId, 1) == dinnerId && TextOf(response, 4, 0x1c, 0) == "6");
        CHECK(TextOf(response, 4, 0x1f) == "1" && TextOf(response, 4, 0x22) == "4" && TextOf(response, 4, 0x20) == "16" &&
              TextOf(response, 4, 0x23) == "11" && TextOf(response, 4, 0x1d) == "<none>");
    }
    {
        // A device add in the same zone, the captured Tea with Grace, sends nothing back.
        const auto response = client.Sync(calendarAdd, calendarKey);
        calendarKey = NextKey(response);
        CHECK(CountTag(response, 0, Change) == 0 && CountTag(response, 0, Add) == 1);
    }
    {
        // A yearly nth add from the device that ends after 5 times.
        auto data = Node(4, {{0x05, CapturedZone()}, {0x06, "0"}, {0x12, "20261126T190000Z"}, {0x26, "Parade"},
                             {0x27, "20261126T170000Z"}});
        auto& rec = data.Add(4, 0x1b);
        for (auto [tag, value] : std::vector<std::pair<uint8_t, const char*>>{
                 {0x1c, "6"}, {0x1e, "5"}, {0x1f, "1"}, {0x22, "4"}, {0x20, "16"}, {0x23, "11"}})
            rec.Add(4, tag, value);
        const auto response = client.Post("Sync", SyncWith(calendarChanges, calendarKey, {Command(Add, "40", data)}));
        calendarKey = NextKey(response);
        const auto id = TextOf(response, 0, ServerId);
        const auto record = profile.FindPimRecord(std::stoll(id.substr(2)));
        CHECK(record && record->fields.at("date") == "2026-11-26" && record->fields.at("startTime") == "09:00");
        CHECK(record->fields.at("repeatType") == "3" && record->fields.at("repeatInterval") == "12" &&
              record->fields.at("repeatOn") == "25" && record->fields.at("repeatUntil") == "2030-11-28" &&
              record->fields.at("repeatOccurrences") == "5");
        // Edited in the profile, it goes back with its count while the end date still matches.
        auto edited = *record;
        edited.fields["summary"] = "Big parade";
        CHECK(profile.SavePimRecord(edited, error));
        const auto back = client.Sync(calendarChanges, calendarKey);
        calendarKey = NextKey(back);
        CHECK(TextOf(back, 4, 0x26) == "Big parade" && TextOf(back, 4, 0x1c) == "6" && TextOf(back, 4, 0x1e) == "5" &&
              TextOf(back, 4, 0x1d) == "<none>" && TextOf(back, 4, 0x1f) == "1" && TextOf(back, 4, 0x23) == "11");
        // A new end date drops the count.
        edited = *profile.FindPimRecord(edited.id);
        edited.fields["repeatUntil"] = "2028-12-01";
        CHECK(profile.SavePimRecord(edited, error));
        const auto until = client.Sync(calendarChanges, calendarKey);
        calendarKey = NextKey(until);
        CHECK(TextOf(until, 4, 0x1e) == "<none>" && TextOf(until, 4, 0x1d) == "20281201T170000Z");
    }
    {
        // Contact fields with no Palm place go both ways.
        const auto response = client.Sync(contactsChanges, contactsKey);
        contactsKey = NextKey(response);
        CHECK(TextOf(response, 1, 0x1a) == "Engines" && TextOf(response, 1, 0x2c) == "Room 12" &&
              TextOf(response, 12, 0x0d) == "Countess");
        CHECK(CountTag(response, 1, 0x18) == 2 && TextOf(response, 1, 0x18, 1) == "Anne");
        auto data = Node(1, {{0x1f, "Ada"}, {0x29, "Lovelace"}, {0x1a, "Difference Engines"}, {0x2c, "Room 14"},
                             {0x34, "William"}, {0x05, "1835-07-08T07:00:00.000Z"}});
        data.Add(1, 0x17).Add(1, 0x18, "Ralph");
        const auto change = client.Post("Sync", SyncWith(contactsChanges, contactsKey, {Command(Change, adaId, data)}));
        contactsKey = NextKey(change);
        const auto record = profile.FindPimRecord(ada.id);
        CHECK(record->fields.at("department") == "Difference Engines" && record->fields.at("officeLocation") == "Room 14" &&
              record->fields.at("spouse") == "William" && record->fields.at("anniversary") == "1835-07-08" &&
              record->fields.at("children") == "Ralph" && !record->fields.count("nickname"));
    }
    {
        const auto response = client.Sync(tasksChanges, tasksKey);
        tasksKey = NextKey(response);
        CHECK(TextOf(response, 0, ServerId) == engineId);
    }

    // Desktop overwrites: a device change is answered with the profile's version and every other
    // linked record goes again too. The device sends its adds in a later request of the same
    // connection; they're taken and deleted again.
    {
        CHECK(profile.SetPimAction("machine:zone", PimKind::contacts, PimAction::desktopOverwrites, error));
        AirSyncServer overwrite(profile, "machine:zone", {PimKind::contacts, PimKind::calendar, PimKind::tasks});
        Client device{overwrite};
        const auto before = profile.PimRecords(PimKind::contacts).size();
        auto changed = Node(1, {{0x1f, "Ada"}, {0x29, "Byron"}});
        const auto response = device.Post("Sync", SyncWith(contactsChanges, contactsKey, {Command(Change, adaId, changed)}));
        contactsKey = NextKey(response);
        CHECK(profile.FindPimRecord(ada.id)->fields.at("lastName") == "Lovelace");
        CHECK(CountTag(response, 0, Change) == 1 && TextOf(response, 0, ServerId) == adaId &&
              TextOf(response, 1, 0x29) == "Lovelace");
        auto added = Node(1, {{0x1f, "Charles"}, {0x29, "Babbage"}});
        const auto add = device.Post("Sync", SyncWith(contactsChanges, contactsKey, {Command(Add, "50", added)}));
        contactsKey = NextKey(add);
        CHECK(profile.PimRecords(PimKind::contacts).size() == before);
        CHECK(TextOf(add, 0, ClientId) == "50" && TextOf(add, 0, ServerId) == "1:new50");
        CHECK(CountTag(add, 0, Delete) == 1 && TextOf(add, 0, ServerId, 1) == "1:new50");
        CHECK(profile.GetPimAction("machine:zone", PimKind::contacts) == PimAction::desktopOverwrites);
        CHECK(overwrite.FinishActions(error));
        CHECK(profile.GetPimAction("machine:zone", PimKind::contacts) == PimAction::synchronize);
        const auto after = device.Sync(contactsChanges, contactsKey);
        contactsKey = NextKey(after);
        CHECK(CountTag(after, 0, Commands) == 0);
        CHECK(overwrite.LastError().empty());
    }
    // Device overwrites: the device's view goes back over a profile edit and a profile delete,
    // and a profile-only record goes.
    {
        auto task = *profile.FindPimRecord(engine.id);
        task.fields["summary"] = "Desktop edit";
        CHECK(profile.SavePimRecord(task, error));
        PimRecord extra;
        extra.kind = PimKind::tasks;
        extra.fields = {{"summary", "Desktop only"}, {"complete", "0"}};
        CHECK(profile.SavePimRecord(extra, error));
        auto meetingNow = *profile.FindPimRecord(meeting.id);
        CHECK(profile.DeletePimRecord(meeting.id, meetingNow.revision, error));
        CHECK(profile.SetPimAction("machine:zone", PimKind::tasks, PimAction::handheldOverwrites, error));
        CHECK(profile.SetPimAction("machine:zone", PimKind::calendar, PimAction::handheldOverwrites, error));
        AirSyncServer overwrite(profile, "machine:zone", {PimKind::contacts, PimKind::calendar, PimKind::tasks});
        Client device{overwrite};
        const auto counts = device.Post("GetItemEstimate", Hex(estimate));
        CHECK(TextOf(counts, 6, 0x0c, 1) == "0" && TextOf(counts, 6, 0x0c, 2) == "0");
        const auto response = device.Sync(tasksChanges, tasksKey);
        tasksKey = NextKey(response);
        CHECK(CountTag(response, 0, Commands) == 0);
        CHECK(profile.FindPimRecord(engine.id)->fields.at("summary") == "Oil the engine");
        CHECK(profile.FindPimRecord(extra.id)->deleted);
        // A device delete wins over a profile edit, and the meeting deleted in the profile comes
        // back from the device's view.
        auto dinnerNow = *profile.FindPimRecord(dinner.id);
        dinnerNow.fields["summary"] = "Desktop dinner";
        CHECK(profile.SavePimRecord(dinnerNow, error));
        WbxmlNode remove(0, Delete);
        remove.Add(0, ServerId, dinnerId);
        const auto calendar = device.Post("Sync", SyncWith(calendarChanges, calendarKey, {remove}));
        calendarKey = NextKey(calendar);
        CHECK(CountTag(calendar, 0, Commands) == 0);
        CHECK(profile.FindPimRecord(dinner.id)->deleted);
        const auto restored = profile.FindPimRecord(meeting.id);
        CHECK(restored && !restored->deleted && restored->fields.at("summary") == "Frond desk meeting" &&
              restored->fields.at("startTime") == "10:00");
        CHECK(overwrite.FinishActions(error));
        CHECK(profile.GetPimAction("machine:zone", PimKind::calendar) == PimAction::synchronize &&
              profile.GetPimAction("machine:zone", PimKind::tasks) == PimAction::synchronize);
        CHECK(overwrite.LastError().empty());
    }
    CHECK(server.LastError().empty());
}

// A captured Sync request with a window size.
Bytes SyncWindow(const char* hex, const std::string& key, std::vector<WbxmlNode> commands, const std::string& window) {
    auto request = Decode(SyncWith(hex, key, std::move(commands)));
    CHECK(Set(request, 0, 0x15, window));
    return EncodeWbxml(request);
}

// Opens a new profile in the library.
void OpenProfile(SyncLibrary& library, const char* name, SyncProfile& profile) {
    std::string error;
    ProfileInfo info;
    CHECK(library.CreateProfile(name, info, error));
    CHECK(profile.Open(library.ProfilePath(info), error));
}

PimRecord Save(SyncProfile& profile, PimKind kind, std::map<std::string, std::string> fields) {
    std::string error;
    PimRecord record;
    record.kind = kind;
    record.fields = std::move(fields);
    CHECK(profile.SavePimRecord(record, error));
    return record;
}

int Live(SyncProfile& profile, PimKind kind) { return int(profile.PimRecords(kind).size()); }

// A profile event with no date is never sent and doesn't stop device adds.
void TestDatelessEvent(SyncLibrary& library) {
    SyncProfile profile;
    OpenProfile(library, "Dateless", profile);
    Save(profile, PimKind::calendar, {{"summary", "No date"}});
    AirSyncServer server(profile, "machine:dateless", {PimKind::calendar});
    Client client{server};
    auto key = NextKey(client.Post("Sync", Hex(calendarStart)));
    CHECK(TextOf(client.Post("GetItemEstimate", Hex(estimate)), 6, 0x0c) == "0");
    const auto first = client.Sync(calendarChanges, key);
    key = NextKey(first);
    CHECK(CountTag(first, 0, Commands) == 0);
    for (const char* id : {"1073741826", "1073741827"}) {
        auto request = Decode(Hex(calendarAdd));
        CHECK(Set(request, 0, SyncKey, key) && Set(request, 0, ClientId, id));
        const auto response = client.Post("Sync", EncodeWbxml(request));
        key = NextKey(response);
        CHECK(TextOf(response, 0, ClientId) == id && TextOf(response, 0, Status, 1) == "1");
    }
    CHECK(Live(profile, PimKind::calendar) == 3 && server.LastError().empty());
}

// After a new partnership the device gets the profile's items, then uploads its own copies of
// them: each copy is dropped again, so both sides keep one. Also with a window smaller than
// the folder, where some items are still unsent when the device uploads.
void TestFirstUpload(SyncLibrary& library) {
    SyncProfile profile;
    OpenProfile(library, "Upload", profile);
    const auto ada = Save(profile, PimKind::contacts, {{"firstName", "Ada"}, {"lastName", "Lovelace"},
                                                       {"phone1", "555-0101"}, {"phoneLabel1", "0"}});
    const auto charles = Save(profile, PimKind::contacts, {{"firstName", "Charles"}, {"lastName", "Babbage"}});
    const auto dot = Save(profile, PimKind::contacts, {{"firstName", "Dot"}});
    AirSyncServer server(profile, "machine:upload", {PimKind::contacts});
    Client client{server};
    auto key = NextKey(client.Post("Sync", Hex(contactsStart)));
    const auto sent = client.Post("Sync", SyncWindow(contactsChanges, key, {}, "1"));
    key = NextKey(sent);
    CHECK(CountTag(sent, 0, Add) == 1 && TextOf(sent, 0, ServerId) == "1:" + std::to_string(ada.id) &&
          CountTag(sent, 0, MoreAvailable) == 1);
    // The device's own copies of both, plus one new contact.
    const auto adaCopy = *Find(sent, 0, 0x1d);
    const auto grace = Node(1, {{0x1f, "Grace"}, {0x29, "Hopper"}});
    const auto upload = client.Post("Sync", SyncWindow(contactsChanges, key,
                                                       {Command(Add, "61", adaCopy),
                                                        Command(Add, "62", Node(1, {{0x1f, "Charles"}, {0x29, "Babbage"},
                                                                                    {0x1e, "Babbage, Charles"}})),
                                                        Command(Add, "63", grace)},
                                                       "1"));
    key = NextKey(upload);
    CHECK(TextOf(upload, 0, ClientId, 0) == "61" && TextOf(upload, 0, ServerId, 0) == "1:new61");
    CHECK(TextOf(upload, 0, ClientId, 1) == "62" && TextOf(upload, 0, ServerId, 1) == "1:" + std::to_string(charles.id));
    CHECK(TextOf(upload, 0, ClientId, 2) == "63");
    CHECK(CountTag(upload, 0, Delete) == 1 && TextOf(upload, 0, ServerId, 3) == "1:new61");
    CHECK(TextOf(upload, 0, ServerId, 4) == "1:" + std::to_string(dot.id));
    CHECK(Live(profile, PimKind::contacts) == 4);
    const auto dotCopy = *Find(upload, 0, 0x1d);
    for (int i = 0; i < 2; ++i) {
        const auto rest = client.Sync(contactsChanges, key);
        key = NextKey(rest);
        CHECK(CountTag(rest, 0, Commands) == 0);
    }
    // Once the upload is over, the same item typed again is a new contact.
    const auto later = client.Post("Sync", SyncWith(contactsChanges, key, {Command(Add, "64", dotCopy)}));
    key = NextKey(later);
    CHECK(TextOf(later, 0, ServerId) != "1:new64" && CountTag(later, 0, Delete) == 0);
    CHECK(Live(profile, PimKind::contacts) == 5 && server.LastError().empty());
}

// The zone learned from a device change resends the folder over more than one request and
// connection; the base revision stays, so a device delete or change in a later request isn't
// taken for a conflict.
void TestZoneAcrossRequests(SyncLibrary& library) {
    SyncProfile profile;
    OpenProfile(library, "Zone change", profile);
    std::vector<PimRecord> meetings;
    for (int i = 1; i <= 4; ++i)
        meetings.push_back(Save(profile, PimKind::calendar, {{"date", "2026-11-24"}, {"startTime", "10:00"},
                                                             {"endTime", "11:00"}, {"summary", "M" + std::to_string(i)}}));
    const auto id = [&](int i) { return "2:" + std::to_string(meetings[size_t(i)].id); };
    const auto item = [&](const char* subject) {
        return Node(4, {{0x05, CapturedZone()}, {0x06, "0"}, {0x12, "20261124T190000Z"}, {0x26, subject},
                        {0x27, "20261124T180000Z"}});
    };
    std::string key;
    {
        AirSyncServer server(profile, "machine:zonechange", {PimKind::calendar});
        Client client{server};
        key = NextKey(client.Post("Sync", Hex(calendarStart)));
        key = NextKey(client.Sync(calendarChanges, key));
        const auto response = client.Post("Sync", SyncWindow(calendarChanges, key, {Command(Change, id(0), item("M1"))}, "1"));
        key = NextKey(response);
        CHECK(CountTag(response, 0, Change) == 1 && TextOf(response, 0, ServerId) == id(1) &&
              TextOf(response, 4, 0x27) == "20261124T180000Z" && CountTag(response, 0, MoreAvailable) == 1);
        CHECK(server.LastError().empty());
    }
    AirSyncServer server(profile, "machine:zonechange", {PimKind::calendar});
    Client client{server};
    WbxmlNode remove(0, Delete);
    remove.Add(0, ServerId, id(2));
    const auto response = client.Post("Sync", SyncWith(calendarChanges, key, {remove, Command(Change, id(3), item("M4 moved"))}));
    key = NextKey(response);
    CHECK(profile.FindPimRecord(meetings[2].id)->deleted);
    CHECK(profile.FindPimRecord(meetings[3].id)->fields.at("summary") == "M4 moved");
    CHECK(server.Counts().conflicts == 0 && CountTag(response, 0, Commands) == 0);
    CHECK(Live(profile, PimKind::calendar) == 3 && server.LastError().empty());
}

// Birthdays go as the device's local midnight and come back on the same day, west and east.
void TestBirthdays(SyncLibrary& library) {
    Bytes east(172, 0);
    const int32_t bias = -780;  // UTC+13, no daylight time
    for (int i = 0; i < 4; ++i) east[size_t(i)] = uint8_t(uint32_t(bias) >> (8 * i));
    const std::pair<std::string, const char*> zones[] = {{CapturedZone(), "1990-05-01T07:00:00.000Z"},
                                                         {AirSyncBase64(east), "1990-04-30T11:00:00.000Z"}};
    SyncProfile profile;
    OpenProfile(library, "Birthdays", profile);
    const auto ada = Save(profile, PimKind::contacts, {{"firstName", "Ada"}, {"birthday", "1990-05-01"}});
    int n = 0;
    for (const auto& [zone, sent] : zones) {
        AirSyncServer server(profile, "machine:birthday" + std::to_string(n++), {PimKind::contacts});
        server.SetDeviceTimeZone(zone);
        Client client{server};
        auto key = NextKey(client.Post("Sync", Hex(contactsStart)));
        const auto response = client.Sync(contactsChanges, key);
        key = NextKey(response);
        CHECK(TextOf(response, 1, 0x08) == sent);
        auto back = *Find(response, 0, 0x1d);
        CHECK(Set(back, 1, 0x1f, "Ada B"));
        const auto change =
            client.Post("Sync", SyncWith(contactsChanges, key, {Command(Change, "1:" + std::to_string(ada.id), back)}));
        key = NextKey(change);
        const auto record = profile.FindPimRecord(ada.id);
        CHECK(record->fields.at("firstName") == "Ada B" && record->fields.at("birthday") == "1990-05-01");
        CHECK(CountTag(change, 0, Commands) == 0 && server.LastError().empty());
    }
}

// Bad input: an empty SyncKey, huge occurrence counts, folders not offered and a Content-Length
// past 32 bits.
struct FakeStream : SyncStream {
    Bytes input, output;
    bool read{false};
    long Read(uint8_t* out, size_t max, int) override {
        if (read || input.size() > max) return -1;
        read = true;
        std::memcpy(out, input.data(), input.size());
        return long(input.size());
    }
    bool Write(const uint8_t* data, size_t size) override {
        output.insert(output.end(), data, data + size);
        return true;
    }
    void Abort() override {}
};

void TestGhostedPhones(SyncLibrary& library) {
    struct Sample {
        uint8_t clearedWork, keptWork, clearedHome, keptHome;
        const char *clearedWorkSlot, *keptWorkSlot, *clearedHomeSlot, *keptHomeSlot;
    };
    const Sample samples[] = {
        {0x13, 0x0c, 0x27, 0x20, "phone1", "phone3", "phone2", "phone4"},
        {0x0c, 0x13, 0x20, 0x27, "phone3", "phone1", "phone4", "phone2"},
    };
    for (const auto& sample : samples) {
        std::string error, key, serverId;
        int64_t id;
        ProfileInfo info;
        CHECK(library.CreateProfile("Ghosted phones " + std::to_string(sample.clearedWork), info, error));
        const auto path = library.ProfilePath(info);
        {
            SyncProfile profile;
            CHECK(profile.Open(path, error));
            id = Save(profile, PimKind::contacts,
                      {{"firstName", "Ada"}, {sample.keptWorkSlot, "work"}, {sample.clearedWorkSlot, "clear work"},
                       {sample.keptHomeSlot, "home"}, {sample.clearedHomeSlot, "clear home"},
                       {"phoneLabel3", "0"}, {"phoneLabel4", "1"}}).id;
            AirSyncServer server(profile, "machine:phones", {PimKind::contacts});
            Client client{server};
            auto initial = Hex(contactsStart);
            initial.insert(initial.end() - 3, {0x60, 0, 1, sample.clearedWork, sample.clearedHome, 1});
            key = NextKey(client.Post("Sync", initial));
            const auto added = client.Sync(contactsChanges, key);
            key = NextKey(added);
            serverId = TextOf(added, 0, ServerId);
            CHECK(TextOf(added, 1, sample.keptWork) == "work");
            CHECK(TextOf(added, 1, sample.keptHome) == "home");
            CHECK(TextOf(added, 1, sample.clearedWork) == "clear work");
            CHECK(TextOf(added, 1, sample.clearedHome) == "clear home");
            auto change = Command(Change, serverId, Node(1, {{0x1f, "Ada"}}));
            key = NextKey(client.Post("Sync", SyncWith(contactsChanges, key, {change})));
        }

        SyncProfile profile;
        CHECK(profile.Open(path, error));
        AirSyncServer server(profile, "machine:phones", {PimKind::contacts});
        Client client{server};
        auto local = *profile.FindPimRecord(id);
        CHECK(local.fields.at(sample.clearedWorkSlot).empty() && local.fields.at(sample.clearedHomeSlot).empty());
        CHECK(local.fields.at(sample.keptWorkSlot) == "work" && local.fields.at(sample.keptHomeSlot) == "home");

        Bytes palmBytes;
        CHECK(EncodePalmPim(local, PalmPimFormat::garnet, palmBytes, error));
        PimRecord palm;
        CHECK(DecodePalmPim(PimKind::contacts, PalmPimFormat::garnet, palmBytes, palm, error));
        palm.fields[sample.keptWorkSlot] = "Palm work";
        ApplyPalmPim(palm, PalmPimFormat::garnet, local);
        CHECK(profile.SavePimRecord(local, error));
        auto exported = client.Sync(contactsChanges, key);
        key = NextKey(exported);
        CHECK(TextOf(exported, 1, sample.keptWork) == "Palm work");
        CHECK(TextOf(exported, 1, sample.keptHome) == "home");
        CHECK(!Find(exported, 1, sample.clearedWork) && !Find(exported, 1, sample.clearedHome));

        auto update = Node(1, {{sample.keptWork, "Windows work"}, {sample.clearedWork, "restored work"}});
        key = NextKey(client.Post("Sync", SyncWith(contactsChanges, key, {Command(Change, serverId, update)})));
        local = *profile.FindPimRecord(id);
        CHECK(local.fields.at(sample.keptWorkSlot) == "Windows work");
        CHECK(local.fields.at(sample.clearedWorkSlot) == "restored work");
        local.fields.erase(sample.clearedWorkSlot);
        CHECK(profile.SavePimRecord(local, error));
        exported = client.Sync(contactsChanges, key);
        CHECK(TextOf(exported, 1, sample.keptWork) == "Windows work");
        CHECK(TextOf(exported, 1, sample.keptHome) == "home");
        CHECK(Find(exported, 1, sample.clearedWork) && TextOf(exported, 1, sample.clearedWork).empty());
    }
}

void TestLegacyPhoneClear(SyncLibrary& library) {
    std::string error, key;
    ProfileInfo info;
    CHECK(library.CreateProfile("Legacy phone clear", info, error));
    const auto path = library.ProfilePath(info);
    const std::string device = "machine:legacy-phones";
    SyncProfile profile;
    CHECK(profile.Open(path, error));
    const auto id = Save(profile, PimKind::contacts,
                         {{"firstName", "Ada"}, {"phone1", "work"}, {"phone2", "home"},
                          {"phone3", "work two"}, {"phone4", "home two"},
                          {"phoneLabel3", "0"}, {"phoneLabel4", "1"}}).id;
    {
        AirSyncServer server(profile, device, {PimKind::contacts});
        Client client{server};
        key = NextKey(client.Post("Sync", Hex(contactsStart)));
        const auto added = client.Sync(contactsChanges, key);
        key = NextKey(added);
        CHECK(TextOf(added, 1, 0x13) == "work" && TextOf(added, 1, 0x0c) == "work two");
        CHECK(TextOf(added, 1, 0x27) == "home" && TextOf(added, 1, 0x20) == "home two");
        auto record = *profile.FindPimRecord(id);
        for (int slot = 1; slot <= 4; ++slot) CHECK(!record.fields.count("airsyncPhoneTag" + std::to_string(slot)));
        record.fields.erase("phone1");
        record.fields.erase("phone2");
        CHECK(profile.SavePimRecord(record, error));
        const auto cleared = client.Sync(contactsChanges, key);
        key = NextKey(cleared);
        CHECK(Find(cleared, 1, 0x13) && TextOf(cleared, 1, 0x13).empty());
        CHECK(Find(cleared, 1, 0x27) && TextOf(cleared, 1, 0x27).empty());
        CHECK(TextOf(cleared, 1, 0x0c) == "work two" && TextOf(cleared, 1, 0x20) == "home two");
    }

    profile.Close();
    CHECK(profile.Open(path, error));
    auto record = *profile.FindPimRecord(id);
    record.fields["phone3"] = "work two edited";
    record.fields["phone4"] = "home two edited";
    CHECK(profile.SavePimRecord(record, error));
    AirSyncServer server(profile, device, {PimKind::contacts});
    Client client{server};
    const auto exported = client.Sync(contactsChanges, key);
    CHECK(!Find(exported, 1, 0x13) && !Find(exported, 1, 0x27));
    CHECK(TextOf(exported, 1, 0x0c) == "work two edited" && TextOf(exported, 1, 0x20) == "home two edited");
}

std::string UncompressedRtf(const std::string& text) {
    auto bytes = Hex("00000000000000004d454c4100000000");
    for (unsigned i = 0; i < 4; ++i) {
        bytes[i] = uint8_t((12 + text.size()) >> (8 * i));
        bytes[4 + i] = uint8_t(text.size() >> (8 * i));
    }
    bytes.insert(bytes.end(), text.begin(), text.end());
    return AirSyncBase64(bytes);
}

void TestLegacyBodies(SyncLibrary& library) {
    std::string error;
    ProfileInfo info;
    CHECK(library.CreateProfile("Legacy bodies", info, error));
    const auto path = library.ProfilePath(info);
    const auto compressed = Hex("2d0000002b0000004c5a4675f1c5c7a703000a007263706731323542320af320"
                                      "68656c090020627705b06c647d0a800fa0");  // MS-OXRTFCP 3.1.1.1
    const auto rtf = AirSyncBase64(compressed);
    const std::string picture = "R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";
    struct Sample {
        PimKind kind;
        const char *start, *changes;
        uint8_t page, body, size, truncated, rich;
        WbxmlNode data;
    };
    const Sample samples[] = {
        {PimKind::contacts, contactsStart, contactsChanges, 1, 0x09, 0x0a, 0x0b, 0x3b, Node(1, {{0x1f, "A"}})},
        {PimKind::calendar, calendarStart, calendarChanges, 4, 0x0b, 0, 0x0c, 0x10,
         Node(4, {{0x26, "B"}, {0x27, "20261124T180000Z"}, {0x12, "20261124T190000Z"}})},
        {PimKind::tasks, tasksStart, tasksChanges, 9, 0x05, 0x06, 0x07, 0x21, Node(9, {{0x20, "C"}})},
    };

    for (const auto& sample : samples) {
        const auto page = sample.page;
        const bool contact = sample.kind == PimKind::contacts;
        std::string key, serverId;
        int64_t id;
        {
            SyncProfile profile;
            CHECK(profile.Open(path, error));
            AirSyncServer server(profile, "machine:rich", {sample.kind});
            Client client{server};
            auto initial = Hex(sample.start);
            // An empty Supported (page 0, token 0x20) ghosts all fields.
            initial.insert(initial.end() - 3, 0x20);
            key = NextKey(client.Post("Sync", initial));
            CHECK(NextKey(client.Post("Sync", initial)) == key);
            auto upload = sample.data;
            upload.Add(page, sample.body, "hello world");
            upload.Add(page, sample.rich, rtf);
            if (contact) {
                upload.Add(page, 0x13, "555-0100");  // BusinessPhone
                upload.Add(page, 0x3c, picture);
            }
            const auto reply = client.Post("Sync", SyncWith(sample.changes, key, {Command(Add, "rich", upload)}));
            key = NextKey(reply);
            serverId = TextOf(reply, 0, ServerId);
            CHECK(TextOf(reply, 0, Status, 1) == "1");
            id = std::stoll(serverId.substr(serverId.find(':') + 1));
        }

        SyncProfile profile;
        CHECK(profile.Open(path, error));
        AirSyncServer server(profile, "machine:rich", {sample.kind});
        Client client{server};
        const auto change = [&](WbxmlNode item) {
            auto reply = client.Post("Sync", SyncWith(sample.changes, key, {Command(Change, serverId, std::move(item))}));
            key = NextKey(reply);
            return reply;
        };
        auto truncated = sample.data;
        const auto before = *profile.FindPimRecord(id);
        if (contact) CHECK(before.fields.at("phone1") == "555-0100");
        auto sparse = Node(page, {});
        sparse.Add(page, 0x3e, "ignored");
        auto later = Decode(SyncWith(sample.changes, key, {Command(Change, serverId, sparse)}));
        // A later Supported must be ignored, even when it lists the note.
        auto* collection = const_cast<WbxmlNode*>(Find(later, 0, 0x0f));
        collection->Add(0, 0x20).Add(page, sample.body);
        const auto laterBytes = EncodeWbxml(later);
        key = NextKey(client.Post("Sync", laterBytes));
        const auto after = *profile.FindPimRecord(id);
        CHECK(after.fields == before.fields);
        CHECK(NextKey(client.Post("Sync", laterBytes)) == key);
        CHECK(profile.FindPimRecord(id)->revision == after.revision);
        truncated.Add(page, sample.body, "hello");
        truncated.Add(page, sample.truncated, "1");
        if (sample.size) truncated.Add(page, sample.size, "11");
        change(truncated);
        auto saved = *profile.FindPimRecord(id);
        CHECK(saved.fields.at("note") == "hello world" && saved.fields.at("airsyncRtf") == rtf);
        if (contact) CHECK(saved.fields.at("airsyncPicture") == picture);
        if (sample.size) {
            auto inconsistent = sample.data;
            inconsistent.Add(page, sample.body, "hello");
            inconsistent.Add(page, sample.size, "11");
            const auto hash = saved.fields.at("airsyncRtfBodyHash");
            change(inconsistent);
            CHECK(profile.FindPimRecord(id)->fields.at("note") == "hello world");
            CHECK(profile.FindPimRecord(id)->fields.at("airsyncRtfBodyHash") == hash);

            // A valid but different RTF payload from a truncated body cannot replace the old one.
            inconsistent.Add(page, sample.rich,
                             AirSyncBase64(Hex("13000000070000004d454c41000000007b5c727466317d")));
            CHECK(!Find(change(inconsistent), 0, Status, 1));
            saved = *profile.FindPimRecord(id);
            CHECK(saved.fields.at("note") == "hello world" && saved.fields.at("airsyncRtf") == rtf);
        }

        auto invalid = sample.data;
        invalid.Add(page, sample.rich, "AAAA");
        CHECK(TextOf(change(invalid), 0, Status, 1) == "6");
        auto corrupt = compressed;
        corrupt[16] ^= 1;
        invalid.children.back().text = AirSyncBase64(corrupt);
        CHECK(TextOf(change(invalid), 0, Status, 1) == "6");
        corrupt = compressed;
        corrupt[8] ^= 1;
        invalid.children.back().text = AirSyncBase64(corrupt);
        CHECK(TextOf(change(invalid), 0, Status, 1) == "6");
        if (contact) {
            invalid = sample.data;
            invalid.Add(page, 0x3c, std::string(49156, 'A'));
            CHECK(TextOf(change(invalid), 0, Status, 1) == "6");
        }
        CHECK(profile.FindPimRecord(id)->revision == saved.revision);
        if (contact) {
            auto atLimit = sample.data;
            atLimit.Add(page, 0x3c, std::string(49152, 'A'));
            change(atLimit);
            saved = *profile.FindPimRecord(id);
            CHECK(saved.fields.at("airsyncPicture") == std::string(49152, 'A'));
        }

        const std::string formattedRtf = "{\\rtf1\\b hello world}";
        const auto richEdit = UncompressedRtf(formattedRtf);
        auto richOnly = Node(page, {{sample.rich, richEdit}});
        CHECK(!Find(change(richOnly), 0, Status, 1));
        saved = *profile.FindPimRecord(id);
        CHECK(saved.fields.at("note") == "hello world" && saved.fields.at("airsyncRtf") == richEdit);

        // A Palm edit merges only fields in its record format.
        PimRecord palm = saved;
        palm.fields.erase("airsyncRtf");
        palm.fields.erase("airsyncRtfBodyHash");
        palm.fields.erase("airsyncPicture");
        ApplyPalmPim(palm, PalmPimFormat::garnet, saved);
        CHECK(saved.fields.at("airsyncRtf") == richEdit);
        if (contact) CHECK(saved.fields.at("airsyncPicture") == std::string(49152, 'A'));
        saved.category = "Edited";
        CHECK(profile.SavePimRecord(saved, error));
        auto response = client.Sync(sample.changes, key);
        key = NextKey(response);
        CHECK(TextOf(response, page, sample.rich) == richEdit && TextOf(response, page, sample.body) == "hello world");
        if (sample.size) CHECK(!Find(response, page, sample.size));
        if (contact) CHECK(TextOf(response, page, 0x3c) == std::string(49152, 'A'));

        // A host body edit takes precedence over the saved RTF. The UTF-8 prefix ends at a character.
        saved = *profile.FindPimRecord(id);
        const std::string prefix(5119, 'a');
        saved.fields["note"] = prefix + "éz";
        if (contact) saved.fields["airsyncPicture"] = std::string(49156, 'A');
        CHECK(profile.SavePimRecord(saved, error));
        response = client.Sync(sample.changes, key);
        key = NextKey(response);
        CHECK(TextOf(response, page, sample.body) == prefix + "é" && TextOf(response, page, sample.truncated) == "1");
        CHECK(Find(response, page, sample.rich) && TextOf(response, page, sample.rich).empty());
        if (contact) CHECK(Find(response, page, 0x3c) && TextOf(response, page, 0x3c).empty());
        if (sample.size) CHECK(TextOf(response, page, sample.size) == "5121");
        CHECK(profile.FindPimRecord(id)->fields.at("note") == prefix + "éz");

        const auto download = [&](const std::string& opt, const std::string& note) {
            saved = *profile.FindPimRecord(id);
            saved.fields["note"] = note;
            CHECK(profile.SavePimRecord(saved, error));
            auto request = Decode(SyncWith(sample.changes, key, {}));
            if (opt.empty()) {
                auto* options = const_cast<WbxmlNode*>(Find(request, 0, 0x17));
                options->children.clear();
            } else {
                CHECK(Set(request, 0, 0x19, opt));
            }
            auto result = client.Post("Sync", EncodeWbxml(request));
            key = NextKey(result);
            return result;
        };
        for (const auto& [opt, limit] : std::map<std::string, size_t>{{"6", 20480}, {"7", 51200}, {"8", 102400}}) {
            const auto boundary = std::string(limit - 1, 'x') + "😀";
            auto result = download(opt, boundary);
            CHECK(TextOf(result, page, sample.body) == boundary && !Find(result, page, sample.truncated));
            result = download(opt, boundary + "z");
            CHECK(TextOf(result, page, sample.body) == boundary && TextOf(result, page, sample.truncated) == "1");
            if (sample.size) CHECK(TextOf(result, page, sample.size) == std::to_string(limit + 1));
        }
        const std::string full(110000, 'x');
        for (const auto* opt : {"", "9"}) {
            auto result = download(opt, full);
            CHECK(TextOf(result, page, sample.body) == full && !Find(result, page, sample.truncated));
        }
        auto cleared = download("9", "");
        CHECK(Find(cleared, page, sample.body) && TextOf(cleared, page, sample.body).empty());

        auto restore = sample.data;
        restore.Add(page, sample.body, "clear this");
        restore.Add(page, sample.rich, rtf);
        if (contact) restore.Add(page, 0x3c, picture);
        change(restore);
        auto clear = sample.data;
        clear.Add(page, sample.body);
        clear.Add(page, sample.rich);
        if (contact) clear.Add(page, 0x3c);
        change(clear);
        saved = *profile.FindPimRecord(id);
        CHECK(!saved.fields.count("note") && !saved.fields.count("airsyncRtf") && !saved.fields.count("airsyncPicture"));
        // Start again with a selective Supported list. It replaces the earlier empty list.
        const bool calendar = sample.kind == PimKind::calendar;
        const uint8_t optionalTag = contact ? 0x29 : calendar ? 0x17 : 0x0c;
        const char* optionalField = contact ? "lastName" : calendar ? "location" : "dueDate";
        auto initial = Hex(sample.start);
        initial.insert(initial.end() - 3, {0x60, 0, page, sample.rich, optionalTag, 1});
        key = NextKey(client.Post("Sync", initial));
        saved.fields["note"] = "keep this text";
        saved.fields["unrelated"] = "keep this field";
        CHECK(profile.SavePimRecord(saved, error));
        response = client.Sync(sample.changes, key);
        key = NextKey(response);
        auto formatted = sample.data;
        formatted.Add(page, sample.body, "keep this text");
        formatted.Add(page, sample.rich, rtf);
        formatted.Add(page, optionalTag, contact ? "Lovelace" : calendar ? "Library" : "2026-11-26T00:00:00.000Z");
        if (contact) formatted.Add(page, 0x3c, picture);
        change(formatted);
        CHECK(!profile.FindPimRecord(id)->fields.at(optionalField).empty());
        change(sample.data);
        saved = *profile.FindPimRecord(id);
        CHECK(saved.fields.at("note") == "keep this text" && !saved.fields.count("airsyncRtf"));
        CHECK(!saved.fields.count(optionalField) || saved.fields.at(optionalField).empty());
        if (contact) CHECK(saved.fields.at("airsyncPicture") == picture);

        // A reset without Supported returns to full replacement for captured WM5 requests.
        key = NextKey(client.Post("Sync", Hex(sample.start)));
        response = client.Sync(sample.changes, key);
        key = NextKey(response);
        change(formatted);
        change(sample.data);
        saved = *profile.FindPimRecord(id);
        CHECK(!saved.fields.count("note") && !saved.fields.count("airsyncRtf") && !saved.fields.count("airsyncPicture"));
        CHECK(saved.fields.at("unrelated") == "keep this field");

        WbxmlNode remove(0, Delete);
        remove.Add(0, ServerId, serverId);
        NextKey(client.Post("Sync", SyncWith(sample.changes, key, {remove})));
        CHECK(profile.FindPimRecord(id)->deleted);
    }
}

void TestWm5ContactNote(SyncLibrary& library) {
    SyncProfile profile;
    OpenProfile(library, "WM5 contact note", profile);
    std::string error;
    const auto id = Save(profile, PimKind::contacts, {{"firstName", "Ada"}, {"lastName", "Lovelace"},
                                                      {"note", "Old note"}, {"airsyncPicture", "AQID"}}).id;
    AirSyncServer server(profile, "machine:wm5-notes", {PimKind::contacts});
    Client client{server};
    auto key = NextKey(client.Post("Sync", Hex(contactsStart)));
    key = NextKey(client.Sync(contactsChanges, key));
    const auto serverId = "1:" + std::to_string(id);
    // Synthetic LZFu with the WM5 packet's escaped text and formatting controls.
    const auto rich = AirSyncBase64(Hex(
        "97000000820000004c5a4675044378ac010009205c6465666630007b5c666f6e747462006c7b5c66305c666e"
        "00696c205461686f6d00613b7d7d7b5c636f006c6f7274626c3b5c00726564305c67726500656e305c626c75"
        "6500303b7d5c66305c6600733136205c273465005c2736355c273737005c2732305c62205c002736655c2736"
        "665c002737345c2736355c0062305c706172207d011510"));
    auto item = Node(1, {{0x1e, "Lovelace, Ada"}, {0x1f, "Ada"}, {0x29, "Lovelace"}, {0x3b, rich}});
    const auto upload = [&] {
        const auto reply = client.Post("Sync", SyncWith(contactsChanges, key, {Command(Change, serverId, item)}));
        CHECK(!Find(reply, 0, Status, 1));
        key = NextKey(reply);
    };
    upload();
    auto saved = *profile.FindPimRecord(id);
    CHECK(saved.fields.at("note") == "New note");
    CHECK(saved.fields.at("airsyncRtf") == rich && saved.fields.at("airsyncPicture") == "AQID");
    const auto accented = UncompressedRtf(R"({\rtf1\ansi\ansicpg1252 Caf\'e9 \{\'80\}})");
    CHECK(Set(item, 1, 0x3b, accented));
    upload();
    saved = *profile.FindPimRecord(id);
    CHECK(saved.fields.at("note") == "Caf\xc3\xa9 {\xe2\x82\xac}");
    CHECK(saved.fields.at("airsyncRtf") == accented && saved.fields.at("airsyncPicture") == "AQID");

    for (const auto* unsupported : {R"({\rtf1\ansi\ansicpg932 \'82\'a0})",
                                    R"({\rtf1\ansi{\fonttbl{\f0\fnil\fcharset204 Arial;}}\f0 \'e9})"}) {
        CHECK(Set(item, 1, 0x3b, UncompressedRtf(unsupported)));
        const auto rejected = client.Post("Sync", SyncWith(contactsChanges, key, {Command(Change, serverId, item)}));
        CHECK(TextOf(rejected, 0, Status, 1) == "6");
        key = NextKey(rejected);
        CHECK(profile.FindPimRecord(id)->fields == saved.fields);
        CHECK(profile.FindPimRecord(id)->revision == saved.revision);
    }
    CHECK(Set(item, 1, 0x3b, accented));
    upload();
    CHECK(profile.FindPimRecord(id)->fields == saved.fields);
    saved = *profile.FindPimRecord(id);
    saved.fields["note"] = "Edited on host";
    CHECK(profile.SavePimRecord(saved, error));
    auto response = client.Sync(contactsChanges, key);
    key = NextKey(response);
    CHECK(TextOf(response, 1, 0x09) == "Edited on host" && TextOf(response, 1, 0x3b).empty());
    upload();
    response = client.Sync(contactsChanges, key);
    key = NextKey(response);
    CHECK(!Find(response, 1, 0x3b));
    CHECK(profile.FindPimRecord(id)->fields.at("note") == "Edited on host");
    item.Add(1, 0x09);
    item.Add(1, 0x3c);
    CHECK(Set(item, 1, 0x3b, ""));
    upload();
    saved = *profile.FindPimRecord(id);
    CHECK(!saved.fields.count("note") && !saved.fields.count("airsyncRtf") && !saved.fields.count("airsyncPicture"));
}

void TestDeleteRetry(SyncLibrary& library) {
    SyncProfile profile;
    std::string error;
    ProfileInfo info;
    CHECK(library.CreateProfile("Delete retry", info, error));
    CHECK(profile.Open(library.ProfilePath(info), error));
    const std::string device = "machine:delete-retry";
    const auto id = Save(profile, PimKind::contacts, {{"firstName", "Ada"}}).id;
    const auto other = Save(profile, PimKind::contacts, {{"firstName", "Grace"}}).id;
    Bytes request, answer;
    int64_t deletedRevision;
    {
        AirSyncServer server(profile, device, {PimKind::contacts});
        Client client{server};
        auto key = NextKey(client.Post("Sync", Hex(contactsStart)));
        key = NextKey(client.Sync(contactsChanges, key));
        CHECK(profile.SetPimAction(device, PimKind::contacts, PimAction::handheldOverwrites, error));
        WbxmlNode remove(0, Delete);
        remove.Add(0, ServerId, "1:" + std::to_string(id));
        request = SyncWith(contactsChanges, key, {remove});
    }
    {
        AirSyncServer server(profile, device, {PimKind::contacts});
        Client client{server};
        CHECK(NextKey(client.Post("Sync", request)) == "3");
        answer = client.last.body;
        const auto deleted = profile.FindPimRecord(id);
        CHECK(deleted->deleted);
        deletedRevision = deleted->revision;
    }
    profile.Close();
    CHECK(profile.Open(library.ProfilePath(info), error));
    CHECK(profile.GetPimAction(device, PimKind::contacts) == PimAction::handheldOverwrites);
    {
        AirSyncServer server(profile, device, {PimKind::contacts});
        Client client{server};
        auto distinct = Decode(request);
        CHECK(Set(distinct, 0, ServerId, "1:" + std::to_string(other)));
        CHECK(TextOf(client.Post("Sync", EncodeWbxml(distinct)), 0, Status) == "3");
        CHECK(!profile.FindPimRecord(other)->deleted);
        CHECK(NextKey(client.Post("Sync", request)) == "3" && client.last.body == answer);
        CHECK(profile.FindPimRecord(id)->revision == deletedRevision && server.Counts().deleted == 0);
        CHECK(server.Finished() && server.FinishActions(error));
        CHECK(profile.GetPimAction(device, PimKind::contacts) == PimAction::synchronize);
        CHECK(NextKey(client.Sync(contactsChanges, "3")) == "4");
    }
    AirSyncServer server(profile, device, {PimKind::contacts});
    Client client{server};
    CHECK(TextOf(client.Post("Sync", request), 0, Status) == "3");
}

void TestLargeDeleteRetry(SyncLibrary& library) {
    SyncProfile profile;
    std::string error;
    ProfileInfo info;
    CHECK(library.CreateProfile("Large Delete retry", info, error));
    const auto path = library.ProfilePath(info);
    CHECK(profile.Open(path, error));
    const std::string device = "machine:large-delete-retry";
    const auto id = Save(profile, PimKind::contacts, {{"firstName", "Delete me"}}).id;
    const std::string picture(48 * 1024, 'A');
    Bytes request, answer;
    int64_t deletedRevision;
    std::string next;
    {
        AirSyncServer server(profile, device, {PimKind::contacts});
        Client client{server};
        auto key = NextKey(client.Post("Sync", Hex(contactsStart)));
        key = NextKey(client.Sync(contactsChanges, key));
        for (int i = 0; i < 6; ++i)
            Save(profile, PimKind::contacts, {{"firstName", "Picture " + std::to_string(i)}, {"airsyncPicture", picture}});
        WbxmlNode remove(0, Delete);
        remove.Add(0, ServerId, "1:" + std::to_string(id));
        request = SyncWith(contactsChanges, key, {remove});
        const auto reply = client.Post("Sync", request);
        next = NextKey(reply);
        CHECK(CountTag(reply, 1, 0x3c) == 6 && CountTag(reply, 0, MoreAvailable) == 0);
        answer = client.last.body;
        CHECK(answer.size() > 256 * 1024);
        CHECK(profile.FindPimRecord(id)->deleted);
        deletedRevision = profile.FindPimRecord(id)->revision;
    }

    profile.Close();
    CHECK(profile.Open(path, error));
    AirSyncServer server(profile, device, {PimKind::contacts});
    Client client{server};
    CHECK(NextKey(client.Post("Sync", request)) == next && client.last.body == answer);
    CHECK(profile.FindPimRecord(id)->revision == deletedRevision && server.Counts().deleted == 0);
    const auto settled = client.Sync(contactsChanges, next);
    CHECK(TextOf(settled, 0, Status) == "1" && CountTag(settled, 0, Commands) == 0);
}

void TestNotesUnavailable(SyncLibrary& library) {
    SyncProfile profile;
    OpenProfile(library, "Notes unavailable", profile);
    const auto memo = Save(profile, PimKind::memos, {{"text", "Palm memo"}});
    AirSyncServer server(profile, "machine:notes", {PimKind::contacts, PimKind::calendar, PimKind::tasks, PimKind::memos});
    Client client{server};
    const auto folders = client.Post("FolderSync", Hex(folderSync));
    CHECK(TextOf(folders, 7, 0x17) == "3" && CountTag(folders, 7, 0x0f) == 3);
    CHECK(AirSyncFolderId(PimKind::memos).empty());

    // Even with a valid Contacts id, a Notes class cannot address it.
    auto sync = Decode(Hex(contactsStart));
    CHECK(Set(sync, 0, 0x10, "Notes"));
    CHECK(TextOf(client.Post("Sync", EncodeWbxml(sync)), 0, Status) == "8");
    auto request = Decode(Hex(estimate));
    CHECK(Set(request, 6, 0x09, "Notes"));
    const auto result = client.Post("GetItemEstimate", EncodeWbxml(request));
    CHECK(TextOf(result, 6, 0x0e) == "2" && CountTag(result, 6, 0x0c) == 2);
    CHECK(profile.FindPimRecord(memo.id)->fields.at("text") == "Palm memo");
}

void TestBadInput(SyncLibrary& library) {
    SyncProfile profile;
    OpenProfile(library, "Bad input", profile);
    AirSyncServer server(profile, "machine:bad", {PimKind::calendar});
    Client client{server};
    {
        const auto response = client.Sync(calendarStart, "");
        CHECK(TextOf(response, 0, Status) == "3" && TextOf(response, 0, SyncKey) == "0");
    }
    {
        const auto counts = client.Post("GetItemEstimate", Hex(estimate));
        CHECK(TextOf(counts, 6, 0x0e, 0) == "2" && TextOf(counts, 6, 0x0e, 1) == "1" && TextOf(counts, 6, 0x0e, 2) == "2");
        CHECK(CountTag(counts, 6, 0x0c) == 1);
    }
    auto key = NextKey(client.Post("Sync", Hex(calendarStart)));
    const std::pair<const char*, const char*> counts[] = {{"2000000000", ""}, {"99999999999", ""}, {"3", "2026-12-08"}};
    int n = 0;
    for (const auto& [occurrences, until] : counts) {
        auto data = Node(4, {{0x06, "0"}, {0x12, "20261124T110000Z"}, {0x26, "Weekly"}, {0x27, "20261124T100000Z"}});
        auto& rec = data.Add(4, 0x1b);
        rec.Add(4, 0x1c, "1"), rec.Add(4, 0x1e, occurrences), rec.Add(4, 0x1f, n < 2 ? "99" : "1"), rec.Add(4, 0x20, "4");
        const auto start = std::clock();
        const auto response =
            client.Post("Sync", SyncWith(calendarChanges, key, {Command(Add, std::to_string(70 + n++), data)}));
        CHECK(std::clock() - start < CLOCKS_PER_SEC);
        key = NextKey(response);
        const auto record = profile.FindPimRecord(std::stoll(TextOf(response, 0, ServerId).substr(2)));
        CHECK(record && record->fields.at("repeatUntil") == until);
    }
    {
        // Content-Length 2^32 + 1 used to read as 1.
        FakeStream stream;
        const std::string text = "POST /Microsoft-Server-ActiveSync?Cmd=Sync HTTP/1.1\r\nContent-Length: 4294967297\r\n\r\nX";
        stream.input.assign(text.begin(), text.end());
        server.Serve(stream);
        CHECK(stream.output.empty());
        FakeStream options;
        const std::string ok = "OPTIONS /Microsoft-Server-ActiveSync HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
        options.input.assign(ok.begin(), ok.end());
        server.Serve(options);
        const std::string answer(options.output.begin(), options.output.end());
        CHECK(answer.compare(0, 15, "HTTP/1.1 200 OK") == 0);
    }
    CHECK(server.LastError().empty());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    // Times without a device zone use the host's; make it UTC.
    setenv("TZ", "UTC0", 1);
    tzset();
    const std::filesystem::path root = argv[1];
    std::filesystem::remove_all(root);

    // WBXML: the captured FolderSync decodes and encodes back to the same bytes.
    {
        const auto bytes = Hex(folderSync);
        const auto node = Decode(bytes);
        CHECK(node.page == 7 && node.tag == 0x16 && node.Text(7, 0x12) == "0");
        CHECK(EncodeWbxml(node) == bytes);
        const auto task = Hex(taskChange);
        CHECK(EncodeWbxml(Decode(task)) == task);
        WbxmlNode bad;
        std::string error;
        CHECK(!DecodeWbxml(bytes.data(), bytes.size() - 2, bad, error));
        // A body that ends after the header or a page switch.
        for (const char* hex : {"03016a00", "03016a0000", "03016a000001"}) {
            const auto body = Hex(hex);
            CHECK(!DecodeWbxml(body.data(), body.size(), bad, error));
        }
        // Nesting past 64 levels.
        const auto nested = [](int depth) {
            Bytes body{0x03, 0x01, 0x6a, 0x00};
            body.insert(body.end(), size_t(depth), 0x45);
            body.insert(body.end(), size_t(depth), 0x01);
            return body;
        };
        CHECK(DecodeWbxml(nested(60).data(), 4 + 120, bad, error));
        CHECK(!DecodeWbxml(nested(70).data(), 4 + 140, bad, error));
        // Opaque lengths past 32 bits or past the body.
        for (const char* hex : {"03016a0045c39080808002414201", "03016a0045c3104142"}) {
            const auto body = Hex(hex);
            CHECK(!DecodeWbxml(body.data(), body.size(), bad, error));
        }
    }

    SyncLibrary library;
    std::string error;
    CHECK(library.Open(root.string(), error));
    ProfileInfo info;
    CHECK(library.CreateProfile("Test", info, error));
    SyncProfile profile;
    CHECK(profile.Open(library.ProfilePath(info), error));

    PimRecord ada;
    ada.kind = PimKind::contacts;
    ada.fields = {{"firstName", "Ada"}, {"lastName", "Lovelace"}, {"company", "Analytical Engines"},
                  {"phone1", "555-0101"}, {"phoneLabel1", "0"}, {"phone2", "ada@example.org"}, {"phoneLabel2", "4"},
                  {"phone3", "555-0199"}, {"phoneLabel3", "7"}, {"custom1", "kept"}};
    CHECK(profile.SavePimRecord(ada, error));
    PimRecord meeting;
    meeting.kind = PimKind::calendar;
    meeting.fields = {{"date", "2026-11-24"}, {"startTime", "10:00"}, {"endTime", "11:30"}, {"summary", "Frond desk meeting"},
                      {"location", "Room 4"}, {"alarmAdvance", "15"}, {"alarmUnit", "0"}, {"repeatType", "2"},
                      {"repeatOn", "4"}, {"repeatInterval", "1"}, {"repeatUntil", ""}, {"repeatWeekStart", "0"}};
    CHECK(profile.SavePimRecord(meeting, error));
    PimRecord engine;
    engine.kind = PimKind::tasks;
    engine.fields = {{"summary", "Oil the engine"}, {"dueDate", "2026-11-26"}, {"priority", "2"}, {"complete", "0"}};
    CHECK(profile.SavePimRecord(engine, error));

    AirSyncServer server(profile, "machine:test", {PimKind::contacts, PimKind::calendar, PimKind::tasks});
    Client client{server};

    // OPTIONS offers 2.5, the version the device asks for.
    {
        AirSyncRequest options;
        options.method = "OPTIONS";
        options.target = "/Microsoft-Server-ActiveSync?User=DEFAULT&DeviceId=B97255B2BF6FC351D0C41D5FD635657D&DeviceType=PocketPC";
        const auto response = server.Handle(options);
        bool versions = false;
        for (const auto& [name, value] : response.headers)
            versions |= name == "MS-ASProtocolVersions" && value.find("2.5") != std::string::npos;
        CHECK(response.status == 200 && versions);
        AirSyncRequest status;
        status.method = "POST";
        status.target = "/Microsoft-Server-ActiveSync/SyncStat.dll";
        const std::string text = "<SyncStatus><SyncBegin Partner=\"\" Datatype=\"\" /></SyncStatus>";
        status.body.assign(text.begin(), text.end());
        const auto answer = server.Handle(status);
        CHECK(answer.status == 200 && answer.body.empty());
    }

    // FolderSync: one folder each, fixed ServerIds and default folder types.
    {
        const auto folders = client.Post("FolderSync", Hex(folderSync));
        CHECK(TextOf(folders, 7, 0x0c) == "1" && TextOf(folders, 7, 0x12) == "1" && TextOf(folders, 7, 0x17) == "3");
        CHECK(TextOf(folders, 7, 0x08, 0) == AirSyncFolderId(PimKind::contacts) && TextOf(folders, 7, 0x0a, 0) == "9");
        CHECK(TextOf(folders, 7, 0x08, 1) == AirSyncFolderId(PimKind::calendar) && TextOf(folders, 7, 0x0a, 1) == "8");
        CHECK(TextOf(folders, 7, 0x08, 2) == AirSyncFolderId(PimKind::tasks) && TextOf(folders, 7, 0x0a, 2) == "7");
    }

    // Initial sync: key 0 gives key 1 and no items; the next request gets the profile's.
    std::string contactsKey = NextKey(client.Post("Sync", Hex(contactsStart)));
    std::string calendarKey = NextKey(client.Post("Sync", Hex(calendarStart)));
    std::string tasksKey = NextKey(client.Post("Sync", Hex(tasksStart)));
    CHECK(contactsKey == "1" && calendarKey == "1" && tasksKey == "1");
    CHECK(CountTag(Decode(client.last.body), 0, Commands) == 0);
    {
        const auto counts = client.Post("GetItemEstimate", Hex(estimate));
        CHECK(TextOf(counts, 6, 0x0c, 0) == "1" && TextOf(counts, 6, 0x0c, 1) == "1" && TextOf(counts, 6, 0x0c, 2) == "1");
    }
    const auto adaId = "1:" + std::to_string(ada.id), meetingId = "2:" + std::to_string(meeting.id),
               engineId = "3:" + std::to_string(engine.id);
    {
        const auto response = client.Sync(contactsChanges, contactsKey);
        contactsKey = NextKey(response);
        CHECK(contactsKey == "2" && CountTag(response, 0, Add) == 1 && TextOf(response, 0, ServerId) == adaId);
        CHECK(TextOf(response, 1, 0x1f) == "Ada" && TextOf(response, 1, 0x29) == "Lovelace");
        CHECK(TextOf(response, 1, 0x13) == "555-0101" && TextOf(response, 1, 0x1b) == "ada@example.org" &&
              TextOf(response, 1, 0x2b) == "555-0199");
        // A repeated request, as after a lost answer, gets the same answer and changes nothing.
        const auto again = client.Sync(contactsChanges, "1");
        CHECK(EncodeWbxml(again) == EncodeWbxml(response));
    }
    {
        const auto response = client.Sync(calendarChanges, calendarKey);
        calendarKey = NextKey(response);
        CHECK(TextOf(response, 0, ServerId) == meetingId && TextOf(response, 4, 0x26) == "Frond desk meeting");
        CHECK(TextOf(response, 4, 0x27) == "20261124T100000Z" && TextOf(response, 4, 0x12) == "20261124T113000Z");
        CHECK(TextOf(response, 4, 0x24) == "15" && TextOf(response, 4, 0x1c) == "1" && TextOf(response, 4, 0x20) == "4");
    }
    {
        const auto response = client.Sync(tasksChanges, tasksKey);
        tasksKey = NextKey(response);
        CHECK(TextOf(response, 0, ServerId) == engineId && TextOf(response, 9, 0x20) == "Oil the engine");
        CHECK(TextOf(response, 9, 0x0c) == "2026-11-26T00:00:00.000Z" && TextOf(response, 9, 0x0e) == "1");
    }

    // Client adds get a record and a ServerId for their ClientId.
    {
        const auto response = client.Sync(contactsAdd, contactsKey);
        contactsKey = NextKey(response);
        CHECK(TextOf(response, 0, ClientId) == "2147483650" && TextOf(response, 0, Status, 1) == "1");
        const auto serverId = TextOf(response, 0, ServerId);
        const auto record = profile.FindPimRecord(std::stoll(serverId.substr(2)));
        CHECK(record && record->fields.at("firstName") == "Grace" && record->fields.at("lastName") == "Hopper" &&
              record->fields.at("company") == "Navy");
    }
    // The device's zone comes with its calendar add; the profile's meeting goes again in it.
    {
        const auto response = client.Sync(calendarAdd, calendarKey);
        calendarKey = NextKey(response);
        CHECK(TextOf(response, 0, ClientId) == "1073741826");
        const auto serverId = TextOf(response, 0, ServerId);
        const auto record = profile.FindPimRecord(std::stoll(serverId.substr(2)));
        CHECK(record && record->fields.at("summary") == "Tea with Grace" && record->fields.at("date") == "2006-01-01" &&
              record->fields.at("startTime") == "12:00" && record->fields.at("endTime") == "13:00" &&
              record->fields.at("location") == "Library");
        CHECK(CountTag(response, 0, Change) == 1 && TextOf(response, 0, ServerId, 1) == meetingId);
        CHECK(TextOf(response, 4, 0x27) == "20261124T180000Z");  // 10:00 Pacific standard time
    }
    // Client changes go through the link.
    {
        const auto response = client.Sync(taskChange, tasksKey, engineId);
        tasksKey = NextKey(response);
        const auto record = profile.FindPimRecord(engine.id);
        CHECK(record && record->fields.at("complete") == "1" && record->fields.at("completionDate") == "2006-01-01");
        CHECK(record->fields.at("priority") == "2");  // same importance keeps the Palm priority
        CHECK(CountTag(response, 0, Commands) == 0);
    }
    {
        const auto response = client.Sync(taskAdd, tasksKey);
        tasksKey = NextKey(response);
        CHECK(TextOf(response, 0, ClientId) == "3221225474");
    }

    // A contact change keeps fields AirSync doesn't carry and phones in their slots.
    auto change = Decode(Hex(contactsAdd));
    {
        auto* commands = const_cast<WbxmlNode*>(Find(change, 0, Commands));
        auto& item = commands->children.at(0);
        item.tag = Change;
        item.children.at(0) = WbxmlNode(0, ServerId, adaId);
        auto& data = item.children.at(1);
        data.children = {WbxmlNode(1, 0x1f, "Ada"), WbxmlNode(1, 0x29, "Lovelace"), WbxmlNode(1, 0x19, "Engines Ltd"),
                         WbxmlNode(1, 0x13, "555-0101"), WbxmlNode(1, 0x1b, "\"Ada\" <ada@example.org>"),
                         WbxmlNode(1, 0x27, "555-0123")};
        CHECK(Set(change, 0, SyncKey, contactsKey));
        const auto response = client.Post("Sync", EncodeWbxml(change));
        contactsKey = NextKey(response);
        const auto record = profile.FindPimRecord(ada.id);
        CHECK(record->fields.at("company") == "Engines Ltd" && record->fields.at("custom1") == "kept");
        CHECK(record->fields.at("phone2") == "ada@example.org" && record->fields.at("phoneLabel2") == "4");
        // The mobile number went, so the new home number takes its slot.
        CHECK(record->fields.at("phone3") == "555-0123" && record->fields.at("phoneLabel3") == "1");
    }

    // Conflict: both sides changed the same contact. Each side ends with both versions.
    {
        auto record = *profile.FindPimRecord(ada.id);
        record.fields["company"] = "Desktop Engines";
        CHECK(profile.SavePimRecord(record, error));
        CHECK(Set(change, 1, 0x19, "Device Engines") && Set(change, 0, SyncKey, contactsKey));
        const auto response = client.Post("Sync", EncodeWbxml(change));
        contactsKey = NextKey(response);
        CHECK(server.Counts().conflicts == 1);
        CHECK(profile.FindPimRecord(ada.id)->fields.at("company") == "Desktop Engines");
        int copies = 0;
        for (const auto& contact : profile.PimRecords(PimKind::contacts))
            copies += contact.fields.count("company") && contact.fields.at("company") == "Device Engines";
        CHECK(copies == 1);
        CHECK(CountTag(response, 0, Change) == 1 && CountTag(response, 0, Add) == 1);
        CHECK(TextOf(response, 1, 0x19) == "Desktop Engines" && TextOf(response, 1, 0x19, 1) == "Device Engines");
    }

    // Deletes both ways.
    {
        WbxmlNode request = Decode(Hex(contactsChanges));
        CHECK(Set(request, 0, SyncKey, contactsKey));
        auto* collection = const_cast<WbxmlNode*>(Find(request, 0, 0x0f));
        collection->Add(0, Commands).Add(0, Delete).Add(0, ServerId, adaId);
        const auto response = client.Post("Sync", EncodeWbxml(request));
        contactsKey = NextKey(response);
        CHECK(profile.FindPimRecord(ada.id)->deleted);
        CHECK(CountTag(response, 0, Commands) == 0);

        const auto tea = profile.PimRecords(PimKind::calendar);
        CHECK(tea.size() == 2);
        CHECK(profile.DeletePimRecord(meeting.id, profile.FindPimRecord(meeting.id)->revision, error));
        const auto deleted = client.Sync(calendarChanges, calendarKey);
        calendarKey = NextKey(deleted);
        CHECK(CountTag(deleted, 0, Delete) == 1 && TextOf(deleted, 0, ServerId) == meetingId);
    }

    // A stale key makes the device start again.
    {
        const auto response = client.Sync(tasksChanges, "99");
        CHECK(TextOf(response, 0, Status) == "3");
    }
    CHECK(server.LastError().empty());

    TestTimeZoneRead();
    TestKnownZone(library);
    TestDatelessEvent(library);
    TestFirstUpload(library);
    TestZoneAcrossRequests(library);
    TestBirthdays(library);
    TestBadInput(library);
    TestGhostedPhones(library);
    TestLegacyPhoneClear(library);
    TestLegacyBodies(library);
    TestWm5ContactNote(library);
    TestDeleteRetry(library);
    TestLargeDeleteRetry(library);
    TestNotesUnavailable(library);
    std::printf("sync-airsync: ok\n");
    return 0;
}
