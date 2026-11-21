#ifndef FROND_SYNC_AIR_SYNC_H
#define FROND_SYNC_AIR_SYNC_H

// The desktop end of Desktop AirSync, which Windows Mobile 5 uses to sync Pocket Outlook with
// ActiveSync: Exchange ActiveSync 2.5 over HTTP (MS-ASHTTP, MS-ASCMD, MS-ASWBXML) on desktop
// port 26675. Contacts, calendar and tasks sync to the profile's PIM records.

#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "SyncStore.h"
#include "SyncStream.h"

namespace frondSync {

struct AirSyncRequest {
    std::string method, target;
    std::map<std::string, std::string> headers;  // names in lower case
    Bytes body;
};

struct AirSyncResponse {
    int status{200};
    std::vector<std::pair<std::string, std::string>> headers;
    Bytes body;
};

struct AirSyncCounts {
    unsigned toDevice{0}, fromDevice{0}, deleted{0}, conflicts{0};
};

// Thread-safe: each guest connection calls Handle or Serve from its own thread.
class AirSyncServer {
   public:
    // deviceKey scopes the links and sync keys; kinds are the folders offered.
    AirSyncServer(SyncProfile& profile, std::string deviceKey, std::vector<PimKind> kinds);

    AirSyncResponse Handle(const AirSyncRequest& request);
    // Answers requests on one keep-alive connection until it closes or Stop is called.
    void Serve(SyncStream& stream);
    void Stop();

    // The device's TIME_ZONE_INFORMATION in base64, as read from its registry. Calendar items
    // it sends update it too. Kept in the profile for later syncs.
    void SetDeviceTimeZone(const std::string& base64);

    // Ends the one-time overwrite actions of the folders that synced. Call once the guest has
    // finished; the device sends each kind of change in its own request, so the action holds
    // for the whole connection.
    bool FinishActions(std::string& error);

    // Appends each request and answer, for qualification captures.
    void SetTrace(FILE* file) { trace = file; }

    // True once every folder has synced with nothing left to send, or the guest posted SyncEnd.
    bool Finished() const;
    // Time since the last request; the time since construction before the first one.
    std::chrono::steady_clock::duration Idle() const;
    unsigned Requests() const;
    AirSyncCounts Counts() const;
    std::string LastError() const;

   private:
    AirSyncResponse HandleLocked(const AirSyncRequest& request);
    Bytes FolderSync(const Bytes& body, int& status);
    Bytes Sync(const AirSyncRequest& frame, int& status);
    Bytes Estimate(const Bytes& body, int& status);
    void Trace(const std::string& text);
    // The folder's PIM action, read once per connection.
    PimAction Action(PimKind kind);

    SyncProfile& profile;
    const std::string deviceKey;
    std::vector<PimKind> kinds;
    mutable std::mutex mutex;
    std::chrono::steady_clock::time_point last;
    unsigned requests{0};
    bool syncEnd{false}, stopping{false};
    std::map<PimKind, bool> settled, overwriteQueued;
    std::map<PimKind, PimAction> actions;
    AirSyncCounts counts;
    std::string lastError, timeZone;
    // The last Sync request and its answer, to repeat when the guest retries a lost answer.
    std::string lastSyncTarget;
    Bytes lastSyncRequest, lastSyncResponse;
    FILE* trace{nullptr};
};

// The folder ServerId Frond gives a kind, and its item ServerIds as "<folder>:<record id>".
std::string AirSyncFolderId(PimKind kind);
std::string AirSyncBase64(const Bytes& data);

}  // namespace frondSync

#endif
