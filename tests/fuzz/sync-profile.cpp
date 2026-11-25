// Sync profile and library databases (SQLite files in the sync folder) as Frond opens and reads
// them, for a sync folder copied from elsewhere.
#include <fuzzer/FuzzedDataProvider.h>

#include <fstream>

#include "Scratch.h"
#include "sync/SyncStore.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    using namespace frondSync;
    namespace fs = std::filesystem;
    static unsigned runs = 0;
    runs = 0;  // a fresh folder every run
    const auto root = FuzzScratch("sync-profile", runs);
    FuzzedDataProvider data(bytes, size);
    const bool library = data.ConsumeBool();
    const auto file = data.ConsumeRemainingBytes<uint8_t>();
    std::ofstream(fs::path(root) / (library ? "profiles.db" : "profile.db"), std::ios::binary)
        .write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    std::string error;
    if (library) {
        SyncLibrary lib;
        if (lib.Open(root, error)) {
            for (const auto& p : lib.Profiles()) lib.ProfilePath(p);
            lib.Assignments();
        }
        return 0;
    }
    SyncProfile profile;
    if (!profile.Open(root, error)) return 0;
    for (const auto& device : profile.BackupDevices(true)) {
        for (const auto& entry : profile.Backups(device)) {
            Bytes backup;
            profile.LoadBackup(device, entry.name, backup);
        }
    }
    for (const auto& item : profile.InstallQueue()) {
        Bytes install;
        profile.LoadInstall(item.id, install);
    }
    for (int kind = 0; kind < 4; ++kind) profile.PimRecords(PimKind(kind), true);
    profile.LogEntries("machine:fuzz");
    profile.PimLinks("machine:fuzz", error);
    return 0;
}
