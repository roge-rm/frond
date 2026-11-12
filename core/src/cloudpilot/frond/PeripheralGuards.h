#pragma once
#include <string>
#include "device_profile5.h"

inline std::string peripheralRefusal68k(const DeviceProfile5& profile, const std::string& command, const std::string& argument) {
        if ((command=="audio-on" || (command=="sound-mode" && argument!="off")) && !profile.capabilities.sound)
            return "Sound is not qualified for this device.";
        if (command=="serial-mode" && argument!="off" && !profile.capabilities.serial)
            return "Serial is not qualified for this device.";
        if (command=="irda-mode" && argument!="off" && !profile.capabilities.infrared)
            return "Infrared is not qualified for this device.";
        if (command=="hotsync" && !deviceProfileHasHotSyncButton5(profile.deviceType))
            return "HotSync is not qualified for this device.";
        return "";
}
