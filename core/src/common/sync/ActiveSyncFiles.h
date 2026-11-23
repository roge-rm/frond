#ifndef FROND_ACTIVE_SYNC_FILES_H
#define FROND_ACTIVE_SYNC_FILES_H

#include <string>
#include <vector>

#include "SynceRapi.h"

namespace frondSync {

class SyncProfile;

std::vector<std::string> ListActiveSyncFiles(const std::string& profileFolder);
void AddActiveSyncFile(const std::string& profileFolder, const std::string& name, const std::string& source);
void ExportActiveSyncFile(const std::string& profileFolder, const std::string& name, const std::string& destination);

// An existing profile Files folder enables exchange with My Documents\Frond Sync.
std::string ExchangeActiveSyncFiles(FrondRapi* client, SyncProfile& profile, const std::string& device,
                                    const std::string& profileFolder, bool syncNotes);

}  // namespace frondSync

#endif
