#include "bluetooth-hotsync.h"

#include <cstdio>

#include "sync/SyncManager.h"

namespace {

frondSync::SyncEndpoint& Endpoint() {
    // Construct the manager first so the endpoint can stop safely at process exit.
    frondSync::SyncManager::Get();
    static frondSync::SyncEndpoint endpoint;
    return endpoint;
}

}  // namespace

void frond_bt_sync_configure(const char* folder, const char* profile) {
    frondSync::SyncOptions options;
    options.root = folder;
    if (profile) options.profile = profile;
    auto& manager = frondSync::SyncManager::Get();
    manager.Configure(options);
    manager.SetListener([](const std::string& status) {
        std::fprintf(stderr, "SPP HotSync: %s\n", status.c_str());
    });
}

void frond_bt_sync_open() { Endpoint().Start(frondSync::Transport::serial); }

void frond_bt_sync_close() { Endpoint().Stop(); }

bool frond_bt_sync_input(const uint8_t* data, size_t size) {
    auto* pipe = Endpoint().Pipe();
    return pipe && pipe->RoomFromDevice() >= size && pipe->DeviceToHost(data, size) == size;
}

size_t frond_bt_sync_output(uint8_t* data, size_t max) {
    auto* pipe = Endpoint().Pipe();
    return pipe ? pipe->HostToDevice(data, max) : 0;
}
