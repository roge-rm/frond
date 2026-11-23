#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>

#include "ActiveSyncFiles.h"
#include "PwiNote.h"
#include "SyncStore.h"

using namespace frondSync;
namespace fs = std::filesystem;

struct FrondRapi {
    std::map<std::string, Bytes> files;
    std::string open;
    size_t offset{};
    std::function<void(const std::string&, const std::string&)> beforeMove;
    std::function<void(const std::string&, const std::string&)> afterMove;
    bool connected{true};
};

namespace {
constexpr char prefix[] = "\\My Documents\\Frond Sync\\";
int failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %d: %s\n", __LINE__, #expr); ++failures; } } while (0)

std::string RelativePath(const uint16_t* path) {
    std::string absolute;
    while (*path) absolute.push_back(char(*path++));
    CHECK(absolute.compare(0, sizeof(prefix) - 1, prefix) == 0);
    return absolute.substr(sizeof(prefix) - 1);
}

void Put(const fs::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    CHECK(bool(file));
}

Bytes Get(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    CHECK(bool(file));
    return Bytes(std::istreambuf_iterator<char>(file), {});
}

void Reconcile(const fs::path& profilePath) {
    std::string error;
    SyncProfile profile;
    fs::create_directory(profilePath);
    if (!profile.Open(profilePath.string(), error)) throw std::runtime_error(error);
    const auto folder = fs::path(ActiveSyncFilesFolder(profilePath.string(), true));
    FrondRapi guest;
    const auto pass = [&] { return ExchangeActiveSyncFiles(&guest, profile, "device", profilePath.string(), true); };

    Put(folder / "plain.txt", {'a'});
    guest.files["Test.pwi"] = WritePwiNote("guest text").value();
    CHECK(pass().find("1 sent, 1 received, 0 differed") != std::string::npos);
    CHECK(guest.files.at("plain.txt") == Bytes{'a'});
    auto memos = profile.PimRecords(PimKind::memos);
    CHECK(memos.size() == 1);
    auto memo = memos.at(0);
    CHECK(memo.category == "WM5 Notes" && memo.fields.at("text") == "guest text");

    guest.files["Test.pwi"] = WritePwiNote("guest edit").value();
    Put(folder / "plain.txt", {'b'});
    CHECK(pass().find("1 sent, 1 received, 0 differed") != std::string::npos);
    CHECK(guest.files.at("plain.txt") == Bytes{'b'});
    CHECK(ReadPwiNote(Get(folder / "Test.pwi")) == "guest edit");
    memo = profile.FindPimRecord(memo.id).value();
    CHECK(memo.fields.at("text") == "guest edit");

    memo.fields["text"] = "host edit";
    CHECK(profile.SavePimRecord(memo, error));
    guest.files.erase("plain.txt");
    CHECK(pass().find("1 sent, 0 received, 0 differed") != std::string::npos);
    CHECK(!fs::exists(folder / "plain.txt"));
    CHECK(ReadPwiNote(guest.files.at("Test.pwi")) == "host edit");

    // A guest edit while parking the old version must never be replaced.
    Put(folder / "Test.pwi", WritePwiNote("local file edit").value());
    const auto raced = WritePwiNote("raced guest save").value();
    const auto later = WritePwiNote("later guest save").value();
    guest.beforeMove = [&](const auto& source, const auto& destination) {
        if (source == "Test.pwi" && destination.find(".frond-recovery") == 0)
            guest.files[source] = raced;
    };
    guest.afterMove = [&](const auto& source, const auto& destination) {
        if (source == "Test.pwi" && destination.find(".frond-recovery") == 0)
            guest.files[source] = later;
    };
    try {
        pass();
        CHECK(false);
    } catch (const std::runtime_error&) {}
    guest.beforeMove = {};
    guest.afterMove = {};
    CHECK(guest.files.at("Test.pwi") == later);
    CHECK(std::any_of(guest.files.begin(), guest.files.end(), [&](const auto& entry) {
        return entry.first.find(".frond-recovery") == 0 && entry.second == raced;
    }));
    CHECK(pass().find("1 differed") != std::string::npos);
    CHECK(ReadPwiNote(Get(folder / "Test.pwi")) == "local file edit");

    // Resolve the conflict, then propagate deletion without a new memo or a resurrection.
    Put(folder / "Test.pwi", later);
    pass();
    memo = profile.FindPimRecord(memo.id).value();
    CHECK(memo.fields.at("text") == "later guest save");
    CHECK(profile.DeletePimRecord(memo.id, memo.revision, error));
    CHECK(pass().find("1 removed") != std::string::npos);
    CHECK(!fs::exists(folder / "Test.pwi") && !guest.files.count("Test.pwi"));
    CHECK(profile.PimRecords(PimKind::memos).empty());

    PimRecord pending;
    pending.category = "WM5 Notes";
    pending.fields["text"] = "new profile memo";
    CHECK(profile.SavePimRecord(pending, error));
    PimRecord secret = pending;
    secret.id = 0;
    secret.secret = true;
    CHECK(profile.SavePimRecord(secret, error));
    CHECK(pass().find("1 sent, 0 received, 0 differed") != std::string::npos);
    CHECK(ReadPwiNote(guest.files.at("Frond Memo " + std::to_string(pending.id) + ".pwi")) == "new profile memo");
    CHECK(!guest.files.count("Frond Memo " + std::to_string(secret.id) + ".pwi"));

    const auto pendingName = "Frond Memo " + std::to_string(pending.id) + ".pwi";
    pending = profile.FindPimRecord(pending.id).value();
    pending.secret = true;
    CHECK(profile.SavePimRecord(pending, error));
    FrondRapi secondGuest;
    ExchangeActiveSyncFiles(&secondGuest, profile, "another device", profilePath.string(), true);
    CHECK(!secondGuest.files.count(pendingName));
    CHECK(guest.files.count(pendingName));

    pending.secret = false;
    pending.fields["text"] = "edited memo";
    CHECK(profile.SavePimRecord(pending, error));
    Put(folder / pendingName, WritePwiNote("different file").value());
    CHECK(pass().find("Notes: 1 differed") != std::string::npos);
    CHECK(ReadPwiNote(guest.files.at(pendingName)) == "new profile memo");
    CHECK(profile.FindPimRecord(pending.id)->fields.at("text") == "edited memo");

    guest.files["Test.pwi"] = WritePwiNote("note name reused").value();
    pass();
    auto live = profile.PimRecords(PimKind::memos);
    CHECK(std::any_of(live.begin(), live.end(), [&](const auto& record) {
        return record.id != memo.id && record.fields.at("text") == "note name reused";
    }));

    Put(folder / "interrupted.txt", {'a'});
    pass();
    Put(folder / "interrupted.txt", {'b'});
    guest.afterMove = [&](const auto& source, const auto&) {
        if (source == "interrupted.txt") guest.connected = false;
    };
    try {
        pass();
        CHECK(false);
    } catch (const std::runtime_error&) {}
    guest.afterMove = {};
    guest.connected = true;
    CHECK(!guest.files.count("interrupted.txt"));
    pass();
    CHECK(guest.files.at("interrupted.txt") == Bytes{'b'});
    CHECK(std::any_of(guest.files.begin(), guest.files.end(), [](const auto& entry) {
        return entry.first.find("-interrupted.txt") != std::string::npos && entry.second == Bytes{'a'};
    }));
}

}  // namespace

