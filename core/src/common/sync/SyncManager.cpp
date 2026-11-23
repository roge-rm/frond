#include "SyncManager.h"

#include "Dlp.h"
#include "HotSync.h"
#include "Link.h"
#include "SyncStore.h"

namespace frondSync {

const char* TransportName(Transport transport) {
    switch (transport) {
        case Transport::usb:
            return "usb";
        case Transport::modem:
            return "modem";
        case Transport::network:
            return "network";
        default:
            return "serial";
    }
}

std::string SyncStatus::Describe() const {
    switch (state) {
        case State::off:
            return "off";
        case State::waiting:
            return "waiting " + transport + (message.empty() ? "" : "; last: " + message);
        case State::syncing: {
            std::string text = "syncing " + transport + " " + stage;
            if (total) text += " " + std::to_string(done) + "/" + std::to_string(total);
            if (!item.empty()) text += " " + item;
            return text;
        }
        case State::done:
            return "done " + transport + ": " + message;
        default:
            return "failed " + transport + ": " + message;
    }
}

SyncManager& SyncManager::Get() {
    static SyncManager manager;
    return manager;
}

void SyncManager::Configure(const SyncOptions& value) {
    std::lock_guard lock(mutex);
    options = value;
    network = value.network;
}

void SyncManager::SetNetworkEnabled(bool enabled) {
    std::lock_guard lock(mutex);
    options.network = enabled;
    network = enabled;
}

bool ConfiguredHotSyncUsesNetwork(const SyncOptions& options) {
    if (options.machineKey.empty()) return false;
    SyncLibrary library;
    std::string error;
    if (!library.Open(options.root.empty() ? DefaultSyncRoot() : options.root, error)) return false;
    const auto key = MachineDeviceKey(options.machineKey);
    const auto assigned = library.FindAssignment(key);
    const auto selected = !options.profile.empty() ? library.FindProfile(options.profile)
                                                    : assigned ? library.FindProfile(assigned->profileId) : std::nullopt;
    SyncProfile profile;
    return selected && profile.Open(library.ProfilePath(*selected), error) && profile.HotSyncUsesNetwork(key);
}

SyncOptions SyncManager::Options() const {
    std::lock_guard lock(mutex);
    return options;
}

std::string SyncManager::Root() const {
    std::lock_guard lock(mutex);
    return options.root.empty() ? DefaultSyncRoot() : options.root;
}

SyncStatus SyncManager::Status() const {
    std::lock_guard lock(mutex);
    return status;
}

void SyncManager::SetListener(std::function<void(const std::string&)> value) {
    std::lock_guard lock(mutex);
    listener = std::move(value);
}

void SyncManager::Update(const std::function<void(SyncStatus&)>& change) {
    std::function<void(const std::string&)> tell;
    std::string line;
    {
        std::lock_guard lock(mutex);
        const auto before = status.Describe();
        change(status);
        status.time = NowSeconds();
        line = status.Describe();
        if (line != before) tell = listener;
    }
    if (tell) tell(line);
}

void SyncManager::EndpointOpened(Transport transport) {
    {
        std::lock_guard lock(mutex);
        ++endpoints;
    }
    Update([&](SyncStatus& s) {
        if (s.state == SyncStatus::State::off) s.state = SyncStatus::State::waiting, s.transport = TransportName(transport);
    });
}

void SyncManager::EndpointClosed(Transport transport) {
    bool last;
    {
        std::lock_guard lock(mutex);
        last = --endpoints <= 0;
        if (endpoints < 0) endpoints = 0;
    }
    if (last)
        Update([&](SyncStatus& s) {
            if (s.state == SyncStatus::State::syncing) return;
            if (transport == Transport::network &&
                (s.state == SyncStatus::State::done || s.state == SyncStatus::State::failed))
                return;
            s.state = SyncStatus::State::off;
        });
}

void SyncManager::Serve(SyncStream& stream, Transport transport, const std::atomic<bool>& stop) {
    while (!stop) {
        std::string error;
        auto link = AcceptHandheld(stream, transport, stop, error);
        if (!link) {
            if (stop || error == "closed") return;
            if (!error.empty())
                Update([&](SyncStatus& s) {
                    if (s.state == SyncStatus::State::syncing) return;
                    s.state = SyncStatus::State::failed, s.transport = TransportName(transport), s.message = error, ++s.syncs;
                });
            if (transport == Transport::network) return;
            continue;
        }
        std::unique_lock sessionLock(sessionMutex, std::try_to_lock);
        if (!sessionLock) {
            DlpClient(*link).EndOfSync(dlpTermOther);
            link->Finish();
            if (transport == Transport::network) return;
            continue;
        }
        Update([&](SyncStatus& s) {
            s.state = SyncStatus::State::syncing, s.transport = TransportName(transport), s.stage = "connecting";
            s.done = s.total = 0, s.item.clear(), s.message.clear();
        });
        const auto result = RunHotSync(*link, Options(), Root(), transport, *this, &stop);
        Update([&](SyncStatus& s) {
            s.state = result.ok ? SyncStatus::State::done : SyncStatus::State::failed;
            s.message = result.message, s.stage.clear(), s.item.clear(), s.done = s.total = 0;
            ++s.syncs;
        });
        link->Finish();
        if (transport == Transport::network) return;
    }
}

void SyncEndpoint::Start(Transport value) {
    Stop();
    transport = value;
    pipe = std::make_shared<SyncPipe>();
    end = std::make_unique<SyncPipe::End>(pipe);
    stop = false;
    finished = false;
    SyncManager::Get().EndpointOpened(transport);
    thread = std::thread([this] {
        auto& manager = SyncManager::Get();
        try {
            manager.Serve(*end, transport, stop);
        } catch (const std::exception& error) {
            manager.Update([&](SyncStatus& status) {
                status.state = SyncStatus::State::failed;
                status.transport = TransportName(transport);
                status.message = error.what();
                ++status.syncs;
            });
        }
        finished = true;
    });
}

void SyncEndpoint::Stop() {
    if (!pipe) return;
    stop = true;
    end->Abort();
    if (thread.joinable()) thread.join();
    SyncManager::Get().EndpointClosed(transport);
    end.reset();
    pipe.reset();
}

}  // namespace frondSync
