#ifndef FROND_ACTIVE_SYNC_PARTNERSHIP_H
#define FROND_ACTIVE_SYNC_PARTNERSHIP_H

#include <cstdint>
#include <string>

#include "SynceRapi.h"

namespace frondSync {

bool HasActiveSyncPartnership(FrondRapi* client, uint32_t profileId, const std::string& deviceGuid);

// Creates or verifies this profile's desktop partnership without reusing someone else's slot.
uint32_t EnsureActiveSyncPartnership(FrondRapi* client, uint32_t profileId, const std::string& deviceGuid);

}  // namespace frondSync

#endif
