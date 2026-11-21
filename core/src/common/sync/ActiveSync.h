#ifndef FROND_ACTIVE_SYNC_H
#define FROND_ACTIVE_SYNC_H

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "AirSync.h"
#include "SynceRapi.h"
#include "SyncManager.h"

namespace frondSync {

bool ConfiguredActiveSyncUsesSerial();

// WM5 makes one TCP connection for the control messages and a second for RAPI.
// The emulation thread owns the guest network; all file and profile work runs here.
class ActiveSyncSession {
   public:
    explicit ActiveSyncSession(bool serial = false) : serial(serial) {}
    ~ActiveSyncSession();
    ActiveSyncSession(const ActiveSyncSession&) = delete;
    ActiveSyncSession& operator=(const ActiveSyncSession&) = delete;

    void Start();
    void Stop();
    bool Running() const { return running; }
    SyncPipe& Pipe(size_t channel) { return *pipes[channel]; }
    // A pipe for a new guest connection to desktop port 26675, or nullptr while Pocket Outlook
    // sync isn't running. Called from the emulation thread.
    std::shared_ptr<SyncPipe> AcceptAirSync();

   private:
    void Run();
    // Serves Desktop AirSync until the guest's sync finishes. Returns the summary.
    std::string SyncPocketOutlook(FrondRapi* client, SyncProfile& profile, const std::string& key,
                                  const std::string& guid, const std::vector<PimKind>& kinds);
    std::mutex airMutex;
    AirSyncServer* air{nullptr};
    std::vector<std::thread> airThreads;
    std::vector<std::shared_ptr<SyncPipe>> airPipes;
    std::array<std::shared_ptr<SyncPipe>, 2> pipes{std::make_shared<SyncPipe>(), std::make_shared<SyncPipe>()};
    const bool serial;
    std::thread worker;
    std::atomic<bool> running{false};
};

}  // namespace frondSync

#endif
