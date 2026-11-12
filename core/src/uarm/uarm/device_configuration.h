#ifndef _DEVICE_CONFIGURATION_H_
#define _DEVICE_CONFIGURATION_H_

#include <cstdint>

#include "device_type5.h"

#include "device_profile5.h"

struct DisplayConfiguration {
    uint16_t width{0};
    uint16_t height{0};
    uint16_t graffitiHeight{0};
    uint16_t density{0};
};

bool deviceConfigurationDeviceSupportsDisplayMode(DeviceType5 deviceType, DisplayMode displayMode);

DisplayMode deviceConfigurationDefaultDisplayMode(DeviceType5 deviceType);

DisplayConfiguration deviceConfigurationDisplayConfigForMode(DisplayMode mode);
DisplayConfiguration deviceConfigurationDisplayConfigForDevice(DeviceType5 type, DisplayMode mode);

bool deviceConfigurationSupportsRamSize(DeviceType5 deviceType, uint32_t ramSize);

uint32_t deviceConfigurationGetNandSize(DeviceType5 deviceType);

#endif  // _DEVICE_CONFIGURATION_H_
