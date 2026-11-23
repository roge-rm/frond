#include "../installer-shims/PalmConduitAbi.h"
#include "../installer-shims/PalmConduitProtocol.h"

#include <cstring>
#include <string>

using namespace frondConduit;

template <typename T, typename Id>
T SyncProc(Id ordinalOrName) {
    return reinterpret_cast<T>(GetProcAddress(GetModuleHandleW(L"Sync20.dll"), ordinalOrName));
}

template <typename T>
T SyncOrdinal(WORD ordinal) {
    return SyncProc<T>(MAKEINTRESOURCEA(ordinal));
}

// A live guest can verify the same bridge without the fake device's fixed handles.
long LiveFixture() {
    const auto create = SyncOrdinal<long (*)(CreateDb&)>(27);
    const auto close = SyncOrdinal<long (*)(BYTE)>(28);
    const auto write = SyncOrdinal<long (*)(Record&)>(32);
    const auto read = SyncOrdinal<long (*)(Record&)>(36);
    if (!create || !close || !write || !read) return 20;
    CreateDb database{};
    std::strcpy(database.name, "FrondConduitProof");
    database.creator = 0x46726f6e;
    database.type = 0x44415441;
    database.flags = 8;
    database.version = 1;
    if (const auto rc = create(database)) return rc;

    BYTE written[] = {'w', 'r', 'o', 't', 'e'};
    Record record{database.handle, 0, 0, 0, 0, 0, 0, sizeof(written), written, 0};
    if (const auto rc = write(record)) return rc;
    if (!record.id) return 21;

    BYTE received[16]{};
    Record check{database.handle, 0, 0, 0, 0, 0, 0, sizeof(received), received, 0};
    if (const auto rc = read(check)) return rc;
    if (check.id != record.id || check.size != sizeof(written) || std::memcmp(written, received, sizeof(written)))
        return 22;
    return close(database.handle);
}