bool FrondRapiConnected(const FrondRapi* client) { return client->connected; }
bool FrondRapiMakeDirectory(FrondRapi*, const uint16_t*) { return true; }
bool FrondRapiList(FrondRapi* client, const uint16_t*, FrondRapiEntry entry, void* context) {
    for (const auto& [name, bytes] : client->files) {
        if (name.find('\\') != std::string::npos) continue;
        const std::vector<uint16_t> wide(name.begin(), name.end());
        if (!entry(context, wide.data(), wide.size(), 0, bytes.size())) return false;
    }
    return true;
}
uint32_t FrondRapiCreate(FrondRapi* client, const uint16_t* path) {
    const auto name = RelativePath(path);
    if (!client->files.try_emplace(name).second) return UINT32_MAX;
    client->open = name;
    client->offset = 0;
    return 1;
}
uint32_t FrondRapiOpenFile(FrondRapi* client, const uint16_t* path) {
    const auto name = RelativePath(path);
    if (!client->files.count(name)) return UINT32_MAX;
    client->open = name;
    client->offset = 0;
    return 1;
}
bool FrondRapiRead(FrondRapi* client, uint32_t, void* data, uint32_t size, uint32_t* count) {
    const auto& bytes = client->files.at(client->open);
    *count = std::min<size_t>(size, bytes.size() - client->offset);
    std::copy_n(bytes.begin() + client->offset, *count, static_cast<uint8_t*>(data));
    client->offset += *count;
    return true;
}
bool FrondRapiWrite(FrondRapi* client, uint32_t, const void* data, uint32_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    auto& file = client->files.at(client->open);
    file.insert(file.end(), bytes, bytes + size);
    return true;
}
bool FrondRapiCloseFile(FrondRapi* client, uint32_t) {
    client->open.clear();
    return true;
}
bool FrondRapiMove(FrondRapi* client, const uint16_t* source, const uint16_t* destination) {
    const auto from = RelativePath(source), to = RelativePath(destination);
    if (client->beforeMove) client->beforeMove(from, to);
    if (!client->files.count(from) || client->files.count(to)) return false;
    auto file = client->files.extract(from);
    file.key() = to;
    client->files.insert(std::move(file));
    if (client->afterMove) client->afterMove(from, to);
    return client->connected;
}
bool FrondRapiRemove(FrondRapi* client, const uint16_t* path) {
    return client->files.erase(RelativePath(path));
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const fs::path profile = fs::path(argv[1]) / "files-profile";
    try {
        Reconcile(profile);
    } catch (const std::exception& failure) {
        std::printf("FAIL: %s\n", failure.what());
        ++failures;
    }
    fs::remove_all(profile);
    if (!failures) std::printf("sync-files: ok\n");
    return failures ? 1 : 0;
}
