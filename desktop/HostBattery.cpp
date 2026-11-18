#include "HostBattery.h"

#if defined(Q_OS_WIN)
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#endif

namespace frondBattery {

HostState readHost() {
#if defined(Q_OS_WIN)
    SYSTEM_POWER_STATUS status;
    if (!GetSystemPowerStatus(&status)) return {};
    // BatteryFlag 128 is no battery, 255 and a percent of 255 are unknown.
    if (status.BatteryFlag == 128 || status.BatteryFlag == 255 || status.BatteryLifePercent > 100) return {};
    return {status.BatteryLifePercent, status.ACLineStatus != 0};
#elif defined(Q_OS_MACOS)
    HostState state;
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info) return state;
    CFArrayRef list = IOPSCopyPowerSourcesList(info);
    if (list) {
        for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
            auto description = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
            if (!description) continue;
            auto type = static_cast<CFStringRef>(CFDictionaryGetValue(description, CFSTR(kIOPSTypeKey)));
            if (!type || CFStringCompare(type, CFSTR(kIOPSInternalBatteryType), 0) != kCFCompareEqualTo) continue;
            int current = 0, maximum = 0;
            auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(description, CFSTR(kIOPSCurrentCapacityKey)));
            if (number) CFNumberGetValue(number, kCFNumberIntType, &current);
            number = static_cast<CFNumberRef>(CFDictionaryGetValue(description, CFSTR(kIOPSMaxCapacityKey)));
            if (number) CFNumberGetValue(number, kCFNumberIntType, &maximum);
            auto power = static_cast<CFStringRef>(CFDictionaryGetValue(description, CFSTR(kIOPSPowerSourceStateKey)));
            if (maximum <= 0) break;
            state.percent = std::clamp(current * 100 / maximum, 0, 100);
            state.charging = power && CFStringCompare(power, CFSTR(kIOPSACPowerValue), 0) == kCFCompareEqualTo;
            break;
        }
        CFRelease(list);
    }
    CFRelease(info);
    return state;
#else
    return parsePowerSupply("/sys/class/power_supply");
#endif
}

}  // namespace frondBattery