bool CheckInstallUser(const SyncProperties& props) {
    const auto module = LoadLibraryExW(L"InstAide.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    if (!module) return false;
    const auto count = reinterpret_cast<int (WINAPI*)()>(GetProcAddress(module, "PltGetUserCount"));
    const auto userName = reinterpret_cast<int (WINAPI*)(int, char*, short*)>(
        GetProcAddress(module, "PltGetUser"));
    const auto directory = reinterpret_cast<int (WINAPI*)(const char*, char*, int*)>(
        GetProcAddress(module, "PltGetUserDirectory"));
    const auto install = reinterpret_cast<int (WINAPI*)(const char*, const char*)>(
        GetProcAddress(module, "PltInstallFile"));
    const auto remove = reinterpret_cast<int (WINAPI*)(const char*, const char*)>(
        GetProcAddress(module, "PltRemoveInstallFile"));
    const auto fileCount = reinterpret_cast<int (WINAPI*)(const char*, const char*)>(
        GetProcAddress(module, "PltGetFileCount"));
    const auto userId = reinterpret_cast<int (WINAPI*)(const char*, DWORD*)>(
        GetProcAddress(module, "PlmGetUserIDFromName"));
    const auto cardCount = reinterpret_cast<int (WINAPI*)(DWORD, const void*, int*)>(
        GetProcAddress(module, "PlmSlotGetFileCount"));
    const auto cardFile = reinterpret_cast<int (WINAPI*)(DWORD, const void*, int, void*, int*, void*)>(
        GetProcAddress(module, "PlmSlotGetFileInfo"));
    char path[256]{};
    char name[64]{};
    short nameSize = sizeof(name);
    int size = sizeof(path);
    DWORD id = 0;
    int cardFiles = 17;
    const bool ok = count && count() == 1 && userName &&
        userName(0, name, &nameSize) == 5 && nameSize == 5 && !std::strcmp(name, props.user) &&
        directory && directory(props.user, path, &size) > 0 && size == int(std::strlen(path)) &&
        std::string(path) + "\\FixtureData\\" == props.userPath && install && remove &&
        fileCount && fileCount(props.user, "*.*") == 0 &&
        userId && !userId(props.user, &id) && id == 123 &&
        cardCount && !cardCount(id, nullptr, &cardFiles) && cardFiles == 0 &&
        cardFile && cardFile(id, nullptr, 0, nullptr, nullptr, nullptr) != 0 &&
        install(props.user, "Fixture.prc") == ERROR_NOT_READY &&
        remove(props.user, "Fixture.prc") == ERROR_NOT_READY;
    FreeLibrary(module);
    return ok;
}

extern "C" __declspec(dllexport) long OpenConduit(Progress progress, SyncProperties& props) {
    if (!std::strcmp(props.localName, "live")) return LiveFixture();
    if (props.type != 1 || props.creator != 0x46726f6e || !props.userPath[0] ||
        std::strcmp(props.user, "Frond") || !props.remoteNames[0] ||
        std::strcmp(props.remoteNames[0], "FixtureDB"))
        return 1;
    if (!CheckInstallUser(props)) return 16;
    char message[] = "Fixture progress";
    if (!progress || progress(message)) return 2;

    const auto start = SyncOrdinal<long (*)(DWORD&)>(22);
    const auto finish = SyncOrdinal<long (*)(DWORD)>(23);
    const auto user = SyncOrdinal<long (*)(User&)>(24);
    const auto open = SyncOrdinal<long (*)(const char*, int, BYTE&, BYTE)>(26);
    const auto close = SyncOrdinal<long (*)(BYTE)>(28);
    const auto write = SyncOrdinal<long (*)(Record&)>(32);
    const auto read = SyncOrdinal<long (*)(Record&)>(36);
    const auto count = SyncProc<long (*)(BYTE, WORD&)>("SyncGetDBRecordCount");
    if (!start || !finish || !user || !open || !close || !write || !read || !count) return 3;

    DWORD token = 0;
    User info{};
    const bool implicit = !std::strcmp(props.localName, "implicit");
    if ((!implicit && (start(token) || !token)) || user(info) || info.id != 123 ||
        std::strcmp(info.name, "Frond"))
        return 4;

    BYTE handle = 0;
    const auto status = open("FixtureDB", 0, handle, 0xc0);
    if (!std::strcmp(props.localName, "deny")) {
        if (status != readOnly || finish(token)) return 5;
        return 0;
    }
    if (status || handle != 9) return 6;

    const auto find = SyncProc<long (*)(FindName&, DbInfo&)>("SyncFindDbByName");
    const auto readPref = SyncProc<long (*)(Preference&)>("SyncReadAppPreference");
    const auto writePref = SyncProc<long (*)(Preference&)>("SyncWriteAppPreference");
    if (!find || !readPref || !writePref) return 12;
    char databaseName[] = "FixtureDB";
    FindName query{0xc0, 0, databaseName};
    DbInfo metadata{};
    if (find(query, metadata) || metadata.base.creator != 0x46726f6e || metadata.records != 8 ||
        std::strcmp(metadata.base.name, databaseName))
        return 13;

    BYTE prefData[4]{};
    Preference pref{0, 0x46726f6e, 99, 4, 0, 0, TRUE, sizeof(prefData), prefData, 0};
    if (readPref(pref) || pref.version != 4 || pref.actual != 3 || pref.returned != 3 ||
        std::memcmp(prefData, "abc", 3))
        return 14;
    prefData[2] = 'd';
    pref.bufferSize = 3;
    const auto preferenceStatus = writePref(pref);
    if (!std::strcmp(props.localName, "deny-pref")) {
        if (preferenceStatus != readOnly || close(handle) || finish(token)) return 17;
        return 0;
    }
    if (preferenceStatus) return 15;

    WORD records = 0;
    if (count(handle, records) || records != 8) return 7;
    BYTE data[4]{};
    Record record{handle, 0, 7, 0, 0, 0, 0, sizeof(data), data, 0};
    if (read(record) || record.size != 3 || record.id != 555 || record.index != 7 ||
        record.attributes != 0x10 || record.category != 3 || std::memcmp(data, "old", 3))
        return 8;

    std::memcpy(data, "new", 3);
    record.bufferSize = 3;
    if (write(record) || record.size != 3 || record.id != 555) return 9;
    auto writeResource = SyncOrdinal<long (*)(Resource&)>(43);
    Resource resource{handle, 0x64617461, 123, {}, 3, 0, data, 1};
    if (!writeResource || writeResource(resource) || resource.reserved) return 24;
    if (!std::strcmp(props.localName, "failed")) return 23;
    if (close(handle) || (!implicit && finish(token))) return 10;

    const auto log = reinterpret_cast<long (*)(const char*, int, BOOL)>(
        GetProcAddress(GetModuleHandleW(L"hslog20.dll"), MAKEINTRESOURCEA(52)));
    if (!log || log("Fixture log", -1, FALSE)) return 11;
    return 0;
}
