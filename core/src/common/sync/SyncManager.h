#ifndef FROND_SYNC_MANAGER_H
#define FROND_SYNC_MANAGER_H

// Frond's built-in HotSync desktop. A core process has one manager; its HotSync ports
// (serial, USB, the modem's HotSync number, Network HotSync through the virtual router) set
// to "sync" each run an endpoint, a thread that waits for the handheld to start a HotSync and
// runs it against the assigned profile. The frontends read Status() through the cores'
// sync-state command and start a sync with sync-start, which presses the device's HotSync
// button with a port routed here. Thread-safe.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "SyncStream.h"

namespace frondSync {

enum class Transport { serial, usb, modem, network };
const char* TransportName(Transport transport);

struct SyncOptions {
    std::string root;        // the sync folder; empty: the platform default
    std::string profile;     // profile name for this machine; empty: its stored assignment, else Default
    std::string machineKey;  // the frontend's id for this machine; empty: key by the Palm user
    bool network{false};    // route the virtual host's Network HotSync to this manager
    std::string userName;    // name for a handheld that has no user yet; empty: the profile's name
    int networkReplyIdleMs{0}; // override the default DLP reply deadline for a slow network route
};

bool ConfiguredHotSyncUsesNetwork(const SyncOptions& options);

struct SyncStatus {
    enum class State { off, waiting, syncing, done, failed };
    State state{State::off};
    std::string transport;  // the port the current or last sync used
    std::string stage;      // connecting, user, install, backup, finishing
    int done{0}, total{0};  // progress in the stage
    std::string item;       // the database being worked on
    std::string profile, user;
    std::string message;    // the last sync's summary or why it failed
    uint64_t syncs{0};      // syncs finished, ok or not, since the process started
    int64_t time{0};        // when the state last changed, UTC seconds
    // One line: off | waiting PORT | syncing PORT STAGE N/M ITEM | done ... | failed: WHY.
    std::string Describe() const;
};

class SyncManager {
   public:
    static SyncManager& Get();

    void Configure(const SyncOptions& options);
    SyncOptions Options() const;
    bool NetworkEnabled() const { return network; }
    void SetNetworkEnabled(bool enabled);
    std::string Root() const;  // the folder in use, the default filled in
    SyncStatus Status() const;
    // Called with each Describe() line when the state changes; on the engine's thread.
    void SetListener(std::function<void(const std::string&)> listener);

    // Runs on the caller's thread. Network streams carry one HotSync per connection.
    void Serve(SyncStream& stream, Transport transport, const std::atomic<bool>& stop);

    // Endpoints count themselves so Status() shows waiting while one is open.
    void EndpointOpened(Transport transport);
    void EndpointClosed(Transport transport);

    // Used by the session while it runs.
    void Update(const std::function<void(SyncStatus&)>& change);

   private:
    SyncManager() = default;
    mutable std::mutex mutex;
    std::mutex sessionMutex;
    SyncOptions options;
    std::atomic<bool> network{false};
    SyncStatus status;
    int endpoints{0};
    std::function<void(const std::string&)> listener;
};

// A thread running SyncManager::Serve on a stream, for a transport that polls a SyncPipe.
class SyncEndpoint {
   public:
    SyncEndpoint() = default;
    ~SyncEndpoint() { Stop(); }
    SyncEndpoint(const SyncEndpoint&) = delete;
    SyncEndpoint& operator=(const SyncEndpoint&) = delete;

    void Start(Transport transport);
    void Stop();
    bool Running() const { return bool(pipe); }
    bool Finished() const { return finished; }
    // The transport's side; null while stopped.
    SyncPipe* Pipe() const { return pipe.get(); }

   private:
    std::shared_ptr<SyncPipe> pipe;
    std::unique_ptr<SyncPipe::End> end;
    std::atomic<bool> stop{false}, finished{false};
    std::thread thread;
    Transport transport{Transport::serial};
};

}  // namespace frondSync

#endif
