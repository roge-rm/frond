#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "PimSync.h"

using namespace frondSync;

namespace {

void Check(bool ok, const char* expression, int line) {
    if (ok) return;
    std::fprintf(stderr, "sync-pim.cpp:%d: %s\n", line, expression);
    std::exit(1);
}

#define CHECK(...) Check(bool((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

Bytes WithText(Bytes bytes, const char* text) {
    const std::string s = text;
    bytes.insert(bytes.end(), s.begin(), s.end());
    bytes.push_back(0);
    return bytes;
}

void Append16(Bytes& b, unsigned v) {
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}

void Append32(Bytes& b, uint32_t v) {
    Append16(b, v >> 16);
    Append16(b, v & 65535);
}

uint32_t Int(const Bytes& data, unsigned offset, unsigned size) {
    uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i) value = (value << 8) | data.at(offset + i);
    return value;
}

struct Example {
    PimKind kind;
    PalmPimFormat format;
    Bytes bytes;
    std::string name;
    std::string field;
    std::string value;
};

std::vector<Example> Examples() {
    const auto classic = PalmPimFormat::classic, garnet = PalmPimFormat::garnet;
    const auto classicContact = WithText({0x00, 0x05, 0x47, 0x10, 0, 0, 0, 1, 0}, "Drummond");
    Bytes garnetContact = WithText({0x03, 0x65, 0x47, 0x10, 2, 0x10, 0, 0x21, 0, 0, 0, 1, 0, 0, 4, 1, 0},
                                   "Dubé");
    // Contact text is in the Palm's code page, the picture/unknown blob is opaque.
    const auto utf8 = std::find(garnetContact.begin(), garnetContact.end(), 0xc3);
    garnetContact.erase(utf8);
    garnetContact.at(20) = 0xe9;
    garnetContact = WithText(WithText(std::move(garnetContact), "Second address"), "Extended note");
    garnetContact.insert(garnetContact.end(), {'T', 'e', 's', 't', 0, 2, 0x88, 0x99});
    Bytes classicCalendar = WithText({10, 15, 11, 0, 0xf2, 0x39, 0x7c, 0x18,
                                      10, 0, 2, 0, 0xff, 0xff, 1, 2, 1, 0, 0, 1, 0xf2, 0x3a}, "Standup");
    classicCalendar = WithText(std::move(classicCalendar), "Agenda");
    Bytes garnetCalendar = WithText({0xff, 0xff, 0xff, 0xff, 0xf2, 0x39, 0x06, 0}, "Out of office");
    garnetCalendar = WithText(std::move(garnetCalendar), "Workshop");
    garnetCalendar.insert(garnetCalendar.end(), {'B', 'd', '0', '2', 0, 2, 1, 2});
    auto classicTask = WithText(WithText({0xff, 0xff, 0x82}, "Due today"), "");
    auto garnetTask = WithText(WithText({0xfc, 0, 0, 3, 0, 2, 0xf2, 0x39, 0xf2, 0x3a, 9, 30, 0, 1,
                                        0xf2, 0x39, 1, 0, 0xff, 0xff, 1, 0, 0, 0}, "Recurring"), "Task note");
    return {
        {PimKind::contacts, classic, classicContact, "AddressDB", "lastName", "Drummond"},
        {PimKind::contacts, garnet, garnetContact, "ContactsDB-PAdd", "lastName", "Dubé"},
        {PimKind::calendar, classic, classicCalendar, "DatebookDB", "summary", "Standup"},
        {PimKind::calendar, garnet, garnetCalendar, "CalendarDB-PDat", "summary", "Out of office"},
        {PimKind::tasks, classic, classicTask, "ToDoDB", "summary", "Due today"},
        {PimKind::tasks, garnet, garnetTask, "TasksDB-PTod", "summary", "Recurring"},
        {PimKind::memos, classic, WithText({}, "Note"), "MemoDB", "text", "Note"},
        {PimKind::memos, garnet, WithText({}, "Call Dan"), "MemosDB-PMem", "text", "Call Dan"},
    };
}

void TestSelection() {
    DbInfo classic, garnet;
    classic.name = "MemoDB";
    classic.creator = FourCc("memo");
    classic.type = FourCc("DATA");
    garnet.name = "MemosDB-PMem";
    garnet.creator = FourCc("PMem");
    garnet.type = FourCc("DATA");
    CHECK(SelectPalmPimDatabases({classic}).at(0).format == PalmPimFormat::classic);
    CHECK(SelectPalmPimDatabases({classic, garnet}).at(0).format == PalmPimFormat::garnet);
    garnet.attributes = dbReadOnly;
    CHECK(SelectPalmPimDatabases({classic, garnet}).empty());
}

