#ifndef FROND_ACTIVE_SYNC_PARTNERSHIP_H
#define FROND_ACTIVE_SYNC_PARTNERSHIP_H

#include <cstdint>
#include <string>
#include <vector>

#include "PimRecord.h"

#include "SynceRapi.h"

namespace frondSync {

bool HasActiveSyncPartnership(FrondRapi* client, uint32_t profileId, const std::string& deviceGuid);

// Creates or verifies this profile's desktop partnership without reusing someone else's slot.
uint32_t EnsureActiveSyncPartnership(FrondRapi* client, uint32_t profileId, const std::string& deviceGuid);

// Makes this profile's partnership the current one, enables the Pocket Outlook providers Frond
// serves (kinds; the others are turned off) and asks the device to start a Desktop AirSync sync with it. The device resets the
// current partnership when it disconnects, so this runs on every connect.
void StartActiveSyncDesktopSync(FrondRapi* client, uint32_t profileId, const std::string& deviceGuid,
                                const std::vector<PimKind>& kinds);

// The device's TIME_ZONE_INFORMATION in base64 from its registry, or empty when it can't be read.
// source, if given, gets the registry key it came from.
std::string ReadActiveSyncTimeZone(FrondRapi* client, std::string* source = nullptr);

}  // namespace frondSync

#endif
