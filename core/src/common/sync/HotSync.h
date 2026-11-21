#ifndef FROND_SYNC_HOTSYNC_H
#define FROND_SYNC_HOTSYNC_H

// One HotSync with a handheld that has started it, in HotSync Manager's order: system and
// user info (a new user is named and given an ID), Install, personal data and Backup, then the user's
// last-sync PC and date, the handheld's log entry and EndOfSync.

#include <string>

#include "Dlp.h"
#include "PimSync.h"
#include "SyncManager.h"
#include "SyncStore.h"

namespace frondSync {

struct HotSyncResult {
    bool ok{false};
    std::string profile, user, deviceKey;
    bool fast{false};  // the handheld last synced with this profile
    int installed{0}, installFailed{0}, backedUp{0}, backupSkipped{0}, backupFailed{0};
    PimSyncCounts pim;
    bool reset{false};
    std::string message;
};

HotSyncResult RunHotSync(PacketLink& link, const SyncOptions& options, const std::string& root, Transport transport,
                         SyncManager& manager);

}  // namespace frondSync

#endif