void TestCodecs() {
    for (const auto& example : Examples()) {
        PimRecord r;
        std::string error;
        CHECK(DecodePalmPim(example.kind, example.format, example.bytes, r, error));
        CHECK(r.fields.at(example.field) == example.value);
        Bytes encoded;
        CHECK(EncodePalmPim(r, example.format, encoded, error));
        CHECK(encoded == example.bytes);
        auto extras = r.extras;
        if (example.name == "ContactsDB-PAdd") {
            CHECK(r.fields.at("street2") == "Second address");
            CHECK(r.fields.at("note") == "Extended note");
        }
        r.fields[example.field] = "Edited on desktop";
        CHECK(EncodePalmPim(r, example.format, encoded, error));
        CHECK(DecodePalmPim(example.kind, example.format, encoded, r, error));
        CHECK(r.fields.at(example.field) == "Edited on desktop");
        CHECK(r.extras == extras);
        CHECK(!DecodePalmPim(example.kind, example.format, {}, r, error));
    }

    Bytes all;
    for (unsigned i = 1; i < 256; ++i) all.push_back(uint8_t(i));
    Bytes encoded;
    CHECK(EncodePalmPimText(PalmPimText(all), encoded) && encoded == all);
    CHECK(!EncodePalmPimText("漢字", encoded));

    const auto examples = Examples();
    PimRecord classic, garnet;
    std::string error;
    CHECK(DecodePalmPim(PimKind::contacts, PalmPimFormat::classic, examples[0].bytes, classic, error));
    CHECK(DecodePalmPim(PimKind::contacts, PalmPimFormat::garnet, examples[1].bytes, garnet, error));
    garnet.fields["webpage"] = "https://example.test/";
    garnet.fields["firstName"] = "First";
    const auto extras = garnet.extras;
    ApplyPalmPim(classic, PalmPimFormat::classic, garnet);
    CHECK(garnet.fields.at("lastName") == "Drummond" && garnet.fields.count("firstName") == 0);
    CHECK(garnet.fields.count("webpage") == 1);
    CHECK(garnet.extras == extras);
}

class Device : public PacketLink {
   public:
    Device() : categories(300) {
        SetLabel(8, "Work");
        categories[290] = 0xe3;  // Database-specific settings after the category section.
    }

    void SetLabel(unsigned slot, const std::string& name) {
        const auto begin = categories.begin() + 2 + slot * 16;
        std::fill_n(begin, 16, 0);
        std::copy(name.begin(), name.end(), begin);
    }

