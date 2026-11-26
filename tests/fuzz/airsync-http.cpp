// Desktop AirSync: HTTP requests from the device on one keep-alive connection, against a
// scratch profile.
#include "FuzzStream.h"
#include "Scratch.h"
#include "sync/AirSync.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    using namespace frondSync;
    static unsigned runs = 0;
    const auto root = FuzzScratch("airsync-http", runs);
    std::string error;
    SyncLibrary library;
    if (!library.Open(root, error)) return 0;
    auto info = library.FindProfile("Fuzz");
    ProfileInfo created;
    if (!info) {
        if (!library.CreateProfile("Fuzz", created, error)) return 0;
        info = created;
    }
    SyncProfile profile;
    if (!profile.Open(library.ProfilePath(*info), error)) return 0;
    FuzzedDataProvider data(bytes, size);
    AirSyncServer server(profile, "machine:fuzz", {PimKind::contacts, PimKind::calendar, PimKind::tasks});
    FuzzStream stream(data);
    server.Serve(stream);
    server.FinishActions(error);
    return 0;
}
