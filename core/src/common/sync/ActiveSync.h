#ifndef FROND_ACTIVE_SYNC_H
#define FROND_ACTIVE_SYNC_H

#include <array>
#include <atomic>
#include <memory>
#include <thread>

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

   private:
    void Run();
    std::array<std::shared_ptr<SyncPipe>, 2> pipes{std::make_shared<SyncPipe>(), std::make_shared<SyncPipe>()};
    const bool serial;
    std::thread worker;
    std::atomic<bool> running{false};
};

}  // namespace frondSync

#endif
