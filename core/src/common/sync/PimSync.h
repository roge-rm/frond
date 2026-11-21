#ifndef FROND_SYNC_PIM_SYNC_H
#define FROND_SYNC_PIM_SYNC_H

#include "Dlp.h"
#include "SyncStore.h"

namespace frondSync {

struct PimSyncCounts {
    unsigned downloaded{0}, uploaded{0}, deleted{0}, archived{0}, conflicts{0};
};

struct PalmPimDatabase {
    PimKind kind;
    PalmPimFormat format;
    std::string name;
};

std::vector<PalmPimDatabase> SelectPalmPimDatabases(const std::vector<DbInfo>& list);

// Scope binds IDs to a device, user and wire database. Only a completed conduit can use
// incremental reads on its next run.
bool SyncPalmPim(DlpClient& dlp, SyncProfile& profile, const std::string& deviceKey,
                 const std::string& userKey, const PalmPimDatabase& database, bool fast,
                 PimSyncCounts& counts, std::string& error);

}  // namespace frondSync

#endif
