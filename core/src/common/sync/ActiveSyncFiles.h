#ifndef FROND_ACTIVE_SYNC_FILES_H
#define FROND_ACTIVE_SYNC_FILES_H

#include <string>
#include <vector>

#include "SynceRapi.h"

namespace frondSync {

std::vector<std::string> ListActiveSyncFiles(const std::string& profileFolder);
void AddActiveSyncFile(const std::string& profileFolder, const std::string& name, const std::string& source);
void ExportActiveSyncFile(const std::string& profileFolder, const std::string& name, const std::string& destination);

// An existing profile Files folder enables exchange with My Documents\Frond Sync.
std::string ExchangeActiveSyncFiles(FrondRapi* client, const std::string& folder);

}  // namespace frondSync

#endif