    bool Send(const Bytes& request) override {
        uint8_t function = 0;
        std::vector<DlpArg> args;
        CHECK(DlpParseRequest(request, function, args));
        calls.push_back(function);
        Bytes arg;
        if (!args.empty()) arg = args.front().data;
        Bytes data;
        uint16_t rc = 0;
        switch (function) {
            case dlpOpenConduit: break;
            case dlpOpenDB:
                CHECK(arg.at(1) & 0x10);
                data = {handle};
                break;
            case dlpCloseDB: break;
            case dlpReadAppBlock:
                Append16(data, unsigned(categories.size()));
                data.insert(data.end(), categories.begin(), categories.end());
                break;
            case dlpWriteAppBlock:
                CHECK(arg.at(0) == handle && Int(arg, 2, 2) == categories.size());
                categories.assign(arg.begin() + 4, arg.end());
                CHECK(categories.at(290) == 0xe3);
                break;
            case dlpReadOpenDBInfo: Append16(data, unsigned(records.size())); break;
            case dlpReadRecord: {
                const auto index = Int(arg, 2, 2);
                if (index >= records.size()) rc = dlpErrNotFound;
                else data = Wire(records[index], index);
                break;
            }
            case dlpReadRecordIDList: {
                const auto start = Int(arg, 2, 2);
                CHECK(start <= records.size());
                Append16(data, unsigned(records.size() - start));
                for (auto index = start; index < records.size(); ++index) Append32(data, records[index].uid);
                break;
            }
            case dlpResetRecordIndex: cursor = 0; break;
            case dlpReadNextModifiedRec:
                while (cursor < records.size() && !(records[cursor].flags & (recDirty | recDeleted | recArchived))) ++cursor;
                if (cursor == records.size()) rc = dlpErrNotFound;
                else {
                    data = Wire(records[cursor], unsigned(cursor));
                    ++cursor;
                }
                break;
            case dlpWriteRecord: {
                CHECK(arg.at(6) == 0 || arg.at(6) == recSecret);
                auto uid = Int(arg, 2, 4);
                DlpRecord r{uid, arg.at(6), arg.at(7), Bytes(arg.begin() + 8, arg.end())};
                if (!uid) {
                    r.uid = uid = nextUid++;
                    records.push_back(r);
                } else {
                    auto existing = std::find_if(records.begin(), records.end(), [=](const auto& a) { return a.uid == uid; });
                    CHECK(existing != records.end());
                    *existing = r;
                }
                Append32(data, uid);
                if (loseWriteReply) {
                    loseWriteReply = false;
                    reply.clear();
                    error = "Simulated disconnect during write";
                    return true;
                }
                break;
            }
            case dlpDeleteRecord: {
                const auto uid = Int(arg, 2, 4);
                const auto it = std::find_if(records.begin(), records.end(), [=](const auto& a) { return a.uid == uid; });
                if (it == records.end()) rc = dlpErrNotFound;
                else records.erase(it);
                break;
            }
            case dlpCleanUpDatabase:
                records.erase(std::remove_if(records.begin(), records.end(), [](const auto& r) {
                    return r.flags & (recDeleted | recArchived);
                }), records.end());
                break;
            case dlpResetSyncFlags:
                for (auto& r : records) r.flags &= ~recDirty;
                break;
            default: CHECK(false);
        }
        reply = DlpResponse(function, rc, data.empty() ? std::vector<DlpArg>{} : std::vector<DlpArg>{{0x20, data}});
        return true;
    }

    bool Receive(Bytes& message, int) override {
        if (reply.empty()) return false;
        message = std::move(reply);
        return true;
    }

    std::string Name() const override { return "test"; }

    Bytes Wire(const DlpRecord& record, unsigned index) {
        Bytes data;
        Append32(data, record.uid);
        Append16(data, index);
        Append16(data, unsigned(record.data.size()));
        data.push_back(record.flags);
        data.push_back(record.category);
        data.insert(data.end(), record.data.begin(), record.data.end());
        return data;
    }

    std::vector<uint8_t> calls;
    std::vector<DlpRecord> records;
    bool loseWriteReply{false};

   private:
    static constexpr uint8_t handle = 7;
    size_t cursor{0};
    uint32_t nextUid{100};
    Bytes categories;
    Bytes reply;
};

void TestConduit(const Example& example, const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    SyncProfile profile;
    std::string error;
    CHECK(profile.Open(folder.string(), error));
    Device device;
    DlpClient dlp(device);
    const PalmPimDatabase db{example.kind, example.format, example.name};
    auto sync = [&](bool fast, bool success = true) {
        PimSyncCounts counts;
        error.clear();
        auto ok = SyncPalmPim(dlp, profile, "machine:test", "machine:test/user", db, fast, counts, error);
        if (ok != success) std::fprintf(stderr, "%s: %s\n", db.name.c_str(), error.c_str());
        CHECK(ok == success);
        return counts;
    };
    auto editGuest = [&](DlpRecord& wire, const std::string& text) {
        PimRecord r;
        CHECK(DecodePalmPim(example.kind, example.format, wire.data, r, error));
        r.fields[example.field] = text;
        CHECK(EncodePalmPim(r, example.format, wire.data, error));
        wire.flags |= recDirty;
    };

    device.records.push_back(DlpRecord{1, recDirty | recSecret, 8, example.bytes});
    CHECK(sync(false).downloaded == 1);
    auto records = profile.PimRecords(example.kind);
    CHECK(records.size() == 1 && records[0].category == "Work" && records[0].secret);
    CHECK(records[0].fields.at(example.field) == example.value);
    auto local = records.front();
    local.fields[example.field] = "Desktop edit";
    CHECK(profile.SavePimRecord(local, error));
    CHECK(!profile.SavePimRecord(records.front(), error));  // An optimistic update cannot lose an edit.
    device.calls.clear();
    CHECK(sync(true).uploaded == 1);
    CHECK(std::find(device.calls.begin(), device.calls.end(), dlpReadNextModifiedRec) != device.calls.end());
    CHECK(std::find(device.calls.begin(), device.calls.end(), dlpReadRecord) == device.calls.end());
    PimRecord decoded;
    CHECK(DecodePalmPim(example.kind, example.format, device.records.front().data, decoded, error));
    CHECK(decoded.fields.at(example.field) == "Desktop edit");
    CHECK(device.records.front().flags == recSecret && device.records.front().category == 8);

    editGuest(device.records.front(), "Handheld edit");
    CHECK(sync(true).downloaded == 1);
    local = *profile.FindPimRecord(local.id);
    CHECK(local.fields.at(example.field) == "Handheld edit");

    device.SetLabel(8, "Work renamed");
    CHECK(sync(true).downloaded == 1);
    local = *profile.FindPimRecord(local.id);
    CHECK(local.category == "Work renamed");

    local.fields[example.field] = "Desktop conflict";
    CHECK(profile.SavePimRecord(local, error));
    editGuest(device.records.front(), "Palm conflict");
    const auto conflict = sync(true);
    CHECK(conflict.conflicts == 1 && conflict.uploaded == 1);
    CHECK(profile.PimRecords(example.kind).size() == 2 && device.records.size() == 2);

    local.fields[example.field] = "Change beats delete";
    CHECK(profile.SavePimRecord(local, error));
    device.records.back().flags |= recDeleted | recDirty;
    CHECK(sync(true).uploaded == 1);
    CHECK(profile.PimRecords(example.kind).size() == 2 && device.records.size() == 2);

    PimRecord newRecord = local;
    newRecord.id = 0;
    newRecord.category = "New category";
    newRecord.fields[example.field] = "Retry me";
    CHECK(profile.SavePimRecord(newRecord, error));
    device.loseWriteReply = true;
    sync(true, false);
    CHECK(device.records.size() == 3);
    device.error.clear();
    sync(true);
    CHECK(device.records.size() == 3 && profile.PimRecords(example.kind).size() == 3);

    device.records.back().flags |= recDeleted | recArchived | recDirty;
    CHECK(sync(true).archived == 1);
    CHECK(profile.FindPimRecord(newRecord.id)->archived && device.records.size() == 2);

    CHECK(profile.SetPimAction("machine:test", example.kind, PimAction::skip, error));
    device.calls.clear();
    CHECK(sync(true).downloaded == 0 && device.calls.empty());

    CHECK(profile.SetPimAction("machine:test", example.kind, PimAction::handheldOverwrites, error));
    local = *profile.FindPimRecord(local.id);
    local.fields[example.field] = "Will be overwritten";
    CHECK(profile.SavePimRecord(local, error));
    sync(true);
    CHECK(profile.FindPimRecord(local.id)->fields.at(example.field) == "Change beats delete");
    CHECK(profile.GetPimAction("machine:test", example.kind) == PimAction::skip);

    CHECK(profile.SetPimAction("machine:test", example.kind, PimAction::desktopOverwrites, error));
    auto unmapped = device.records.front();
    unmapped.uid = 500;
    device.records.insert(device.records.begin(), unmapped);
    PimRecord invalid = local;
    invalid.id = 0;
    invalid.category = "a category longer than the handheld can store";
    CHECK(profile.SavePimRecord(invalid, error));
    device.calls.clear();
    sync(true, false);
    CHECK(device.records.front().uid == 500 &&
          std::find(device.calls.begin(), device.calls.end(), dlpDeleteRecord) == device.calls.end() &&
          std::find(device.calls.begin(), device.calls.end(), dlpWriteRecord) == device.calls.end());
    CHECK(profile.DeletePimRecord(invalid.id, invalid.revision, error));
    editGuest(device.records.back(), "Will be overwritten");
    CHECK(sync(true).uploaded == 1);
    CHECK(DecodePalmPim(example.kind, example.format, device.records.back().data, decoded, error));
    CHECK(decoded.fields.at(example.field) == "Change beats delete");
}

}  // namespace

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::filesystem::path scratch(argv[1]);
    std::filesystem::remove_all(scratch);
    TestSelection();
    TestCodecs();
    for (const auto& example : Examples()) TestConduit(example, scratch / example.name);
    std::filesystem::remove_all(scratch);
    std::puts("sync-pim: ok");
}
